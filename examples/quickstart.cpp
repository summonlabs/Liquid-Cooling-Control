// Quickstart: build a synthetic loop, observe it, circulate, raise the flow
// setpoint, isolate the loop and return it to service, using only the public
// library API.
//
// The plant is SIMULATED.  This example demonstrates control semantics; it is
// not evidence of behaviour on real cooling hardware.

#include <cstdio>
#include <memory>
#include <string>

#include "liquidcooling/liquidcooling.hpp"

namespace {

using namespace liquidcooling;

constexpr std::uint64_t kLoop = 1;
constexpr std::uint64_t kCdu = 1;
constexpr std::uint64_t kPump = 2;
constexpr std::uint64_t kValve = 3;
constexpr std::uint64_t kOperator = 1;
constexpr std::uint64_t kEngineer = 2;

void report(const char* step, const Status& status) {
    std::printf("  %-26s %s\n", step, status.to_display_string().c_str());
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
    const std::string root = argc > 1 ? argv[1] : "lcc-quickstart-store";

    Policy policy = Policy::defaults(ConfigGeneration::from_value(1));
    policy.principals.principals.push_back(PrincipalRecord{PrincipalId::from_value(kOperator), "operator",
                                                           AuthorityCapabilities::production_operator(), true});
    policy.principals.principals.push_back(PrincipalRecord{PrincipalId::from_value(kEngineer), "engineer",
                                                           AuthorityCapabilities::service_engineer(), true});

    LoopRecord loop;
    loop.id = LoopId::from_value(kLoop);
    loop.name = "loop-a";
    loop.isolation = IsolationState::Open;
    loop.devices.push_back(DeviceRecord{DeviceId::from_value(kCdu), DeviceKind::Cdu,
                                        DeviceGeneration::from_value(1), LifecycleState::Present});
    loop.devices.push_back(DeviceRecord{DeviceId::from_value(kPump), DeviceKind::Pump,
                                        DeviceGeneration::from_value(1), LifecycleState::Present});
    loop.devices.push_back(DeviceRecord{DeviceId::from_value(kValve), DeviceKind::Valve,
                                        DeviceGeneration::from_value(1), LifecycleState::Present});

    SyntheticPlantConfig plant;
    plant.loop = loop.id;
    plant.devices.push_back(SyntheticDevice{DeviceId::from_value(kCdu), DeviceKind::Cdu,
                                            DeviceGeneration::from_value(1)});
    plant.devices.push_back(SyntheticDevice{DeviceId::from_value(kPump), DeviceKind::Pump,
                                            DeviceGeneration::from_value(1)});
    plant.devices.push_back(SyntheticDevice{DeviceId::from_value(kValve), DeviceKind::Valve,
                                            DeviceGeneration::from_value(1)});

    auto clock = std::make_shared<ManualClock>();
    auto adapter = std::make_shared<SyntheticAdapter>(plant, clock);

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
    std::printf("store: %s\n", runtime.status().value().store.root_display.c_str());

    // Evidence channels are only usable inside the freshness window, so the
    // manual clock advances by one second between steps and the loop is
    // re-observed before each exposure-increasing action.
    const auto tick = [&clock]() { clock->advance(Duration::from_value(1'000'000'000)); };
    const auto observe = [&]() {
        const auto result = runtime.observe(LoopId::from_value(kLoop));
        report("observe", result.ok() ? Status{} : result.status());
    };
    const auto revision = [&]() { return runtime.status().value().revision; };

    observe();

    const auto operator_token = runtime.issue_authority(
        AuthorityRequest{PrincipalId::from_value(kOperator), LoopId::from_value(kLoop),
                         Duration::from_value(600'000'000'000)});
    const auto engineer_token = runtime.issue_authority(
        AuthorityRequest{PrincipalId::from_value(kEngineer), LoopId::from_value(kLoop),
                         Duration::from_value(600'000'000'000)});
    if (!operator_token.ok() || !engineer_token.ok()) {
        std::printf("authority issuance failed\n");
        return 1;
    }

    const auto device_action = [&](ActionKind action, DeviceId device, IdempotencyKey key,
                                   const char* label) {
        TransitionRequest request;
        request.action = action;
        request.loop = LoopId::from_value(kLoop);
        request.target = device;
        request.expected_device_generation = DeviceGeneration::from_value(1);
        request.expected_revision = revision();
        request.authority = operator_token.value();
        request.idempotency_key = key;
        request.reason = label;
        report(label, runtime.execute(request));
    };

    tick();
    observe();
    device_action(ActionKind::OpenValve, DeviceId::from_value(kValve),
                  IdempotencyKey::create("qs-open-valve").value(), "open-loop-valve");
    tick();
    observe();
    device_action(ActionKind::StartPump, DeviceId::from_value(kPump),
                  IdempotencyKey::create("qs-start-pump").value(), "start-pump");
    tick();
    observe();

    {
        TransitionRequest request;
        request.action = ActionKind::SetFlowTarget;
        request.loop = LoopId::from_value(kLoop);
        request.flow_target = FlowRate::from_value(150'000);
        request.has_flow_target = true;
        request.expected_revision = revision();
        request.authority = operator_token.value();
        request.idempotency_key = IdempotencyKey::create("qs-set-flow").value();
        request.reason = "raise flow";
        report("set-flow-target", runtime.execute(request));
    }
    tick();
    observe();
    {
        const auto status = runtime.loop_status(LoopId::from_value(kLoop));
        std::printf("  %-26s %s\n", "measured flow",
                    to_display_string(status.value().evidence.flow.value).c_str());
        std::printf("  %-26s %s\n", "loop state",
                    std::string(to_string(status.value().loop.operating)).c_str());
    }

    tick();
    observe();
    {
        TransitionRequest request;
        request.action = ActionKind::IsolateLoop;
        request.loop = LoopId::from_value(kLoop);
        request.expected_revision = revision();
        request.authority = operator_token.value();
        request.idempotency_key = IdempotencyKey::create("qs-isolate").value();
        request.reason = "prepare for service";
        report("isolate-loop", runtime.execute(request));
    }
    tick();
    observe();

    {
        TransitionRequest request;
        request.action = ActionKind::EnterService;
        request.loop = LoopId::from_value(kLoop);
        request.expected_revision = revision();
        request.authority = engineer_token.value();
        request.idempotency_key = IdempotencyKey::create("qs-enter-service").value();
        request.reason = "pump service";
        report("enter-service", runtime.execute(request));
    }
    {
        TransitionRequest request;
        request.action = ActionKind::StartPump;
        request.loop = LoopId::from_value(kLoop);
        request.target = DeviceId::from_value(kPump);
        request.expected_device_generation = DeviceGeneration::from_value(1);
        request.expected_revision = revision();
        request.authority = operator_token.value();
        request.idempotency_key = IdempotencyKey::create("qs-service-start").value();
        request.reason = "attempt start during service";
        report("start during service", runtime.execute(request));
    }

    const auto final_status = runtime.status();
    std::printf("revision=%llu commits=%llu open-attempts=%llu journal=%llu\n",
                static_cast<unsigned long long>(final_status.value().revision.value()),
                static_cast<unsigned long long>(final_status.value().store.commits_completed),
                static_cast<unsigned long long>(final_status.value().open_attempts),
                static_cast<unsigned long long>(final_status.value().store.journal_entries));
    const auto closed = runtime.close();
    report("close", closed.ok() ? Status{} : closed.status());
    return closed.ok() ? 0 : 1;
}
