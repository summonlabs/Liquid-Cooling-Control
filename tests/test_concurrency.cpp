#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

}  // namespace

TEST(concurrency, adapter_callbacks_never_run_under_the_runtime_lock) {
    lcctest::LoopFixture fixture;
    auto probe = std::make_shared<lcctest::ReentrancyProbeAdapter>(fixture.adapter);
    const std::string root = lcctest::make_temp_root("conc-reentrancy");

    RuntimeConfig config = lcctest::fixture_config(fixture, root);
    config.adapter = probe;
    auto opened = Runtime::open(config);
    REQUIRE(opened.ok());
    Runtime& runtime = *opened.value();
    probe->attach(&runtime);

    // Every callback below runs the runtime's own public API on a second thread
    // and joins it.  If the runtime held its state mutex across the callback the
    // join could never complete, so reaching the end of this case is the proof.
    REQUIRE(runtime.observe(loop_id()).ok());
    fixture.adapter->set_valve_position(DeviceId::from_value(kValveA), ValvePosition::Open);
    REQUIRE(runtime.observe(loop_id()).ok());

    const auto token = lcctest::token_for(runtime, lcctest::LoopFixture::kOperator, loop_id());
    REQUIRE(token.ok());
    for (int index = 0; index < 8; ++index) {
        TransitionRequest request =
            lcctest::make_request(runtime, index % 2 == 0 ? ActionKind::CloseValve : ActionKind::OpenValve,
                                  DeviceId::from_value(kValveA), token.value(),
                                  "probe-" + std::to_string(index));
        const auto result = runtime.execute(request);
        CHECK(result.ok());
        // The command may be blocked by the previous unresolved attempt; either
        // way the callback path has been exercised.
    }
    CHECK(probe->probe_count() > 0);
    CHECK(runtime.status().ok());
    (void)runtime.close();
    lcctest::remove_tree(root);
}

TEST(concurrency, concurrent_readers_and_writers_keep_the_state_consistent) {
    lcctest::Scenario scenario("conc-mixed");
    REQUIRE(scenario.opened());
    Runtime& runtime = scenario.rt();
    scenario.fixture.adapter->set_pump_state(DeviceId::from_value(kPumpA), PumpState::Running);
    scenario.fixture.adapter->set_valve_position(DeviceId::from_value(kValveA), ValvePosition::Open);
    scenario.fixture.adapter->set_valve_position(DeviceId::from_value(kValveB), ValvePosition::Open);
    REQUIRE(scenario.observe().ok());

    const auto token = scenario.operator_token();
    REQUIRE(token.is_set());

    std::atomic<bool> stop{false};
    std::atomic<std::size_t> observations{0};
    std::atomic<std::size_t> executions{0};
    std::atomic<std::size_t> status_reads{0};
    std::atomic<bool> failure{false};
    StateRevision highest{};

    const auto observer_body = [&]() {
        while (!stop.load()) {
            const auto result = runtime.observe(loop_id());
            if (!result.ok() && result.code() != StatusCode::EvidenceStale) {
                failure.store(true);
            }
            observations.fetch_add(1);
        }
    };
    const auto reader_body = [&]() {
        while (!stop.load()) {
            const auto status = runtime.status();
            if (!status.ok()) {
                failure.store(true);
            }
            const auto loops = runtime.loops();
            if (!loops.ok() || loops.value().size() != 1) {
                failure.store(true);
            }
            status_reads.fetch_add(1);
        }
    };

    std::vector<std::thread> workers;
    workers.emplace_back(observer_body);
    workers.emplace_back(reader_body);
    workers.emplace_back(reader_body);

    for (int index = 0; index < 40; ++index) {
        const bool close = index % 2 == 0;
        TransitionRequest request = lcctest::make_request(
            runtime, close ? ActionKind::CloseValve : ActionKind::OpenValve,
            DeviceId::from_value(close ? kValveA : kValveB), token, "conc-" + std::to_string(index));
        const auto result = runtime.execute(request);
        // Concurrent observation can legitimately make a plan stale; any other
        // outcome is a defect.
        if (!result.ok() && result.code() != StatusCode::StaleRevision &&
            result.code() != StatusCode::UnresolvedAttemptBlocks &&
            result.code() != StatusCode::AlreadyInTargetState &&
            result.code() != StatusCode::EvidenceStale) {
            failure.store(true);
        }
        const auto status = runtime.status();
        if (!status.ok()) {
            failure.store(true);
        } else {
            if (status.value().revision < highest) {
                failure.store(true);
            }
            highest = status.value().revision;
        }
        executions.fetch_add(1);
    }

    stop.store(true);
    for (auto& worker : workers) {
        worker.join();
    }

    CHECK(!failure.load());
    CHECK(observations.load() > 0);
    CHECK(status_reads.load() > 0);
    CHECK_EQ(executions.load(), 40u);
    CHECK(runtime.journal().ok());

    const auto final_status = runtime.status();
    REQUIRE(final_status.ok());
    CHECK(final_status.value().revision.valid());
    CHECK(final_status.value().store.commits_completed > 0);
}

TEST(concurrency, repeated_open_and_close_is_safe) {
    lcctest::LoopFixture fixture;
    const std::string root = lcctest::make_temp_root("conc-open-close");
    for (int cycle = 0; cycle < 12; ++cycle) {
        auto opened = lcctest::open_runtime(fixture, root);
        REQUIRE(opened.ok());
        Runtime& runtime = *opened.value();
        CHECK(runtime.status().ok());
        // Closing twice is idempotent.
        CHECK(runtime.close().ok());
        CHECK(runtime.close().ok());
        // Work after close is refused rather than silently accepted.
        CHECK_CODE(runtime.observe(loop_id()), StatusCode::ShuttingDown);
        const auto token = lcctest::token_for(runtime, lcctest::LoopFixture::kOperator, loop_id());
        CHECK_CODE(token, StatusCode::ShuttingDown);
        TransitionRequest request = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                          DeviceId::from_value(kValveA), AuthorityToken{},
                                                          "after-close");
        CHECK_CODE(runtime.plan(request), StatusCode::ShuttingDown);
    }
    lcctest::remove_tree(root);
}

TEST(concurrency, destruction_without_close_releases_the_store) {
    lcctest::LoopFixture fixture;
    const std::string root = lcctest::make_temp_root("conc-destructor");
    {
        auto opened = lcctest::open_runtime(fixture, root);
        REQUIRE(opened.ok());
        CHECK(opened.value()->observe(loop_id()).ok());
        // No explicit close: the destructor must release the exclusive lock.
    }
    auto reopened = lcctest::open_runtime(fixture, root);
    CHECK(reopened.ok());
    if (reopened.ok()) {
        (void)reopened.value()->close();
    }
    lcctest::remove_tree(root);
}

TEST(concurrency, concurrent_open_of_one_store_admits_exactly_one_writer) {
    lcctest::LoopFixture fixture;
    const std::string root = lcctest::make_temp_root("conc-exclusive");
    std::atomic<int> successes{0};
    std::atomic<int> locked{0};
    std::vector<std::unique_ptr<Runtime>> keep;
    std::mutex keep_mutex;
    std::vector<std::thread> workers;
    for (int index = 0; index < 4; ++index) {
        workers.emplace_back([&]() {
            auto opened = lcctest::open_runtime(fixture, root);
            if (opened.ok()) {
                successes.fetch_add(1);
                std::lock_guard<std::mutex> guard(keep_mutex);
                keep.push_back(std::move(opened.value()));
            } else if (opened.code() == StatusCode::StoreLocked) {
                locked.fetch_add(1);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    CHECK_EQ(successes.load(), 1);
    CHECK_EQ(locked.load(), 3);
    for (auto& runtime : keep) {
        (void)runtime->close();
    }
    lcctest::remove_tree(root);
}
