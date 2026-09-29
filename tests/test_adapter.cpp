#include <string>

#include "liquidcooling/liquidcooling.hpp"
#include "support.hpp"
#include "testing.hpp"

using namespace liquidcooling;

namespace {

constexpr auto kLoop = lcctest::LoopFixture::kLoop;
constexpr auto kPumpA = lcctest::LoopFixture::kPumpA;
constexpr auto kValveA = lcctest::LoopFixture::kValveA;
constexpr auto kValveB = lcctest::LoopFixture::kValveB;

LoopId loop_id() { return LoopId::from_value(kLoop); }

bool has_contradiction(const std::vector<Contradiction>& contradictions, StatusCode code) {
    for (const auto& contradiction : contradictions) {
        if (contradiction.code == code) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST(adapter, adapter_failure_leaves_the_attempt_unresolved) {
    lcctest::Scenario scenario("ad-unavailable");
    REQUIRE(scenario.opened());
    Runtime& runtime = scenario.rt();
    scenario.fixture.adapter->set_valve_position(DeviceId::from_value(kValveA), ValvePosition::Open);
    REQUIRE(scenario.observe().ok());
    const auto token = scenario.operator_token();

    scenario.fixture.adapter->set_unavailable(true);
    TransitionRequest request = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                      DeviceId::from_value(kValveA), token,
                                                      "unavailable-close");
    const auto result = runtime.execute(request);
    REQUIRE(result.ok());
    CHECK_EQ(result.value().attempt.status, AttemptStatus::Unresolved);
    CHECK_EQ(result.value().attempt.adapter_code, StatusCode::AdapterUnavailable);
    CHECK(result.value().deferred);
    CHECK(result.value().attempt.is_open());

    // The failure does not silently become success.
    CHECK(!is_ok(result.value().outcome));

    // A recovered adapter does not retroactively create an effect: the command
    // never reached the plant, so the attempt stays open until the effect
    // deadline turns the missing effect into a contradiction.
    scenario.fixture.adapter->set_unavailable(false);
    const auto verification = runtime.verify(result.value().attempt.id);
    REQUIRE(verification.ok());
    CHECK_EQ(verification.value().status, AttemptStatus::Unresolved);
    CHECK_EQ(verification.value().code, StatusCode::EffectPending);
    scenario.fixture.clock->advance(Duration::from_value(10'000'000'000));
    const auto later = runtime.verify(result.value().attempt.id);
    REQUIRE(later.ok());
    CHECK_EQ(later.value().status, AttemptStatus::Contradicted);
    CHECK_EQ(later.value().code, StatusCode::EffectContradicted);
}

TEST(adapter, device_level_rejections_resolve_the_attempt_as_failed) {
    lcctest::Scenario scenario("ad-reject");
    REQUIRE(scenario.opened());
    Runtime& runtime = scenario.rt();
    scenario.fixture.adapter->set_valve_position(DeviceId::from_value(kValveA), ValvePosition::Open);
    REQUIRE(scenario.observe().ok());
    const auto token = scenario.operator_token();

    SyntheticStep rejected;
    rejected.ack_mode = SyntheticAckMode::Reject;
    scenario.fixture.adapter->push_step(rejected);
    TransitionRequest request = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                      DeviceId::from_value(kValveA), token, "reject-close");
    const auto result = runtime.execute(request);
    REQUIRE(result.ok());
    CHECK_EQ(result.value().attempt.status, AttemptStatus::Failed);
    CHECK_EQ(result.value().attempt.adapter_code, StatusCode::AdapterRejected);
    CHECK(!result.value().attempt.is_open());

    SyntheticStep busy;
    busy.ack_mode = SyntheticAckMode::Busy;
    scenario.fixture.adapter->push_step(busy);
    TransitionRequest busy_request = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                           DeviceId::from_value(kValveA), token,
                                                           "busy-close");
    const auto busy_result = runtime.execute(busy_request);
    REQUIRE(busy_result.ok());
    CHECK_EQ(busy_result.value().attempt.adapter_code, StatusCode::AdapterBusy);

    SyntheticStep missing;
    missing.ack_mode = SyntheticAckMode::DeviceMissing;
    scenario.fixture.adapter->push_step(missing);
    TransitionRequest missing_request = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                              DeviceId::from_value(kValveA), token,
                                                              "missing-close");
    const auto missing_result = runtime.execute(missing_request);
    REQUIRE(missing_result.ok());
    CHECK_EQ(missing_result.value().attempt.adapter_code, StatusCode::AdapterDeviceMissing);
}

TEST(adapter, acknowledgement_for_a_different_device_generation_is_refused) {
    lcctest::Scenario scenario("ad-ack-generation");
    REQUIRE(scenario.opened());
    Runtime& runtime = scenario.rt();
    scenario.fixture.adapter->set_valve_position(DeviceId::from_value(kValveA), ValvePosition::Open);
    REQUIRE(scenario.observe().ok());
    const auto token = scenario.operator_token();

    SyntheticStep mismatch;
    mismatch.ack_mode = SyntheticAckMode::GenerationMismatch;
    scenario.fixture.adapter->push_step(mismatch);
    TransitionRequest request = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                      DeviceId::from_value(kValveA), token, "ack-gen");
    const auto result = runtime.execute(request);
    REQUIRE(result.ok());
    CHECK(!is_ok(result.value().outcome));
    // The adapter reported a generation mismatch, which does not prove that no
    // physical effect occurred, so the attempt keeps fencing actuation.
    CHECK(result.value().attempt.is_open());
    CHECK_EQ(result.value().attempt.status, AttemptStatus::Unresolved);
    CHECK_EQ(result.value().attempt.adapter_code, StatusCode::AdapterGenerationMismatch);
}

TEST(adapter, stale_sensor_sequence_is_rejected_rather_than_believed) {
    lcctest::Scenario scenario("ad-stale-sequence");
    REQUIRE(scenario.opened());
    Runtime& runtime = scenario.rt();
    scenario.fixture.adapter->set_valve_position(DeviceId::from_value(kValveA), ValvePosition::Open);
    REQUIRE(scenario.observe().ok());
    const auto accepted_sequence = runtime.loop_status(loop_id()).value().evidence.sequence;
    REQUIRE(accepted_sequence.valid());

    scenario.fixture.adapter->force_stale_sequence_once();
    // An observation pass that accepts nothing is reported as a failure rather
    // than silently succeeding with no fresh evidence.
    const auto replayed = runtime.observe(loop_id());
    CHECK_CODE(replayed, StatusCode::EvidenceStale);
    // The previously accepted evidence is retained rather than overwritten by
    // the replayed sample.
    CHECK_EQ(runtime.loop_status(loop_id()).value().evidence.sequence, accepted_sequence);

    // A repeated sequence is likewise refused.
    SyntheticStep stale;
    stale.stale_sequence = true;
    scenario.fixture.adapter->push_step(stale);
    const auto token = scenario.operator_token();
    TransitionRequest request = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                      DeviceId::from_value(kValveA), token, "stale-seq");
    scenario.fixture.clock->advance(Duration::from_value(100'000'000));
    const auto result = runtime.execute(request);
    REQUIRE(result.ok());
    // The observation that follows the command replays the previous sequence, so
    // it cannot prove the effect.
    CHECK(result.value().attempt.is_open());
    CHECK_EQ(result.value().attempt.outcome, StatusCode::EvidenceStale);
}

TEST(adapter, future_timestamps_are_refused) {
    lcctest::Scenario scenario("ad-future");
    REQUIRE(scenario.opened());
    Runtime& runtime = scenario.rt();
    scenario.fixture.adapter->set_valve_position(DeviceId::from_value(kValveA), ValvePosition::Open);
    REQUIRE(scenario.observe().ok());

    SyntheticStep future;
    future.future_timestamp = true;
    scenario.fixture.adapter->push_step(future);
    const auto token = scenario.operator_token();
    TransitionRequest request = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                      DeviceId::from_value(kValveA), token, "future");
    const auto result = runtime.execute(request);
    REQUIRE(result.ok());
    // The command is acknowledged, but the post-command observation is stamped
    // an hour ahead and therefore cannot be used as proof of effect.
    CHECK(result.value().attempt.is_open());
    CHECK_EQ(result.value().attempt.outcome, StatusCode::EvidenceFuture);
}

TEST(adapter, unsupported_leak_channel_is_indeterminate_not_clear) {
    lcctest::Scenario scenario("ad-unsupported-leak");
    REQUIRE(scenario.opened());
    Runtime& runtime = scenario.rt();
    scenario.fixture.adapter->set_valve_position(DeviceId::from_value(kValveA), ValvePosition::Open);

    SyntheticStep unsupported;
    unsupported.unsupported_leak_channel = true;
    scenario.fixture.adapter->push_step(unsupported);
    const auto token = scenario.operator_token();
    TransitionRequest first = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                    DeviceId::from_value(kValveA), token,
                                                    "unsupported-first");
    // Consume the scripted step with a hazard-reducing action.
    REQUIRE(runtime.execute(first).ok());
    const auto observed = runtime.observe(loop_id());
    REQUIRE(observed.ok());
    CHECK(!observed.value().accepted == false);
    CHECK(observed.value().leak_indeterminate);

    TransitionRequest increase = lcctest::make_request(runtime, ActionKind::OpenValve,
                                                       DeviceId::from_value(kValveB), token,
                                                       "unsupported-increase");
    CHECK_CODE(runtime.plan(increase), StatusCode::LeakStateIndeterminate);
}

TEST(adapter, leak_appearance_blocks_and_is_surfaced) {
    lcctest::Scenario scenario("ad-leak-appears");
    REQUIRE(scenario.opened());
    Runtime& runtime = scenario.rt();
    scenario.fixture.adapter->set_valve_position(DeviceId::from_value(kValveA), ValvePosition::Open);

    SyntheticStep leak;
    leak.leak_confirmed = true;
    scenario.fixture.adapter->push_step(leak);
    const auto token = scenario.operator_token();

    // The first command brings the leak into existence on the plant.
    TransitionRequest reduce = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                     DeviceId::from_value(kValveA), token, "leak-appear");
    REQUIRE(runtime.execute(reduce).ok());
    scenario.fixture.clock->advance(Duration::from_value(100'000'000));
    const auto observed = runtime.observe(loop_id());
    REQUIRE(observed.ok());
    CHECK(observed.value().leak_confirmed);
    CHECK(has_contradiction(observed.value().contradictions, StatusCode::LeakConfirmed));

    TransitionRequest increase = lcctest::make_request(runtime, ActionKind::OpenValve,
                                                       DeviceId::from_value(kValveB), token,
                                                       "leak-appear-increase");
    CHECK_CODE(runtime.plan(increase), StatusCode::LeakConfirmed);
}

TEST(adapter, pump_running_without_flow_is_a_contradiction) {
    lcctest::Scenario scenario("ad-starved");
    REQUIRE(scenario.opened());
    Runtime& runtime = scenario.rt();
    scenario.fixture.adapter->set_pump_state(DeviceId::from_value(kPumpA), PumpState::Running);
    scenario.fixture.adapter->set_valve_position(DeviceId::from_value(kValveB), ValvePosition::Open);

    SyntheticStep starved;
    starved.starved_flow = true;
    scenario.fixture.adapter->push_step(starved);
    const auto token = scenario.operator_token();
    TransitionRequest consume = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                      DeviceId::from_value(kValveB), token,
                                                      "starved-consume");
    REQUIRE(runtime.execute(consume).ok());
    const auto observed = runtime.observe(loop_id());
    REQUIRE(observed.ok());
    CHECK(has_contradiction(observed.value().contradictions, StatusCode::EvidenceConflicting));
}

TEST(adapter, valve_reports_open_but_nothing_flows) {
    lcctest::Scenario scenario("ad-open-no-flow");
    REQUIRE(scenario.opened());
    Runtime& runtime = scenario.rt();
    scenario.fixture.adapter->set_pump_state(DeviceId::from_value(kPumpA), PumpState::Running);
    scenario.fixture.adapter->set_valve_position(DeviceId::from_value(kValveA), ValvePosition::Open);

    SyntheticStep starved;
    starved.starved_flow = true;
    scenario.fixture.adapter->push_step(starved);
    const auto token = scenario.operator_token();
    TransitionRequest consume = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                      DeviceId::from_value(kValveB), token,
                                                      "open-no-flow-consume");
    REQUIRE(runtime.execute(consume).ok());
    const auto observed = runtime.observe(loop_id());
    REQUIRE(observed.ok());
    CHECK(has_contradiction(observed.value().contradictions, StatusCode::EvidenceConflicting));
}

TEST(adapter, synthetic_plant_is_deterministic_and_inspectable) {
    lcctest::LoopFixture fixture;
    SyntheticAdapter& adapter = *fixture.adapter;
    CHECK_EQ(adapter.actuation_count(), 0u);
    CHECK_EQ(adapter.read_count(), 0u);
    CHECK(!adapter.any_pump_running());
    CHECK(adapter.all_valves_closed());
    CHECK(adapter.has_device(DeviceId::from_value(kValveA)));
    CHECK(!adapter.has_device(DeviceId::from_value(99)));
    CHECK_EQ(adapter.generation_of(DeviceId::from_value(kValveA)), DeviceGeneration::from_value(1));
    CHECK_EQ(adapter.generation_of(DeviceId::from_value(99)), DeviceGeneration::invalid());

    const auto descriptor = adapter.describe();
    CHECK_EQ(descriptor.protocol_version, 1u);
    CHECK_EQ(descriptor.vendor, std::string("synthetic"));

    adapter.bump_generation(DeviceId::from_value(kValveA));
    CHECK_EQ(adapter.generation_of(DeviceId::from_value(kValveA)), DeviceGeneration::from_value(2));
    adapter.set_device_generation(DeviceId::from_value(kValveA), DeviceGeneration::from_value(5));
    CHECK_EQ(adapter.generation_of(DeviceId::from_value(kValveA)), DeviceGeneration::from_value(5));
    adapter.bump_switchover();
    CHECK_EQ(adapter.describe().switchover_generation.value(), 1u);

    // Commands recorded for inspection are bounded and ordered.
    ObservationRequest request;
    request.loop = fixture.loop.id;
    request.target = DeviceId::from_value(kValveA);
    const auto reading = adapter.read(request);
    REQUIRE(reading.ok());
    CHECK_EQ(reading.value().sequence.value(), 1u);
    CHECK_EQ(reading.value().device_generation.value, DeviceGeneration::from_value(5));
    CHECK(reading.value().valve_position.present);
    CHECK_EQ(reading.value().valve_position.value, ValvePosition::Closed);
}
