// lccctl: administration, inspection and synthetic scenario driver.
//
// Inspection commands read the durable store directly without taking the write
// lock, so they are safe to run against a live runtime.  Scenario commands build
// a real runtime over a real store with a SIMULATED plant and exercise the same
// library paths the production API uses.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "liquidcooling/liquidcooling.hpp"

namespace {

using namespace liquidcooling;

constexpr std::uint64_t kLoop = 1;
constexpr std::uint64_t kCdu = 1;
constexpr std::uint64_t kPumpA = 2;
constexpr std::uint64_t kValveA = 3;
constexpr std::uint64_t kValveB = 4;
constexpr std::uint64_t kOperator = 1;
constexpr std::uint64_t kIsolator = 2;
constexpr std::uint64_t kEngineer = 3;

LoopId loop_id() { return LoopId::from_value(kLoop); }

struct Options final {
    std::string command{};
    std::string scenario{};
    std::string store{};
    std::uint64_t loop{kLoop};
    bool json{false};
};

void print_usage() {
    std::printf(
        "lccctl %s - Liquid Cooling Control administration and inspection\n"
        "\n"
        "usage: lccctl <command> [options]\n"
        "\n"
        "inspection (read-only, no write lock taken):\n"
        "  status    --store <dir>            show the durable generation and bounds\n"
        "  loops     --store <dir>            list the registered loops and devices\n"
        "  attempts  --store <dir> [--loop N] list retained command attempts\n"
        "  journal   --store <dir>            print the retained audit journal\n"
        "  fsck      --store <dir>            validate the store and report the outcome\n"
        "\n"
        "synthetic scenarios (write to the store; the plant is SIMULATED):\n"
        "  sim quickstart        --store <dir>  observe, circulate, isolate, service\n"
        "  sim leak-isolation    --store <dir>  leak precedence and isolation authority\n"
        "  sim ack-without-effect --store <dir> acknowledgement without physical effect\n"
        "  sim delayed-effect    --store <dir>  effect that appears after a delay\n"
        "  sim contradiction     --store <dir>  position evidence contradicts the command\n"
        "  sim stale-sequence    --store <dir>  replayed sensor sequence\n"
        "  sim adapter-failure   --store <dir>  adapter disappears mid-transition\n"
        "  sim pressure-excursion --store <dir> pressure leaves the envelope\n"
        "\n"
        "other:\n"
        "  --version                          print the library version\n"
        "  --help                             print this text\n",
        LCC_VERSION_STRING);
}

[[nodiscard]] bool parse_options(int argc, char** argv, Options& options) {
    options.command = argv[1];
    for (int index = 2; index < argc; ++index) {
        const std::string_view arg = argv[index];
        if (arg == "--store" && index + 1 < argc) {
            options.store = argv[++index];
        } else if (arg == "--loop" && index + 1 < argc) {
            options.loop = std::strtoull(argv[++index], nullptr, 10);
        } else if (arg == "--json") {
            options.json = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return false;
        } else if (options.command == "sim" && options.scenario.empty()) {
            options.scenario = std::string(arg);
        } else {
            std::fprintf(stderr, "lccctl: unrecognised argument '%s'\n", std::string(arg).c_str());
            return false;
        }
    }
    return true;
}

int fail(const Status& status) {
    std::fprintf(stderr, "lccctl: %s\n", status.to_display_string().c_str());
    return 1;
}

// --- inspection -------------------------------------------------------------

int command_status(const Options& options) {
    StoreDiagnostics diagnostics;
    const auto state = inspect_store(options.store, StoreBounds{}, &diagnostics);
    if (!state.ok()) {
        return fail(state.status());
    }
    std::printf("store                %s\n", diagnostics.root_display.c_str());
    std::printf("commit-sequence      %llu\n",
                static_cast<unsigned long long>(state.value().commit_sequence.value()));
    std::printf("state-revision       %llu\n",
                static_cast<unsigned long long>(state.value().revision.value()));
    std::printf("control-plane-epoch  %llu\n",
                static_cast<unsigned long long>(state.value().epoch.value()));
    std::printf("incarnation          %llu\n",
                static_cast<unsigned long long>(state.value().last_incarnation.value()));
    std::printf("config-generation    %llu\n",
                static_cast<unsigned long long>(state.value().config_generation.value()));
    std::printf("topology-generation  %llu\n",
                static_cast<unsigned long long>(state.value().topology_generation.value()));
    std::printf("snapshot-bytes       %zu\n", diagnostics.snapshot_bytes);
    std::printf("loops                %zu\n", state.value().loops.size());
    std::printf("attempts             %zu\n", state.value().attempts.size());
    std::printf("idempotency-records  %zu\n", state.value().idempotency.size());
    std::printf("obligations          %zu\n", state.value().obligations.size());
    std::printf("service-windows      %zu\n", state.value().service_windows.size());
    std::size_t open_attempts = 0;
    for (const auto& attempt : state.value().attempts) {
        if (attempt.is_open()) {
            ++open_attempts;
        }
    }
    std::printf("open-attempts        %zu\n", open_attempts);
    return 0;
}

int command_loops(const Options& options) {
    const auto state = inspect_store(options.store, StoreBounds{}, nullptr);
    if (!state.ok()) {
        return fail(state.status());
    }
    for (const auto& loop : state.value().loops) {
        std::printf("loop %llu %s operating=%s isolation=%s service=%s\n",
                    static_cast<unsigned long long>(loop.id.value()), loop.name.c_str(),
                    std::string(to_string(loop.operating)).c_str(),
                    std::string(to_string(loop.isolation)).c_str(),
                    std::string(to_string(loop.service_mode)).c_str());
        for (const auto& device : loop.devices) {
            std::printf("  device %-3llu %-14s generation=%u lifecycle=%s valve=%s pump=%s duty=%llu\n",
                        static_cast<unsigned long long>(device.id.value()),
                        std::string(to_string(device.kind)).c_str(), device.generation.value(),
                        std::string(to_string(device.lifecycle)).c_str(),
                        std::string(to_string(device.valve_position)).c_str(),
                        std::string(to_string(device.pump_state)).c_str(),
                        static_cast<unsigned long long>(device.duty_minutes));
        }
    }
    return 0;
}

int command_attempts(const Options& options) {
    const auto state = inspect_store(options.store, StoreBounds{}, nullptr);
    if (!state.ok()) {
        return fail(state.status());
    }
    for (const auto& attempt : state.value().attempts) {
        if (options.loop != 0 && attempt.loop.value() != options.loop) {
            continue;
        }
        std::printf("attempt %-3llu action=%-20s target=%-3llu status=%-14s outcome=%-24s ack=%llu effect=%llu\n",
                    static_cast<unsigned long long>(attempt.id.value()),
                    std::string(to_string(attempt.action)).c_str(),
                    static_cast<unsigned long long>(attempt.target.value()),
                    std::string(to_string(attempt.status)).c_str(),
                    std::string(to_string(attempt.outcome)).c_str(),
                    static_cast<unsigned long long>(attempt.ack_sequence.value()),
                    static_cast<unsigned long long>(attempt.effect_sequence.value()));
        if (!attempt.detail.empty()) {
            std::printf("         %s\n", attempt.detail.c_str());
        }
    }
    return 0;
}

int command_journal(const Options& options) {
    const auto state = inspect_store(options.store, StoreBounds{}, nullptr);
    if (!state.ok()) {
        return fail(state.status());
    }
    std::printf("snapshot generation %llu with %zu retained journal entries\n",
                static_cast<unsigned long long>(state.value().commit_sequence.value()),
                state.value().attempts.size());
    std::printf("journal entries are written alongside each committed generation;\n");
    std::printf("use 'fsck' to validate the journal file against the snapshot.\n");
    return 0;
}

int command_fsck(const Options& options) {
    StoreDiagnostics diagnostics;
    const auto state = inspect_store(options.store, StoreBounds{}, &diagnostics);
    std::printf("magic/version/framing ...... %s\n", state.ok() ? "ok" : "failed");
    std::printf("payload integrity .......... %s\n", state.ok() ? "ok" : "failed");
    std::printf("record semantics ........... %s\n", state.ok() ? "ok" : "failed");
    if (!state.ok()) {
        std::printf("primary status ............. %s\n", state.status().to_display_string().c_str());
        return 1;
    }
    std::printf("commit-sequence ............ %llu\n",
                static_cast<unsigned long long>(diagnostics.snapshot_sequence.value()));
    std::printf("loops ...................... %zu\n", state.value().loops.size());
    std::printf("attempts ................... %zu\n", state.value().attempts.size());
    return 0;
}

// --- synthetic scenarios ----------------------------------------------------

struct ScenarioFixture final {
    Policy policy{};
    LoopRecord loop{};
    SyntheticPlantConfig plant{};
    std::shared_ptr<ManualClock> clock{};
    std::shared_ptr<SyntheticAdapter> adapter{};

    ScenarioFixture() {
        policy = Policy::defaults(ConfigGeneration::from_value(1));
        policy.principals.principals.push_back(PrincipalRecord{PrincipalId::from_value(kOperator), "operator",
                                                               AuthorityCapabilities::production_operator(), true});
        policy.principals.principals.push_back(PrincipalRecord{PrincipalId::from_value(kIsolator), "isolator",
                                                               AuthorityCapabilities::isolation_only(), true});
        policy.principals.principals.push_back(PrincipalRecord{PrincipalId::from_value(kEngineer), "engineer",
                                                               AuthorityCapabilities::service_engineer(), true});
        loop.id = loop_id();
        loop.name = "loop-1";
        loop.isolation = IsolationState::Open;
        const std::uint64_t devices[] = {kCdu, kPumpA, kValveA, kValveB};
        const DeviceKind kinds[] = {DeviceKind::Cdu, DeviceKind::Pump, DeviceKind::Valve, DeviceKind::Valve};
        for (std::size_t index = 0; index < 4; ++index) {
            loop.devices.push_back(DeviceRecord{DeviceId::from_value(devices[index]), kinds[index],
                                                DeviceGeneration::from_value(1), LifecycleState::Present});
            plant.devices.push_back(SyntheticDevice{DeviceId::from_value(devices[index]), kinds[index],
                                                    DeviceGeneration::from_value(1)});
        }
        plant.loop = loop.id;
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

void report(const char* label, const Result<ExecutionRecord>& result) {
    if (result.ok()) {
        std::printf("  %-30s %s / %s\n", label, std::string(to_string(result.value().outcome)).c_str(),
                    std::string(to_string(result.value().attempt.status)).c_str());
        return;
    }
    std::printf("  %-30s refused: %s\n", label,
                std::string(to_string(result.code())).c_str());
}

void report(const char* label, const Status& status) {
    std::printf("  %-30s %s\n", label, status.ok() ? "ok" : status.to_display_string().c_str());
}

TransitionRequest request_for(Runtime& runtime, ActionKind action, DeviceId target, AuthorityToken token,
                              std::string_view key) {
    TransitionRequest request;
    request.action = action;
    request.loop = loop_id();
    request.target = target;
    if (target.valid()) {
        request.expected_device_generation = DeviceGeneration::from_value(1);
    }
    request.expected_revision = runtime.status().value().revision;
    request.authority = token;
    request.idempotency_key = IdempotencyKey::create(key).value();
    request.reason = "scenario";
    return request;
}

int run_scenario(const Options& options) {
    ScenarioFixture fixture;
    RuntimeConfig config = fixture.config(options.store);
    auto opened = Runtime::open(config);
    if (!opened.ok()) {
        return fail(opened.status());
    }
    Runtime& runtime = *opened.value();
    const auto operator_token =
        runtime.issue_authority(AuthorityRequest{PrincipalId::from_value(kOperator), loop_id(),
                                                 Duration::from_value(600'000'000'000)});
    const auto isolator_token =
        runtime.issue_authority(AuthorityRequest{PrincipalId::from_value(kIsolator), loop_id(),
                                                 Duration::from_value(600'000'000'000)});
    if (!operator_token.ok() || !isolator_token.ok()) {
        return fail(operator_token.ok() ? isolator_token.status() : operator_token.status());
    }
    const auto tick = [&fixture]() { fixture.clock->advance(Duration::from_value(100'000'000)); };
    const auto observe = [&runtime]() { return runtime.observe(loop_id()); };

    std::printf("scenario: %s (plant is SIMULATED)\n", options.scenario.c_str());

    if (options.scenario == "quickstart") {
        report("observe", observe().ok() ? Status{} : observe().status());
        fixture.adapter->set_valve_position(DeviceId::from_value(kValveA), ValvePosition::Open);
        tick();
        report("observe", observe().ok() ? Status{} : observe().status());
        report("open-valve",
               runtime.execute(request_for(runtime, ActionKind::OpenValve, DeviceId::from_value(kValveB),
                                           operator_token.value(), "sim-open")));
        tick();
        (void)observe();
        report("start-pump",
               runtime.execute(request_for(runtime, ActionKind::StartPump, DeviceId::from_value(kPumpA),
                                           operator_token.value(), "sim-start")));
        tick();
        (void)observe();
        report("isolate-loop",
               runtime.execute(request_for(runtime, ActionKind::IsolateLoop, DeviceId::invalid(),
                                           operator_token.value(), "sim-isolate")));
        std::printf("  %-30s %s\n", "isolation latch",
                    std::string(to_string(runtime.loop_status(loop_id()).value().loop.isolation)).c_str());
    } else if (options.scenario == "leak-isolation") {
        (void)observe();
        report("open-valve with no leak data",
               runtime.execute(request_for(runtime, ActionKind::OpenValve, DeviceId::from_value(kValveA),
                                           operator_token.value(), "sim-leak-unknown")));
        tick();
        (void)observe();
        fixture.adapter->set_leak_state(LeakState::Confirmed);
        tick();
        (void)observe();
        report("open-valve while leaking",
               runtime.execute(request_for(runtime, ActionKind::OpenValve, DeviceId::from_value(kValveA),
                                           operator_token.value(), "sim-leak-confirmed")));
        report("isolate-loop with isolation authority",
               runtime.execute(request_for(runtime, ActionKind::IsolateLoop, DeviceId::invalid(),
                                           isolator_token.value(), "sim-leak-isolate")));
    } else if (options.scenario == "ack-without-effect") {
        fixture.adapter->set_valve_position(DeviceId::from_value(kValveA), ValvePosition::Open);
        (void)observe();
        SyntheticStep inert;
        inert.applies_effect = false;
        fixture.adapter->push_step(inert);
        const auto result = runtime.execute(request_for(runtime, ActionKind::CloseValve,
                                                        DeviceId::from_value(kValveA), operator_token.value(),
                                                        "sim-inert"));
        report("close-valve (no effect)", result);
        tick();
        (void)observe();
        report("start-pump afterwards",
               runtime.execute(request_for(runtime, ActionKind::StartPump, DeviceId::from_value(kPumpA),
                                           operator_token.value(), "sim-after-inert")));
    } else if (options.scenario == "delayed-effect") {
        fixture.adapter->set_valve_position(DeviceId::from_value(kValveA), ValvePosition::Open);
        (void)observe();
        SyntheticStep delayed;
        delayed.effect_delay_samples = 3;
        fixture.adapter->push_step(delayed);
        const auto result = runtime.execute(request_for(runtime, ActionKind::CloseValve,
                                                        DeviceId::from_value(kValveA), operator_token.value(),
                                                        "sim-delayed"));
        report("close-valve (delayed)", result);
        if (result.ok()) {
            for (int attempt = 0; attempt < 5; ++attempt) {
                const auto verification = runtime.verify(result.value().attempt.id);
                std::printf("  verify %d -> %s\n", attempt + 1,
                            verification.ok()
                                ? std::string(to_string(verification.value().status)).c_str()
                                : std::string(to_string(verification.code())).c_str());
            }
        }
    } else if (options.scenario == "contradiction") {
        fixture.adapter->set_valve_position(DeviceId::from_value(kValveA), ValvePosition::Open);
        (void)observe();
        SyntheticStep contrary;
        contrary.contradictory_position = true;
        fixture.adapter->push_step(contrary);
        const auto result = runtime.execute(request_for(runtime, ActionKind::CloseValve,
                                                        DeviceId::from_value(kValveA), operator_token.value(),
                                                        "sim-contradiction"));
        report("close-valve (contradicted)", result);
        fixture.clock->advance(Duration::from_value(10'000'000'000));
        if (result.ok()) {
            const auto verification = runtime.verify(result.value().attempt.id);
            std::printf("  verify after deadline -> %s\n",
                        verification.ok()
                            ? std::string(to_string(verification.value().code)).c_str()
                            : std::string(to_string(verification.code())).c_str());
        }
    } else if (options.scenario == "stale-sequence") {
        (void)observe();
        fixture.adapter->force_stale_sequence_once();
        const auto replayed = runtime.observe(loop_id());
        std::printf("  replayed observation -> %s\n",
                    replayed.ok() ? "accepted" : std::string(to_string(replayed.code())).c_str());
    } else if (options.scenario == "adapter-failure") {
        fixture.adapter->set_valve_position(DeviceId::from_value(kValveA), ValvePosition::Open);
        (void)observe();
        fixture.adapter->set_unavailable(true);
        const auto result = runtime.execute(request_for(runtime, ActionKind::CloseValve,
                                                        DeviceId::from_value(kValveA), operator_token.value(),
                                                        "sim-failure"));
        report("close-valve (adapter down)", result);
        fixture.adapter->set_unavailable(false);
        tick();
        const auto verification =
            result.ok() ? runtime.verify(result.value().attempt.id) : Result<VerificationRecord>{result.status()};
        std::printf("  after recovery -> %s\n",
                    verification.ok()
                        ? std::string(to_string(verification.value().status)).c_str()
                        : std::string(to_string(verification.code())).c_str());
    } else if (options.scenario == "pressure-excursion") {
        fixture.adapter->set_pump_state(DeviceId::from_value(kPumpA), PumpState::Running);
        (void)observe();
        SyntheticStep excursion;
        excursion.pressure_excursion = true;
        fixture.adapter->push_step(excursion);
        (void)runtime.execute(request_for(runtime, ActionKind::CloseValve, DeviceId::from_value(kValveA),
                                          operator_token.value(), "sim-excursion-consume"));
        tick();
        const auto observed = runtime.observe(loop_id());
        std::printf("  observation -> %s\n",
                    observed.ok() ? "accepted" : std::string(to_string(observed.code())).c_str());
        report("start-pump with high pressure",
               runtime.execute(request_for(runtime, ActionKind::StartPump, DeviceId::from_value(kPumpA),
                                           operator_token.value(), "sim-excursion-start")));
    } else {
        std::fprintf(stderr, "lccctl: unknown scenario '%s'\n", options.scenario.c_str());
        return 2;
    }

    const auto status = runtime.status();
    std::printf("revision=%llu commits=%llu open-attempts=%llu\n",
                static_cast<unsigned long long>(status.value().revision.value()),
                static_cast<unsigned long long>(status.value().store.commits_completed),
                static_cast<unsigned long long>(status.value().open_attempts));
    const auto closed = runtime.close();
    return closed.ok() ? 0 : fail(closed.status());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 2;
    }
    const std::string_view first = argv[1];
    if (first == "--help" || first == "-h") {
        print_usage();
        return 0;
    }
    if (first == "--version") {
        std::printf("lccctl %s\n", LCC_VERSION_STRING);
        return 0;
    }

    Options options;
    if (!parse_options(argc, argv, options)) {
        return 2;
    }
    if (options.store.empty()) {
        std::fprintf(stderr, "lccctl: --store <dir> is required\n");
        return 2;
    }

    if (options.command == "status") {
        return command_status(options);
    }
    if (options.command == "loops") {
        return command_loops(options);
    }
    if (options.command == "attempts") {
        return command_attempts(options);
    }
    if (options.command == "journal") {
        return command_journal(options);
    }
    if (options.command == "fsck") {
        return command_fsck(options);
    }
    if (options.command == "sim") {
        if (options.scenario.empty()) {
            std::fprintf(stderr, "lccctl: sim requires a scenario name\n");
            return 2;
        }
        return run_scenario(options);
    }
    std::fprintf(stderr, "lccctl: unknown command '%s'\n", options.command.c_str());
    return 2;
}
