// Leak isolation precedence and unresolved-attempt fencing.
//
// Demonstrates, on a SIMULATED plant:
//   1. exposure-increasing actuation is refused while leak evidence is absent;
//   2. with fresh, clear leak evidence the same request is admitted;
//   3. isolation authority is broader than production authority: a responder
//      with isolation rights only can reduce hazard but cannot add flow;
//   4. a confirmed leak blocks exposure-increasing actuation outright;
//   5. the isolation latch follows proven physical effect, not the command
//      acknowledgement;
//   6. an acknowledged command whose effect never appears stays unresolved and
//      fences incompatible actuation, while hazard-reducing actuation remains
//      available.

#include <cstdio>
#include <memory>
#include <string>

#include "liquidcooling/liquidcooling.hpp"

namespace {

using namespace liquidcooling;

constexpr std::uint64_t kLoop = 1;
constexpr std::uint64_t kCdu = 1;
constexpr std::uint64_t kPump = 2;
constexpr std::uint64_t kValveA = 3;
constexpr std::uint64_t kValveB = 4;
constexpr std::uint64_t kOperator = 1;
constexpr std::uint64_t kResponder = 2;
constexpr std::uint64_t kEngineer = 3;

void report(const char* step, const Status& status) {
    std::printf("  %-30s %s\n", step, status.to_display_string().c_str());
}

void report(const char* step, const Result<ExecutionRecord>& result) {
    if (result.ok()) {
        report(step, Status{result.value().outcome, std::string(to_string(result.value().attempt.status))});
    } else {
        report(step, result.status());
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string root = argc > 1 ? argv[1] : "lcc-leak-store";

    Policy policy = Policy::defaults(ConfigGeneration::from_value(1));
    policy.principals.principals.push_back(PrincipalRecord{PrincipalId::from_value(kOperator), "operator",
                                                           AuthorityCapabilities::production_operator(), true});
    // The responder holds isolation rights only: no production capability.
    policy.principals.principals.push_back(PrincipalRecord{PrincipalId::from_value(kResponder), "responder",
                                                           AuthorityCapabilities::isolation_only(), true});
    // The engineer holds service rights, which never include production rights.
    policy.principals.principals.push_back(PrincipalRecord{PrincipalId::from_value(kEngineer), "engineer",
                                                           AuthorityCapabilities::service_engineer(), true});

    LoopRecord loop;
    loop.id = LoopId::from_value(kLoop);
    loop.name = "loop-b";
    loop.isolation = IsolationState::Open;
    loop.devices.push_back(DeviceRecord{DeviceId::from_value(kCdu), DeviceKind::Cdu,
                                        DeviceGeneration::from_value(1), LifecycleState::Present});
    loop.devices.push_back(DeviceRecord{DeviceId::from_value(kPump), DeviceKind::Pump,
                                        DeviceGeneration::from_value(1), LifecycleState::Present,
                                        ValvePosition::Unknown, PumpState::Running});
    loop.devices.push_back(DeviceRecord{DeviceId::from_value(kValveA), DeviceKind::Valve,
                                        DeviceGeneration::from_value(1), LifecycleState::Present,
                                        ValvePosition::Open});
    loop.devices.push_back(DeviceRecord{DeviceId::from_value(kValveB), DeviceKind::Valve,
                                        DeviceGeneration::from_value(1), LifecycleState::Present,
                                        ValvePosition::Open});

    SyntheticPlantConfig plant;
    plant.loop = loop.id;
    plant.devices.push_back(SyntheticDevice{DeviceId::from_value(kCdu), DeviceKind::Cdu,
                                            DeviceGeneration::from_value(1)});
    plant.devices.push_back(SyntheticDevice{DeviceId::from_value(kPump), DeviceKind::Pump,
                                            DeviceGeneration::from_value(1)});
    plant.devices.push_back(SyntheticDevice{DeviceId::from_value(kValveA), DeviceKind::Valve,
                                            DeviceGeneration::from_value(1)});
    plant.devices.push_back(SyntheticDevice{DeviceId::from_value(kValveB), DeviceKind::Valve,
                                            DeviceGeneration::from_value(1)});

    auto clock = std::make_shared<ManualClock>();
    auto adapter = std::make_shared<SyntheticAdapter>(plant, clock);
    adapter->set_flow(FlowRate::from_value(120'000));
    // The simulated plant starts already circulating.
    adapter->set_pump_state(DeviceId::from_value(kPump), PumpState::Running);
    adapter->set_valve_position(DeviceId::from_value(kValveA), ValvePosition::Open);
    adapter->set_valve_position(DeviceId::from_value(kValveB), ValvePosition::Open);

    RuntimeConfig config;
    config.store_root = root;
    config.policy = policy;
    config.topology_generation = TopologyGeneration::from_value(1);
    config.adapter = adapter;
    config.clock = clock;
    config.initial_loops.push_back(loop);

    auto opened = Runtime::open(config);
    if (!opened.ok()) {
        std::printf("open failed: %s\n", opened.status().to_display_string().c_str());
        return 1;
    }
    Runtime& runtime = *opened.value();

    const auto tick = [&clock]() { clock->advance(Duration::from_value(1'000'000'000)); };
    const auto observe = [&]() {
        const auto result = runtime.observe(LoopId::from_value(kLoop));
        report("observe", result.ok() ? Status{} : result.status());
    };
    const auto revision = [&]() { return runtime.status().value().revision; };
    const auto operator_token = runtime.issue_authority(
        AuthorityRequest{PrincipalId::from_value(kOperator), LoopId::from_value(kLoop),
                         Duration::from_value(600'000'000'000)});
    const auto responder_token = runtime.issue_authority(
        AuthorityRequest{PrincipalId::from_value(kResponder), LoopId::from_value(kLoop),
                         Duration::from_value(600'000'000'000)});
    const auto engineer_token = runtime.issue_authority(
        AuthorityRequest{PrincipalId::from_value(kEngineer), LoopId::from_value(kLoop),
                         Duration::from_value(600'000'000'000)});
    if (!operator_token.ok() || !responder_token.ok() || !engineer_token.ok()) {
        std::printf("authority issuance failed\n");
        return 1;
    }

    const auto raise_flow = [&](const char* key, FlowRate target, const AuthorityToken& token,
                                const char* label) {
        TransitionRequest request;
        request.action = ActionKind::SetFlowTarget;
        request.loop = LoopId::from_value(kLoop);
        request.flow_target = target;
        request.has_flow_target = true;
        request.expected_revision = revision();
        request.authority = token;
        request.idempotency_key = IdempotencyKey::create(key).value();
        request.reason = label;
        report(label, runtime.execute(request));
    };

    std::printf("1. no leak evidence yet\n");
    raise_flow("leak-flow-0", FlowRate::from_value(130'000), operator_token.value(), "raise flow (unknown leak)");

    std::printf("2. fresh clear leak evidence\n");
    observe();
    raise_flow("leak-flow-1", FlowRate::from_value(130'000), operator_token.value(), "raise flow (clear leak)");

    std::printf("3. isolation authority is broader, not equal\n");
    tick();
    observe();
    raise_flow("leak-flow-2", FlowRate::from_value(140'000), responder_token.value(), "responder raises flow");
    {
        TransitionRequest request;
        request.action = ActionKind::CloseValve;
        request.loop = LoopId::from_value(kLoop);
        request.target = DeviceId::from_value(kValveB);
        request.expected_device_generation = DeviceGeneration::from_value(1);
        request.expected_revision = revision();
        request.authority = responder_token.value();
        request.idempotency_key = IdempotencyKey::create("leak-close-b").value();
        request.reason = "responder closes a valve";
        report("responder closes valve", runtime.execute(request));
    }
    tick();
    observe();
    {
        TransitionRequest request;
        request.action = ActionKind::OpenValve;
        request.loop = LoopId::from_value(kLoop);
        request.target = DeviceId::from_value(kValveB);
        request.expected_device_generation = DeviceGeneration::from_value(1);
        request.expected_revision = revision();
        request.authority = operator_token.value();
        request.idempotency_key = IdempotencyKey::create("leak-open-b").value();
        request.reason = "restore the valve";
        report("operator reopens valve", runtime.execute(request));
    }

    std::printf("4. confirmed leak blocks exposure-increasing work\n");
    adapter->set_leak_state(LeakState::Confirmed);
    tick();
    observe();
    raise_flow("leak-flow-3", FlowRate::from_value(150'000), operator_token.value(), "raise flow (leaking)");

    std::printf("5. isolation remains available and latches on proven effect\n");
    {
        TransitionRequest request;
        request.action = ActionKind::IsolateLoop;
        request.loop = LoopId::from_value(kLoop);
        request.expected_revision = revision();
        request.authority = responder_token.value();
        request.idempotency_key = IdempotencyKey::create("leak-isolate").value();
        request.reason = "respond to leak";
        report("isolate-loop", runtime.execute(request));
    }
    std::printf("  %-30s %s\n", "isolation latch",
                std::string(to_string(runtime.loop_status(LoopId::from_value(kLoop)).value().loop.isolation))
                    .c_str());

    std::printf("6. an acknowledged command with no effect fences later work\n");
    adapter->set_leak_state(LeakState::None);
    tick();
    observe();
    {
        TransitionRequest request;
        request.action = ActionKind::ClearIsolation;
        request.loop = LoopId::from_value(kLoop);
        request.expected_revision = revision();
        request.authority = operator_token.value();
        request.idempotency_key = IdempotencyKey::create("leak-clear").value();
        request.reason = "resume production";
        report("clear-isolation", runtime.execute(request));
    }
    tick();
    observe();
    {
        TransitionRequest request;
        request.action = ActionKind::OpenValve;
        request.loop = LoopId::from_value(kLoop);
        request.target = DeviceId::from_value(kValveB);
        request.expected_device_generation = DeviceGeneration::from_value(1);
        request.expected_revision = revision();
        request.authority = operator_token.value();
        request.idempotency_key = IdempotencyKey::create("leak-resume-b").value();
        request.reason = "resume circulation path";
        report("operator reopens valve", runtime.execute(request));
    }
    tick();
    observe();
    // The next open command is acknowledged but the simulated actuator never
    // moves, so the effect cannot be established.
    SyntheticStep inert;
    inert.applies_effect = false;
    adapter->push_step(inert);
    {
        TransitionRequest request;
        request.action = ActionKind::OpenValve;
        request.loop = LoopId::from_value(kLoop);
        request.target = DeviceId::from_value(kValveA);
        request.expected_device_generation = DeviceGeneration::from_value(1);
        request.expected_revision = revision();
        request.authority = operator_token.value();
        request.idempotency_key = IdempotencyKey::create("leak-open-a").value();
        request.reason = "reopen loop";
        report("open-valve (no effect)", runtime.execute(request));
    }
    tick();
    observe();
    raise_flow("leak-flow-4", FlowRate::from_value(160'000), operator_token.value(),
               "raise flow after unresolved");
    {
        TransitionRequest request;
        request.action = ActionKind::StartPump;
        request.loop = LoopId::from_value(kLoop);
        request.target = DeviceId::from_value(kPump);
        request.expected_device_generation = DeviceGeneration::from_value(1);
        request.expected_revision = revision();
        request.authority = operator_token.value();
        request.idempotency_key = IdempotencyKey::create("leak-start-pump").value();
        request.reason = "start circulation";
        report("start-pump after unresolved", runtime.execute(request));
    }
    {
        TransitionRequest request;
        request.action = ActionKind::SetFlowTarget;
        request.loop = LoopId::from_value(kLoop);
        request.flow_target = FlowRate::from_value(110'000);
        request.has_flow_target = true;
        request.expected_revision = revision();
        request.authority = operator_token.value();
        request.idempotency_key = IdempotencyKey::create("leak-lower-flow").value();
        request.reason = "lower flow instead";
        report("lower flow stays available", runtime.execute(request));
    }
    tick();
    observe();
    {
        TransitionRequest request;
        request.action = ActionKind::CloseValve;
        request.loop = LoopId::from_value(kLoop);
        request.target = DeviceId::from_value(kValveB);
        request.expected_device_generation = DeviceGeneration::from_value(1);
        request.expected_revision = revision();
        request.authority = responder_token.value();
        request.idempotency_key = IdempotencyKey::create("leak-close-b-again").value();
        request.reason = "reduce exposure despite the open attempt";
        report("close-valve stays available", runtime.execute(request));
    }

    std::printf("7. service authority abandons the unresolved attempt\n");
    {
        const auto attempts = runtime.attempts(LoopId::from_value(kLoop));
        AttemptId open_attempt{};
        for (const auto& attempt : attempts.value()) {
            if (attempt.is_open()) {
                open_attempt = attempt.id;
            }
        }
        const auto abandoned = runtime.abandon(open_attempt, engineer_token.value(),
                                               "actuator confirmed mechanically stuck");
        report("abandon", abandoned.ok() ? Status{} : abandoned.status());
    }

    const auto status = runtime.status();
    std::printf("revision=%llu open-attempts=%llu journal=%llu\n",
                static_cast<unsigned long long>(status.value().revision.value()),
                static_cast<unsigned long long>(status.value().open_attempts),
                static_cast<unsigned long long>(status.value().store.journal_entries));
    const auto closed = runtime.close();
    report("close", closed.ok() ? Status{} : closed.status());
    return closed.ok() ? 0 : 1;
}
