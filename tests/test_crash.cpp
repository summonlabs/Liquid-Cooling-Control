#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
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

std::string read_text(const std::string& path) {
    std::ifstream file(path);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

/// Asserts the crash-invariant the child maintains: revision, commit sequence
/// and topology generation always advance together, so a state that mixes two
/// publications is detectable.
void check_crash_invariant(const DurableState& state) {
    CHECK(state.commit_sequence.valid());
    CHECK_EQ(state.revision.value(), state.commit_sequence.value());
    CHECK_EQ(state.topology_generation.value(), state.commit_sequence.value());
}

}  // namespace

TEST(crash, process_death_during_commit_never_leaves_a_hybrid_generation) {
    lcctest::LoopFixture fixture;
    const std::string root = lcctest::make_temp_root("crash-commit");
    const std::string exe = lcctest::executable_path();
    REQUIRE(!exe.empty());

    // Kill the writer at a range of delays so that death lands at different
    // points inside the commit sequence, including between the two renames.
    const unsigned delays[] = {20, 30, 45, 65, 95, 145, 210};
    std::uint64_t highest_observed = 0;
    for (const unsigned delay : delays) {
        lcctest::ChildProcess child =
            lcctest::spawn_child(exe, {"--child", "commit-forever", root, "2"});
        REQUIRE(child.valid());
        lcctest::sleep_milliseconds(delay);
        lcctest::kill_child(child, 21);
        const int exit_code = lcctest::wait_child(child);
        CHECK(exit_code != 0);

        StoreDiagnostics diagnostics;
        const auto state = inspect_store(root, StoreBounds{}, &diagnostics);
        if (!state.ok()) {
            // A writer killed before its first publication leaves no
            // authoritative generation at all, which is permitted; anything
            // else is a torn publication and is a defect.
            CHECK_EQ(state.code(), StatusCode::StoreMissing);
            CHECK(!std::filesystem::exists(std::filesystem::path(root + "/lcc.snapshot")));
        } else {
            check_crash_invariant(state.value());
            CHECK(state.value().commit_sequence.value() >= highest_observed);
            highest_observed = state.value().commit_sequence.value();
        }

        // The store must be usable again immediately and must publish a newer
        // generation than the one recovered.
        auto store = DurableStore::open(root, StoreBounds{}, true, RecoveryPolicy::RefuseOnCorruption, "parent");
        REQUIRE(store.ok());
        // The parent maintains the same invariant as the child so that the next
        // generation is equally checkable.
        DurableState next = store.value()->state();
        const JournalCommitSequence predicted =
            next.commit_sequence.valid() ? next.commit_sequence.next() : JournalCommitSequence::from_value(1);
        next.revision = StateRevision::from_value(predicted.value());
        next.topology_generation = TopologyGeneration::from_value(predicted.value());
        const auto committed = store.value()->commit(std::move(next), {});
        CHECK(committed.ok());
        check_crash_invariant(store.value()->state());
        CHECK(store.value()->state().commit_sequence.value() == highest_observed + 1);
        highest_observed = store.value()->state().commit_sequence.value();
    }

    // No staging residue is left behind after a clean open of the last crash.
    std::size_t staging = 0;
    {
        const std::filesystem::directory_iterator listing{std::filesystem::path(root)};
        for (const auto& entry : listing) {
            const std::string name = entry.path().filename().string();
            if (name.size() > 4 && name.rfind("lcc.", 0) == 0 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
                ++staging;
            }
        }
    }
    CHECK_EQ(staging, 0u);
    lcctest::remove_tree(root);
}

TEST(crash, death_leaves_no_authoritative_partial_state) {
    lcctest::LoopFixture fixture;
    const std::string root = lcctest::make_temp_root("crash-partial");
    const std::string exe = lcctest::executable_path();
    REQUIRE(!exe.empty());

    // Each child adds commits to the generation left by the previous one, so
    // the sequence advances monotonically rather than resetting.
    std::uint64_t previous = 0;
    for (int round = 0; round < 6; ++round) {
        lcctest::ChildProcess child = lcctest::spawn_child(
            exe, {"--child", "commit-then-die", root, std::to_string(1 + round)});
        REQUIRE(child.valid());
        const int exit_code = lcctest::wait_child(child);
        CHECK_EQ(exit_code, 9);

        const auto state = inspect_store(root, StoreBounds{}, nullptr);
        REQUIRE(state.ok());
        check_crash_invariant(state.value());
        CHECK(state.value().commit_sequence.value() > previous);
        previous = state.value().commit_sequence.value();
    }
    CHECK(previous > 0);
    lcctest::remove_tree(root);
}

TEST(crash, staging_residue_from_a_dead_writer_is_reclaimed) {
    lcctest::LoopFixture fixture;
    const std::string root = lcctest::make_temp_root("crash-staging");
    const std::string exe = lcctest::executable_path();
    const int exit_code =
        lcctest::run_child(exe, {"--child", "die-with-staging", root});
    CHECK_EQ(exit_code, 11);
    CHECK(std::filesystem::exists(std::filesystem::path(root + "/lcc.snapshot.1234-1.tmp")));

    auto opened = lcctest::open_runtime(fixture, root);
    CHECK_CODE(opened, StatusCode::Ok);
    if (!opened.ok()) {
        return;
    }
    CHECK_EQ(opened.value()->status().value().store.staging_files_removed, 1u);
    CHECK(!std::filesystem::exists(std::filesystem::path(root + "/lcc.snapshot.1234-1.tmp")));
    (void)opened.value()->close();
    lcctest::remove_tree(root);
}

TEST(crash, restart_after_death_during_actuation_does_not_reactuate) {
    lcctest::LoopFixture fixture;
    const std::string root = lcctest::make_temp_root("crash-actuation");
    const std::string marker = root + "/child-marker.txt";
    const std::string exe = lcctest::executable_path();
    REQUIRE(!exe.empty());

    const int exit_code = lcctest::run_child(exe, {"--child", "die-during-actuation", root, marker});
    CHECK_EQ(exit_code, 13);
    const std::string recorded = read_text(marker);
    CHECK(!recorded.empty());

    // A brand new adapter: any actuation performed by the restarted runtime
    // would be visible here.
    auto fresh_adapter = std::make_shared<SyntheticAdapter>(fixture.plant, fixture.clock);
    RuntimeConfig config = lcctest::fixture_config(fixture, root);
    config.adapter = fresh_adapter;
    auto opened = Runtime::open(config);
    REQUIRE(opened.ok());
    Runtime& runtime = *opened.value();
    CHECK_EQ(fresh_adapter->actuation_count(), 0u);

    // The unresolved attempt survived the restart and still fences actuation.
    const auto attempts = runtime.attempts(loop_id());
    REQUIRE(attempts.ok());
    std::size_t open_attempts = 0;
    AttemptId stuck{};
    for (const auto& attempt : attempts.value()) {
        if (attempt.is_open()) {
            ++open_attempts;
            stuck = attempt.id;
            CHECK_EQ(attempt.action, ActionKind::OpenValve);
            CHECK_EQ(attempt.target, DeviceId::from_value(kValveA));
        }
    }
    CHECK_EQ(open_attempts, 1u);
    REQUIRE(stuck.valid());

    // Recovered evidence is never current physical evidence.
    const auto loop_status = runtime.loop_status(loop_id());
    REQUIRE(loop_status.ok());
    CHECK(!loop_status.value().evidence_usable);
    CHECK(loop_status.value().leak_indeterminate);

    const auto token = lcctest::token_for(runtime, lcctest::LoopFixture::kOperator, loop_id());
    REQUIRE(token.ok());
    TransitionRequest start = lcctest::make_request(runtime, ActionKind::StartPump,
                                                    DeviceId::from_value(kPumpA), token.value(),
                                                    "restart-start");
    CHECK_CODE(runtime.plan(start), StatusCode::UnresolvedAttemptBlocks);
    CHECK_EQ(fresh_adapter->actuation_count(), 0u);

    // A hazard-reducing action is still admissible, and it is the only actuation
    // performed after the restart.
    const auto isolator = lcctest::token_for(runtime, lcctest::LoopFixture::kIsolator, loop_id());
    REQUIRE(isolator.ok());
    TransitionRequest close = lcctest::make_request(runtime, ActionKind::CloseValve,
                                                    DeviceId::from_value(kValveB), isolator.value(),
                                                    "restart-close");
    const auto closed = runtime.execute(close);
    CHECK(closed.ok());
    CHECK_EQ(fresh_adapter->actuation_count(), 1u);
    const auto recorded_commands = fresh_adapter->commands();
    REQUIRE(!recorded_commands.empty());
    CHECK_EQ(recorded_commands.front().action, ActionKind::CloseValve);

    // Explicit abandonment under service authority clears the fence.
    const auto engineer = lcctest::token_for(runtime, lcctest::LoopFixture::kEngineer, loop_id());
    REQUIRE(engineer.ok());
    CHECK(runtime.abandon(stuck, engineer.value(), "verified the valve is seized after restart").ok());
    CHECK_EQ(runtime.status().value().open_attempts, 0u);

    // With clear evidence and no open attempt the loop can be worked again.
    REQUIRE(runtime.observe(loop_id()).ok());
    TransitionRequest after = lcctest::make_request(runtime, ActionKind::StartPump,
                                                    DeviceId::from_value(kPumpA), token.value(),
                                                    "restart-after-abandon");
    const auto started = runtime.execute(after);
    CHECK_CODE(started, StatusCode::Ok);
    if (!started.ok()) {
        return;
    }
    CHECK_EQ(fresh_adapter->actuation_count(), 2u);

    (void)runtime.close();
    lcctest::remove_tree(root);
}

TEST(crash, repeated_death_and_restart_cycles_keep_one_generation) {
    lcctest::LoopFixture fixture;
    const std::string root = lcctest::make_temp_root("crash-cycles");
    const std::string exe = lcctest::executable_path();
    std::uint64_t previous_sequence = 0;
    for (int cycle = 0; cycle < 8; ++cycle) {
        lcctest::ChildProcess child = lcctest::spawn_child(exe, {"--child", "commit-forever", root, "1"});
        REQUIRE(child.valid());
        lcctest::sleep_milliseconds(20 + static_cast<unsigned>(cycle) * 7);
        lcctest::kill_child(child, 22);
        (void)lcctest::wait_child(child);

        const auto state = inspect_store(root, StoreBounds{}, nullptr);
        if (state.ok()) {
            check_crash_invariant(state.value());
            CHECK(state.value().commit_sequence.value() >= previous_sequence);
            previous_sequence = state.value().commit_sequence.value();
        } else {
            CHECK_EQ(state.code(), StatusCode::StoreMissing);
        }
    }
    CHECK(previous_sequence > 0);
    lcctest::remove_tree(root);
}
