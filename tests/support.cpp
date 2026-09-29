#include "support.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <system_error>

namespace lcctest {

namespace {

std::atomic<std::uint64_t> g_unique{0};

}  // namespace

std::string temp_path(std::string_view name) {
    std::error_code error;
    const auto base = std::filesystem::temp_directory_path(error);
    if (error) {
        return std::string("liquidcooling-tests/") + std::string(name);
    }
    return (base / std::filesystem::path(std::string(name))).string();
}

std::string make_temp_root(std::string_view tag) {
    const auto counter = g_unique.fetch_add(1) + 1;
    const std::string name = "liquidcooling-" + std::string(tag) + "-" +
                             std::to_string(static_cast<unsigned long long>(::_getpid())) + "-" +
                             std::to_string(static_cast<unsigned long long>(counter));
    const std::string path = temp_path(name);
    std::error_code error;
    std::filesystem::remove_all(path, error);
    std::filesystem::create_directories(path, error);
    return path;
}

void remove_tree(const std::string& path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
}

LoopFixture::LoopFixture() {
    policy = Policy::defaults(ConfigGeneration::from_value(1));
    policy.principals.principals.push_back(PrincipalRecord{PrincipalId::from_value(kOperator), "operator",
                                                           AuthorityCapabilities::production_operator(), true});
    policy.principals.principals.push_back(PrincipalRecord{PrincipalId::from_value(kIsolator), "isolator",
                                                           AuthorityCapabilities::isolation_only(), true});
    policy.principals.principals.push_back(PrincipalRecord{PrincipalId::from_value(kEngineer), "engineer",
                                                           AuthorityCapabilities::service_engineer(), true});
    policy.principals.principals.push_back(PrincipalRecord{PrincipalId::from_value(kAuditor), "auditor",
                                                           AuthorityCapabilities::auditor(), true});

    loop.id = LoopId::from_value(kLoop);
    loop.name = "loop-test";
    loop.isolation = IsolationState::Open;
    loop.devices.push_back(DeviceRecord{DeviceId::from_value(kCdu), DeviceKind::Cdu,
                                        DeviceGeneration::from_value(1), LifecycleState::Present});
    loop.devices.push_back(DeviceRecord{DeviceId::from_value(kPumpA), DeviceKind::Pump,
                                        DeviceGeneration::from_value(1), LifecycleState::Present});
    loop.devices.push_back(DeviceRecord{DeviceId::from_value(kPumpB), DeviceKind::Pump,
                                        DeviceGeneration::from_value(1), LifecycleState::Present});
    loop.devices.push_back(DeviceRecord{DeviceId::from_value(kValveA), DeviceKind::Valve,
                                        DeviceGeneration::from_value(1), LifecycleState::Present});
    loop.devices.push_back(DeviceRecord{DeviceId::from_value(kValveB), DeviceKind::Valve,
                                        DeviceGeneration::from_value(1), LifecycleState::Present});

    plant.loop = loop.id;
    plant.devices.push_back(SyntheticDevice{DeviceId::from_value(kCdu), DeviceKind::Cdu,
                                            DeviceGeneration::from_value(1)});
    plant.devices.push_back(SyntheticDevice{DeviceId::from_value(kPumpA), DeviceKind::Pump,
                                            DeviceGeneration::from_value(1)});
    plant.devices.push_back(SyntheticDevice{DeviceId::from_value(kPumpB), DeviceKind::Pump,
                                            DeviceGeneration::from_value(1)});
    plant.devices.push_back(SyntheticDevice{DeviceId::from_value(kValveA), DeviceKind::Valve,
                                            DeviceGeneration::from_value(1)});
    plant.devices.push_back(SyntheticDevice{DeviceId::from_value(kValveB), DeviceKind::Valve,
                                            DeviceGeneration::from_value(1)});

    clock = std::make_shared<ManualClock>();
    adapter = std::make_shared<SyntheticAdapter>(plant, clock);
}

RuntimeConfig fixture_config(const LoopFixture& fixture, const std::string& root, bool create_if_missing) {
    RuntimeConfig config;
    config.store_root = root;
    config.create_store_if_missing = create_if_missing;
    config.policy = fixture.policy;
    config.topology_generation = TopologyGeneration::from_value(1);
    config.adapter = fixture.adapter;
    config.clock = fixture.clock;
    config.initial_loops.push_back(fixture.loop);
    return config;
}

Result<std::unique_ptr<Runtime>> open_runtime(const LoopFixture& fixture, const std::string& root,
                                              bool create_if_missing) {
    return Runtime::open(fixture_config(fixture, root, create_if_missing));
}

Result<AuthorityToken> token_for(Runtime& runtime, std::uint64_t principal, LoopId loop) {
    return runtime.issue_authority(
        AuthorityRequest{PrincipalId::from_value(principal), loop, Duration::from_value(600'000'000'000)});
}

Scenario::Scenario(std::string_view tag) {
    root = make_temp_root(tag);
    auto result = open_runtime(fixture, root);
    if (result.ok()) {
        runtime = std::move(result.value());
    }
}

Scenario::~Scenario() {
    if (runtime) {
        (void)runtime->close();
    }
    remove_tree(root);
}

AuthorityToken Scenario::operator_token() {
    const auto token = token_for(*runtime, LoopFixture::kOperator, loop_id());
    return token.ok() ? token.value() : AuthorityToken{};
}

AuthorityToken Scenario::isolator_token() {
    const auto token = token_for(*runtime, LoopFixture::kIsolator, loop_id());
    return token.ok() ? token.value() : AuthorityToken{};
}

AuthorityToken Scenario::engineer_token() {
    const auto token = token_for(*runtime, LoopFixture::kEngineer, loop_id());
    return token.ok() ? token.value() : AuthorityToken{};
}

AuthorityToken Scenario::auditor_token() {
    const auto token = token_for(*runtime, LoopFixture::kAuditor, loop_id());
    return token.ok() ? token.value() : AuthorityToken{};
}

void Scenario::reset_store() {
    if (runtime) {
        (void)runtime->close();
        runtime.reset();
    }
    remove_tree(root);
    auto result = open_runtime(fixture, root);
    if (result.ok()) {
        runtime = std::move(result.value());
    }
}

Result<ObservationOutcome> Scenario::observe(Duration advance) {
    fixture.clock->advance(advance);
    return runtime->observe(loop_id());
}

TransitionRequest make_request(Runtime& runtime, ActionKind action, DeviceId target,
                               AuthorityToken authority, std::string_view key) {
    TransitionRequest request;
    request.action = action;
    request.loop = LoopId::from_value(LoopFixture::kLoop);
    request.target = target;
    if (target.valid()) {
        request.expected_device_generation = DeviceGeneration::from_value(1);
    }
    request.expected_revision = runtime.status().value().revision;
    request.authority = authority;
    request.idempotency_key = IdempotencyKey::create(key).value();
    request.reason = "test";
    return request;
}

}  // namespace lcctest
