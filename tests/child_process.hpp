#pragma once

// Real operating-system child processes and real process death.
//
// Crash and exclusion tests must not be simulated inside one process: the point
// is that the operating system reclaims resources and that durable state
// survives a genuine, non-cooperative termination.

#include <cstdint>
#include <string>
#include <vector>

namespace lcctest {

struct ChildProcess final {
    void* handle{nullptr};
    unsigned long pid{0};

    [[nodiscard]] bool valid() const noexcept { return handle != nullptr; }
};

/// Absolute path of the currently running test executable.
[[nodiscard]] std::string executable_path();

/// Starts a child process without waiting for it.
[[nodiscard]] ChildProcess spawn_child(const std::string& exe, const std::vector<std::string>& args);

/// True while the child has not exited.
[[nodiscard]] bool child_running(ChildProcess& child);

/// Terminates the child immediately, as an external fault would.
void kill_child(ChildProcess& child, int exit_code);

/// Waits for the child and returns its exit code.
int wait_child(ChildProcess& child);

/// Runs a child to completion and returns its exit code.
int run_child(const std::string& exe, const std::vector<std::string>& args);

/// Terminates the current process without unwinding, flushing or atexit work.
[[noreturn]] void die_now(int exit_code);

/// Stops the platform from showing an interactive crash dialog for this process.
void suppress_crash_ui();

/// Suspends the calling thread for the given number of milliseconds.
void sleep_milliseconds(unsigned milliseconds);

}  // namespace lcctest
