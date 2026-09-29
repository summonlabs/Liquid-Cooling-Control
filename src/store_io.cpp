#include <algorithm>
#include <span>
#include <string>
#include <utility>

#include "liquidcooling/store.hpp"
#include "platform.hpp"
#include "store_framing.hpp"

namespace liquidcooling {
namespace {

constexpr std::string_view kSnapshotFileName = "lcc.snapshot";
constexpr std::string_view kJournalFileName = "lcc.journal";
constexpr std::string_view kPreviousFileName = "lcc.snapshot.prev";
constexpr std::string_view kLockFileName = "lcc.store.lock";
constexpr std::string_view kSnapshotMagic = "LCCSNP01";
constexpr std::string_view kJournalMagic = "LCCJNL01";

[[nodiscard]] bool looks_like_staging_file(std::string_view name) {
    return name.size() > 4 && name.rfind("lcc.", 0) == 0 && name.compare(name.size() - 4, 4, ".tmp") == 0;
}

[[nodiscard]] Result<std::vector<JournalEntry>> read_journal_file(const std::string& path,
                                                                 const StoreBounds& bounds,
                                                                 JournalCommitSequence* sequence) {
    const auto bytes = detail::read_file(path, bounds.max_file_bytes);
    if (!bytes.ok()) {
        return bytes.status();
    }
    const auto parsed = detail::parse_framed_file(bytes.value(), kJournalMagic, bounds);
    if (!parsed.ok()) {
        return parsed.status();
    }
    auto entries = decode_journal_payload(parsed.value().payload, bounds, sequence);
    if (!entries.ok()) {
        return entries.status();
    }
    if (parsed.value().record_count != entries.value().size() + 1) {
        return Status{StatusCode::StoreCorrupt, "journal record count disagrees with its framing header"};
    }
    return entries.value();
}

}  // namespace

struct DurableStore::Impl final {
    detail::CanonicalPath root{};
    detail::ExclusiveLock lock{};
    StoreBounds bounds{};
};

DurableStore::~DurableStore() = default;

Result<void> DurableStore::remove_stale_staging() {
    const auto listing = detail::list_directory(root_display(), impl_->bounds.max_staging_files + 16);
    if (!listing.ok()) {
        return listing.status();
    }
    for (const auto& name : listing.value()) {
        if (!looks_like_staging_file(name)) {
            continue;
        }
        if (diagnostics_.staging_files_removed >= impl_->bounds.max_staging_files) {
            return Status{StatusCode::StoreBoundsExceeded,
                          "more staging files are present than the configured maximum"};
        }
        const auto path = impl_->root.child(name);
        if (!path.ok()) {
            return path.status();
        }
        const auto removed = detail::remove_file(path.value());
        if (!removed.ok()) {
            return removed.status();
        }
        ++diagnostics_.staging_files_removed;
    }
    return ok_result();
}

Result<std::unique_ptr<DurableStore>> DurableStore::open(const std::string& root, const StoreBounds& bounds,
                                                         bool create_if_missing, RecoveryPolicy recovery,
                                                         std::string_view owner_note) {
    const auto canonical = detail::CanonicalPath::create(root, create_if_missing);
    if (!canonical.ok()) {
        return canonical.status();
    }
    const auto lock_path = canonical.value().child(kLockFileName);
    if (!lock_path.ok()) {
        return lock_path.status();
    }
    auto lock = detail::ExclusiveLock::acquire(lock_path.value(), owner_note);
    if (!lock.ok()) {
        return lock.status();
    }

    std::unique_ptr<DurableStore> store(new DurableStore());
    store->impl_ = std::make_unique<Impl>();
    store->impl_->root = canonical.value();
    store->impl_->lock = std::move(lock.value());
    store->impl_->bounds = bounds;
    store->diagnostics_.root_display = canonical.value().display();
    store->diagnostics_.root_identity = canonical.value().identity();

    const auto loaded = store->load(create_if_missing, recovery);
    if (!loaded.ok()) {
        return loaded.status();
    }
    store->diagnostics_.opened = true;
    return store;
}

Result<void> DurableStore::load(bool create_if_missing, RecoveryPolicy recovery) {
    const auto staging = remove_stale_staging();
    if (!staging.ok()) {
        return staging.status();
    }

    const auto snapshot_path = impl_->root.child(kSnapshotFileName);
    if (!snapshot_path.ok()) {
        return snapshot_path.status();
    }
    const auto journal_path = impl_->root.child(kJournalFileName);
    if (!journal_path.ok()) {
        return journal_path.status();
    }
    const auto previous_path = impl_->root.child(kPreviousFileName);
    if (!previous_path.ok()) {
        return previous_path.status();
    }

    const auto snapshot_exists = detail::path_exists(snapshot_path.value());
    if (!snapshot_exists.ok()) {
        return snapshot_exists.status();
    }

    if (!snapshot_exists.value()) {
        const auto previous_exists = detail::path_exists(previous_path.value());
        if (previous_exists.ok() && previous_exists.value()) {
            return Status{StatusCode::StoreCorrupt,
                          "the authoritative snapshot is absent while a previous generation exists; "
                          "explicit recovery is required"};
        }
        if (!create_if_missing) {
            return Status{StatusCode::StoreMissing, "no durable store exists at the configured root"};
        }
        state_ = DurableState{};
        diagnostics_.fresh = true;
        return ok_result();
    }

    const auto snapshot_bytes = detail::read_file(snapshot_path.value(), impl_->bounds.max_file_bytes);
    if (!snapshot_bytes.ok()) {
        return snapshot_bytes.status();
    }
    diagnostics_.snapshot_bytes = snapshot_bytes.value().size();

    const auto parsed = detail::parse_framed_file(snapshot_bytes.value(), kSnapshotMagic, impl_->bounds);
    if (!parsed.ok()) {
        if (recovery != RecoveryPolicy::UsePreviousGeneration) {
            return Status{parsed.status().code(),
                          std::string(parsed.status().message()) +
                              "; open with previous-generation recovery to fall back explicitly"};
        }
        return recover_from_previous();
    }

    auto decoded = decode_snapshot_payload(parsed.value().payload, impl_->bounds);
    if (!decoded.ok()) {
        if (recovery != RecoveryPolicy::UsePreviousGeneration) {
            return Status{decoded.status().code(),
                          std::string(decoded.status().message()) +
                              "; open with previous-generation recovery to fall back explicitly"};
        }
        return recover_from_previous();
    }

    state_ = std::move(decoded.value());
    diagnostics_.snapshot_sequence = state_.commit_sequence;
    if (parsed.value().sequence != state_.commit_sequence) {
        return Status{StatusCode::StoreCorrupt, "snapshot framing sequence disagrees with its payload"};
    }
    const auto modified = detail::file_modified_unix_nanos(snapshot_path.value());
    if (modified.ok()) {
        diagnostics_.snapshot_modified_utc_nanos = std::to_string(modified.value());
    }

    const auto journal_exists = detail::path_exists(journal_path.value());
    if (!journal_exists.ok()) {
        return journal_exists.status();
    }
    if (!journal_exists.value()) {
        diagnostics_.journal_absent = true;
        diagnostics_.journal_lagging = state_.commit_sequence.valid();
        return ok_result();
    }
    JournalCommitSequence journal_sequence{};
    const auto entries = read_journal_file(journal_path.value(), impl_->bounds, &journal_sequence);
    if (!entries.ok()) {
        return entries.status();
    }
    diagnostics_.journal_sequence = journal_sequence;
    diagnostics_.journal_entries = entries.value().size();
    if (state_.commit_sequence < journal_sequence) {
        return Status{StatusCode::StoreCorrupt,
                      "the journal is ahead of the authoritative snapshot; publication was torn"};
    }
    if (journal_sequence < state_.commit_sequence) {
        diagnostics_.journal_lagging = true;
    }
    return ok_result();
}

Result<void> DurableStore::recover_from_previous() {
    const auto previous_path = impl_->root.child(kPreviousFileName);
    if (!previous_path.ok()) {
        return previous_path.status();
    }
    const auto exists = detail::path_exists(previous_path.value());
    if (!exists.ok()) {
        return exists.status();
    }
    if (!exists.value()) {
        return Status{StatusCode::StoreMissing, "no previous generation is available for recovery"};
    }
    const auto bytes = detail::read_file(previous_path.value(), impl_->bounds.max_file_bytes);
    if (!bytes.ok()) {
        return bytes.status();
    }
    const auto parsed = detail::parse_framed_file(bytes.value(), kSnapshotMagic, impl_->bounds);
    if (!parsed.ok()) {
        return Status{parsed.status().code(),
                      std::string("previous generation is unusable: ") + std::string(parsed.status().message())};
    }
    auto decoded = decode_snapshot_payload(parsed.value().payload, impl_->bounds);
    if (!decoded.ok()) {
        return Status{decoded.status().code(),
                      std::string("previous generation is unusable: ") + std::string(decoded.status().message())};
    }
    DurableState recovered = std::move(decoded.value());
    if (parsed.value().sequence != recovered.commit_sequence) {
        return Status{StatusCode::StoreCorrupt, "previous generation framing disagrees with its payload"};
    }
    if (state_.commit_sequence.valid() && !(state_.commit_sequence < recovered.commit_sequence) &&
        state_.commit_sequence.valid()) {
        // A recovery that would move the store backwards is refused: the
        // authoritative generation must never regress silently.
        if (!(recovered.commit_sequence < state_.commit_sequence)) {
            return Status{StatusCode::StoreCorrupt,
                          "previous generation is not older than the authoritative generation"};
        }
    }
    // Republish the previous generation as the newest generation so that exactly
    // one authoritative generation exists after recovery.
    recovered.commit_sequence = recovered.commit_sequence.next();
    const auto status = commit(std::move(recovered), {});
    if (!status.ok()) {
        return status;
    }
    diagnostics_.recovered_from_previous = true;
    return ok_result();
}

Result<void> DurableStore::commit(DurableState next, std::vector<JournalEntry> entries) {
    if (!impl_ || !impl_->lock.held()) {
        return Status{StatusCode::StoreNotOpen, "the durable store is not open"};
    }
    if (entries.size() > impl_->bounds.max_journal_entries) {
        entries.erase(entries.begin(),
                      entries.begin() + static_cast<std::ptrdiff_t>(entries.size() - impl_->bounds.max_journal_entries));
    }
    next.commit_sequence = state_.commit_sequence.valid() ? state_.commit_sequence.next()
                                                          : JournalCommitSequence::from_value(1);

    const auto snapshot_payload = encode_snapshot_payload(next, impl_->bounds);
    if (!snapshot_payload.ok()) {
        return snapshot_payload.status();
    }
    const auto journal_payload = encode_journal_payload(next, entries, impl_->bounds);
    if (!journal_payload.ok()) {
        return journal_payload.status();
    }

    // Reject a payload that cannot be decoded after encoding: publication must
    // never be able to introduce state the loader would refuse.
    const auto round_trip = decode_snapshot_payload(snapshot_payload.value(), impl_->bounds);
    if (!round_trip.ok()) {
        return Status{StatusCode::InternalError,
                      std::string("snapshot payload failed self-validation: ") +
                          std::string(round_trip.status().message())};
    }

    const std::uint64_t snapshot_records = 1 + next.loops.size() + next.evidence.size() + next.attempts.size() +
                                           next.idempotency.size() + next.obligations.size() +
                                           next.service_windows.size() + next.revoked_tokens.size();
    const std::uint64_t journal_records = 1 + entries.size();

    const auto snapshot_file =
        detail::frame_file(kSnapshotMagic, next.commit_sequence, snapshot_records, snapshot_payload.value());
    const auto journal_file = detail::frame_file(kJournalMagic, next.commit_sequence, journal_records,
                                                 journal_payload.value());

    const auto snapshot_path = impl_->root.child(kSnapshotFileName);
    if (!snapshot_path.ok()) {
        return snapshot_path.status();
    }
    const auto journal_path = impl_->root.child(kJournalFileName);
    if (!journal_path.ok()) {
        return journal_path.status();
    }
    const auto previous_path = impl_->root.child(kPreviousFileName);
    if (!previous_path.ok()) {
        return previous_path.status();
    }
    const std::string suffix = detail::staging_suffix();
    const auto snapshot_stage = impl_->root.child("lcc.snapshot." + suffix);
    if (!snapshot_stage.ok()) {
        return snapshot_stage.status();
    }
    const auto journal_stage = impl_->root.child("lcc.journal." + suffix);
    if (!journal_stage.ok()) {
        return journal_stage.status();
    }
    const auto previous_stage = impl_->root.child("lcc.snapshot.prev." + suffix);
    if (!previous_stage.ok()) {
        return previous_stage.status();
    }

    auto discard_staging = [&]() {
        (void)detail::remove_file(snapshot_stage.value());
        (void)detail::remove_file(journal_stage.value());
        (void)detail::remove_file(previous_stage.value());
    };

    // Step 1 and 2: stage both files and verify what the file system accepted.
    const auto snapshot_written = detail::write_file_durable(snapshot_stage.value(), snapshot_file);
    if (!snapshot_written.ok()) {
        discard_staging();
        return snapshot_written.status();
    }
    const auto journal_written = detail::write_file_durable(journal_stage.value(), journal_file);
    if (!journal_written.ok()) {
        discard_staging();
        return journal_written.status();
    }

    // Step 3: preserve the generation that is about to be superseded.
    const auto current_exists = detail::path_exists(snapshot_path.value());
    if (current_exists.ok() && current_exists.value()) {
        const auto current_bytes = detail::read_file(snapshot_path.value(), impl_->bounds.max_file_bytes);
        if (!current_bytes.ok()) {
            discard_staging();
            return current_bytes.status();
        }
        const auto previous_written =
            detail::write_file_durable(previous_stage.value(), current_bytes.value());
        if (!previous_written.ok()) {
            discard_staging();
            return previous_written.status();
        }
        const auto published_previous = detail::replace_file(previous_stage.value(), previous_path.value());
        if (!published_previous.ok()) {
            discard_staging();
            return published_previous.status();
        }
    }

    // Step 4: the commit point.  Everything before this is staging.
    const auto published = detail::replace_file(snapshot_stage.value(), snapshot_path.value());
    if (!published.ok()) {
        discard_staging();
        return published.status();
    }
    state_ = std::move(next);
    ++diagnostics_.commits_completed;
    diagnostics_.fresh = false;
    diagnostics_.snapshot_sequence = state_.commit_sequence;

    // Step 5: publish the audit journal.  A failure here leaves a lagging
    // journal, which is reported rather than hidden; the snapshot remains the
    // single authoritative generation.
    const auto journal_published = detail::replace_file(journal_stage.value(), journal_path.value());
    if (!journal_published.ok()) {
        (void)detail::remove_file(previous_stage.value());
        diagnostics_.journal_lagging = true;
        return ok_result();
    }
    diagnostics_.journal_lagging = false;
    diagnostics_.journal_absent = false;
    diagnostics_.journal_sequence = state_.commit_sequence;
    diagnostics_.journal_entries = entries.size();
    return ok_result();
}

Result<void> DurableStore::verify() {
    if (!impl_ || !impl_->lock.held()) {
        return Status{StatusCode::StoreNotOpen, "the durable store is not open"};
    }
    const auto snapshot_path = impl_->root.child(kSnapshotFileName);
    if (!snapshot_path.ok()) {
        return snapshot_path.status();
    }
    const auto bytes = detail::read_file(snapshot_path.value(), impl_->bounds.max_file_bytes);
    if (!bytes.ok()) {
        return bytes.status();
    }
    const auto parsed = detail::parse_framed_file(bytes.value(), kSnapshotMagic, impl_->bounds);
    if (!parsed.ok()) {
        return parsed.status();
    }
    const auto decoded = decode_snapshot_payload(parsed.value().payload, impl_->bounds);
    if (!decoded.ok()) {
        return decoded.status();
    }
    if (decoded.value().commit_sequence != state_.commit_sequence) {
        return Status{StatusCode::StoreCorrupt, "on-disk snapshot does not match the open store generation"};
    }
    return ok_result();
}

Result<std::vector<JournalEntry>> DurableStore::read_journal() const {
    if (!impl_ || !impl_->lock.held()) {
        return Status{StatusCode::StoreNotOpen, "the durable store is not open"};
    }
    const auto journal_path = impl_->root.child(kJournalFileName);
    if (!journal_path.ok()) {
        return journal_path.status();
    }
    const auto exists = detail::path_exists(journal_path.value());
    if (!exists.ok()) {
        return exists.status();
    }
    if (!exists.value()) {
        return std::vector<JournalEntry>{};
    }
    JournalCommitSequence ignored{};
    return read_journal_file(journal_path.value(), impl_->bounds, &ignored);
}

Result<DurableState> inspect_store(const std::string& root, const StoreBounds& bounds,
                                   StoreDiagnostics* diagnostics) {
    const auto canonical = detail::CanonicalPath::create(root, false);
    if (!canonical.ok()) {
        return canonical.status();
    }
    if (diagnostics != nullptr) {
        diagnostics->root_display = canonical.value().display();
        diagnostics->root_identity = canonical.value().identity();
    }
    const auto snapshot_path = canonical.value().child(kSnapshotFileName);
    if (!snapshot_path.ok()) {
        return snapshot_path.status();
    }
    const auto bytes = detail::read_file(snapshot_path.value(), bounds.max_file_bytes);
    if (!bytes.ok()) {
        return bytes.status();
    }
    const auto parsed = detail::parse_framed_file(bytes.value(), kSnapshotMagic, bounds);
    if (!parsed.ok()) {
        return parsed.status();
    }
    const auto decoded = decode_snapshot_payload(parsed.value().payload, bounds);
    if (!decoded.ok()) {
        return decoded.status();
    }
    if (diagnostics != nullptr) {
        diagnostics->snapshot_sequence = decoded.value().commit_sequence;
        diagnostics->snapshot_bytes = bytes.value().size();
        const auto modified = detail::file_modified_unix_nanos(snapshot_path.value());
        if (modified.ok()) {
            diagnostics->snapshot_modified_utc_nanos = std::to_string(modified.value());
        }
    }
    return decoded.value();
}

}  // namespace liquidcooling
