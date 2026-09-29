#pragma once

// Shared fixtures and helpers for the Liquid Cooling Control test suite.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "child_process.hpp"
#include "liquidcooling/liquidcooling.hpp"
#include "testing.hpp"

namespace lcctest {

using namespace liquidcooling;  // NOLINT(google-build-using-namespace) - test-only convenience

/// Deterministic temporary directory rooted outside the source tree.
[[nodiscard]] std::string make_temp_root(std::string_view tag);
void remove_tree(const std::string& path);

/// A filesystem path under the process temporary directory.
[[nodiscard]] std::string temp_path(std::string_view name);

struct LoopFixture final {
    static constexpr std::uint64_t kLoop = 1;
    static constexpr std::uint64_t kCdu = 1;
    static constexpr std::uint64_t kPumpA = 2;
    static constexpr std::uint64_t kPumpB = 3;
    static constexpr std::uint64_t kValveA = 4;
    static constexpr std::uint64_t kValveB = 5;
    static constexpr std::uint64_t kOperator = 1;
    static constexpr std::uint64_t kIsolator = 2;
    static constexpr std::uint64_t kEngineer = 3;
    static constexpr std::uint64_t kAuditor = 4;

    LoopRecord loop{};
    Policy policy{};
    SyntheticPlantConfig plant{};
    std::shared_ptr<ManualClock> clock{};
    std::shared_ptr<SyntheticAdapter> adapter{};

    LoopFixture();
};

[[nodiscard]] RuntimeConfig fixture_config(const LoopFixture& fixture, const std::string& root,
                                           bool create_if_missing = true);

/// Opens a runtime over the fixture, returning the owning pointer.
[[nodiscard]] liquidcooling::Result<std::unique_ptr<liquidcooling::Runtime>> open_runtime(
    const LoopFixture& fixture, const std::string& root, bool create_if_missing = true);

/// Builds a transition request against the current runtime revision.
[[nodiscard]] liquidcooling::TransitionRequest make_request(liquidcooling::Runtime& runtime,
                                                            liquidcooling::ActionKind action,
                                                            liquidcooling::DeviceId target,
                                                            liquidcooling::AuthorityToken authority,
                                                            std::string_view key);

[[nodiscard]] liquidcooling::Result<liquidcooling::AuthorityToken> token_for(
    liquidcooling::Runtime& runtime, std::uint64_t principal, liquidcooling::LoopId loop);

/// A fixture, a temporary store and an open runtime, torn down together.
class Scenario final {
public:
    explicit Scenario(std::string_view tag);
    ~Scenario();

    Scenario(const Scenario&) = delete;
    Scenario& operator=(const Scenario&) = delete;

    [[nodiscard]] bool opened() const noexcept { return runtime != nullptr; }
    [[nodiscard]] liquidcooling::Runtime& rt() const { return *runtime; }
    [[nodiscard]] liquidcooling::LoopId loop_id() const {
        return liquidcooling::LoopId::from_value(LoopFixture::kLoop);
    }

    [[nodiscard]] liquidcooling::AuthorityToken operator_token();
    [[nodiscard]] liquidcooling::AuthorityToken isolator_token();
    [[nodiscard]] liquidcooling::AuthorityToken engineer_token();
    [[nodiscard]] liquidcooling::AuthorityToken auditor_token();

    /// Closes the runtime, deletes the store directory and reopens a fresh one.
    void reset_store();

    /// Observe the loop, advancing the manual clock by the given amount first.
    liquidcooling::Result<liquidcooling::ObservationOutcome> observe(
        liquidcooling::Duration advance = liquidcooling::Duration::from_value(100'000'000));

    LoopFixture fixture{};
    std::string root{};
    std::unique_ptr<liquidcooling::Runtime> runtime{};
};

/// Adapter that re-enters the runtime from inside a callback.
///
/// The probe runs on a separate thread and is joined before the callback
/// returns.  If the runtime held its state mutex across the callback the probe
/// could never acquire it and the join would block, so termination of this test
/// is itself the proof that no lock is held across the adapter boundary.
class ReentrancyProbeAdapter final : public liquidcooling::ILoopAdapter {
public:
    explicit ReentrancyProbeAdapter(std::shared_ptr<SyntheticAdapter> inner) : inner_(std::move(inner)) {}

    void attach(liquidcooling::Runtime* runtime) noexcept { runtime_ = runtime; }
    [[nodiscard]] std::size_t probe_count() const noexcept { return probes_.load(); }

    [[nodiscard]] liquidcooling::AdapterDescriptor describe() const override {
        return inner_->describe();
    }

    [[nodiscard]] liquidcooling::Result<liquidcooling::CommandAck> actuate(
        const liquidcooling::ActuationCommand& command) override {
        probe();
        return inner_->actuate(command);
    }

    [[nodiscard]] liquidcooling::Result<liquidcooling::LoopReading> read(
        const liquidcooling::ObservationRequest& request) override {
        probe();
        return inner_->read(request);
    }

private:
    void probe() {
        liquidcooling::Runtime* runtime = runtime_;
        if (runtime == nullptr) {
            return;
        }
        probes_.fetch_add(1);
        std::thread worker([runtime]() {
            const auto status = runtime->status();
            CHECK(status.ok());
        });
        worker.join();
    }

    std::shared_ptr<SyntheticAdapter> inner_{};
    liquidcooling::Runtime* runtime_{nullptr};
    std::atomic<std::size_t> probes_{0};
};

}  // namespace lcctest
