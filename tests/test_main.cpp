#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "child_process.hpp"
#include "testing.hpp"

// Child-process entry points used by the crash, exclusion and recovery tests.
int lcc_child_main(int argc, char** argv);

namespace lcctest {

int run(std::string_view group_filter, std::string_view case_filter) {
    std::size_t executed = 0;
    std::size_t failed_cases = 0;
    for (const auto& test : Registry::cases()) {
        if (!group_filter.empty() && group_filter != "all" && test.group != group_filter) {
            continue;
        }
        if (!case_filter.empty() && test.name.find(case_filter) == std::string::npos) {
            continue;
        }
        Registry::current_case() = test.group + "." + test.name;
        const std::size_t before = Registry::failure_count();
        try {
            test.body();
        } catch (const std::exception& error) {
            Registry::record_failure(std::string("unhandled exception: ") + error.what(), __FILE__, __LINE__);
        } catch (...) {
            Registry::record_failure("unhandled non-standard exception", __FILE__, __LINE__);
        }
        ++executed;
        if (Registry::failure_count() != before) {
            ++failed_cases;
        } else {
            std::fprintf(stdout, "ok   %s\n", Registry::current_case().c_str());
        }
    }
    std::fprintf(stdout, "%s: %zu case(s) executed, %zu failed, %zu assertion failure(s)\n",
                 std::string(group_filter).c_str(), executed, failed_cases, Registry::failure_count());
    if (executed == 0) {
        std::fprintf(stderr, "no test case matched '%s'\n", std::string(group_filter).c_str());
        return 2;
    }
    return Registry::failure_count() == 0 ? 0 : 1;
}

}  // namespace lcctest

int main(int argc, char** argv) {
    lcctest::suppress_crash_ui();
    if (argc >= 2 && std::strcmp(argv[1], "--child") == 0) {
        return lcc_child_main(argc - 2, argv + 2);
    }
    const std::string group = argc >= 2 ? argv[1] : "all";
    const std::string filter = argc >= 3 ? argv[2] : "";
    return lcctest::run(group, filter);
}
