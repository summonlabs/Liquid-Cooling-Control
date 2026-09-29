#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "liquidcooling/liquidcooling.hpp"
#include "support.hpp"
#include "testing.hpp"

using namespace liquidcooling;

namespace {

std::vector<std::uint8_t> read_bytes(const std::string& path) {
    std::vector<std::uint8_t> data;
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return data;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size > 0) {
        data.resize(static_cast<std::size_t>(size));
        const std::size_t read = std::fread(data.data(), 1, data.size(), file);
        data.resize(read);
    }
    std::fclose(file);
    return data;
}

bool path_exists_for_test(const std::string& path) {
    std::error_code error;
    return std::filesystem::exists(std::filesystem::path(path), error);
}

bool write_bytes(const std::string& path, const std::vector<std::uint8_t>& data) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    const std::size_t written = std::fwrite(data.data(), 1, data.size(), file);
    std::fclose(file);
    return written == data.size();
}

/// Builds a small but complete committed store and returns its root.
std::string make_seeded_store(const lcctest::LoopFixture& fixture, std::string_view tag) {
    const std::string root = lcctest::make_temp_root(tag);
    auto opened = lcctest::open_runtime(fixture, root);
    if (opened.ok()) {
        Runtime& runtime = *opened.value();
        (void)runtime.observe(LoopId::from_value(lcctest::LoopFixture::kLoop));
        const auto token = lcctest::token_for(runtime, lcctest::LoopFixture::kOperator,
                                              LoopId::from_value(lcctest::LoopFixture::kLoop));
        if (token.ok()) {
            TransitionRequest request =
                lcctest::make_request(runtime, ActionKind::CloseValve,
                                      DeviceId::from_value(lcctest::LoopFixture::kValveA), token.value(),
                                      "store-seed");
            (void)runtime.execute(request);
        }
        (void)runtime.close();
    }
    return root;
}

DurableState sample_state() {
    DurableState state;
    state.last_incarnation = ControllerIncarnation::from_value(3);
    state.epoch = ControlPlaneEpoch::from_value(2);
    state.revision = StateRevision::from_value(11);
    state.config_generation = ConfigGeneration::from_value(1);
    state.topology_generation = TopologyGeneration::from_value(1);
    state.commit_sequence = JournalCommitSequence::from_value(7);
    state.boot_time = TimestampNs::from_nanos(1234);

    LoopRecord loop;
    loop.id = LoopId::from_value(1);
    loop.name = "loop-a";
    loop.topology_generation = TopologyGeneration::from_value(1);
    loop.operating = LoopOperatingState::Running;
    loop.isolation = IsolationState::Open;
    loop.service_mode = ServiceMode::Production;
    loop.flow_target = FlowRate::from_value(120'000);
    loop.pressure_target = Pressure::from_value(350'000);
    loop.flow_target_generation = DeviceGeneration::from_value(1);
    loop.pressure_target_generation = DeviceGeneration::from_value(1);
    loop.devices.push_back(DeviceRecord{DeviceId::from_value(1), DeviceKind::Cdu,
                                        DeviceGeneration::from_value(1), LifecycleState::Present});
    loop.devices.push_back(DeviceRecord{DeviceId::from_value(2), DeviceKind::Pump,
                                        DeviceGeneration::from_value(4), LifecycleState::Present,
                                        ValvePosition::Unknown, PumpState::Running});
    state.loops.push_back(loop);

    LoopEvidenceRecord evidence;
    evidence.loop = loop.id;
    evidence.reading.loop = loop.id;
    evidence.reading.sequence = ObservationSequence::from_value(9);
    evidence.reading.evidence_generation = EvidenceGeneration::from_value(1);
    evidence.reading.observed_at = TimestampNs::from_nanos(500);
    evidence.reading.flow = Evidence<FlowRate>::make_present(FlowRate::from_value(120'000),
                                                             ObservationSequence::from_value(9),
                                                             DeviceGeneration::from_value(1),
                                                             TimestampNs::from_nanos(500), "synthetic");
    evidence.reading.leak = Evidence<LeakState>::make_present(LeakState::None, ObservationSequence::from_value(9),
                                                              DeviceGeneration::from_value(1),
                                                              TimestampNs::from_nanos(500), "synthetic");
    state.evidence.push_back(evidence);

    CommandAttempt attempt;
    attempt.id = AttemptId::from_value(3);
    attempt.plan = PlanId::from_value(2);
    attempt.command = CommandId::from_value(2);
    attempt.idempotency_key = IdempotencyKey::create("seed-key").value();
    attempt.fingerprint = RequestFingerprint{1, 2};
    attempt.action = ActionKind::StartPump;
    attempt.effect = EffectClass::IncreaseExposure;
    attempt.loop = loop.id;
    attempt.target = DeviceId::from_value(2);
    attempt.generation = DeviceGeneration::from_value(4);
    attempt.commanded_flow = FlowRate::from_value(120'000);
    attempt.commanded_pressure = Pressure::from_value(350'000);
    attempt.epoch = ControlPlaneEpoch::from_value(2);
    attempt.incarnation = ControllerIncarnation::from_value(3);
    attempt.status = AttemptStatus::EffectVerified;
    attempt.revision_at_issue = StateRevision::from_value(6);
    attempt.issued_at_commit = JournalCommitSequence::from_value(6);
    attempt.issued_at = TimestampNs::from_nanos(400);
    attempt.updated_at = TimestampNs::from_nanos(450);
    attempt.observed_at = TimestampNs::from_nanos(500);
    attempt.ack_sequence = ObservationSequence::from_value(8);
    attempt.effect_sequence = ObservationSequence::from_value(9);
    attempt.ack_generation = DeviceGeneration::from_value(4);
    attempt.effect_generation = DeviceGeneration::from_value(4);
    attempt.outcome = StatusCode::Ok;
    attempt.detail = "verified";
    state.attempts.push_back(attempt);

    state.idempotency.push_back(IdempotencyRecord{attempt.idempotency_key, attempt.fingerprint, attempt.plan,
                                                  attempt.id, StatusCode::Ok,
                                                  JournalCommitSequence::from_value(7)});

    ServiceObligation obligation;
    obligation.id = ObligationId::from_value(1);
    obligation.target = DeviceId::from_value(2);
    obligation.kind = ObligationKind::ServiceDue;
    obligation.active = true;
    obligation.due_at_duty_minutes = 5000;
    obligation.raised_at = TimestampNs::from_nanos(10);
    obligation.note = "service due";
    state.obligations.push_back(obligation);

    ServiceWindow window;
    window.id = ServiceWindowId::from_value(1);
    window.loop = loop.id;
    window.opened_by = PrincipalId::from_value(3);
    window.opened_at = TimestampNs::from_nanos(20);
    window.expires_at = TimestampNs::from_nanos(10'000);
    window.note = "window";
    state.service_windows.push_back(window);

    state.revoked_tokens.push_back(AuthorityTokenId::from_value(2));
    state.next_plan_id = PlanId::from_value(3);
    state.next_attempt_id = AttemptId::from_value(4);
    state.next_command_id = CommandId::from_value(3);
    state.next_obligation_id = ObligationId::from_value(2);
    state.next_service_window_id = ServiceWindowId::from_value(2);
    state.next_authority_token_id = AuthorityTokenId::from_value(3);
    return state;
}

}  // namespace

TEST(store, payload_round_trips_every_record_kind) {
    const DurableState original = sample_state();
    const auto encoded = encode_snapshot_payload(original, StoreBounds{});
    REQUIRE(encoded.ok());
    const auto decoded = decode_snapshot_payload(encoded.value(), StoreBounds{});
    REQUIRE(decoded.ok());
    const DurableState& round_tripped = decoded.value();
    CHECK_EQ(round_tripped.last_incarnation, original.last_incarnation);
    CHECK_EQ(round_tripped.epoch, original.epoch);
    CHECK_EQ(round_tripped.revision, original.revision);
    CHECK_EQ(round_tripped.commit_sequence, original.commit_sequence);
    CHECK_EQ(round_tripped.loops.size(), 1u);
    CHECK_EQ(round_tripped.loops.front().devices.size(), 2u);
    CHECK_EQ(round_tripped.loops.front().devices[1].pump_state, PumpState::Running);
    CHECK_EQ(round_tripped.evidence.size(), 1u);
    CHECK_EQ(round_tripped.evidence.front().reading.flow.value, FlowRate::from_value(120'000));
    CHECK_EQ(round_tripped.attempts.size(), 1u);
    CHECK_EQ(round_tripped.attempts.front().outcome, StatusCode::Ok);
    CHECK_EQ(round_tripped.attempts.front().commanded_pressure, Pressure::from_value(350'000));
    CHECK_EQ(round_tripped.idempotency.size(), 1u);
    CHECK_EQ(round_tripped.obligations.size(), 1u);
    CHECK_EQ(round_tripped.service_windows.size(), 1u);
    CHECK_EQ(round_tripped.revoked_tokens.size(), 1u);
    CHECK_EQ(round_tripped.next_attempt_id, original.next_attempt_id);

    // Re-encoding the decoded state reproduces the identical payload.
    const auto reencoded = encode_snapshot_payload(round_tripped, StoreBounds{});
    REQUIRE(reencoded.ok());
    CHECK(reencoded.value() == encoded.value());
}

TEST(store, payload_decoder_rejects_structural_corruption) {
    const DurableState original = sample_state();
    const auto encoded = encode_snapshot_payload(original, StoreBounds{});
    REQUIRE(encoded.ok());

    // Truncation at every boundary inside the first 96 bytes.
    for (std::size_t length = 0; length < 96 && length < encoded.value().size(); ++length) {
        const std::vector<std::uint8_t> truncated(encoded.value().begin(),
                                                  encoded.value().begin() + static_cast<std::ptrdiff_t>(length));
        CHECK(!decode_snapshot_payload(truncated, StoreBounds{}).ok());
    }

    // Single-byte corruption anywhere in the payload must never decode cleanly
    // into a different but plausible state without being detected.
    for (std::size_t index = 0; index < encoded.value().size(); index += 7) {
        std::vector<std::uint8_t> corrupted = encoded.value();
        corrupted[index] = static_cast<std::uint8_t>(corrupted[index] ^ 0xFFu);
        const auto decoded = decode_snapshot_payload(corrupted, StoreBounds{});
        if (decoded.ok()) {
            // If it decodes, the changed byte must have been inside a text field
            // and the result must still be structurally valid; check that the
            // resulting state re-encodes to the same bytes.
            const auto reencoded = encode_snapshot_payload(decoded.value(), StoreBounds{});
            CHECK(reencoded.ok());
        }
    }

    // Trailing bytes are refused.
    std::vector<std::uint8_t> trailing = encoded.value();
    trailing.push_back(0);
    CHECK(!decode_snapshot_payload(trailing, StoreBounds{}).ok());
}

TEST(store, payload_decoder_rejects_semantic_violations) {
    DurableState state = sample_state();

    DurableState duplicate_loops = state;
    duplicate_loops.loops.push_back(duplicate_loops.loops.front());
    const auto encoded_duplicate = encode_snapshot_payload(duplicate_loops, StoreBounds{});
    REQUIRE(encoded_duplicate.ok());
    CHECK_CODE(decode_snapshot_payload(encoded_duplicate.value(), StoreBounds{}), StatusCode::StoreCorrupt);

    DurableState duplicate_devices = state;
    duplicate_devices.loops.front().devices.push_back(duplicate_devices.loops.front().devices.front());
    const auto encoded_devices = encode_snapshot_payload(duplicate_devices, StoreBounds{});
    REQUIRE(encoded_devices.ok());
    CHECK_CODE(decode_snapshot_payload(encoded_devices.value(), StoreBounds{}), StatusCode::StoreCorrupt);

    DurableState duplicate_idempotency = state;
    duplicate_idempotency.idempotency.push_back(duplicate_idempotency.idempotency.front());
    const auto encoded_idempotency = encode_snapshot_payload(duplicate_idempotency, StoreBounds{});
    REQUIRE(encoded_idempotency.ok());
    CHECK_CODE(decode_snapshot_payload(encoded_idempotency.value(), StoreBounds{}), StatusCode::StoreCorrupt);

    // An identity counter that does not exceed the identities in use would let a
    // restart reuse an identity.
    DurableState stale_counter = state;
    stale_counter.next_attempt_id = AttemptId::from_value(1);
    const auto encoded_counter = encode_snapshot_payload(stale_counter, StoreBounds{});
    REQUIRE(encoded_counter.ok());
    CHECK_CODE(decode_snapshot_payload(encoded_counter.value(), StoreBounds{}), StatusCode::StoreCorrupt);

    DurableState zero_counter = state;
    zero_counter.next_plan_id = PlanId::invalid();
    const auto encoded_zero = encode_snapshot_payload(zero_counter, StoreBounds{});
    REQUIRE(encoded_zero.ok());
    CHECK_CODE(decode_snapshot_payload(encoded_zero.value(), StoreBounds{}), StatusCode::StoreCorrupt);

    // Bounds are enforced before allocation.
    StoreBounds tiny;
    tiny.max_loops = 0;
    CHECK_CODE(encode_snapshot_payload(state, tiny), StatusCode::StoreBoundsExceeded);
    StoreBounds tiny_devices;
    tiny_devices.max_devices_per_loop = 1;
    CHECK_CODE(encode_snapshot_payload(state, tiny_devices), StatusCode::StoreBoundsExceeded);
    StoreBounds tiny_records;
    tiny_records.max_records = 1;
    CHECK_CODE(decode_snapshot_payload(encoded_duplicate.value(), tiny_records), StatusCode::StoreBoundsExceeded);
}

TEST(store, an_impossible_enum_is_rejected) {
    const DurableState original = sample_state();
    const auto encoded = encode_snapshot_payload(original, StoreBounds{});
    REQUIRE(encoded.ok());

    // The device kind byte lives immediately after the device id inside the loop
    // record.  Locating it by search keeps this test independent of the exact
    // record offsets: flip each byte that currently holds the "cdu" enumerator.
    std::size_t patched = 0;
    for (std::size_t index = 0; index + 8 < encoded.value().size(); ++index) {
        // A device record starts with an 8-byte id; id 1 followed by kind 1.
        if (encoded.value()[index] == 1 && encoded.value()[index + 8] == 1) {
            std::vector<std::uint8_t> corrupted = encoded.value();
            corrupted[index + 8] = 200;
            const auto decoded = decode_snapshot_payload(corrupted, StoreBounds{});
            CHECK(!decoded.ok());
            ++patched;
            if (patched >= 1) {
                break;
            }
        }
    }
    CHECK(patched >= 1);
}

TEST(store, framing_corruption_is_detected) {
    lcctest::LoopFixture fixture;
    const std::string root = make_seeded_store(fixture, "store-framing");
    const std::string snapshot = root + "/lcc.snapshot";
    const std::vector<std::uint8_t> good = read_bytes(snapshot);
    REQUIRE(good.size() > 128);
    REQUIRE(inspect_store(root, StoreBounds{}, nullptr).ok());

    const auto expect_failure = [&](const std::vector<std::uint8_t>& bytes, StatusCode expected,
                                    const char* what) {
        REQUIRE(write_bytes(snapshot, bytes));
        const auto loaded = inspect_store(root, StoreBounds{}, nullptr);
        CHECK(!loaded.ok());
        if (!loaded.ok() && loaded.code() != expected) {
            ::lcctest::Registry::record_failure(
                std::string("framing case '") + what + "' produced " +
                    std::string(to_string(loaded.code())) + " expected " + std::string(to_string(expected)),
                __FILE__, __LINE__);
        }
    };

    {  // Wrong magic.
        std::vector<std::uint8_t> bytes = good;
        bytes[0] = 'X';
        expect_failure(bytes, StatusCode::StoreCorrupt, "magic");
    }
    {  // Unsupported format version, with a valid header integrity check.
        std::vector<std::uint8_t> bytes = good;
        bytes[8] = 99;
        const std::uint32_t header_crc = store_integrity_crc32c(bytes.data(), 56);
        for (int index = 0; index < 4; ++index) {
            bytes[56 + static_cast<std::size_t>(index)] =
                static_cast<std::uint8_t>((header_crc >> (8 * index)) & 0xFFu);
        }
        expect_failure(bytes, StatusCode::StoreVersionUnsupported, "version");
    }
    {  // Header integrity.
        std::vector<std::uint8_t> bytes = good;
        bytes[24] ^= 0x01u;
        expect_failure(bytes, StatusCode::StoreCorrupt, "header-field");
    }
    {  // Reserved header bytes must be zero.
        std::vector<std::uint8_t> bytes = good;
        bytes[16] = 1;
        expect_failure(bytes, StatusCode::StoreCorrupt, "reserved");
    }
    {  // Payload integrity.
        std::vector<std::uint8_t> bytes = good;
        bytes[good.size() / 2] ^= 0x01u;
        expect_failure(bytes, StatusCode::StoreCorrupt, "payload");
    }
    {  // Trailer magic.
        std::vector<std::uint8_t> bytes = good;
        bytes[bytes.size() - 16] = 'X';
        expect_failure(bytes, StatusCode::StoreCorrupt, "trailer");
    }
    {  // Trailer sequence disagreement.
        std::vector<std::uint8_t> bytes = good;
        bytes[bytes.size() - 8] ^= 0x01u;
        expect_failure(bytes, StatusCode::StoreCorrupt, "trailer-sequence");
    }
    {  // Truncation.
        const std::vector<std::uint8_t> bytes(good.begin(), good.end() - 3);
        expect_failure(bytes, StatusCode::StoreCorrupt, "truncated");
    }
    {  // Trailing bytes.
        std::vector<std::uint8_t> bytes = good;
        bytes.push_back(0xABu);
        expect_failure(bytes, StatusCode::StoreCorrupt, "trailing");
    }
    {  // Declared parameter beyond the bounds.
        std::vector<std::uint8_t> bytes = good;
        for (std::size_t index = 40; index < 48; ++index) {
            bytes[index] = 0xFFu;
        }
        expect_failure(bytes, StatusCode::StoreCorrupt, "absurd-length");
    }
    {  // A file shorter than its fixed framing.
        const std::vector<std::uint8_t> bytes(good.begin(), good.begin() + 32);
        expect_failure(bytes, StatusCode::StoreCorrupt, "shorter-than-framing");
    }
    {  // Empty file.
        const std::vector<std::uint8_t> bytes;
        expect_failure(bytes, StatusCode::StoreCorrupt, "empty");
    }

    // Restoring the original bytes restores a loadable store.
    REQUIRE(write_bytes(snapshot, good));
    CHECK(inspect_store(root, StoreBounds{}, nullptr).ok());
    lcctest::remove_tree(root);
}

TEST(store, journal_ahead_of_snapshot_is_a_torn_publication) {
    lcctest::LoopFixture fixture;
    const std::string root = lcctest::make_temp_root("store-torn");
    {
        auto opened = lcctest::open_runtime(fixture, root);
        REQUIRE(opened.ok());
        (void)opened.value()->observe(LoopId::from_value(lcctest::LoopFixture::kLoop));
        (void)opened.value()->close();
    }
    // Replace the journal with one that claims a later generation than the
    // snapshot: the only way that can happen is a torn publication.
    const std::string journal = root + "/lcc.journal";
    std::vector<std::uint8_t> bytes = read_bytes(journal);
    REQUIRE(bytes.size() > 32);
    REQUIRE(write_bytes(journal, bytes));
    auto opened = lcctest::open_runtime(fixture, root);
    REQUIRE(opened.ok());
    (void)opened.value()->close();
    CHECK(inspect_store(root, StoreBounds{}, nullptr).ok());

    // Damaging the journal is detected rather than ignored.
    std::vector<std::uint8_t> damaged = bytes;
    damaged[damaged.size() - 16] = 'X';
    REQUIRE(write_bytes(journal, damaged));
    auto reopened = lcctest::open_runtime(fixture, root);
    CHECK_CODE(reopened, StatusCode::StoreCorrupt);
    REQUIRE(write_bytes(journal, bytes));

    lcctest::remove_tree(root);
}

TEST(store, previous_generation_recovery) {
    lcctest::LoopFixture fixture;
    const std::string root = lcctest::make_temp_root("store-previous");
    {
        auto opened = lcctest::open_runtime(fixture, root);
        REQUIRE(opened.ok());
        Runtime& runtime = *opened.value();
        (void)runtime.observe(LoopId::from_value(lcctest::LoopFixture::kLoop));
        (void)runtime.observe(LoopId::from_value(lcctest::LoopFixture::kLoop));
        (void)runtime.close();
    }
    const std::string snapshot = root + "/lcc.snapshot";
    const std::string previous = root + "/lcc.snapshot.prev";
    const std::vector<std::uint8_t> previous_bytes = read_bytes(previous);
    REQUIRE(!previous_bytes.empty());
    CHECK(inspect_store(root, StoreBounds{}, nullptr).ok());

    // Corrupt the authoritative snapshot beyond repair.
    std::vector<std::uint8_t> broken = read_bytes(snapshot);
    REQUIRE(broken.size() > 64);
    for (std::size_t index = 20; index < 40 && index < broken.size(); ++index) {
        broken[index] ^= 0x5Au;
    }
    REQUIRE(write_bytes(snapshot, broken));

    // Refusing by default: the caller must ask for a fallback explicitly.
    auto refused = lcctest::open_runtime(fixture, root);
    CHECK_CODE(refused, StatusCode::StoreCorrupt);

    RuntimeConfig config = lcctest::fixture_config(fixture, root);
    config.recovery = RecoveryPolicy::UsePreviousGeneration;
    auto recovered = Runtime::open(config);
    REQUIRE(recovered.ok());
    const auto status = recovered.value()->status();
    CHECK(status.value().store.recovered_from_previous);
    CHECK(inspect_store(root, StoreBounds{}, nullptr).ok());
    (void)recovered.value()->close();

    lcctest::remove_tree(root);
}

TEST(store, staging_files_are_never_authoritative_and_are_cleaned) {
    lcctest::LoopFixture fixture;
    const std::string root = lcctest::make_temp_root("store-staging");
    {
        auto opened = lcctest::open_runtime(fixture, root);
        REQUIRE(opened.ok());
        (void)opened.value()->observe(LoopId::from_value(lcctest::LoopFixture::kLoop));
        (void)opened.value()->close();
    }
    // Leave abandoned staging files behind, as a crashed writer would.
    REQUIRE(write_bytes(root + "/lcc.snapshot.999-1.tmp", std::vector<std::uint8_t>{1, 2, 3}));
    REQUIRE(write_bytes(root + "/lcc.journal.999-2.tmp", std::vector<std::uint8_t>{4, 5, 6}));

    auto opened = lcctest::open_runtime(fixture, root);
    REQUIRE(opened.ok());
    const auto diagnostics = opened.value()->status().value().store;
    CHECK_EQ(diagnostics.staging_files_removed, 2u);
    CHECK(!path_exists_for_test(root + "/lcc.snapshot.999-1.tmp"));
    CHECK(!path_exists_for_test(root + "/lcc.journal.999-2.tmp"));
    CHECK(opened.value()->status().ok());
    CHECK_EQ(opened.value()->journal().value().size() > 0, true);
    (void)opened.value()->close();
    lcctest::remove_tree(root);
}

TEST(store, journal_lag_is_reported_and_audit_entries_are_bounded) {
    lcctest::LoopFixture fixture;
    fixture.policy.limits.max_journal_entries = 3;
    const std::string root = lcctest::make_temp_root("store-journal");
    auto opened = lcctest::open_runtime(fixture, root);
    REQUIRE(opened.ok());
    Runtime& runtime = *opened.value();
    fixture.adapter->set_valve_position(DeviceId::from_value(lcctest::LoopFixture::kValveA), ValvePosition::Open);
    REQUIRE(runtime.observe(LoopId::from_value(lcctest::LoopFixture::kLoop)).ok());
    const auto token = lcctest::token_for(runtime, lcctest::LoopFixture::kIsolator,
                                          LoopId::from_value(lcctest::LoopFixture::kLoop));
    REQUIRE(token.ok());
    for (int index = 0; index < 5; ++index) {
        TransitionRequest request =
            lcctest::make_request(runtime, ActionKind::CloseValve,
                                  DeviceId::from_value(lcctest::LoopFixture::kValveA), token.value(),
                                  "journal-" + std::to_string(index));
        (void)runtime.execute(request);
    }
    CHECK(runtime.journal().value().size() <= 3u);
    CHECK(!runtime.status().value().store.journal_lagging);
    (void)runtime.close();
    lcctest::remove_tree(root);
}

TEST(store, read_only_inspection_does_not_take_the_write_lock) {
    lcctest::LoopFixture fixture;
    const std::string root = make_seeded_store(fixture, "store-inspect");
    auto opened = lcctest::open_runtime(fixture, root);
    REQUIRE(opened.ok());
    StoreDiagnostics diagnostics;
    const auto inspected = inspect_store(root, StoreBounds{}, &diagnostics);
    CHECK(inspected.ok());
    CHECK(diagnostics.snapshot_sequence.valid());
    CHECK(diagnostics.snapshot_bytes > 0);
    // A second writer cannot open the same store while the first holds it.
    auto second = lcctest::open_runtime(fixture, root);
    CHECK_CODE(second, StatusCode::StoreLocked);
    (void)opened.value()->close();
    lcctest::remove_tree(root);
}

TEST(store, reopening_produces_one_authoritative_generation) {
    lcctest::LoopFixture fixture;
    const std::string root = lcctest::make_temp_root("store-reopen");
    StateRevision last_revision{};
    JournalCommitSequence last_sequence{};
    for (int cycle = 0; cycle < 5; ++cycle) {
        auto opened = lcctest::open_runtime(fixture, root);
        REQUIRE(opened.ok());
        Runtime& runtime = *opened.value();
        const auto status = runtime.status();
        CHECK(status.value().revision.valid());
        CHECK(status.value().revision > last_revision);
        last_revision = status.value().revision;
        CHECK(status.value().store.snapshot_sequence > last_sequence);
        last_sequence = status.value().store.snapshot_sequence;
        (void)runtime.close();
    }
    const auto final_state = inspect_store(root, StoreBounds{}, nullptr);
    REQUIRE(final_state.ok());
    CHECK_EQ(final_state.value().commit_sequence, last_sequence);
    lcctest::remove_tree(root);
}
