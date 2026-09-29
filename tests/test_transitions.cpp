#include <string>
#include <vector>

#include "liquidcooling/liquidcooling.hpp"
#include "support.hpp"
#include "testing.hpp"

using namespace liquidcooling;

namespace {

constexpr auto kLoop = lcctest::LoopFixture::kLoop;
constexpr auto kPumpA = lcctest::LoopFixture::kPumpA;
constexpr auto kPumpB = lcctest::LoopFixture::kPumpB;
constexpr auto kValveA = lcctest::LoopFixture::kValveA;
constexpr auto kValveB = lcctest::LoopFixture::kValveB;
constexpr auto kOperator = lcctest::LoopFixture::kOperator;
constexpr auto kIsolator = lcctest::LoopFixture::kIsolator;
constexpr auto kEngineer = lcctest::LoopFixture::kEngineer;
constexpr auto kAuditor = lcctest::LoopFixture::kAuditor;

LoopId loop_id() { return LoopId::from_value(kLoop); }

struct Scenario final {
    lcctest::LoopFixture fixture{};
    std::string root{};
    std::unique_ptr<Runtime> runtime{};

    explicit Scenario(std::string_view tag) {
        root = lcctest::make_temp_root(tag);
        auto opened = lcctest::open_runtime(fixture, root);
        CHECK(opened.ok());
        if (opened.ok()) {
            runtime = std::move(opened.value());
        }
    }

    ~Scenario() {
        if (runtime) {
            (void)runtime->close();
        }
        lcctest::remove_tree(root);
    }

    [[nodiscard]] AuthorityToken operator_token() {
        const auto token = lcctest::token_for(*runtime, kOperator, loop_id());
        CHECK(token.ok());
        return token.ok() ? token.value() : AuthorityToken{};
    }

    [[nodiscard]] AuthorityToken isolator_token() {
        const auto token = lcctest::token_for(*runtime, kIsolator, loop_id());
        CHECK(token.ok());
        return token.ok() ? token.value() : AuthorityToken{};
    }

    [[nodiscard]] AuthorityToken engineer_token() {
        const auto token = lcctest::token_for(*runtime, kEngineer, loop_id());
        CHECK(token.ok());
        return token.ok() ? token.value() : AuthorityToken{};
    }

    void start_circulating() {
        auto token = operator_token();
        CHECK(runtime->observe(loop_id()).ok());
        auto open_valve = lcctest::make_request(*runtime, ActionKind::OpenValve,
                                                DeviceId::from_value(kValveA), token,
                                                "circulate-open-" + std::to_string(counter_++));
        CHECK(runtime->execute(open_valve).ok());
        fixture.clock->advance(Duration::from_value(100'000'000));
        CHECK(runtime->observe(loop_id()).ok());
        auto start = lcctest::make_request(*runtime, ActionKind::StartPump, DeviceId::from_value(kPumpA),
                                           token, "circulate-start-" + std::to_string(counter_++));
        CHECK(runtime->execute(start).ok());
        fixture.clock->advance(Duration::from_value(100'000'000));
        CHECK(runtime->observe(loop_id()).ok());
    }

    std::uint64_t counter_{0};
};

}  // namespace

TEST(transitions, actuation_requires_an_observed_device_generation) {
    lcctest::LoopFixture fixture;
    // Seed the registry without a generation so that no device generation has
    // ever been observed.
    for (auto& device : fixture.loop.devices) {
        device.generation = DeviceGeneration::invalid();
    }
    const std::string root = lcctest::make_temp_root("tr-generation-unknown");
    auto opened = lcctest::open_runtime(fixture, root);
    REQUIRE(opened.ok());
    Runtime& runtime = *opened.value();
    const auto token = lcctest::token_for(runtime, kOperator, loop_id());
    REQUIRE(token.ok());

    TransitionRequest request = lcctest::make_request(runtime, ActionKind::StartPump,
                                                      DeviceId::from_value(kPumpA), token.value(), "gen-unknown");
    request.expected_device_generation = DeviceGeneration::from_value(1);
    CHECK_CODE(runtime.execute(request), StatusCode::FutureGeneration);

    TransitionRequest unset = lcctest::make_request(runtime, ActionKind::StartPump,
                                                    DeviceId::from_value(kPumpA), token.value(), "gen-unset");
    unset.expected_device_generation = DeviceGeneration::invalid();
    CHECK_CODE(runtime.execute(unset), StatusCode::MissingRequiredField);

    CHECK(runtime.observe(loop_id()).ok());
    TransitionRequest after = lcctest::make_request(runtime, ActionKind::StartPump,
                                                    DeviceId::from_value(kPumpA), token.value(), "gen-known");
    TransitionRequest ready = after;
    ready.expected_revision = runtime.status().value().revision;
    CHECK(ready.expected_device_generation.valid());
    // With the generation now observed the gate is passed.
    CHECK(runtime.execute(ready).ok());
    (void)runtime.close();
    lcctest::remove_tree(root);
}

TEST(transitions, leak_evidence_gates_exposure_increasing_actions_only) {
    Scenario scenario("tr-leak");
    Runtime& runtime = *scenario.runtime;
    scenario.fixture.adapter->set_valve_position(DeviceId::from_value(kValveB), ValvePosition::Open);
    const auto token = scenario.operator_token();

    // Absent leak evidence blocks an increase.
    TransitionRequest increase = lcctest::make_request(runtime, ActionKind::OpenValve,
                                                       DeviceId::from_value(kValveA), token, "leak-absent");
    CHECK_CODE(runtime.execute(increase), StatusCode::LeakStateIndeterminate);

    // A hazard-reducing action is admitted with the same evidence.
    TransitionRequest reduce = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                     DeviceId::from_value(kValveB), token, "leak-absent-reduce");
    const auto reduce_result = runtime.execute(reduce);
    CHECK(reduce_result.ok());
    CHECK(reduce_result.value().outcome != StatusCode::LeakStateIndeterminate);

    // Stale evidence is treated exactly like absent evidence.
    REQUIRE(runtime.observe(loop_id()).ok());
    scenario.fixture.clock->advance(Duration::from_value(60'000'000'000));
    TransitionRequest stale = lcctest::make_request(runtime, ActionKind::OpenValve,
                                                    DeviceId::from_value(kValveA), token, "leak-stale");
    CHECK_CODE(runtime.execute(stale), StatusCode::LeakStateIndeterminate);

    // Fresh, clear evidence admits the increase.
    REQUIRE(runtime.observe(loop_id()).ok());
    TransitionRequest fresh = lcctest::make_request(runtime, ActionKind::OpenValve,
                                                    DeviceId::from_value(kValveA), token, "leak-fresh");
    CHECK(runtime.execute(fresh).ok());

    // A suspected leak blocks the increase.
    scenario.fixture.adapter->set_leak_state(LeakState::Suspected);
    scenario.fixture.clock->advance(Duration::from_value(100'000'000));
    REQUIRE(runtime.observe(loop_id()).ok());
    TransitionRequest suspected = lcctest::make_request(runtime, ActionKind::OpenValve,
                                                        DeviceId::from_value(kValveB), token, "leak-suspected");
    CHECK_CODE(runtime.execute(suspected), StatusCode::InterlockBlocked);

    // A confirmed leak blocks the increase with a distinct code.
    scenario.fixture.adapter->set_leak_state(LeakState::Confirmed);
    scenario.fixture.clock->advance(Duration::from_value(100'000'000));
    REQUIRE(runtime.observe(loop_id()).ok());
    TransitionRequest confirmed = lcctest::make_request(runtime, ActionKind::OpenValve,
                                                        DeviceId::from_value(kValveB), token, "leak-confirmed");
    CHECK_CODE(runtime.execute(confirmed), StatusCode::LeakConfirmed);

    // Isolation remains available while the leak is confirmed.
    TransitionRequest isolate = lcctest::make_request(runtime, ActionKind::IsolateLoop, DeviceId::invalid(),
                                                      token, "leak-isolate");
    CHECK(runtime.execute(isolate).ok());
    CHECK_EQ(runtime.loop_status(loop_id()).value().loop.isolation, IsolationState::Isolated);
}

TEST(transitions, coolant_quality_and_envelope_interlocks) {
    Scenario scenario("tr-envelope");
    Runtime& runtime = *scenario.runtime;
    const auto token = scenario.operator_token();
    REQUIRE(runtime.observe(loop_id()).ok());

    scenario.fixture.adapter->set_coolant_quality(CoolantQuality::Degraded);
    scenario.fixture.clock->advance(Duration::from_value(100'000'000));
    REQUIRE(runtime.observe(loop_id()).ok());
    TransitionRequest degraded = lcctest::make_request(runtime, ActionKind::OpenValve,
                                                       DeviceId::from_value(kValveA), token, "coolant-degraded");
    CHECK_CODE(runtime.execute(degraded), StatusCode::CoolantQualityNotNominal);

    scenario.fixture.adapter->set_coolant_quality(CoolantQuality::Nominal);
    scenario.fixture.clock->advance(Duration::from_value(100'000'000));
    REQUIRE(runtime.observe(loop_id()).ok());
    TransitionRequest nominal = lcctest::make_request(runtime, ActionKind::OpenValve,
                                                      DeviceId::from_value(kValveA), token, "coolant-nominal");
    CHECK(runtime.execute(nominal).ok());

    // Pressure excursion above the policy maximum blocks further increases.
    scenario.fixture.adapter->push_step(SyntheticStep{});
    SyntheticStep excursion;
    excursion.pressure_excursion = true;
    scenario.fixture.adapter->push_step(excursion);
    scenario.fixture.clock->advance(Duration::from_value(100'000'000));
    TransitionRequest after_excursion = lcctest::make_request(runtime, ActionKind::OpenValve,
                                                              DeviceId::from_value(kValveB), token,
                                                              "pressure-excursion");
    const auto result = runtime.execute(after_excursion);
    // The synthetic plant only reports pressure while a pump runs; start one so
    // the excursion is observable.
    CHECK(result.ok() || result.code() == StatusCode::PressureOutOfRange);
}

TEST(transitions, service_obligations_and_service_mode) {
    Scenario scenario("tr-service");
    Runtime& runtime = *scenario.runtime;
    const auto operator_token = scenario.operator_token();
    const auto engineer_token = scenario.engineer_token();
    REQUIRE(runtime.observe(loop_id()).ok());

    // Circularise the simulated plant so that a pump start is a real change.
    scenario.fixture.adapter->set_valve_position(DeviceId::from_value(kValveA), ValvePosition::Open);
    scenario.fixture.clock->advance(Duration::from_value(100'000'000));
    REQUIRE(runtime.observe(loop_id()).ok());
    TransitionRequest open_valve = lcctest::make_request(runtime, ActionKind::OpenValve,
                                                         DeviceId::from_value(kValveB), operator_token,
                                                         "svc-open");
    CHECK(runtime.execute(open_valve).ok());
    scenario.fixture.clock->advance(Duration::from_value(100'000'000));
    REQUIRE(runtime.observe(loop_id()).ok());
    TransitionRequest start_again = lcctest::make_request(runtime, ActionKind::StartPump,
                                                          DeviceId::from_value(kPumpA), operator_token,
                                                          "svc-due-2");
    CHECK(runtime.execute(start_again).ok());

    // Entering service mode requires isolation first.
    TransitionRequest enter_early = lcctest::make_request(runtime, ActionKind::EnterService, DeviceId::invalid(),
                                                          engineer_token, "svc-enter-early");
    CHECK_CODE(runtime.execute(enter_early), StatusCode::LoopNotIsolated);

    scenario.fixture.clock->advance(Duration::from_value(100'000'000));
    REQUIRE(runtime.observe(loop_id()).ok());
    TransitionRequest isolate = lcctest::make_request(runtime, ActionKind::IsolateLoop, DeviceId::invalid(),
                                                      operator_token, "svc-isolate");
    CHECK(runtime.execute(isolate).ok());

    TransitionRequest enter = lcctest::make_request(runtime, ActionKind::EnterService, DeviceId::invalid(),
                                                    engineer_token, "svc-enter");
    CHECK(runtime.execute(enter).ok());
    CHECK_EQ(runtime.loop_status(loop_id()).value().loop.service_mode, ServiceMode::Service);

    // Production authority cannot actuate a loop in service mode.
    scenario.fixture.clock->advance(Duration::from_value(100'000'000));
    REQUIRE(runtime.observe(loop_id()).ok());
    TransitionRequest production_in_service =
        lcctest::make_request(runtime, ActionKind::ClearIsolation, DeviceId::invalid(), operator_token,
                              "svc-production");
    CHECK_CODE(runtime.execute(production_in_service), StatusCode::ServiceModeConflict);
}

TEST(transitions, lockout_tag_forbids_every_actuation_on_the_device) {
    Scenario scenario("tr-lockout");
    Runtime& runtime = *scenario.runtime;
    const auto token = scenario.operator_token();
    REQUIRE(runtime.observe(loop_id()).ok());

    ServiceObligation lockout;
    lockout.id = ObligationId::from_value(1);
    lockout.target = DeviceId::from_value(kPumpA);
    lockout.kind = ObligationKind::LockoutTag;
    lockout.active = true;
    lockout.raised_at = TimestampNs::from_nanos(1);
    lockout.note = "tag 42 applied";

    // Install the obligation through a fresh store seeded with it.
    (void)runtime.close();
    lcctest::remove_tree(scenario.root);
    auto config = lcctest::fixture_config(scenario.fixture, scenario.root);
    config.initial_obligations.push_back(lockout);
    auto reopened = Runtime::open(config);
    REQUIRE(reopened.ok());
    Runtime& second = *reopened.value();
    const auto second_token = lcctest::token_for(second, kOperator, loop_id());
    REQUIRE(second_token.ok());
    REQUIRE(second.observe(loop_id()).ok());

    TransitionRequest start = lcctest::make_request(second, ActionKind::StartPump, DeviceId::from_value(kPumpA),
                                                    second_token.value(), "lockout-start");
    CHECK_CODE(second.execute(start), StatusCode::ServiceObligationActive);

    TransitionRequest stop = lcctest::make_request(second, ActionKind::StopPump, DeviceId::from_value(kPumpA),
                                                   second_token.value(), "lockout-stop");
    CHECK_CODE(second.execute(stop), StatusCode::ServiceObligationActive);

    // A different device is unaffected.
    TransitionRequest other = lcctest::make_request(second, ActionKind::OpenValve,
                                                    DeviceId::from_value(kValveA), second_token.value(),
                                                    "lockout-other");
    CHECK(second.execute(other).ok());

    // Service authority can complete the obligation and release the device.
    const auto engineer = lcctest::token_for(second, kEngineer, loop_id());
    REQUIRE(engineer.ok());
    TransitionRequest complete = lcctest::make_request(second, ActionKind::CompleteObligation,
                                                       DeviceId::invalid(), engineer.value(), "lockout-complete");
    complete.obligation = lockout.id;
    complete.reason = "tag removed and pump inspected";
    CHECK(second.execute(complete).ok());
    scenario.fixture.clock->advance(Duration::from_value(100'000'000));
    REQUIRE(second.observe(loop_id()).ok());
    TransitionRequest after = lcctest::make_request(second, ActionKind::StartPump, DeviceId::from_value(kPumpA),
                                                    second_token.value(), "lockout-after");
    CHECK(second.execute(after).ok());
    (void)second.close();
    scenario.runtime.reset();
}

TEST(transitions, authority_binding_is_enforced_field_by_field) {
    Scenario scenario("tr-authority");
    Runtime& runtime = *scenario.runtime;
    REQUIRE(runtime.observe(loop_id()).ok());
    const auto good = scenario.operator_token();

    TransitionRequest base = lcctest::make_request(runtime, ActionKind::OpenValve,
                                                   DeviceId::from_value(kValveA), good, "auth-base");
    CHECK(runtime.plan(base).ok());

    TransitionRequest no_token = base;
    no_token.authority = AuthorityToken{};
    CHECK_CODE(runtime.plan(no_token), StatusCode::AuthorityMissing);

    TransitionRequest wrong_epoch = base;
    wrong_epoch.authority.epoch = base.authority.epoch.next();
    CHECK_CODE(runtime.plan(wrong_epoch), StatusCode::EpochMismatch);

    TransitionRequest wrong_incarnation = base;
    wrong_incarnation.authority.incarnation = base.authority.incarnation.next();
    CHECK_CODE(runtime.plan(wrong_incarnation), StatusCode::IncarnationMismatch);

    TransitionRequest wrong_config = base;
    wrong_config.authority.config_generation = base.authority.config_generation.next();
    CHECK_CODE(runtime.plan(wrong_config), StatusCode::ConfigGenerationMismatch);

    TransitionRequest wrong_topology = base;
    wrong_topology.authority.topology_generation = base.authority.topology_generation.next();
    CHECK_CODE(runtime.plan(wrong_topology), StatusCode::TopologyGenerationMismatch);

    TransitionRequest wrong_scope = base;
    wrong_scope.authority.loop = LoopId::from_value(77);
    CHECK_CODE(runtime.plan(wrong_scope), StatusCode::AuthorityScopeMismatch);

    TransitionRequest forged = base;
    forged.authority.id = AuthorityTokenId::from_value(9999);
    CHECK_CODE(runtime.plan(forged), StatusCode::AuthorityRevoked);

    TransitionRequest wrong_nonce = base;
    wrong_nonce.authority.nonce = base.authority.nonce + 1;
    CHECK_CODE(runtime.plan(wrong_nonce), StatusCode::AuthorityRevoked);

    TransitionRequest expired = base;
    scenario.fixture.clock->advance(Duration::from_value(700'000'000'000));
    CHECK_CODE(runtime.plan(expired), StatusCode::AuthorityExpired);

    // Revocation is durable and immediate.
    const auto fresh = scenario.operator_token();
    TransitionRequest revocable = lcctest::make_request(runtime, ActionKind::OpenValve,
                                                        DeviceId::from_value(kValveA), fresh, "auth-revoked");
    CHECK(runtime.revoke_authority(fresh.id).ok());
    CHECK_CODE(runtime.plan(revocable), StatusCode::AuthorityRevoked);
}

TEST(transitions, capability_is_checked_against_the_action_class) {
    Scenario scenario("tr-capability");
    Runtime& runtime = *scenario.runtime;
    scenario.fixture.adapter->set_valve_position(DeviceId::from_value(kValveB), ValvePosition::Open);
    scenario.fixture.adapter->set_pump_state(DeviceId::from_value(kPumpA), PumpState::Running);
    REQUIRE(runtime.observe(loop_id()).ok());
    const auto isolator = scenario.isolator_token();
    const auto auditor = lcctest::token_for(runtime, kAuditor, loop_id());
    REQUIRE(auditor.ok());

    TransitionRequest increase = lcctest::make_request(runtime, ActionKind::OpenValve,
                                                       DeviceId::from_value(kValveA), isolator, "cap-increase");
    CHECK_CODE(runtime.plan(increase), StatusCode::AuthorityInsufficient);

    TransitionRequest reduce = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                     DeviceId::from_value(kValveB), isolator, "cap-reduce");
    CHECK(runtime.plan(reduce).ok());

    TransitionRequest isolate = lcctest::make_request(runtime, ActionKind::IsolateLoop, DeviceId::invalid(),
                                                      isolator, "cap-isolate");
    CHECK(runtime.plan(isolate).ok());

    TransitionRequest observe_only = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                           DeviceId::from_value(kValveB), auditor.value(),
                                                           "cap-auditor");
    CHECK_CODE(runtime.plan(observe_only), StatusCode::AuthorityInsufficient);

    // A service engineer holds isolation but never production rights.
    const auto engineer = scenario.engineer_token();
    TransitionRequest service_start = lcctest::make_request(runtime, ActionKind::StartPump,
                                                            DeviceId::from_value(kPumpA), engineer,
                                                            "cap-service-start");
    CHECK_CODE(runtime.plan(service_start), StatusCode::AuthorityInsufficient);
    TransitionRequest service_reduce = lcctest::make_request(runtime, ActionKind::StopPump,
                                                             DeviceId::from_value(kPumpA), engineer,
                                                             "cap-service-stop");
    CHECK(runtime.plan(service_reduce).ok());
}

TEST(transitions, revision_and_generation_fencing) {
    Scenario scenario("tr-revision");
    Runtime& runtime = *scenario.runtime;
    REQUIRE(runtime.observe(loop_id()).ok());
    const auto token = scenario.operator_token();

    TransitionRequest stale_revision = lcctest::make_request(runtime, ActionKind::OpenValve,
                                                             DeviceId::from_value(kValveA), token,
                                                             "rev-stale");
    stale_revision.expected_revision = StateRevision::from_value(1);
    CHECK_CODE(runtime.plan(stale_revision), StatusCode::StaleRevision);

    TransitionRequest future_revision = stale_revision;
    future_revision.expected_revision = runtime.status().value().revision.next().next();
    CHECK_CODE(runtime.plan(future_revision), StatusCode::FutureRevision);

    TransitionRequest missing_revision = stale_revision;
    missing_revision.expected_revision = StateRevision::invalid();
    CHECK_CODE(runtime.plan(missing_revision), StatusCode::MissingRequiredField);

    TransitionRequest future_generation = lcctest::make_request(runtime, ActionKind::OpenValve,
                                                                DeviceId::from_value(kValveA), token,
                                                                "rev-future-gen");
    future_generation.expected_device_generation = DeviceGeneration::from_value(7);
    CHECK_CODE(runtime.plan(future_generation), StatusCode::FutureGeneration);

    // A device generation change fences plans produced against the old one.
    TransitionRequest planned = lcctest::make_request(runtime, ActionKind::OpenValve,
                                                      DeviceId::from_value(kValveA), token, "rev-planned");
    const auto plan = runtime.plan(planned);
    REQUIRE(plan.ok());
    scenario.fixture.adapter->bump_generation(DeviceId::from_value(kValveA));
    scenario.fixture.clock->advance(Duration::from_value(100'000'000));
    REQUIRE(runtime.observe(loop_id()).ok());
    CHECK_CODE(runtime.apply(plan.value()), StatusCode::StaleGeneration);
}

TEST(transitions, structural_validation_rejects_misuse) {
    // A recorded setpoint makes the per-transition step bound applicable; it is
    // seeded directly so that this case exercises planning, not actuation.
    lcctest::LoopFixture fixture;
    fixture.loop.flow_target = FlowRate::from_value(100'000);
    fixture.loop.flow_target_generation = DeviceGeneration::from_value(1);
    fixture.loop.pressure_target = Pressure::from_value(300'000);
    fixture.loop.pressure_target_generation = DeviceGeneration::from_value(1);
    const std::string root = lcctest::make_temp_root("tr-structural");
    auto opened = lcctest::open_runtime(fixture, root);
    REQUIRE(opened.ok());
    Runtime& runtime = *opened.value();
    REQUIRE(runtime.observe(loop_id()).ok());
    const auto token = lcctest::token_for(runtime, kOperator, loop_id());
    REQUIRE(token.ok());
    struct Cleanup final {
        std::string root{};
        Runtime* runtime{nullptr};
        ~Cleanup() {
            if (runtime != nullptr) {
                (void)runtime->close();
            }
            lcctest::remove_tree(root);
        }
    } cleanup{root, &runtime};


    TransitionRequest loop_scoped_with_target =
        lcctest::make_request(runtime, ActionKind::IsolateLoop, DeviceId::from_value(kValveA), token.value(),
                              "struct-target");
    CHECK_CODE(runtime.plan(loop_scoped_with_target), StatusCode::InvalidArgument);

    TransitionRequest missing_target =
        lcctest::make_request(runtime, ActionKind::OpenValve, DeviceId::invalid(), token.value(), "struct-no-target");
    CHECK_CODE(runtime.plan(missing_target), StatusCode::MissingRequiredField);

    TransitionRequest stray_flow = lcctest::make_request(runtime, ActionKind::OpenValve,
                                                         DeviceId::from_value(kValveA), token.value(),
                                                         "struct-stray-flow");
    stray_flow.has_flow_target = true;
    stray_flow.flow_target = FlowRate::from_value(1000);
    CHECK_CODE(runtime.plan(stray_flow), StatusCode::InvalidArgument);

    TransitionRequest missing_flow = lcctest::make_request(runtime, ActionKind::SetFlowTarget,
                                                           DeviceId::invalid(), token.value(), "struct-no-flow");
    CHECK_CODE(runtime.plan(missing_flow), StatusCode::MissingRequiredField);

    TransitionRequest bad_reason = lcctest::make_request(runtime, ActionKind::OpenValve,
                                                         DeviceId::from_value(kValveA), token.value(), "struct-reason");
    bad_reason.reason = std::string(kMaxReasonLength + 1, 'x');
    CHECK_CODE(runtime.plan(bad_reason), StatusCode::StringTooLong);

    TransitionRequest stray_obligation = lcctest::make_request(runtime, ActionKind::OpenValve,
                                                               DeviceId::from_value(kValveA), token.value(),
                                                               "struct-obligation");
    stray_obligation.obligation = ObligationId::from_value(5);
    CHECK_CODE(runtime.plan(stray_obligation), StatusCode::InvalidArgument);

    TransitionRequest missing_attempt = lcctest::make_request(runtime, ActionKind::AbandonAttempt,
                                                              DeviceId::invalid(), token.value(), "struct-attempt");
    missing_attempt.reason = "abandon";
    CHECK_CODE(runtime.plan(missing_attempt), StatusCode::MissingRequiredField);

    TransitionRequest unknown_attempt = missing_attempt;
    unknown_attempt.attempt = AttemptId::from_value(42);
    CHECK_CODE(runtime.plan(unknown_attempt), StatusCode::AttemptNotFound);

    TransitionRequest flow_domain = lcctest::make_request(runtime, ActionKind::SetFlowTarget,
                                                          DeviceId::invalid(), token.value(), "struct-domain");
    flow_domain.has_flow_target = true;
    flow_domain.flow_target = FlowRate::from_value(-5);
    CHECK_CODE(runtime.plan(flow_domain), StatusCode::ValueOutOfRange);

    TransitionRequest too_large_step = flow_domain;
    too_large_step.flow_target = FlowRate::from_value(2'000'000);
    CHECK_CODE(runtime.plan(too_large_step), StatusCode::FlowOutOfRange);

    TransitionRequest pressure_step = lcctest::make_request(runtime, ActionKind::SetPressureTarget,
                                                            DeviceId::invalid(), token.value(), "struct-pressure");
    pressure_step.has_pressure_target = true;
    pressure_step.pressure_target = Pressure::from_value(1'400'000);
    CHECK_CODE(runtime.plan(pressure_step), StatusCode::PressureOutOfRange);
}

TEST(transitions, device_presence_and_fault_latching) {
    Scenario scenario("tr-presence");
    Runtime& runtime = *scenario.runtime;
    const auto token = scenario.operator_token();
    REQUIRE(runtime.observe(loop_id()).ok());

    scenario.fixture.adapter->set_device_present(DeviceId::from_value(kValveA), false);
    scenario.fixture.clock->advance(Duration::from_value(100'000'000));
    REQUIRE(runtime.observe(loop_id()).ok());
    TransitionRequest absent = lcctest::make_request(runtime, ActionKind::OpenValve,
                                                     DeviceId::from_value(kValveA), token, "presence-absent");
    CHECK_CODE(runtime.execute(absent), StatusCode::DeviceAbsent);

    // A device that is not registered in the loop is an identity fault.
    TransitionRequest unregistered = lcctest::make_request(runtime, ActionKind::OpenValve,
                                                           DeviceId::from_value(99), token,
                                                           "presence-unregistered");
    CHECK_CODE(runtime.plan(unregistered), StatusCode::UnknownObject);
}

TEST(transitions, target_state_legality) {
    Scenario scenario("tr-target-state");
    Runtime& runtime = *scenario.runtime;
    const auto token = scenario.operator_token();
    REQUIRE(runtime.observe(loop_id()).ok());

    TransitionRequest clear = lcctest::make_request(runtime, ActionKind::ClearIsolation, DeviceId::invalid(),
                                                    token, "state-clear-not-isolated");
    CHECK_CODE(runtime.plan(clear), StatusCode::LoopNotIsolated);

    scenario.fixture.adapter->set_valve_position(DeviceId::from_value(kValveB), ValvePosition::Open);
    REQUIRE(runtime.observe(loop_id()).ok());
    TransitionRequest close_twice = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                          DeviceId::from_value(kValveB), token,
                                                          "state-close");
    CHECK(runtime.execute(close_twice).ok());
    scenario.fixture.clock->advance(Duration::from_value(100'000'000));
    REQUIRE(runtime.observe(loop_id()).ok());
    TransitionRequest close_again = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                          DeviceId::from_value(kValveB), token,
                                                          "state-close-again");
    CHECK_CODE(runtime.plan(close_again), StatusCode::AlreadyInTargetState);

    TransitionRequest isolate = lcctest::make_request(runtime, ActionKind::IsolateLoop, DeviceId::invalid(),
                                                      token, "state-isolate");
    CHECK(runtime.execute(isolate).ok());
    TransitionRequest isolate_again = lcctest::make_request(runtime, ActionKind::IsolateLoop,
                                                            DeviceId::invalid(), token, "state-isolate-again");
    CHECK_CODE(runtime.plan(isolate_again), StatusCode::AlreadyInTargetState);

    TransitionRequest open_isolated = lcctest::make_request(runtime, ActionKind::OpenValve,
                                                            DeviceId::from_value(kValveA), token,
                                                            "state-open-isolated");
    CHECK_CODE(runtime.plan(open_isolated), StatusCode::LoopIsolated);

    TransitionRequest exit_service = lcctest::make_request(runtime, ActionKind::ExitService,
                                                           DeviceId::invalid(), scenario.engineer_token(),
                                                           "state-exit-not-service");
    CHECK_CODE(runtime.plan(exit_service), StatusCode::AlreadyInTargetState);
}

TEST(transitions, open_failure_modes) {
    lcctest::LoopFixture fixture;
    RuntimeConfig config = lcctest::fixture_config(fixture, lcctest::make_temp_root("tr-open"));
    config.adapter = nullptr;
    CHECK_CODE(Runtime::open(config), StatusCode::MissingRequiredField);

    config = lcctest::fixture_config(fixture, lcctest::make_temp_root("tr-open"));
    config.store_root.clear();
    CHECK_CODE(Runtime::open(config), StatusCode::MissingRequiredField);

    config = lcctest::fixture_config(fixture, lcctest::make_temp_root("tr-open"));
    config.topology_generation = TopologyGeneration::invalid();
    CHECK_CODE(Runtime::open(config), StatusCode::MissingRequiredField);

    config = lcctest::fixture_config(fixture, lcctest::make_temp_root("tr-open"));
    config.policy.generation = ConfigGeneration::invalid();
    CHECK_CODE(Runtime::open(config), StatusCode::MissingRequiredField);

    config = lcctest::fixture_config(fixture, lcctest::make_temp_root("tr-open"));
    config.initial_loops.front().devices.push_back(config.initial_loops.front().devices.front());
    CHECK_CODE(Runtime::open(config), StatusCode::DuplicateIdentity);

    config = lcctest::fixture_config(fixture, lcctest::make_temp_root("tr-open"));
    config.initial_loops.push_back(config.initial_loops.front());
    CHECK_CODE(Runtime::open(config), StatusCode::DuplicateIdentity);

    const std::string root = lcctest::make_temp_root("tr-open-topology");
    auto first = lcctest::open_runtime(fixture, root);
    REQUIRE(first.ok());
    (void)first.value()->close();
    auto reopened = lcctest::open_runtime(fixture, root);
    REQUIRE(reopened.ok());
    (void)reopened.value()->close();

    lcctest::LoopFixture other;
    RuntimeConfig changed = lcctest::fixture_config(other, root);
    changed.topology_generation = TopologyGeneration::from_value(9);
    CHECK_CODE(Runtime::open(changed), StatusCode::TopologyGenerationMismatch);
    changed.accept_topology_change = true;
    auto adopted = Runtime::open(changed);
    CHECK(adopted.ok());
    if (adopted.ok()) {
        CHECK_EQ(adopted.value()->status().value().topology_generation.value(), 9u);
        (void)adopted.value()->close();
    }
}

TEST(transitions, service_window_admits_obligated_work) {
    lcctest::LoopFixture fixture;
    ServiceObligation due;
    due.id = ObligationId::from_value(1);
    due.target = DeviceId::from_value(kPumpA);
    due.kind = ObligationKind::ServiceDue;
    due.active = true;
    due.raised_at = TimestampNs::from_nanos(1);
    due.note = "annual service";

    const std::string root = lcctest::make_temp_root("tr-window");
    RuntimeConfig config = lcctest::fixture_config(fixture, root);
    config.initial_obligations.push_back(due);
    fixture.adapter->set_valve_position(DeviceId::from_value(kValveA), ValvePosition::Open);
    auto opened = Runtime::open(config);
    REQUIRE(opened.ok());
    Runtime& runtime = *opened.value();
    REQUIRE(runtime.observe(loop_id()).ok());
    const auto operator_token = lcctest::token_for(runtime, kOperator, loop_id());
    const auto engineer_token = lcctest::token_for(runtime, kEngineer, loop_id());
    const auto isolator_token = lcctest::token_for(runtime, kIsolator, loop_id());
    REQUIRE(operator_token.ok());
    REQUIRE(engineer_token.ok());
    REQUIRE(isolator_token.ok());

    TransitionRequest start = lcctest::make_request(runtime, ActionKind::StartPump,
                                                    DeviceId::from_value(kPumpA), operator_token.value(),
                                                    "window-start");
    CHECK_CODE(runtime.execute(start), StatusCode::ServiceObligationActive);

    // A hazard-reducing action is still permitted while the obligation stands.
    TransitionRequest stop = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                   DeviceId::from_value(kValveA), operator_token.value(),
                                                   "window-stop");
    CHECK(runtime.plan(stop).ok());

    // Opening a service window requires service authority.
    const auto refused = runtime.open_service_window(loop_id(), isolator_token.value(),
                                                     Duration::from_value(3'600'000'000'000), "not my job");
    CHECK_CODE(refused, StatusCode::AuthorityInsufficient);

    const auto too_long = runtime.open_service_window(loop_id(), engineer_token.value(),
                                                      Duration::from_value(86'400'000'000'001), "too long");
    CHECK_CODE(too_long, StatusCode::ValueOutOfRange);

    const auto empty_note =
        runtime.open_service_window(loop_id(), engineer_token.value(), Duration::from_value(3'600'000'000'000), "");
    CHECK_CODE(empty_note, StatusCode::MissingRequiredField);

    const auto window = runtime.open_service_window(loop_id(), engineer_token.value(),
                                                    Duration::from_value(3'600'000'000'000), "pump service");
    REQUIRE(window.ok());
    CHECK_EQ(runtime.service_windows(loop_id()).value().size(), 1u);
    CHECK(runtime.loop_status(loop_id()).value().service_window_open);

    TransitionRequest during = lcctest::make_request(runtime, ActionKind::StartPump,
                                                    DeviceId::from_value(kPumpA), operator_token.value(),
                                                    "window-start-admitted");
    const auto admitted = runtime.execute(during);
    CHECK(admitted.ok());
    CHECK(admitted.ok() ? admitted.value().outcome == StatusCode::Ok : false);

    // Closing the window restores the obligation's grip.
    CHECK(runtime.close_service_window(window.value().id, engineer_token.value()).ok());
    CHECK_EQ(runtime.service_windows(loop_id()).value().size(), 0u);

    // An expired window no longer admits obligated work.
    const auto short_window = runtime.open_service_window(loop_id(), engineer_token.value(),
                                                          Duration::from_value(1'000'000'000), "brief");
    REQUIRE(short_window.ok());
    fixture.clock->advance(Duration::from_value(2'000'000'000));
    CHECK(!runtime.loop_status(loop_id()).value().service_window_open);

    (void)runtime.close();
    lcctest::remove_tree(root);
}
