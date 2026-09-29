// Out-of-tree consumer: links the exported target and runs a real lifecycle.
//
// The plant is SIMULATED; the library, the durable store and the file system
// work are real.  A successful run proves the installed package is usable.

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include <liquidcooling/liquidcooling.hpp>

using namespace liquidcooling;

namespace {

constexpr std::uint64_t kLoop = 1;
constexpr std::uint64_t kCdu = 1;
constexpr std::uint64_t kPump = 2;
constexpr std::uint64_t kValve = 3;
constexpr std::uint64_t kOperator = 1;

}  // namespace

int main(int argc, char** argv) {
    const std::string root = argc > 1 ? argv[1] : "lcc-downstream-store";
    std::error_code error;
    std::filesystem::remove_all(root, error);

    Policy policy = Policy::defaults(ConfigGeneration::from_value(1));
    policy.principals.principals.push_back(PrincipalRecord{PrincipalId::from_value(kOperator), "operator",
                                                           AuthorityCapabilities::production_operator(), true});

    LoopRecord loop;
    loop.id = LoopId::from_value(kLoop);
    loop.name = "loop-downstream";
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
        std::printf("downstream: open failed: %s\n", opened.status().to_display_string().c_str());
        return 1;
    }
    Runtime& runtime = *opened.value();
    std::printf("downstream: linked Liquid Cooling Control %s\n", Version::string);
    std::printf("downstream: store opened at %s\n", runtime.status().value().store.root_display.c_str());

    const LoopId loop_id = LoopId::from_value(kLoop);
    if (!runtime.observe(loop_id).ok()) {
        std::printf("downstream: observe failed\n");
        return 1;
    }
    const auto token = runtime.issue_authority(
        AuthorityRequest{PrincipalId::from_value(kOperator), loop_id, Duration::from_value(600'000'000'000)});
    if (!token.ok()) {
        std::printf("downstream: authority failed\n");
        return 1;
    }

    adapter->set_valve_position(DeviceId::from_value(kValve), ValvePosition::Open);
    clock->advance(Duration::from_value(100'000'000));
    if (!runtime.observe(loop_id).ok()) {
        std::printf("downstream: second observe failed\n");
        return 1;
    }

    TransitionRequest request;
    request.action = ActionKind::CloseValve;
    request.loop = loop_id;
    request.target = DeviceId::from_value(kValve);
    request.expected_device_generation = DeviceGeneration::from_value(1);
    request.expected_revision = runtime.status().value().revision;
    request.authority = token.value();
    request.idempotency_key = IdempotencyKey::create("downstream-close").value();
    request.reason = "downstream smoke test";

    const auto execution = runtime.execute(request);
    if (!execution.ok()) {
        std::printf("downstream: transition refused: %s\n", execution.status().to_display_string().c_str());
        return 1;
    }
    std::printf("downstream: transition %s / %s\n",
                std::string(to_string(execution.value().outcome)).c_str(),
                std::string(to_string(execution.value().attempt.status)).c_str());
    if (!is_ok(execution.value().outcome)) {
        return 1;
    }

    // Reopening proves the durable generation survives the process lifecycle.
    const auto closed = runtime.close();
    if (!closed.ok()) {
        std::printf("downstream: close failed\n");
        return 1;
    }
    auto reopened = Runtime::open(config);
    if (!reopened.ok()) {
        std::printf("downstream: reopen failed: %s\n", reopened.status().to_display_string().c_str());
        return 1;
    }
    StoreDiagnostics diagnostics;
    const auto state = inspect_store(root, StoreBounds{}, &diagnostics);
    if (!state.ok()) {
        std::printf("downstream: store inspection failed\n");
        return 1;
    }
    std::printf("downstream: durable generation %llu holds %zu loop(s) and %zu attempt(s)\n",
                static_cast<unsigned long long>(state.value().commit_sequence.value()),
                state.value().loops.size(), state.value().attempts.size());
    const auto final_close = reopened.value()->close();
    if (!final_close.ok()) {
        return 1;
    }
    std::printf("downstream: ok\n");
    return 0;
}
