#include <string>

#include "liquidcooling/liquidcooling.hpp"
#include "support.hpp"
#include "testing.hpp"

using namespace liquidcooling;

namespace {

constexpr auto kPumpA = lcctest::LoopFixture::kPumpA;
constexpr auto kPumpB = lcctest::LoopFixture::kPumpB;
constexpr auto kValveA = lcctest::LoopFixture::kValveA;
constexpr auto kValveB = lcctest::LoopFixture::kValveB;

/// Puts the simulated loop into circulation so that effects are observable.
void circulate(lcctest::Scenario& scenario) {
    scenario.fixture.adapter->set_pump_state(DeviceId::from_value(kPumpA), PumpState::Running);
    scenario.fixture.adapter->set_valve_position(DeviceId::from_value(kValveA), ValvePosition::Open);
    scenario.fixture.adapter->set_valve_position(DeviceId::from_value(kValveB), ValvePosition::Open);
    REQUIRE(scenario.observe().ok());
}

}  // namespace

TEST(attempts, acknowledgement_is_not_proof_of_effect) {
    lcctest::Scenario scenario("at-ack");
    REQUIRE(scenario.opened());
    circulate(scenario);
    Runtime& runtime = scenario.rt();
    const auto token = scenario.operator_token();

    // The plant acknowledges every command but never moves the actuator.
    SyntheticStep inert;
    inert.applies_effect = false;
    scenario.fixture.adapter->push_step(inert);

    TransitionRequest request = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                      DeviceId::from_value(kValveA), token, "ack-no-effect");
    const auto result = runtime.execute(request);
    REQUIRE(result.ok());
    CHECK_EQ(result.value().attempt.status, AttemptStatus::Unresolved);
    CHECK_EQ(result.value().attempt.outcome, StatusCode::EffectPending);
    CHECK(result.value().attempt.ack_sequence.valid());
    CHECK(result.value().deferred);
    CHECK(!attempt_is_terminal(result.value().attempt.status));

    // Once the effect deadline elapses the mismatch becomes a contradiction.
    scenario.fixture.clock->advance(Duration::from_value(10'000'000'000));
    const auto verification = runtime.verify(result.value().attempt.id);
    REQUIRE(verification.ok());
    CHECK_EQ(verification.value().status, AttemptStatus::Contradicted);
    CHECK_EQ(verification.value().code, StatusCode::EffectContradicted);
}

TEST(attempts, delayed_effect_is_pending_then_verified) {
    lcctest::Scenario scenario("at-delay");
    REQUIRE(scenario.opened());
    circulate(scenario);
    Runtime& runtime = scenario.rt();
    const auto token = scenario.operator_token();

    SyntheticStep delayed;
    delayed.effect_delay_samples = 3;
    scenario.fixture.adapter->push_step(delayed);

    TransitionRequest request = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                      DeviceId::from_value(kValveA), token, "delay-close");
    const auto result = runtime.execute(request);
    REQUIRE(result.ok());
    CHECK_EQ(result.value().attempt.status, AttemptStatus::Unresolved);
    CHECK_EQ(result.value().attempt.outcome, StatusCode::EffectPending);

    // Further observations are still inside the effect deadline.
    (void)runtime.verify(result.value().attempt.id);
    const auto still_pending = runtime.verify(result.value().attempt.id);
    REQUIRE(still_pending.ok());
    CHECK_EQ(still_pending.value().status, AttemptStatus::Unresolved);
    CHECK_EQ(still_pending.value().code, StatusCode::EffectPending);

    // The delay elapses and the effect becomes provable.
    scenario.fixture.clock->advance(Duration::from_value(100'000'000));
    const auto verified = runtime.verify(result.value().attempt.id);
    REQUIRE(verified.ok());
    CHECK_EQ(verified.value().status, AttemptStatus::EffectVerified);
    CHECK_EQ(verified.value().code, StatusCode::Ok);
    CHECK(verified.value().evidence_sequence.valid());
    CHECK_EQ(verified.value().evidence_generation, DeviceGeneration::from_value(1));
}

TEST(attempts, contradictory_evidence_is_reported_not_ignored) {
    lcctest::Scenario scenario("at-contradiction");
    REQUIRE(scenario.opened());
    circulate(scenario);
    Runtime& runtime = scenario.rt();
    const auto token = scenario.operator_token();

    SyntheticStep contrary;
    contrary.contradictory_position = true;
    scenario.fixture.adapter->push_step(contrary);

    TransitionRequest request = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                      DeviceId::from_value(kValveA), token, "contradict");
    const auto result = runtime.execute(request);
    REQUIRE(result.ok());
    scenario.fixture.clock->advance(Duration::from_value(10'000'000'000));
    const auto verification = runtime.verify(result.value().attempt.id);
    REQUIRE(verification.ok());
    CHECK_EQ(verification.value().status, AttemptStatus::Contradicted);
    CHECK_EQ(verification.value().code, StatusCode::EffectContradicted);

    // A contradicted attempt is terminal but the loop must be re-observed
    // before further work; it no longer fences actuation.
    TransitionRequest next = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                   DeviceId::from_value(kValveB), token, "after-contradiction");
    const auto planned = runtime.plan(next);
    CHECK(planned.ok());
}

TEST(attempts, unresolved_attempts_fence_incompatible_actuation_but_not_reduction) {
    lcctest::Scenario scenario("at-fence");
    REQUIRE(scenario.opened());
    circulate(scenario);
    Runtime& runtime = scenario.rt();
    const auto token = scenario.operator_token();
    const auto isolator = scenario.isolator_token();

    SyntheticStep inert;
    inert.applies_effect = false;
    scenario.fixture.adapter->push_step(inert);

    TransitionRequest blocked = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                      DeviceId::from_value(kValveA), token, "fence-open");
    const auto result = runtime.execute(blocked);
    REQUIRE(result.ok());
    REQUIRE(result.value().attempt.is_open());
    REQUIRE(scenario.observe().ok());

    TransitionRequest incompatible = lcctest::make_request(runtime, ActionKind::OpenValve,
                                                           DeviceId::from_value(kValveB), token,
                                                           "fence-incompatible");
    CHECK_CODE(runtime.plan(incompatible), StatusCode::UnresolvedAttemptBlocks);

    TransitionRequest compatible = lcctest::make_request(runtime, ActionKind::SetFlowTarget,
                                                         DeviceId::invalid(), token, "fence-flow");
    compatible.has_flow_target = true;
    compatible.flow_target = FlowRate::from_value(60'000);
    CHECK_CODE(runtime.plan(compatible), StatusCode::UnresolvedAttemptBlocks);

    // Hazard-reducing actuation stays available even while fenced.
    TransitionRequest reduce = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                     DeviceId::from_value(kValveB), isolator, "fence-reduce");
    const auto reduce_result = runtime.plan(reduce);
    CHECK(reduce_result.ok());

    // A retry of the identical intent is compatible rather than blocking.
    TransitionRequest retry = blocked;
    retry.expected_revision = runtime.status().value().revision;
    const auto retry_result = runtime.plan(retry);
    CHECK(retry_result.ok());
}

TEST(attempts, idempotent_replay_does_not_actuate_again) {
    lcctest::Scenario scenario("at-idempotent");
    REQUIRE(scenario.opened());
    circulate(scenario);
    Runtime& runtime = scenario.rt();
    const auto token = scenario.operator_token();

    TransitionRequest request = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                      DeviceId::from_value(kValveA), token, "replay-key");
    const auto first = runtime.execute(request);
    REQUIRE(first.ok());
    const std::size_t actuations_after_first = scenario.fixture.adapter->actuation_count();

    // The retry is byte-identical to the original request, which is what a
    // caller that lost the response would resend.  The state has moved on, so
    // this can only work if replay is resolved before state validation.
    const TransitionRequest retry = request;
    const auto second = runtime.execute(retry);
    CHECK_EQ(runtime.status().value().idempotency_records, 1u);
    CHECK_CODE(second, StatusCode::Ok);
    if (!second.ok()) {
        return;
    }
    CHECK(second.value().idempotent_replay);
    CHECK_EQ(second.value().attempt.id, first.value().attempt.id);
    CHECK_EQ(scenario.fixture.adapter->actuation_count(), actuations_after_first);
    CHECK_EQ(second.value().outcome, first.value().outcome);

    // Re-applying the original plan is equally replay safe even though the
    // state it was planned against no longer exists.
    const auto third = runtime.apply(first.value().plan);
    CHECK_CODE(third, StatusCode::Ok);
    if (!third.ok()) {
        return;
    }
    CHECK(third.value().idempotent_replay);
    CHECK_EQ(scenario.fixture.adapter->actuation_count(), actuations_after_first);
    // Planning the stale request on its own is still refused: replay is a
    // property of execution, not of validation.
    CHECK_CODE(runtime.plan(retry), StatusCode::StaleRevision);
}

TEST(attempts, idempotency_key_reuse_with_a_different_intent_conflicts) {
    lcctest::Scenario scenario("at-conflict");
    REQUIRE(scenario.opened());
    circulate(scenario);
    Runtime& runtime = scenario.rt();
    const auto token = scenario.operator_token();

    TransitionRequest first = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                    DeviceId::from_value(kValveA), token, "conflict-key");
    REQUIRE(runtime.execute(first).ok());

    // Different action, same key.
    TransitionRequest different_action = lcctest::make_request(runtime, ActionKind::OpenValve,
                                                               DeviceId::from_value(kValveB), token,
                                                               "conflict-key");
    CHECK_CODE(runtime.execute(different_action), StatusCode::IdempotencyConflict);

    // Same action, different target.
    TransitionRequest different_target = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                               DeviceId::from_value(kValveB), token,
                                                               "conflict-key");
    CHECK_CODE(runtime.execute(different_target), StatusCode::IdempotencyConflict);

    // Same action and target but a different bound revision.
    TransitionRequest different_revision = first;
    different_revision.expected_revision = first.expected_revision.next();
    CHECK_CODE(runtime.execute(different_revision), StatusCode::IdempotencyConflict);
}

TEST(attempts, abandon_requires_service_authority_and_unblocks_actuation) {
    lcctest::Scenario scenario("at-abandon");
    REQUIRE(scenario.opened());
    circulate(scenario);
    Runtime& runtime = scenario.rt();
    const auto token = scenario.operator_token();
    const auto isolator = scenario.isolator_token();
    const auto engineer = scenario.engineer_token();

    SyntheticStep inert;
    inert.applies_effect = false;
    scenario.fixture.adapter->push_step(inert);
    TransitionRequest stuck = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                    DeviceId::from_value(kValveA), token, "abandon-close");
    const auto result = runtime.execute(stuck);
    REQUIRE(result.ok());
    REQUIRE(result.value().attempt.is_open());

    CHECK_CODE(runtime.abandon(result.value().attempt.id, isolator, "no service rights"),
               StatusCode::AuthorityInsufficient);
    CHECK_CODE(runtime.abandon(result.value().attempt.id, engineer, ""), StatusCode::MissingRequiredField);
    CHECK_CODE(runtime.abandon(AttemptId::from_value(9999), engineer, "unknown"),
               StatusCode::AttemptNotFound);

    CHECK(runtime.abandon(result.value().attempt.id, engineer, "actuator mechanically stuck").ok());
    CHECK_CODE(runtime.abandon(result.value().attempt.id, engineer, "again"), StatusCode::AttemptNotOpen);

    REQUIRE(scenario.observe().ok());
    TransitionRequest after = lcctest::make_request(runtime, ActionKind::SetFlowTarget,
                                                   DeviceId::invalid(), token, "abandon-after");
    after.has_flow_target = true;
    after.flow_target = FlowRate::from_value(150'000);
    CHECK(runtime.plan(after).ok());

    // Abandoning through the action set is equivalent and equally fenced.
    SyntheticStep inert_again;
    inert_again.applies_effect = false;
    scenario.fixture.adapter->push_step(inert_again);
    TransitionRequest stuck_again = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                          DeviceId::from_value(kValveB), token,
                                                          "abandon-close-2");
    const auto second = runtime.execute(stuck_again);
    REQUIRE(second.ok());
    TransitionRequest abandon_action = lcctest::make_request(runtime, ActionKind::AbandonAttempt,
                                                             DeviceId::invalid(), engineer, "abandon-action");
    abandon_action.attempt = second.value().attempt.id;
    abandon_action.reason = "operator confirmed the valve is seized";
    CHECK(runtime.execute(abandon_action).ok());
    CHECK_EQ(runtime.status().value().open_attempts, 0u);
}

TEST(attempts, plan_identity_is_consumed_once) {
    lcctest::Scenario scenario("at-plan-id");
    REQUIRE(scenario.opened());
    circulate(scenario);
    Runtime& runtime = scenario.rt();
    const auto token = scenario.operator_token();

    TransitionRequest request = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                      DeviceId::from_value(kValveA), token, "plan-id");
    const auto first_plan = runtime.plan(request);
    REQUIRE(first_plan.ok());
    const auto execution = runtime.apply(first_plan.value());
    REQUIRE(execution.ok());
    CHECK_EQ(execution.value().plan.id, first_plan.value().id);

    TransitionRequest second_request = lcctest::make_request(runtime, ActionKind::OpenValve,
                                                             DeviceId::from_value(kValveA), token,
                                                             "plan-id-2");
    second_request.expected_revision = runtime.status().value().revision;
    const auto second_plan = runtime.plan(second_request);
    CHECK_CODE(second_plan, StatusCode::Ok);
    if (!second_plan.ok()) {
        return;
    }
    CHECK(second_plan.value().id != first_plan.value().id);
}

TEST(attempts, plan_binding_is_revalidated_at_apply_time) {
    lcctest::Scenario scenario("at-plan-binding");
    REQUIRE(scenario.opened());
    circulate(scenario);
    Runtime& runtime = scenario.rt();
    const auto token = scenario.operator_token();

    TransitionRequest request = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                      DeviceId::from_value(kValveA), token, "binding");
    const auto plan = runtime.plan(request);
    REQUIRE(plan.ok());

    // Any intervening durable mutation makes the plan stale.
    REQUIRE(scenario.observe().ok());
    CHECK_CODE(runtime.apply(plan.value()), StatusCode::StaleRevision);

    // A tampered fingerprint is refused before any adapter call.
    Plan tampered = plan.value();
    tampered.fingerprint.high ^= 0x1u;
    CHECK_CODE(runtime.apply(tampered), StatusCode::MalformedRequest);
}

TEST(attempts, attempt_limit_is_enforced) {
    lcctest::LoopFixture fixture;
    fixture.policy.limits.max_open_attempts = 3;
    fixture.policy.limits.max_retained_attempts = 8;
    const std::string root = lcctest::make_temp_root("at-limit");
    auto opened = lcctest::open_runtime(fixture, root);
    REQUIRE(opened.ok());
    Runtime& runtime = *opened.value();

    fixture.adapter->set_pump_state(DeviceId::from_value(kPumpA), PumpState::Running);
    fixture.adapter->set_valve_position(DeviceId::from_value(kValveA), ValvePosition::Open);
    fixture.adapter->set_valve_position(DeviceId::from_value(kValveB), ValvePosition::Open);
    REQUIRE(runtime.observe(LoopId::from_value(lcctest::LoopFixture::kLoop)).ok());
    const auto token = lcctest::token_for(runtime, lcctest::LoopFixture::kOperator,
                                          LoopId::from_value(lcctest::LoopFixture::kLoop));
    REQUIRE(token.ok());

    const auto inert_command = [&](ActionKind action, std::uint64_t target, const char* key) {
        SyntheticStep inert;
        inert.applies_effect = false;
        fixture.adapter->push_step(inert);
        TransitionRequest request = lcctest::make_request(runtime, action, DeviceId::from_value(target),
                                                          token.value(), key);
        return runtime.execute(request);
    };

    CHECK(inert_command(ActionKind::CloseValve, kValveA, "limit-1").ok());
    CHECK(inert_command(ActionKind::CloseValve, kValveB, "limit-2").ok());
    CHECK(inert_command(ActionKind::StopPump, kPumpA, "limit-3").ok());
    CHECK_EQ(runtime.status().value().open_attempts, 3u);

    // A fourth command on a device with no open attempt passes the fencing stage
    // and is stopped by the open-attempt bound.
    SyntheticStep inert;
    inert.applies_effect = false;
    fixture.adapter->push_step(inert);
    TransitionRequest overflow = lcctest::make_request(runtime, ActionKind::StopPump,
                                                       DeviceId::from_value(kPumpB), token.value(),
                                                       "limit-overflow");
    CHECK_CODE(runtime.plan(overflow), StatusCode::AttemptLimitExceeded);

    // Retained-attempt trimming never discards an open attempt.
    CHECK_EQ(runtime.status().value().open_attempts, 3u);
    (void)runtime.close();
    lcctest::remove_tree(root);
}
