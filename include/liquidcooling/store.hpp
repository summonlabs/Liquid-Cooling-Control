#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "liquidcooling/attempt.hpp"
#include "liquidcooling/evidence.hpp"
#include "liquidcooling/model.hpp"
#include "liquidcooling/status.hpp"

namespace liquidcooling {

/// On-disk format version of the durable store.
inline constexpr std::uint32_t kStoreFormatVersion = 1;
inline constexpr std::size_t kStoreHeaderBytes = 64;
inline constexpr std::size_t kStoreRecordHeaderBytes = 16;
inline constexpr std::size_t kStoreTrailerBytes = 16;
inline constexpr std::size_t kStoreMaxTextBytes = 512;

/// Bounds applied to every quantity that a store file can declare.
struct StoreBounds final {
    std::size_t max_file_bytes{16u * 1024u * 1024u};
    std::size_t max_records{8192};
    std::size_t max_record_body_bytes{1u * 1024u * 1024u};
    std::size_t max_journal_entries{512};
    std::size_t max_staging_files{64};
    std::size_t max_loops{64};
    std::size_t max_devices_per_loop{64};
    std::size_t max_attempts{256};
    std::size_t max_idempotency_records{1024};
    std::size_t max_obligations{256};
    std::size_t max_service_windows{64};
    std::size_t max_revoked_tokens{4096};
};

/// Policy applied when the authoritative snapshot fails validation.
enum class RecoveryPolicy : std::uint8_t {
    /// Refuse to open; the caller must decide explicitly.
    RefuseOnCorruption = 0,
    /// Fall back to the previous complete generation when it is intact.
    UsePreviousGeneration = 1,
};

/// The durable evidence snapshot for one loop.
struct LoopEvidenceRecord final {
    LoopId loop{};
    LoopReading reading{};
};

/// Everything the runtime persists.
///
/// The store contains control state and *recovered* observations.  Recovered
/// observations are marked stale on load and can never authorise a transition
/// until a current adapter read revalidates them.
struct DurableState final {
    ControllerIncarnation last_incarnation{};
    ControlPlaneEpoch epoch{};
    StateRevision revision{};
    ConfigGeneration config_generation{};
    TopologyGeneration topology_generation{};
    JournalCommitSequence commit_sequence{};
    EvidenceGeneration evidence_generation{};
    SwitchoverGeneration switchover_generation{};
    TimestampNs boot_time{};
    std::vector<LoopRecord> loops{};
    std::vector<LoopEvidenceRecord> evidence{};
    std::vector<CommandAttempt> attempts{};
    std::vector<IdempotencyRecord> idempotency{};
    std::vector<ServiceObligation> obligations{};
    std::vector<ServiceWindow> service_windows{};
    std::vector<AuthorityTokenId> revoked_tokens{};

    // Identity counters.  They are persisted so that an identity is never
    // reused after a restart, even when older records have been trimmed.
    PlanId next_plan_id{PlanId::from_value(1)};
    AttemptId next_attempt_id{AttemptId::from_value(1)};
    CommandId next_command_id{CommandId::from_value(1)};
    ObligationId next_obligation_id{ObligationId::from_value(1)};
    ServiceWindowId next_service_window_id{ServiceWindowId::from_value(1)};
    AuthorityTokenId next_authority_token_id{AuthorityTokenId::from_value(1)};
};

/// One bounded audit record describing a completed transition.
struct JournalEntry final {
    JournalCommitSequence commit{};
    StateRevision revision{};
    TimestampNs timestamp{};
    ActionKind action{ActionKind::Unknown};
    StatusCode outcome{StatusCode::Ok};
    AttemptStatus attempt_status{AttemptStatus::Unknown};
    LoopId loop{};
    DeviceId target{};
    AttemptId attempt{};
    RequestFingerprint fingerprint{};
    std::string detail{};
};

/// Observeable health of the durable store.
struct StoreDiagnostics final {
    bool opened{false};
    bool fresh{false};
    bool recovered_from_previous{false};
    bool journal_lagging{false};
    bool journal_absent{false};
    JournalCommitSequence snapshot_sequence{};
    JournalCommitSequence journal_sequence{};
    std::size_t journal_entries{0};
    std::size_t snapshot_bytes{0};
    std::size_t staging_files_removed{0};
    std::uint64_t commits_completed{0};
    std::string root_display{};
    std::string root_identity{};
    std::string snapshot_modified_utc_nanos{};
};

/// The durable store: one exclusive writer, atomic publication, integrity checked.
///
/// Publication protocol and commit point:
///   1. stage the snapshot payload into a uniquely named temporary file,
///      flush it and read it back;
///   2. stage the journal payload the same way;
///   3. copy the current snapshot to the previous-generation file;
///   4. atomically replace the snapshot  <-- COMMIT POINT
///   5. atomically replace the journal.
/// A crash before step 4 leaves the previous generation authoritative; a crash
/// between steps 4 and 5 leaves the new snapshot authoritative with a lagging
/// journal, which is reported rather than hidden.
class DurableStore final {
public:
    [[nodiscard]] static Result<std::unique_ptr<DurableStore>> open(const std::string& root,
                                                                   const StoreBounds& bounds,
                                                                   bool create_if_missing,
                                                                   RecoveryPolicy recovery,
                                                                   std::string_view owner_note);

    ~DurableStore();
    DurableStore(const DurableStore&) = delete;
    DurableStore& operator=(const DurableStore&) = delete;

    [[nodiscard]] const DurableState& state() const noexcept { return state_; }
    [[nodiscard]] const StoreDiagnostics& diagnostics() const noexcept { return diagnostics_; }
    [[nodiscard]] const std::string& root_display() const noexcept { return diagnostics_.root_display; }

    /// Publishes a new generation.  The in-memory state is updated only after the
    /// snapshot replace has succeeded.
    [[nodiscard]] Result<void> commit(DurableState next, std::vector<JournalEntry> entries);

    /// Replaces the authoritative snapshot with the previous complete generation.
    [[nodiscard]] Result<void> recover_from_previous();

    /// Re-reads and re-validates both files without mutating anything.
    [[nodiscard]] Result<void> verify();

    [[nodiscard]] Result<std::vector<JournalEntry>> read_journal() const;

private:
    DurableStore() = default;

    [[nodiscard]] Result<void> load(bool create_if_missing, RecoveryPolicy recovery);
    [[nodiscard]] Result<void> remove_stale_staging();
    [[nodiscard]] Result<DurableState> load_snapshot_file(const std::string& path,
                                                          std::vector<JournalEntry>* unused) const;

    struct Impl;
    std::unique_ptr<Impl> impl_{};
    DurableState state_{};
    StoreDiagnostics diagnostics_{};
};

/// Read-only inspection of a store directory.  Takes no write lock, so it works
/// while a runtime holds the store open.
[[nodiscard]] Result<DurableState> inspect_store(const std::string& root, const StoreBounds& bounds,
                                                 StoreDiagnostics* diagnostics);

/// Encodes a snapshot payload.  Exposed so that corruption tests can construct
/// well-formed and deliberately malformed payloads.
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_snapshot_payload(const DurableState& state,
                                                                        const StoreBounds& bounds);

/// Encodes a journal payload.
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_journal_payload(const DurableState& state,
                                                                       const std::vector<JournalEntry>& entries,
                                                                       const StoreBounds& bounds);

/// Decodes a snapshot payload, applying every structural and semantic check.
[[nodiscard]] Result<DurableState> decode_snapshot_payload(std::span<const std::uint8_t> payload,
                                                           const StoreBounds& bounds);

/// The CRC-32C used by the durable format, exposed so that integrity tooling and
/// corruption tests can build and check framed files without duplicating it.
[[nodiscard]] std::uint32_t store_integrity_crc32c(const void* data, std::size_t size) noexcept;

/// Demotes every adapter-origin evidence channel to recovered-origin so that
/// persisted observations can never be mistaken for current physical evidence.
[[nodiscard]] Result<void> mark_evidence_recovered(DurableState& state);

/// Decodes a journal payload.
[[nodiscard]] Result<std::vector<JournalEntry>> decode_journal_payload(std::span<const std::uint8_t> payload,
                                                                       const StoreBounds& bounds,
                                                                       JournalCommitSequence* sequence);

}  // namespace liquidcooling
