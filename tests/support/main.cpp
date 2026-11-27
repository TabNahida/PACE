// doctest entry point for every PACE test executable, plus a per-test-case watchdog.
//
// doctest's `timeout` decorator only reports a slow test after it returns, so a deadlocked
// test would hang CI. The watchdog ends the process with exit status 124 when one test case runs
// longer than its limit and names the test case first. The limit is the test case's
// `doctest::timeout(seconds)` decorator when it has one, otherwise PACE_TEST_TIMEOUT seconds (default 60).
//
// Executables that double as interop peers register a pace::test::command; see pace_test.hpp.

#define DOCTEST_CONFIG_IMPLEMENT
#include "support/pace_test.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

namespace
{
using clock_type = std::chrono::steady_clock;

std::vector<std::pair<std::string_view, pace::test::command_handler>>& commands()
{
    static std::vector<std::pair<std::string_view, pace::test::command_handler>> registered;
    return registered;
}

class watchdog
{
public:
    watchdog()
        : thread_([this] { run(); })
    {
    }

    watchdog(const watchdog&) = delete;
    watchdog& operator=(const watchdog&) = delete;

    ~watchdog()
    {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        changed_.notify_one();
        thread_.join();
    }

    void start(const doctest::TestCaseData& test)
    {
        const double limit = test.m_timeout > 0 ? test.m_timeout : default_limit();
        {
            std::lock_guard lock(mutex_);
            name_ = test.m_name;
            location_ = std::string(test.m_file.c_str()) + ":" + std::to_string(test.m_line);
            limit_seconds_ = limit;
            deadline_ = clock_type::now()
                        + std::chrono::duration_cast<clock_type::duration>(std::chrono::duration<double>(limit));
        }
        changed_.notify_one();
    }

    void finish()
    {
        {
            std::lock_guard lock(mutex_);
            deadline_.reset();
        }
        changed_.notify_one();
    }

private:
    static double default_limit()
    {
        if (const char* value = std::getenv("PACE_TEST_TIMEOUT"))
        {
            char* end = nullptr;
            const double seconds = std::strtod(value, &end);
            if (end != value && seconds > 0)
            {
                return seconds;
            }
        }
        return 60;
    }

    void run()
    {
        std::unique_lock lock(mutex_);
        while (!stopping_)
        {
            if (!deadline_)
            {
                changed_.wait(lock);
                continue;
            }
            const auto deadline = *deadline_;
            if (changed_.wait_until(lock, deadline) == std::cv_status::timeout && deadline_ == deadline)
            {
                std::fprintf(
                    stderr,
                    "\nPACE test watchdog: test case \"%s\" (%s) exceeded %g seconds\n",
                    name_.c_str(),
                    location_.c_str(),
                    limit_seconds_
                );
                std::fflush(stderr);
                // Not std::abort(): crash reporters can keep an aborting process alive for minutes.
                std::_Exit(124);
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<clock_type::time_point> deadline_;
    std::string name_;
    std::string location_;
    double limit_seconds_ = 0;
    bool stopping_ = false;
    std::thread thread_;
};

watchdog& global_watchdog()
{
    static watchdog instance;
    return instance;
}

// A listener sees every test case start and end without changing the selected reporter.
struct watchdog_listener : doctest::IReporter
{
    explicit watchdog_listener(const doctest::ContextOptions&)
    {
    }

    void report_query(const doctest::QueryData&) override
    {
    }

    void test_run_start() override
    {
    }

    void test_run_end(const doctest::TestRunStats&) override
    {
    }

    void test_case_start(const doctest::TestCaseData& test) override
    {
        global_watchdog().start(test);
    }

    void test_case_reenter(const doctest::TestCaseData& test) override
    {
        global_watchdog().start(test);
    }

    void test_case_end(const doctest::CurrentTestCaseStats&) override
    {
        global_watchdog().finish();
    }

    void test_case_exception(const doctest::TestCaseException&) override
    {
    }

    void subcase_start(const doctest::SubcaseSignature&) override
    {
    }

    void subcase_end() override
    {
    }

    void log_assert(const doctest::AssertData&) override
    {
    }

    void log_message(const doctest::MessageData&) override
    {
    }

    void test_case_skipped(const doctest::TestCaseData&) override
    {
    }
};

REGISTER_LISTENER("pace_watchdog", 1, watchdog_listener);
} // namespace

pace::test::command::command(const char* flag, command_handler run)
{
    commands().emplace_back(flag, run);
}

int main(int argc, char** argv)
{
    if (argc > 1)
    {
        for (const auto& [flag, run] : commands())
        {
            if (argv[1] == flag)
            {
                return run(argc, argv);
            }
        }
    }
    global_watchdog();
    doctest::Context context(argc, argv);
    return context.run();
}
