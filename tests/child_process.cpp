#include "child_process.hpp"

#include <chrono>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#include <csignal>
#else
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace lcctest {
namespace {

#ifdef _WIN32

[[nodiscard]] std::wstring to_wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    (void)MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), needed);
    return wide;
}

[[nodiscard]] std::string quote_argument(const std::string& argument) {
    std::string out = "\"";
    for (const char c : argument) {
        if (c == '"') {
            out += "\\\"";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

#endif

}  // namespace

std::string executable_path() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
        return {};
    }
    buffer.resize(length);
    const int needed = WideCharToMultiByte(CP_UTF8, 0, buffer.data(), static_cast<int>(buffer.size()), nullptr, 0,
                                           nullptr, nullptr);
    std::string narrow(static_cast<std::size_t>(needed), '\0');
    (void)WideCharToMultiByte(CP_UTF8, 0, buffer.data(), static_cast<int>(buffer.size()), narrow.data(), needed,
                              nullptr, nullptr);
    return narrow;
#else
    std::vector<char> buffer(4096, '\0');
    const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0) {
        return {};
    }
    return std::string(buffer.data(), static_cast<std::size_t>(length));
#endif
}

ChildProcess spawn_child(const std::string& exe, const std::vector<std::string>& args) {
    ChildProcess child;
#ifdef _WIN32
    std::string command = quote_argument(exe);
    for (const auto& argument : args) {
        command.push_back(' ');
        command += quote_argument(argument);
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    std::wstring wide_command = to_wide(command);
    std::vector<wchar_t> mutable_command(wide_command.begin(), wide_command.end());
    mutable_command.push_back(L'\0');
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &startup, &info)) {
        return child;
    }
    CloseHandle(info.hThread);
    child.handle = info.hProcess;
    child.pid = static_cast<unsigned long>(info.dwProcessId);
    return child;
#else
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const auto& argument : args) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::pid_t pid = 0;
    if (posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), environ) != 0) {
        return child;
    }
    child.handle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(pid));
    child.pid = static_cast<unsigned long>(pid);
    return child;
#endif
}

bool child_running(ChildProcess& child) {
    if (!child.valid()) {
        return false;
    }
#ifdef _WIN32
    const DWORD state = WaitForSingleObject(static_cast<HANDLE>(child.handle), 0);
    return state == WAIT_TIMEOUT;
#else
    int status = 0;
    const ::pid_t pid = static_cast<::pid_t>(reinterpret_cast<std::uintptr_t>(child.handle));
    const ::pid_t result = ::waitpid(pid, &status, WNOHANG);
    return result == 0;
#endif
}

void kill_child(ChildProcess& child, int exit_code) {
    if (!child.valid()) {
        return;
    }
#ifdef _WIN32
    (void)TerminateProcess(static_cast<HANDLE>(child.handle), static_cast<UINT>(exit_code));
#else
    const ::pid_t pid = static_cast<::pid_t>(reinterpret_cast<std::uintptr_t>(child.handle));
    (void)::kill(pid, SIGKILL);
#endif
}

int wait_child(ChildProcess& child) {
    if (!child.valid()) {
        return -1;
    }
#ifdef _WIN32
    HANDLE handle = static_cast<HANDLE>(child.handle);
    (void)WaitForSingleObject(handle, INFINITE);
    DWORD code = 0;
    (void)GetExitCodeProcess(handle, &code);
    CloseHandle(handle);
    child.handle = nullptr;
    return static_cast<int>(code);
#else
    int status = 0;
    const ::pid_t pid = static_cast<::pid_t>(reinterpret_cast<std::uintptr_t>(child.handle));
    (void)::waitpid(pid, &status, 0);
    child.handle = nullptr;
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return 128 + WTERMSIG(status);
#endif
}

int run_child(const std::string& exe, const std::vector<std::string>& args) {
    ChildProcess child = spawn_child(exe, args);
    if (!child.valid()) {
        return -1;
    }
    return wait_child(child);
}

void die_now(int exit_code) {
#ifdef _WIN32
    (void)TerminateProcess(GetCurrentProcess(), static_cast<UINT>(exit_code));
    // TerminateProcess does not return; the fall-through is unreachable.
    for (;;) {
    }
#else
    (void)::kill(::getpid(), SIGKILL);
    ::_exit(exit_code);
#endif
}

void suppress_crash_ui() {
#ifdef _WIN32
    (void)SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    (void)_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    (void)signal(SIGABRT, SIG_DFL);
    SetUnhandledExceptionFilter(nullptr);
#else
    (void)signal(SIGPIPE, SIG_IGN);
#endif
}

void sleep_milliseconds(unsigned milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

}  // namespace lcctest
