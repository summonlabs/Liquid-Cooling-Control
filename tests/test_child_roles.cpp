// Child-process entry points.
//
// These run in a genuinely separate operating-system process so that process
// death, lock release and durable recovery are exercised for real rather than
// simulated inside one address space.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "child_process.hpp"
#include "liquidcooling/liquidcooling.hpp"
#include "support.hpp"

using namespace liquidcooling;

namespace {

constexpr auto kLoop = lcctest::LoopFixture::kLoop;
constexpr auto kPumpA = lcctest::LoopFixture::kPumpA;
constexpr auto kValveA = lcctest::LoopFixture::kValveA;
constexpr auto kValveB = lcctest::LoopFixture::kValveB;

LoopId loop_id() { return LoopId::from_value(kLoop); }

int fail(const char* message) {
    std::fprintf(stderr, "child: %s\n", message);
    return 3;
}

/// Opens the store directly and commits forever, keeping the invariant
/// revision == commit_sequence == topology_generation so that a partially
/// published generation can be detected by the parent.
int commit_forever(const std::string& root, int per_commit_delay_ms) {
    auto store = DurableStore::open(root, StoreBounds{}, true, RecoveryPolicy::RefuseOnCorruption, "child");
    if (!store.ok()) {
        return fail("child could not open the store");
    }
    for (;;) {
        DurableState next = store.value()->state();
        const JournalCommitSequence predicted =
            next.commit_sequence.valid() ? next.commit_sequence.next() : JournalCommitSequence::from_value(1);
        next.revision = StateRevision::from_value(predicted.value());
        next.topology_generation = TopologyGeneration::from_value(predicted.value());
        const auto committed = store.value()->commit(std::move(next), {});
        if (!committed.ok()) {
            return fail("child commit failed");
        }
        if (per_commit_delay_ms > 0) {
            lcctest::sleep_milliseconds(static_cast<unsigned>(per_commit_delay_ms));
        }
    }
}

/// Commits a fixed number of generations and then dies without unwinding.
int commit_then_die(const std::string& root, int commits) {
    auto store = DurableStore::open(root, StoreBounds{}, true, RecoveryPolicy::RefuseOnCorruption, "child");
    if (!store.ok()) {
        return fail("child could not open the store");
    }
    for (int index = 0; index < commits; ++index) {
        DurableState next = store.value()->state();
        const JournalCommitSequence predicted =
            next.commit_sequence.valid() ? next.commit_sequence.next() : JournalCommitSequence::from_value(1);
        next.revision = StateRevision::from_value(predicted.value());
        next.topology_generation = TopologyGeneration::from_value(predicted.value());
        const auto committed = store.value()->commit(std::move(next), {});
        if (!committed.ok()) {
            return fail("child commit failed");
        }
    }
    lcctest::die_now(9);
}

/// Leaves an abandoned staging file behind, then dies.
int die_with_staging(const std::string& root) {
    auto store = DurableStore::open(root, StoreBounds{}, true, RecoveryPolicy::RefuseOnCorruption, "child");
    if (!store.ok()) {
        return fail("child could not open the store");
    }
    DurableState next = store.value()->state();
    const JournalCommitSequence predicted =
        next.commit_sequence.valid() ? next.commit_sequence.next() : JournalCommitSequence::from_value(1);
    next.revision = StateRevision::from_value(predicted.value());
    next.topology_generation = TopologyGeneration::from_value(predicted.value());
    const auto committed = store.value()->commit(std::move(next), {});
    if (!committed.ok()) {
        return fail("child commit failed");
    }
    {
        std::ofstream staging(root + "/lcc.snapshot.1234-1.tmp", std::ios::binary | std::ios::trunc);
        staging << "staging residue";
    }
    lcctest::die_now(11);
}

/// Publishes an artefact atomically, so that its existence in the parent
/// already implies its content is complete.
void write_artifact(const std::string& path, const std::string& content) {
    const std::string staging = path + ".part";
    {
        std::ofstream out(staging, std::ios::trunc);
        out << content;
    }
    std::error_code error;
    std::filesystem::rename(std::filesystem::path(staging), std::filesystem::path(path), error);
}

/// Waits until a release file appears.  The parent drives the handshake, so no
/// timing assumption is involved.
void wait_for_release(const std::string& release_path) {
    for (;;) {
        std::ifstream probe(release_path);
        if (probe.good()) {
            return;
        }
        lcctest::sleep_milliseconds(5);
    }
}

/// Holds the exclusive store lock until the parent releases it.
int hold_lock(const std::string& root, const std::string& marker) {
    auto store = DurableStore::open(root, StoreBounds{}, true, RecoveryPolicy::RefuseOnCorruption, "holder");
    if (!store.ok()) {
        return fail("holder could not open the store");
    }
    write_artifact(marker, "locked");
    wait_for_release(marker + ".release");
    return 0;
}

/// Races to open the store and reports the outcome to a file.
int race_open(const std::string& root, const std::string& report) {
    lcctest::LoopFixture fixture;
    auto opened = lcctest::open_runtime(fixture, root);
    if (opened.ok()) {
        // The winner holds the store until the parent has collected every
        // result, so the race outcome cannot depend on scheduling.
        write_artifact(report, "ok");
        wait_for_release(report + ".release");
        (void)opened.value()->close();
        return 0;
    }
    write_artifact(report, std::string(to_string(opened.code())));
    return 0;
}

/// Issues a command whose effect never appears and then dies mid-lifecycle.
int die_during_actuation(const std::string& root, const std::string& marker) {
    lcctest::LoopFixture fixture;
    auto opened = lcctest::open_runtime(fixture, root);
    if (!opened.ok()) {
        return fail("child could not open the runtime");
    }
    Runtime& runtime = *opened.value();
    fixture.adapter->set_valve_position(DeviceId::from_value(kValveB), ValvePosition::Open);
    if (!runtime.observe(loop_id()).ok()) {
        return fail("child could not observe the loop");
    }
    const auto token = lcctest::token_for(runtime, lcctest::LoopFixture::kOperator, loop_id());
    if (!token.ok()) {
        return fail("child could not obtain authority");
    }
    SyntheticStep inert;
    inert.applies_effect = false;
    fixture.adapter->push_step(inert);
    TransitionRequest request = lcctest::make_request(runtime, ActionKind::OpenValve,
                                                      DeviceId::from_value(kValveA), token.value(), "child-act");
    const auto result = runtime.execute(request);
    if (!result.ok()) {
        return fail("child could not issue the command");
    }
    if (!result.value().attempt.is_open()) {
        return fail("child expected an unresolved attempt");
    }
    write_artifact(marker, std::to_string(result.value().attempt.id.value()) + " " +
                               std::to_string(static_cast<unsigned>(result.value().attempt.status)));
    // Death while the attempt is still open and the store lock is held.
    lcctest::die_now(13);
}

/// Prints a compact summary of the durable store, for parent assertions.
int inspect(const std::string& root) {
    StoreDiagnostics diagnostics;
    const auto state = inspect_store(root, StoreBounds{}, &diagnostics);
    if (!state.ok()) {
        std::printf("error %s\n", std::string(to_string(state.code())).c_str());
        return 0;
    }
    std::printf("sequence=%llu revision=%llu loops=%llu attempts=%llu\n",
                static_cast<unsigned long long>(state.value().commit_sequence.value()),
                static_cast<unsigned long long>(state.value().revision.value()),
                static_cast<unsigned long long>(state.value().loops.size()),
                static_cast<unsigned long long>(state.value().attempts.size()));
    return 0;
}

}  // namespace

int lcc_child_main(int argc, char** argv) {
    // argv[0] is the role; the --child switch has already been consumed.
    if (argc < 1) {
        return fail("child requires a role");
    }
    const std::string role = argv[0];
    if (role == "commit-forever" && argc >= 3) {
        return commit_forever(argv[1], std::atoi(argv[2]));
    }
    if (role == "commit-then-die" && argc >= 3) {
        return commit_then_die(argv[1], std::atoi(argv[2]));
    }
    if (role == "die-with-staging" && argc >= 2) {
        return die_with_staging(argv[1]);
    }
    if (role == "hold-lock" && argc >= 3) {
        return hold_lock(argv[1], argv[2]);
    }
    if (role == "race-open" && argc >= 3) {
        return race_open(argv[1], argv[2]);
    }
    if (role == "die-during-actuation" && argc >= 3) {
        return die_during_actuation(argv[1], argv[2]);
    }
    if (role == "inspect" && argc >= 2) {
        return inspect(argv[1]);
    }
    return fail("unknown child role");
}
