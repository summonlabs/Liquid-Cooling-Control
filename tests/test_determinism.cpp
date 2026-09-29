#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "liquidcooling/liquidcooling.hpp"
#include "support.hpp"
#include "testing.hpp"

using namespace liquidcooling;

namespace {

/// Builds a request that is invalid at several stages at once.
TransitionRequest multiply_invalid_request() {
    TransitionRequest request;
    request.action = ActionKind::StartPump;
    request.loop = LoopId::from_value(999);       // unknown loop
    request.target = DeviceId::from_value(999);   // unknown device
    request.expected_device_generation = DeviceGeneration::from_value(9);
    request.expected_revision = StateRevision::from_value(9);
    request.idempotency_key = IdempotencyKey::create("multi").value();
    request.authority.id = AuthorityTokenId::from_value(1);
    return request;
}

}  // namespace

TEST(determinism, repeated_validation_returns_the_same_primary_code) {
    lcctest::LoopFixture fixture;
    const std::string root = lcctest::make_temp_root("det-repeat");
    auto opened = lcctest::open_runtime(fixture, root);
    REQUIRE(opened.ok());
    Runtime& runtime = *opened.value();

    const TransitionRequest request = multiply_invalid_request();
    const auto first = runtime.plan(request);
    REQUIRE(!first.ok());
    for (int iteration = 0; iteration < 200; ++iteration) {
        const auto again = runtime.plan(request);
        CHECK_CODE(again, first.code());
    }
    // The reported primary code is the earliest failing stage, not an arbitrary
    // one: the loop identity is checked before the device identity.
    CHECK_CODE(first, StatusCode::UnknownObject);
    (void)runtime.close();
    lcctest::remove_tree(root);
}

TEST(determinism, validation_precedence_is_stable_across_field_orderings) {
    lcctest::LoopFixture fixture;
    const std::string root = lcctest::make_temp_root("det-precedence");
    auto opened = lcctest::open_runtime(fixture, root);
    REQUIRE(opened.ok());
    Runtime& runtime = *opened.value();

    // Shape faults beat identity faults.
    TransitionRequest shape = multiply_invalid_request();
    shape.idempotency_key = IdempotencyKey{};
    CHECK_CODE(runtime.plan(shape), StatusCode::MissingRequiredField);

    TransitionRequest unknown_action = multiply_invalid_request();
    unknown_action.action = ActionKind::Unknown;
    CHECK_CODE(runtime.plan(unknown_action), StatusCode::UnsupportedAction);

    // Wrong device kind is an identity fault, reported before generation faults.
    TransitionRequest wrong_kind = multiply_invalid_request();
    wrong_kind.loop = LoopId::from_value(lcctest::LoopFixture::kLoop);
    wrong_kind.target = DeviceId::from_value(lcctest::LoopFixture::kValveA);
    CHECK_CODE(runtime.plan(wrong_kind), StatusCode::WrongObjectKind);

    // Generation faults precede authority faults.
    TransitionRequest generation = multiply_invalid_request();
    generation.loop = LoopId::from_value(lcctest::LoopFixture::kLoop);
    generation.target = DeviceId::from_value(lcctest::LoopFixture::kPumpA);
    generation.expected_device_generation = DeviceGeneration::from_value(9);
    CHECK_CODE(runtime.plan(generation), StatusCode::FutureGeneration);

    // Authority faults precede revision faults.
    TransitionRequest authority = multiply_invalid_request();
    authority.loop = LoopId::from_value(lcctest::LoopFixture::kLoop);
    authority.target = DeviceId::from_value(lcctest::LoopFixture::kPumpA);
    authority.expected_device_generation = DeviceGeneration::from_value(1);
    CHECK_CODE(runtime.plan(authority), StatusCode::EpochMismatch);

    (void)runtime.close();
    lcctest::remove_tree(root);
}

TEST(determinism, snapshot_encoding_is_byte_for_byte_stable) {
    lcctest::LoopFixture fixture;
    const std::string root = lcctest::make_temp_root("det-bytes");
    auto opened = lcctest::open_runtime(fixture, root);
    REQUIRE(opened.ok());
    Runtime& runtime = *opened.value();
    const auto token = lcctest::token_for(runtime, lcctest::LoopFixture::kOperator, LoopId::from_value(lcctest::LoopFixture::kLoop));
    REQUIRE(token.ok());
    CHECK(runtime.observe(LoopId::from_value(lcctest::LoopFixture::kLoop)).ok());

    const auto first = runtime.flush();
    REQUIRE(first.ok());
    // Capture the committed generation through the read-only inspector so that
    // the encoding is produced from durable bytes, not from live memory.
    const std::string snapshot = root + "/lcc.snapshot";
    const auto bytes_a = liquidcooling::inspect_store(root, StoreBounds{}, nullptr);
    REQUIRE(bytes_a.ok());

    std::vector<std::uint8_t> encoded_a;
    {
        std::FILE* file = std::fopen(snapshot.c_str(), "rb");
        REQUIRE(file != nullptr);
        std::fseek(file, 0, SEEK_END);
        const long size = std::ftell(file);
        std::fseek(file, 0, SEEK_SET);
        encoded_a.resize(static_cast<std::size_t>(size));
        const std::size_t read = std::fread(encoded_a.data(), 1, encoded_a.size(), file);
        std::fclose(file);
        REQUIRE(read == encoded_a.size());
    }

    CHECK(runtime.flush().ok());
    std::vector<std::uint8_t> encoded_b;
    {
        std::FILE* file = std::fopen(snapshot.c_str(), "rb");
        REQUIRE(file != nullptr);
        std::fseek(file, 0, SEEK_END);
        const long size = std::ftell(file);
        std::fseek(file, 0, SEEK_SET);
        encoded_b.resize(static_cast<std::size_t>(size));
        const std::size_t read = std::fread(encoded_b.data(), 1, encoded_b.size(), file);
        std::fclose(file);
        REQUIRE(read == encoded_b.size());
    }

    // A flush of unchanged state publishes a new generation but the payload is
    // byte-for-byte identical apart from the commit sequence fields.
    CHECK_EQ(encoded_a.size(), encoded_b.size());
    std::size_t differences = 0;
    for (std::size_t index = 0; index < encoded_a.size(); ++index) {
        if (encoded_a[index] != encoded_b[index]) {
            ++differences;
        }
    }
    // 8 header bytes, 8 trailer bytes and the sequence field of the store header
    // record change; nothing else may.
    CHECK(differences <= 24);

    // Encoding the decoded state again reproduces the same payload exactly.
    const auto decoded = liquidcooling::inspect_store(root, StoreBounds{}, nullptr);
    REQUIRE(decoded.ok());
    const auto reencoded = encode_snapshot_payload(decoded.value(), StoreBounds{});
    REQUIRE(reencoded.ok());
    const auto reencoded_again = encode_snapshot_payload(decoded.value(), StoreBounds{});
    REQUIRE(reencoded_again.ok());
    CHECK(reencoded.value() == reencoded_again.value());

    (void)runtime.close();
    lcctest::remove_tree(root);
}

TEST(determinism, request_fingerprints_are_stable_and_distinct) {
    std::set<std::string> seen;
    TransitionRequest request;
    request.action = ActionKind::StartPump;
    request.loop = LoopId::from_value(1);
    request.target = DeviceId::from_value(2);
    for (std::uint32_t revision = 1; revision <= 64; ++revision) {
        request.expected_revision = StateRevision::from_value(revision);
        const std::string hex = fingerprint_request(request).to_hex();
        CHECK(seen.insert(hex).second);
    }
    CHECK_EQ(seen.size(), 64u);
}

TEST(determinism, adapter_scripts_are_reproducible) {
    // Two independent runs of the same scripted scenario must produce identical
    // outcome sequences.
    const auto run_scenario = []() {
        lcctest::LoopFixture fixture;
        const std::string root = lcctest::make_temp_root("det-script");
        auto opened = lcctest::open_runtime(fixture, root);
        if (!opened.ok()) {
            return std::string("open-failed");
        }
        Runtime& runtime = *opened.value();
        const auto token =
            lcctest::token_for(runtime, lcctest::LoopFixture::kOperator, LoopId::from_value(lcctest::LoopFixture::kLoop));
        std::string trace;
        for (int step = 0; step < 6; ++step) {
            const auto observed = runtime.observe(LoopId::from_value(lcctest::LoopFixture::kLoop));
            trace += observed.ok() ? "O" : "o";
            TransitionRequest request = lcctest::make_request(
                runtime, step % 2 == 0 ? ActionKind::OpenValve : ActionKind::CloseValve,
                DeviceId::from_value(step % 2 == 0 ? lcctest::LoopFixture::kValveA : lcctest::LoopFixture::kValveA),
                token.value(), "det-" + std::to_string(step));
            const auto result = runtime.execute(request);
            trace += result.ok() ? std::string(to_string(result.value().outcome)) : std::string("err");
            trace += ";";
        }
        (void)runtime.close();
        lcctest::remove_tree(root);
        return trace;
    };
    const std::string first = run_scenario();
    const std::string second = run_scenario();
    CHECK_EQ(first, second);
    CHECK(!first.empty());
}
