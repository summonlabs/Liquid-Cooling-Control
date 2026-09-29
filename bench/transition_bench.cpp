// Completed-operation benchmark for durable liquid-cooling transitions.
//
// The measured operation is a *completed* transition: plan, durable attempt
// record, adapter dispatch, post-command observation, effect verification and a
// final durable commit of the resolved attempt including the audit journal.
// Enqueue or submission latency is deliberately not measured.
//
// REAL:      the control runtime, the durable store and its file system syncs
//            run as compiled host code on the machine below.
// SYNTHETIC: the plant behind the adapter is a simulation.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include "liquidcooling/liquidcooling.hpp"

namespace {

using namespace liquidcooling;

constexpr std::uint64_t kLoop = 1;
constexpr std::uint64_t kCdu = 1;
constexpr std::uint64_t kPump = 2;
constexpr std::uint64_t kValve = 3;
constexpr std::uint64_t kOperator = 1;

using SteadyTimer = std::chrono::steady_clock;

[[nodiscard]] double milliseconds_since(SteadyTimer::time_point start) {
    return std::chrono::duration<double, std::milli>(SteadyTimer::now() - start).count();
}

struct Statistics final {
    double minimum_ms{0};
    double p50_ms{0};
    double p95_ms{0};
    double p99_ms{0};
    double maximum_ms{0};
    double mean_ms{0};
    double total_ms{0};
    std::size_t samples{0};
};

[[nodiscard]] Statistics summarise(std::vector<double> values) {
    Statistics stats;
    if (values.empty()) {
        return stats;
    }
    std::sort(values.begin(), values.end());
    stats.samples = values.size();
    stats.minimum_ms = values.front();
    stats.maximum_ms = values.back();
    stats.total_ms = std::accumulate(values.begin(), values.end(), 0.0);
    stats.mean_ms = stats.total_ms / static_cast<double>(values.size());
    const auto percentile = [&values](double fraction) {
        const double index = fraction * static_cast<double>(values.size() - 1);
        const auto position = static_cast<std::size_t>(index + 0.5);
        return values[std::min(position, values.size() - 1)];
    };
    stats.p50_ms = percentile(0.50);
    stats.p95_ms = percentile(0.95);
    stats.p99_ms = percentile(0.99);
    return stats;
}

struct Fixture final {
    Policy policy{};
    LoopRecord loop{};
    std::shared_ptr<ManualClock> clock{};
    std::shared_ptr<SyntheticAdapter> adapter{};

    Fixture() {
        policy = Policy::defaults(ConfigGeneration::from_value(1));
        policy.principals.principals.push_back(PrincipalRecord{PrincipalId::from_value(kOperator), "operator",
                                                               AuthorityCapabilities::production_operator(),
                                                               true});
        loop.id = LoopId::from_value(kLoop);
        loop.name = "loop-bench";
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
        clock = std::make_shared<ManualClock>();
        adapter = std::make_shared<SyntheticAdapter>(plant, clock);
    }

    [[nodiscard]] RuntimeConfig config(const std::string& store) const {
        RuntimeConfig runtime_config;
        runtime_config.store_root = store;
        runtime_config.policy = policy;
        runtime_config.topology_generation = TopologyGeneration::from_value(1);
        runtime_config.adapter = adapter;
        runtime_config.clock = clock;
        runtime_config.initial_loops.push_back(loop);
        return runtime_config;
    }
};

}  // namespace

int main(int argc, char** argv) {
    const std::string store = argc > 1 ? argv[1] : "lcc-bench-store";
    const std::size_t warmup_iterations = argc > 2 ? static_cast<std::size_t>(std::strtoul(argv[2], nullptr, 10)) : 200;
    const std::size_t measured_iterations =
        argc > 3 ? static_cast<std::size_t>(std::strtoul(argv[3], nullptr, 10)) : 1500;

    Fixture fixture;
    // Every iteration is a distinct request, so the idempotency bound is sized
    // to the workload rather than left at its default.
    fixture.policy.limits.max_idempotency_records = warmup_iterations + measured_iterations + 16;
    fixture.policy.limits.max_retained_attempts = 64;
    RuntimeConfig config = fixture.config(store);
    config.store_bounds.max_idempotency_records = fixture.policy.limits.max_idempotency_records;
    auto opened = Runtime::open(config);
    if (!opened.ok()) {
        std::fprintf(stderr, "benchmark could not open the store: %s\n",
                     opened.status().to_display_string().c_str());
        return 1;
    }
    Runtime& runtime = *opened.value();

    const LoopId loop = LoopId::from_value(kLoop);
    fixture.adapter->set_valve_position(DeviceId::from_value(kValve), ValvePosition::Open);
    if (!runtime.observe(loop).ok()) {
        std::fprintf(stderr, "benchmark could not observe the loop\n");
        return 1;
    }
    const auto token = runtime.issue_authority(AuthorityRequest{PrincipalId::from_value(kOperator), loop,
                                                                Duration::from_value(3'600'000'000'000)});
    if (!token.ok()) {
        std::fprintf(stderr, "benchmark could not obtain authority\n");
        return 1;
    }

    std::size_t sequence = 0;
    const auto run_once = [&]() {
        const bool closing = sequence % 2 == 0;
        TransitionRequest request;
        request.action = closing ? ActionKind::CloseValve : ActionKind::OpenValve;
        request.loop = loop;
        request.target = DeviceId::from_value(kValve);
        request.expected_device_generation = DeviceGeneration::from_value(1);
        request.expected_revision = runtime.status().value().revision;
        request.authority = token.value();
        request.idempotency_key = IdempotencyKey::create("bench-" + std::to_string(sequence)).value();
        request.reason = "benchmark";
        ++sequence;
        return runtime.execute(request);
    };

    for (std::size_t index = 0; index < warmup_iterations; ++index) {
        const auto result = run_once();
        if (!result.ok()) {
            std::fprintf(stderr, "warmup transition %zu failed: %s\n", index,
                         result.status().to_display_string().c_str());
            return 1;
        }
    }

    std::vector<double> durable_samples;
    durable_samples.reserve(measured_iterations);
    const auto wall_start = SteadyTimer::now();
    for (std::size_t index = 0; index < measured_iterations; ++index) {
        const auto start = SteadyTimer::now();
        const auto result = run_once();
        const double elapsed = milliseconds_since(start);
        if (!result.ok()) {
            std::fprintf(stderr, "measured transition %zu failed: %s\n", index,
                         result.status().to_display_string().c_str());
            return 1;
        }
        if (!is_ok(result.value().outcome)) {
            std::fprintf(stderr, "measured transition %zu did not complete: %s\n", index,
                         std::string(to_string(result.value().outcome)).c_str());
            return 1;
        }
        durable_samples.push_back(elapsed);
    }
    const double wall_ms = milliseconds_since(wall_start);

    const Statistics stats = summarise(durable_samples);
    const auto status = runtime.status();

    std::printf("Liquid Cooling Control transition benchmark\n");
    std::printf("workload          : plan + durable attempt + adapter dispatch + post-command\n");
    std::printf("                    observation + effect verification + durable commit\n");
    std::printf("                    (snapshot and journal file syncs included)\n");
    std::printf("plant             : SYNTHETIC (in-process simulated loop)\n");
    std::printf("runtime + storage : REAL (host processes and file system)\n");
    std::printf("store             : %s\n", status.value().store.root_display.c_str());
    std::printf("warmup iterations : %zu\n", warmup_iterations);
    std::printf("measured          : %zu\n", stats.samples);
    std::printf("commits completed : %llu\n",
                static_cast<unsigned long long>(status.value().store.commits_completed));
    std::printf("mean              : %.3f ms/op\n", stats.mean_ms);
    std::printf("p50               : %.3f ms/op\n", stats.p50_ms);
    std::printf("p95               : %.3f ms/op\n", stats.p95_ms);
    std::printf("p99               : %.3f ms/op\n", stats.p99_ms);
    std::printf("min / max         : %.3f / %.3f ms\n", stats.minimum_ms, stats.maximum_ms);
    std::printf("throughput        : %.1f completed transitions/s\n",
                static_cast<double>(stats.samples) / (wall_ms / 1000.0));
    std::printf("total wall        : %.1f ms\n", wall_ms);

    const auto closed = runtime.close();
    return closed.ok() ? 0 : 1;
}
