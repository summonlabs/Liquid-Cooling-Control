// Adversarial hardening: deliberate attempts to break the runtime through
// malformed input, hostile adapters, ambiguous paths and resource abuse.

#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <thread>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "liquidcooling/liquidcooling.hpp"
#include "support.hpp"
#include "testing.hpp"

using namespace liquidcooling;

namespace {

constexpr auto kLoop = lcctest::LoopFixture::kLoop;
constexpr auto kPumpA = lcctest::LoopFixture::kPumpA;
constexpr auto kValveA = lcctest::LoopFixture::kValveA;

LoopId loop_id() { return LoopId::from_value(kLoop); }

/// Hostile adapter: throws, mislabels loops and regresses sequences on demand.
class HostileAdapter final : public ILoopAdapter {
public:
    enum class Misbehaviour {
        None,
        ThrowOnActuate,
        ThrowOnRead,
        WrongLoop,
        NegativeFlow,
        EmptyReading,
    };

    explicit HostileAdapter(std::shared_ptr<SyntheticAdapter> inner) : inner_(std::move(inner)) {}

    void set(Misbehaviour behaviour) noexcept { behaviour_ = behaviour; }

    [[nodiscard]] AdapterDescriptor describe() const override { return inner_->describe(); }

    [[nodiscard]] Result<CommandAck> actuate(const ActuationCommand& command) override {
        if (behaviour_ == Misbehaviour::ThrowOnActuate) {
            throw std::runtime_error("hostile adapter threw during actuation");
        }
        return inner_->actuate(command);
    }

    [[nodiscard]] Result<LoopReading> read(const ObservationRequest& request) override {
        if (behaviour_ == Misbehaviour::ThrowOnRead) {
            throw std::runtime_error("hostile adapter threw during a read");
        }
        auto reading = inner_->read(request);
        if (!reading.ok()) {
            return reading;
        }
        switch (behaviour_) {
            case Misbehaviour::WrongLoop:
                reading.value().loop = LoopId::from_value(999);
                break;
            case Misbehaviour::NegativeFlow:
                reading.value().flow = Evidence<FlowRate>::make_present(
                    FlowRate::from_value(-1), reading.value().sequence,
                    reading.value().device_generation.present ? reading.value().device_generation.value
                                                              : DeviceGeneration::from_value(1),
                    reading.value().observed_at, "hostile");
                break;
            case Misbehaviour::EmptyReading:
                return LoopReading{};
            case Misbehaviour::None:
            case Misbehaviour::ThrowOnActuate:
            case Misbehaviour::ThrowOnRead:
                break;
        }
        return reading;
    }

private:
    std::shared_ptr<SyntheticAdapter> inner_{};
    Misbehaviour behaviour_{Misbehaviour::None};
};

}  // namespace

TEST(adversarial, hostile_adapter_exceptions_do_not_escape_or_corrupt_state) {
    lcctest::LoopFixture fixture;
    auto hostile = std::make_shared<HostileAdapter>(fixture.adapter);
    const std::string root = lcctest::make_temp_root("adv-throw");
    RuntimeConfig config = lcctest::fixture_config(fixture, root);
    config.adapter = hostile;
    auto opened = Runtime::open(config);
    REQUIRE(opened.ok());
    Runtime& runtime = *opened.value();
    fixture.adapter->set_valve_position(DeviceId::from_value(kValveA), ValvePosition::Open);
    REQUIRE(runtime.observe(loop_id()).ok());
    const auto token = lcctest::token_for(runtime, lcctest::LoopFixture::kOperator, loop_id());
    REQUIRE(token.ok());

    hostile->set(HostileAdapter::Misbehaviour::ThrowOnRead);
    const auto read_failure = runtime.observe(loop_id());
    CHECK(!read_failure.ok());
    CHECK_EQ(read_failure.code(), StatusCode::AdapterInternalError);

    hostile->set(HostileAdapter::Misbehaviour::ThrowOnActuate);
    TransitionRequest request = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                      DeviceId::from_value(kValveA), token.value(), "hostile");
    const auto actuation = runtime.execute(request);
    CHECK(actuation.ok());
    if (actuation.ok()) {
        CHECK(actuation.value().attempt.is_open());
        CHECK_EQ(actuation.value().attempt.adapter_code, StatusCode::AdapterInternalError);
    }

    // The runtime stays usable and consistent once the adapter behaves again.
    hostile->set(HostileAdapter::Misbehaviour::None);
    fixture.clock->advance(Duration::from_value(100'000'000));
    CHECK(runtime.observe(loop_id()).ok());
    const auto status = runtime.status();
    REQUIRE(status.ok());
    CHECK(status.value().revision.valid());
    CHECK(status.value().store.commits_completed > 0);
    (void)runtime.close();
    lcctest::remove_tree(root);
}

TEST(adversarial, a_reading_for_another_loop_is_refused) {
    lcctest::LoopFixture fixture;
    auto hostile = std::make_shared<HostileAdapter>(fixture.adapter);
    const std::string root = lcctest::make_temp_root("adv-wrong-loop");
    RuntimeConfig config = lcctest::fixture_config(fixture, root);
    config.adapter = hostile;
    auto opened = Runtime::open(config);
    REQUIRE(opened.ok());
    Runtime& runtime = *opened.value();
    hostile->set(HostileAdapter::Misbehaviour::WrongLoop);
    const auto observed = runtime.observe(loop_id());
    CHECK(!observed.ok());
    CHECK_EQ(observed.code(), StatusCode::EvidenceConflicting);
    // Nothing was written, so the runtime is still on its opening generation.
    CHECK(!runtime.loop_status(loop_id()).value().evidence_usable);
    (void)runtime.close();
    lcctest::remove_tree(root);
}

TEST(adversarial, an_empty_reading_is_not_zero_valued_evidence) {
    lcctest::LoopFixture fixture;
    auto hostile = std::make_shared<HostileAdapter>(fixture.adapter);
    const std::string root = lcctest::make_temp_root("adv-empty");
    RuntimeConfig config = lcctest::fixture_config(fixture, root);
    config.adapter = hostile;
    auto opened = Runtime::open(config);
    REQUIRE(opened.ok());
    Runtime& runtime = *opened.value();
    hostile->set(HostileAdapter::Misbehaviour::EmptyReading);
    const auto observed = runtime.observe(loop_id());
    CHECK(!observed.ok());
    const auto status = runtime.loop_status(loop_id());
    REQUIRE(status.ok());
    // Every channel stays absent rather than becoming a usable zero.
    CHECK(!status.value().evidence.flow.present);
    CHECK(!status.value().evidence.leak.present);
    CHECK(!status.value().evidence_usable);

    const auto token = lcctest::token_for(runtime, lcctest::LoopFixture::kOperator, loop_id());
    REQUIRE(token.ok());
    TransitionRequest request = lcctest::make_request(runtime, ActionKind::OpenValve,
                                                      DeviceId::from_value(kValveA), token.value(),
                                                      "empty-evidence");
    CHECK_CODE(runtime.plan(request), StatusCode::LeakStateIndeterminate);
    (void)runtime.close();
    lcctest::remove_tree(root);
}

TEST(adversarial, store_paths_are_rejected_when_ambiguous_or_traversing) {
    lcctest::LoopFixture fixture;
    const auto attempt_open = [&fixture](const std::string& path, bool create) {
        RuntimeConfig config = lcctest::fixture_config(fixture, path, create);
        return Runtime::open(config);
    };

    CHECK_CODE(attempt_open("", true), StatusCode::MissingRequiredField);
    CHECK_CODE(attempt_open(std::string("a\0b", 3), true), StatusCode::StorePathInvalid);
    CHECK_CODE(attempt_open(std::string("..\\escape"), true), StatusCode::StorePathInvalid);
    CHECK_CODE(attempt_open(std::string("trailing.\\store"), true), StatusCode::StorePathInvalid);
    CHECK_CODE(attempt_open(std::string("trailing \\store"), true), StatusCode::StorePathInvalid);
    CHECK_CODE(attempt_open("NUL", true), StatusCode::StorePathInvalid);
    CHECK_CODE(attempt_open("C:\\Temp\\CON", true), StatusCode::StorePathInvalid);
    CHECK_CODE(attempt_open("C:\\Temp\\COM1", true), StatusCode::StorePathInvalid);
    CHECK_CODE(attempt_open("C:\\Temp\\AUX.log", true), StatusCode::StorePathInvalid);
    CHECK_CODE(attempt_open("C:\\Temp\\LPT9", true), StatusCode::StorePathInvalid);
    CHECK_CODE(attempt_open(std::string(5000, 'x'), true), StatusCode::StorePathInvalid);

    // A relative root is resolved against the working directory and every later
    // operation uses the resolved absolute path, so it is usable and its lock
    // identity matches the absolute spelling of the same directory.  The
    // working directory is redirected so that the suite never writes into
    // wherever it happens to be started from.
    {
        const std::string sandbox = lcctest::make_temp_root("adv-relative");
        const std::filesystem::path previous = std::filesystem::current_path();
        std::error_code change_error;
        std::filesystem::current_path(std::filesystem::path(sandbox), change_error);
        if (!change_error) {
            const std::string relative = "lcc-relative-store";
            const std::string identity = (std::filesystem::path(sandbox) / relative).string();
            auto first = attempt_open(relative, true);
            CHECK_CODE(first, StatusCode::Ok);
            if (first.ok()) {
                const std::string root_identity = first.value()->status().value().store.root_identity;
                CHECK_EQ(root_identity.size(), identity.size());
                bool same_identity = root_identity.size() == identity.size();
                for (std::size_t index = 0; same_identity && index < identity.size(); ++index) {
                    const char left = static_cast<char>(std::tolower(static_cast<unsigned char>(identity[index])));
                    same_identity = left == root_identity[index];
                }
                CHECK(same_identity);
                auto second = attempt_open(identity, true);
                CHECK_CODE(second, StatusCode::StoreLocked);
                (void)first.value()->close();
            }
            std::filesystem::current_path(previous, change_error);
        }
        lcctest::remove_tree(sandbox);
    }

    // A file where a directory is required is refused rather than replaced.
    const std::string root = lcctest::make_temp_root("adv-path-file");
    const std::string file = root + "\\not-a-directory";
    {
        std::FILE* handle = std::fopen(file.c_str(), "wb");
        REQUIRE(handle != nullptr);
        std::fputs("x", handle);
        std::fclose(handle);
    }
    CHECK_CODE(attempt_open(file, true), StatusCode::StorePathInvalid);
    lcctest::remove_tree(root);
}

TEST(adversarial, two_spellings_of_one_store_cannot_both_be_the_writer) {
    lcctest::LoopFixture fixture;
    const std::string root = lcctest::make_temp_root("adv-spelling");
    auto first = lcctest::open_runtime(fixture, root);
    REQUIRE(first.ok());

    // The same directory spelled with different case and a trailing separator
    // must resolve to the same logical store and therefore the same lock.
    std::string upper = root;
    for (char& c : upper) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    const std::string with_separator = root + "\\";
    auto second = lcctest::open_runtime(fixture, upper);
    CHECK_CODE(second, StatusCode::StoreLocked);
    auto third = lcctest::open_runtime(fixture, with_separator);
    CHECK_CODE(third, StatusCode::StoreLocked);
    (void)first.value()->close();
    lcctest::remove_tree(root);
}

TEST(adversarial, a_reparse_point_store_root_is_refused) {
    const std::string target = lcctest::make_temp_root("adv-link-target");
    const std::string link = lcctest::temp_path("adv-link-root");
    std::error_code error;
    std::filesystem::remove_all(link, error);
    std::filesystem::create_directory_symlink(target, link, error);
    if (error) {
        // Creating a link may need privileges that this environment does not
        // grant; the check is then unverified rather than assumed to pass.
        lcctest::Registry::note("symbolic link creation is unavailable in this environment; "
                                "reparse-point rejection is unverified");
        lcctest::remove_tree(target);
        return;
    }
    lcctest::LoopFixture fixture;
    auto opened = lcctest::open_runtime(fixture, link);
    CHECK_CODE(opened, StatusCode::StorePathInvalid);
    std::filesystem::remove_all(link, error);
    lcctest::remove_tree(target);
}

TEST(adversarial, runtime_lifetime_is_independent_of_the_adapter) {
    lcctest::LoopFixture fixture;
    const std::string root = lcctest::make_temp_root("adv-lifetime");
    std::shared_ptr<SyntheticAdapter> adapter = fixture.adapter;
    {
        auto opened = lcctest::open_runtime(fixture, root);
        REQUIRE(opened.ok());
        CHECK(opened.value()->observe(loop_id()).ok());
        // The runtime is destroyed without an explicit close.
    }
    // The adapter outlives the runtime and remains usable.
    CHECK_EQ(adapter->describe().protocol_version, 1u);
    auto reopened = lcctest::open_runtime(fixture, root);
    CHECK(reopened.ok());
    if (reopened.ok()) {
        (void)reopened.value()->close();
    }
    lcctest::remove_tree(root);
}

TEST(adversarial, repeated_cycles_do_not_leak_files_or_lock_handles) {
    lcctest::LoopFixture fixture;
    const std::string root = lcctest::make_temp_root("adv-cycles");
    for (int cycle = 0; cycle < 25; ++cycle) {
        auto opened = lcctest::open_runtime(fixture, root);
        REQUIRE(opened.ok());
        Runtime& runtime = *opened.value();
        (void)runtime.observe(loop_id());
        (void)runtime.flush();
        (void)runtime.close();
    }
    std::size_t files = 0;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator{std::filesystem::path(root), error}) {
        const std::string name = entry.path().filename().string();
        ++files;
        CHECK(name != "lcc.snapshot.tmp");
        if (name.size() > 4) {
            CHECK(name.compare(name.size() - 4, 4, ".tmp") != 0);
        }
    }
    CHECK(!error);
    // Exactly the four durable artefacts remain.
    CHECK_EQ(files, 4u);
    lcctest::remove_tree(root);
}

TEST(adversarial, concurrent_status_and_shutdown_are_consistent) {
    lcctest::Scenario scenario("adv-shutdown");
    REQUIRE(scenario.opened());
    Runtime& runtime = scenario.rt();
    std::atomic<bool> stop{false};
    std::atomic<bool> failure{false};
    std::thread reader([&]() {
        while (!stop.load()) {
            const auto status = runtime.status();
            if (!status.ok()) {
                failure.store(true);
            }
        }
    });
    CHECK(runtime.close().ok());
    stop.store(true);
    reader.join();
    CHECK(!failure.load());
    CHECK(runtime.close().ok());
    CHECK_CODE(runtime.status(), StatusCode::Ok);
}
