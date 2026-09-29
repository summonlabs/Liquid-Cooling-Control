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

std::string read_text(const std::string& path) {
    std::ifstream file(path);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

/// Waits until a child produces a file.  The wait ends when the file appears or
/// when the child exits, so it can never spin forever on a dead child and it
/// makes no timing assumption about how long a child needs to start.
bool wait_for_artifact(lcctest::ChildProcess& child, const std::string& path) {
    for (;;) {
        if (std::filesystem::exists(std::filesystem::path(path))) {
            return true;
        }
        if (!lcctest::child_running(child)) {
            return std::filesystem::exists(std::filesystem::path(path));
        }
        lcctest::sleep_milliseconds(5);
    }
}

void release(const std::string& path) {
    std::ofstream out(path + ".release", std::ios::trunc);
    out << "go";
}

}  // namespace

TEST(multiprocess, an_external_process_holding_the_store_excludes_other_writers) {
    lcctest::LoopFixture fixture;
    const std::string root = lcctest::make_temp_root("mp-exclusive");
    const std::string exe = lcctest::executable_path();
    REQUIRE(!exe.empty());

    // Seed a complete generation so that read-only inspection has something to
    // read while the exclusive writer holds the store.
    {
        auto seeded = lcctest::open_runtime(fixture, root);
        REQUIRE(seeded.ok());
        (void)seeded.value()->close();
    }

    const std::string marker = root + "/holder-marker.txt";
    lcctest::ChildProcess holder = lcctest::spawn_child(exe, {"--child", "hold-lock", root, marker});
    REQUIRE(holder.valid());
    // Deterministic handshake: the child signals only after it holds the lock.
    CHECK(wait_for_artifact(holder, marker));

    auto blocked = lcctest::open_runtime(fixture, root);
    CHECK_CODE(blocked, StatusCode::StoreLocked);

    // Read-only inspection is still possible while a writer holds the store.
    CHECK(inspect_store(root, StoreBounds{}, nullptr).ok());

    release(marker);
    const int exit_code = lcctest::wait_child(holder);
    CHECK_EQ(exit_code, 0);

    auto after = lcctest::open_runtime(fixture, root);
    CHECK(after.ok());
    if (after.ok()) {
        CHECK(after.value()->observe(LoopId::from_value(kLoop)).ok());
        (void)after.value()->close();
    }
    lcctest::remove_tree(root);
}

TEST(multiprocess, exactly_one_of_several_processes_becomes_the_writer) {
    lcctest::LoopFixture fixture;
    const std::string root = lcctest::make_temp_root("mp-race");
    const std::string exe = lcctest::executable_path();
    REQUIRE(!exe.empty());

    // Seed the store so that the racing children have something to open.
    {
        auto seeded = lcctest::open_runtime(fixture, root);
        REQUIRE(seeded.ok());
        (void)seeded.value()->close();
    }

    const int participants = 5;
    std::vector<lcctest::ChildProcess> children;
    std::vector<std::string> reports;
    for (int index = 0; index < participants; ++index) {
        const std::string report = root + "/race-" + std::to_string(index) + ".txt";
        reports.push_back(report);
        lcctest::ChildProcess child = lcctest::spawn_child(exe, {"--child", "race-open", root, report});
        REQUIRE(child.valid());
        children.push_back(child);
    }
    // Every child reports before the winner is allowed to release the store, so
    // the outcome cannot depend on how the operating system schedules them.
    for (std::size_t index = 0; index < children.size(); ++index) {
        CHECK(wait_for_artifact(children[index], reports[index]));
    }
    int winners = 0;
    int losers = 0;
    for (const auto& report : reports) {
        const std::string outcome = read_text(report);
        if (outcome == "ok") {
            ++winners;
        } else if (outcome == "store-locked") {
            ++losers;
        }
    }
    CHECK_EQ(winners, 1);
    CHECK_EQ(losers, participants - 1);
    for (const auto& report : reports) {
        release(report);
    }
    for (std::size_t index = 0; index < children.size(); ++index) {
        CHECK_EQ(lcctest::wait_child(children[index]), 0);
    }

    // The store is intact and usable after the race.
    auto final_open = lcctest::open_runtime(fixture, root);
    CHECK(final_open.ok());
    if (final_open.ok()) {
        CHECK(inspect_store(root, StoreBounds{}, nullptr).ok());
        (void)final_open.value()->close();
    }
    lcctest::remove_tree(root);
}

TEST(multiprocess, an_external_reader_observes_only_complete_generations) {
    lcctest::LoopFixture fixture;
    const std::string root = lcctest::make_temp_root("mp-reader");
    const std::string exe = lcctest::executable_path();
    REQUIRE(!exe.empty());

    // Seed a complete generation first so that a reader never legitimately sees
    // an empty store, and any failure is a genuine publication defect.
    {
        auto seeded = lcctest::open_runtime(fixture, root);
        REQUIRE(seeded.ok());
        (void)seeded.value()->close();
    }

    lcctest::ChildProcess writer = lcctest::spawn_child(exe, {"--child", "commit-forever", root, "1"});
    REQUIRE(writer.valid());
    // Give the writer a moment to publish at least one newer generation.
    lcctest::sleep_milliseconds(50);

    std::uint64_t previous = 0;
    std::size_t observed_advances = 0;
    for (int sample = 0; sample < 40; ++sample) {
        const auto state = inspect_store(root, StoreBounds{}, nullptr);
        // Publication is atomic, so a concurrent reader must always observe a
        // complete generation.
        CHECK_CODE(state, StatusCode::Ok);
        if (!state.ok()) {
            break;
        }
        CHECK(state.value().commit_sequence.value() >= previous);
        CHECK_EQ(state.value().revision.value(), state.value().commit_sequence.value());
        CHECK_EQ(state.value().topology_generation.value(), state.value().commit_sequence.value());
        if (state.value().commit_sequence.value() > previous && previous != 0) {
            ++observed_advances;
        }
        previous = state.value().commit_sequence.value();
        lcctest::sleep_milliseconds(3);
    }
    CHECK(observed_advances > 0);
    lcctest::kill_child(writer, 23);
    (void)lcctest::wait_child(writer);
    CHECK(previous > 0);

    const auto final_state = inspect_store(root, StoreBounds{}, nullptr);
    CHECK(final_state.ok());
    lcctest::remove_tree(root);
}
