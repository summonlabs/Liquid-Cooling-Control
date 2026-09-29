#include <string>
#include <vector>

#include "liquidcooling/liquidcooling.hpp"
#include "support.hpp"
#include "testing.hpp"

using namespace liquidcooling;

namespace {

constexpr auto kLoop = lcctest::LoopFixture::kLoop;
constexpr auto kPumpA = lcctest::LoopFixture::kPumpA;
constexpr auto kValveA = lcctest::LoopFixture::kValveA;

}  // namespace

TEST(bounds, loop_and_device_counts_are_bounded) {
    lcctest::LoopFixture fixture;
    fixture.policy.limits.max_loops = 2;
    RuntimeConfig config = lcctest::fixture_config(fixture, lcctest::make_temp_root("bounds-loops"));
    config.initial_loops.push_back(config.initial_loops.front());
    config.initial_loops.back().id = LoopId::from_value(2);
    config.initial_loops.push_back(config.initial_loops.front());
    config.initial_loops.back().id = LoopId::from_value(3);
    CHECK_CODE(Runtime::open(config), StatusCode::TooManyObjects);

    lcctest::LoopFixture other;
    other.policy.limits.max_devices_per_loop = 2;
    RuntimeConfig device_config = lcctest::fixture_config(other, lcctest::make_temp_root("bounds-devices"));
    CHECK_CODE(Runtime::open(device_config), StatusCode::TooManyObjects);
}

TEST(bounds, obligation_and_token_bounds) {
    lcctest::LoopFixture fixture;
    fixture.policy.limits.max_obligations = 1;
    RuntimeConfig config = lcctest::fixture_config(fixture, lcctest::make_temp_root("bounds-obligations"));
    for (int index = 0; index < 2; ++index) {
        ServiceObligation obligation;
        obligation.id = ObligationId::from_value(static_cast<std::uint64_t>(index + 1));
        obligation.target = DeviceId::from_value(kPumpA);
        obligation.kind = ObligationKind::ServiceDue;
        obligation.active = true;
        obligation.note = "service";
        config.initial_obligations.push_back(obligation);
    }
    CHECK_CODE(Runtime::open(config), StatusCode::TooManyObjects);
}

TEST(bounds, issued_authority_is_bounded_with_deterministic_eviction) {
    lcctest::LoopFixture fixture;
    fixture.policy.limits.max_issued_tokens = 4;
    const std::string root = lcctest::make_temp_root("bounds-tokens");
    auto opened = lcctest::open_runtime(fixture, root);
    REQUIRE(opened.ok());
    Runtime& runtime = *opened.value();

    std::vector<AuthorityToken> tokens;
    for (int index = 0; index < 4; ++index) {
        const auto token = runtime.issue_authority(
            AuthorityRequest{PrincipalId::from_value(lcctest::LoopFixture::kOperator),
                             LoopId::from_value(kLoop), Duration::from_value(1'000'000'000)});
        REQUIRE(token.ok());
        tokens.push_back(token.value());
        fixture.clock->advance(Duration::from_value(1'000'000));
    }
    CHECK_EQ(runtime.status().value().issued_authority, 4u);

    const auto overflow = runtime.issue_authority(
        AuthorityRequest{PrincipalId::from_value(lcctest::LoopFixture::kOperator), LoopId::from_value(kLoop),
                         Duration::from_value(1'000'000'000)});
    REQUIRE(overflow.ok());
    CHECK_EQ(runtime.status().value().issued_authority, 4u);

    // The evicted token is the one closest to expiry, which is the oldest here.
    REQUIRE(runtime.observe(LoopId::from_value(kLoop)).ok());
    fixture.adapter->set_valve_position(DeviceId::from_value(kValveA), ValvePosition::Open);
    fixture.clock->advance(Duration::from_value(1'000'000));
    REQUIRE(runtime.observe(LoopId::from_value(kLoop)).ok());
    TransitionRequest evicted = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                      DeviceId::from_value(kValveA), tokens.front(),
                                                      "token-evicted");
    CHECK_CODE(runtime.plan(evicted), StatusCode::AuthorityRevoked);

    TransitionRequest surviving = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                        DeviceId::from_value(kValveA), overflow.value(),
                                                        "token-surviving");
    CHECK(runtime.plan(surviving).ok());

    (void)runtime.close();
    lcctest::remove_tree(root);
}

TEST(bounds, revoked_token_records_are_bounded) {
    lcctest::LoopFixture fixture;
    fixture.policy.limits.max_revoked_tokens = 2;
    const std::string root = lcctest::make_temp_root("bounds-revoked");
    auto opened = lcctest::open_runtime(fixture, root);
    REQUIRE(opened.ok());
    Runtime& runtime = *opened.value();

    std::vector<AuthorityTokenId> ids;
    for (int index = 0; index < 3; ++index) {
        const auto token = runtime.issue_authority(
            AuthorityRequest{PrincipalId::from_value(lcctest::LoopFixture::kOperator),
                             LoopId::from_value(kLoop), Duration::from_value(1'000'000'000)});
        REQUIRE(token.ok());
        ids.push_back(token.value().id);
    }
    CHECK_CODE(runtime.revoke_authority(ids[0]), StatusCode::Ok);
    CHECK_CODE(runtime.revoke_authority(ids[1]), StatusCode::Ok);
    CHECK_CODE(runtime.revoke_authority(ids[2]), StatusCode::ResourceExhausted);
    // Revoking an already revoked token is idempotent and does not consume room.
    CHECK(runtime.revoke_authority(ids[0]).ok());
    (void)runtime.close();
    lcctest::remove_tree(root);
}

TEST(bounds, idempotency_records_are_bounded_by_refusal_not_eviction) {
    lcctest::LoopFixture fixture;
    fixture.policy.limits.max_idempotency_records = 2;
    fixture.policy.limits.max_open_attempts = 8;
    const std::string root = lcctest::make_temp_root("bounds-idempotency");
    auto opened = lcctest::open_runtime(fixture, root);
    REQUIRE(opened.ok());
    Runtime& runtime = *opened.value();
    fixture.adapter->set_valve_position(DeviceId::from_value(kValveA), ValvePosition::Open);
    REQUIRE(runtime.observe(LoopId::from_value(kLoop)).ok());
    const auto token = lcctest::token_for(runtime, lcctest::LoopFixture::kOperator, LoopId::from_value(kLoop));
    REQUIRE(token.ok());

    int admitted = 0;
    StatusCode last_code = StatusCode::Ok;
    for (int index = 0; index < 6; ++index) {
        // Alternate the direction so that each request is a real transition.
        TransitionRequest request =
            lcctest::make_request(runtime, index % 2 == 0 ? ActionKind::CloseValve : ActionKind::OpenValve,
                                  DeviceId::from_value(kValveA), token.value(),
                                  "idem-" + std::to_string(index));
        const auto result = runtime.execute(request);
        if (result.ok()) {
            ++admitted;
        } else {
            last_code = result.code();
            break;
        }
    }
    CHECK_EQ(admitted, 2);
    CHECK_EQ(last_code, StatusCode::ResourceExhausted);
    (void)runtime.close();
    lcctest::remove_tree(root);
}

TEST(bounds, string_fields_are_bounded) {
    lcctest::LoopFixture fixture;
    fixture.loop.name = std::string(kMaxNameLength + 1, 'a');
    RuntimeConfig config = lcctest::fixture_config(fixture, lcctest::make_temp_root("bounds-name"));
    CHECK_CODE(Runtime::open(config), StatusCode::StringTooLong);

    lcctest::LoopFixture long_name;
    RuntimeConfig ok_config = lcctest::fixture_config(long_name, lcctest::make_temp_root("bounds-name-ok"));
    ok_config.initial_loops.front().name = std::string(kMaxNameLength, 'a');
    auto opened = Runtime::open(ok_config);
    CHECK(opened.ok());
    if (opened.ok()) {
        (void)opened.value()->close();
    }
}

TEST(bounds, store_bounds_reject_oversized_files) {
    lcctest::LoopFixture fixture;
    const std::string root = lcctest::make_temp_root("bounds-store");
    {
        auto opened = lcctest::open_runtime(fixture, root);
        REQUIRE(opened.ok());
        (void)opened.value()->close();
    }
    StoreBounds tiny;
    tiny.max_file_bytes = 16;
    const auto inspected = inspect_store(root, tiny, nullptr);
    CHECK(!inspected.ok());
    CHECK(inspected.code() == StatusCode::StoreBoundsExceeded || inspected.code() == StatusCode::StoreCorrupt);

    RuntimeConfig config = lcctest::fixture_config(fixture, root);
    config.store_bounds = tiny;
    CHECK_CODE(Runtime::open(config), StatusCode::StoreBoundsExceeded);
    lcctest::remove_tree(root);
}

TEST(bounds, policy_bounds_are_validated_at_open) {
    lcctest::LoopFixture fixture;
    fixture.policy.limits.max_open_attempts = 10;
    fixture.policy.limits.max_retained_attempts = 5;
    RuntimeConfig config = lcctest::fixture_config(fixture, lcctest::make_temp_root("bounds-policy"));
    CHECK_CODE(Runtime::open(config), StatusCode::InvalidArgument);

    lcctest::LoopFixture huge;
    huge.policy.limits.max_loops = 100'000;
    RuntimeConfig huge_config = lcctest::fixture_config(huge, lcctest::make_temp_root("bounds-policy-2"));
    CHECK_CODE(Runtime::open(huge_config), StatusCode::InvalidArgument);
}
