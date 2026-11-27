// Awaitable process signals. Every signal is raised only after the subscription exists and, where the test is about
// waking a suspended wait, after the wait has suspended; deliveries are counted, so no test depends on timing.
#include <pace/runtime.hpp>
#include "support/pace_test.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <signal.h>
#endif

using namespace std::chrono_literals;

namespace
{
using pace::process_signal;
using pace::test::require;

bool canceled(const std::system_error& error)
{
    return error.code() == std::errc::operation_canceled;
}

// The OS signal on POSIX; the in-process equivalent on Windows, where these signals have no OS source.
void raise_signal(process_signal value)
{
#ifdef _WIN32
    pace::deliver_signal(value);
#else
    int number = 0;
    switch (value)
    {
        case process_signal::hangup:
            number = SIGHUP;
            break;
        case process_signal::interrupt:
            number = SIGINT;
            break;
        case process_signal::terminate:
            number = SIGTERM;
            break;
        case process_signal::user1:
            number = SIGUSR1;
            break;
        case process_signal::user2:
            number = SIGUSR2;
            break;
    }
    require(std::raise(number) == 0, "raise");
#endif
}

// Suspends in set.wait() and reports what it got; `waiting` turns true right before the wait.
pace::task<process_signal> wait_on(pace::signal_set& set, bool& waiting)
{
    waiting = true;
    co_return co_await set.wait();
}

pace::task<void> suspended_wait_wakes()
{
    auto& io = pace::this_context();
    pace::signal_set set(io, {process_signal::user1});
    co_await pace::with_task_group(
        [&](pace::task_group& group) -> pace::task<void>
        {
            bool waiting = false;
            auto result = group.spawn(wait_on(set, waiting));
            while (!waiting)
            {
                co_await io.yield();
            }
            raise_signal(process_signal::user1);
            require(co_await result == process_signal::user1, "suspended wait resumed with SIGUSR1");
        }
    );
}

pace::task<void> deliveries_are_counted()
{
    auto& io = pace::this_context();
    pace::signal_set set(io, {process_signal::user1, process_signal::user2, process_signal::hangup});
    require(set.contains(process_signal::user2) && !set.contains(process_signal::terminate), "contains");
    // Raised while no wait is pending: each delivery is kept, and reported in enumeration order.
    raise_signal(process_signal::user2);
    raise_signal(process_signal::user1);
    raise_signal(process_signal::user1);
    require(co_await set.wait() == process_signal::user1, "first pending");
    require(co_await set.wait() == process_signal::user1, "second pending");
    require(co_await set.wait() == process_signal::user2, "third pending");
    // A set created after a delivery does not see it.
    raise_signal(process_signal::hangup);
    pace::signal_set later(io, {process_signal::hangup});
    require(co_await set.wait() == process_signal::hangup, "earlier set saw SIGHUP");
    co_await pace::with_task_group(
        [&](pace::task_group& group) -> pace::task<void>
        {
            bool waiting = false;
            auto result = group.spawn(wait_on(later, waiting));
            while (!waiting)
            {
                co_await io.yield();
            }
            raise_signal(process_signal::hangup);
            require(co_await result == process_signal::hangup, "later set saw only the later SIGHUP");
        }
    );
}

pace::task<void> sets_in_one_context_each_see_a_delivery()
{
    auto& io = pace::this_context();
    pace::signal_set a(io, {process_signal::user2});
    pace::signal_set b(io, {process_signal::user2, process_signal::user1});
    co_await pace::with_task_group(
        [&](pace::task_group& group) -> pace::task<void>
        {
            bool first = false;
            bool second = false;
            auto ra = group.spawn(wait_on(a, first));
            auto rb = group.spawn(wait_on(b, second));
            while (!first || !second)
            {
                co_await io.yield();
            }
            raise_signal(process_signal::user2);
            require(co_await ra == process_signal::user2, "first set");
            require(co_await rb == process_signal::user2, "second set");
        }
    );
    bool duplicate = false;
    co_await pace::with_task_group(
        [&](pace::task_group& group) -> pace::task<void>
        {
            bool waiting = false;
            auto pending = group.spawn(wait_on(a, waiting));
            while (!waiting)
            {
                co_await io.yield();
            }
            try
            {
                (void)co_await a.wait();
            }
            catch (const std::logic_error&)
            {
                duplicate = true;
            }
            raise_signal(process_signal::user2);
            co_await pending;
        }
    );
    require(duplicate, "a second pending wait on one set is rejected");
}

pace::task<void> cancellation_and_stop()
{
    auto& io = pace::this_context();
    pace::signal_set set(io, {process_signal::user1});
    pace::cancellation_source source;
    co_await pace::with_task_group(
        [&](pace::task_group& group) -> pace::task<void>
        {
            bool waiting = false;
            auto result = group.spawn(
                [&]() -> pace::task<bool>
                {
                    try
                    {
                        (void)co_await pace::with_cancellation(source.token(), wait_on(set, waiting));
                    }
                    catch (const std::system_error& error)
                    {
                        co_return canceled(error);
                    }
                    co_return false;
                }
            );
            while (!waiting)
            {
                co_await io.yield();
            }
            source.request_cancellation();
            require(co_await result, "cancelled signal wait");
        }
    );
    // The cancelled wait consumed nothing: a delivery afterwards is reported to the next wait.
    raise_signal(process_signal::user1);
    require(co_await set.wait() == process_signal::user1, "delivery after cancellation");
}

// The graceful-shutdown pattern from the documentation: a service runs until SIGINT or SIGTERM.
pace::task<void> graceful_shutdown(bool& stopped_serving)
{
    auto& io = pace::this_context();
    bool waiting = false;
    co_await pace::with_task_group(
        [&](pace::task_group& group) -> pace::task<void>
        {
            (void)group.spawn(
                [&]() -> pace::task<void>
                {
                    try
                    {
                        co_await io.sleep_for(1h); // stands in for an accept loop
                    }
                    catch (const std::system_error& error)
                    {
                        stopped_serving = canceled(error);
                        throw;
                    }
                }
            );
            (void)group.spawn(
                [&]() -> pace::task<void>
                {
                    while (!waiting)
                    {
                        co_await io.yield();
                    }
                    raise_signal(process_signal::terminate);
                }
            );
            auto shutdown = pace::wait_for_signal({process_signal::interrupt, process_signal::terminate});
            waiting = true;
            require(co_await std::move(shutdown) == process_signal::terminate, "shutdown signal");
            group.cancel();
        }
    );
}
} // namespace

TEST_CASE("a suspended signal wait resumes when the signal is raised")
{
    pace::run(suspended_wait_wakes);
}

TEST_CASE("signal deliveries are counted per set and reported in order")
{
    pace::run(deliveries_are_counted);
}

TEST_CASE("every signal set in a context sees each delivery; one wait per set")
{
    pace::run(sets_in_one_context_each_see_a_delivery);
}

TEST_CASE("signal waits are cancelled by their token without losing later deliveries")
{
    pace::run(cancellation_and_stop);
}

TEST_CASE("stopping the context cancels a pending signal wait")
{
    pace::io_context io;
    pace::signal_set set(io, {process_signal::user2}); // created before run(), on this thread
    bool waiting = false;
    bool cancelled = false;
    io.spawn(
        [](pace::signal_set& signals, bool& registered, bool& result) -> pace::task<void>
        {
            try
            {
                registered = true;
                (void)co_await signals.wait();
            }
            catch (const std::system_error& error)
            {
                result = canceled(error);
            }
        }(set, waiting, cancelled)
    );
    io.spawn(
        [](pace::io_context& context, const bool& registered) -> pace::task<void>
        {
            while (!registered)
            {
                co_await context.yield();
            }
            context.stop();
        }(io, waiting)
    );
    io.run();
    require(cancelled, "stop cancels the signal wait");
}

TEST_CASE("signal sets in different contexts and threads each see a delivery")
{
    constexpr std::size_t count = 3;
    std::vector<std::unique_ptr<pace::io_context>> contexts;
    std::vector<std::unique_ptr<pace::signal_set>> sets;
    std::atomic<std::size_t> registered{0};
    std::atomic<std::size_t> received{0};
    for (std::size_t i = 0; i < count; ++i)
    {
        contexts.push_back(std::make_unique<pace::io_context>());
        sets.push_back(
            std::make_unique<pace::signal_set>(
                *contexts.back(),
                std::initializer_list<process_signal>{process_signal::user1}
            )
        );
        contexts.back()->spawn(
            [](pace::signal_set& set,
               std::atomic<std::size_t>& ready,
               std::atomic<std::size_t>& done) -> pace::task<void>
            {
                ready.fetch_add(1);
                if (co_await set.wait() == process_signal::user1)
                {
                    done.fetch_add(1);
                }
                set.context().stop();
            }(*sets.back(), registered, received)
        );
    }
    std::vector<std::thread> threads;
    for (auto& context : contexts)
    {
        threads.emplace_back([&io = *context] { io.run(); });
    }
    // Every wait has at least started; one that has not suspended yet still sees the counted delivery.
    while (registered.load() != count)
    {
        std::this_thread::yield();
    }
    raise_signal(process_signal::user1);
    for (auto& thread : threads)
    {
        thread.join();
    }
    require(received.load() == count, "each context saw the delivery");
}

TEST_CASE("deliver_signal from another thread reaches a waiting set")
{
    pace::run(
        []() -> pace::task<void>
        {
            auto& io = pace::this_context();
            pace::signal_set set(io, {process_signal::hangup});
            std::atomic<bool> waiting{false};
            std::thread sender(
                [&waiting]
                {
                    while (!waiting.load())
                    {
                        std::this_thread::yield();
                    }
                    pace::deliver_signal(process_signal::hangup);
                }
            );
            try
            {
                waiting.store(true);
                require(co_await set.wait() == process_signal::hangup, "delivered hangup");
            }
            catch (...)
            {
                sender.join();
                throw;
            }
            sender.join();
        }
    );
}

TEST_CASE("SIGTERM ends a service through the two-line shutdown pattern")
{
    bool stopped_serving = false;
    pace::run([&] { return graceful_shutdown(stopped_serving); });
    require(stopped_serving, "the serving task was cancelled");
}

#ifndef _WIN32
namespace
{
std::atomic<int> custom_handler_calls{0};

void custom_handler(int)
{
    custom_handler_calls.fetch_add(1);
}
} // namespace

TEST_CASE("the previous disposition is restored when the last set goes")
{
    struct sigaction original{};
    require(::sigaction(SIGUSR2, nullptr, &original) == 0, "read the disposition");
    struct sigaction custom{};
    custom.sa_handler = &custom_handler;
    sigemptyset(&custom.sa_mask);
    require(::sigaction(SIGUSR2, &custom, nullptr) == 0, "install a custom handler");
    const auto current = []
    {
        struct sigaction now{};
        require(::sigaction(SIGUSR2, nullptr, &now) == 0, "read the disposition");
        return now.sa_handler;
    };
    {
        pace::io_context io;
        auto first =
            std::make_unique<pace::signal_set>(io, std::initializer_list<process_signal>{process_signal::user2});
        require(current() != &custom_handler, "PACE owns the disposition while subscribed");
        {
            pace::signal_set second(io, {process_signal::user2});
            raise_signal(process_signal::user2);
        }
        require(current() != &custom_handler, "one set still subscribes");
        first.reset();
        require(current() == &custom_handler, "restored after the last set");
        require(custom_handler_calls.load() == 0, "the replaced handler was not called while PACE owned the signal");
    }
    raise_signal(process_signal::user2);
    require(custom_handler_calls.load() == 1, "the restored handler runs again");
    require(::sigaction(SIGUSR2, &original, nullptr) == 0, "restore the original disposition");
}
#endif

TEST_CASE("signal sets reject empty lists and unknown values")
{
    pace::io_context io;
    bool empty = false;
    try
    {
        pace::signal_set set(io, {});
    }
    catch (const std::invalid_argument&)
    {
        empty = true;
    }
    require(empty, "empty list rejected");
    bool unknown = false;
    try
    {
        pace::signal_set set(io, {static_cast<process_signal>(42)});
    }
    catch (const std::invalid_argument&)
    {
        unknown = true;
    }
    require(unknown, "unknown value rejected");
    require(std::string_view(pace::signal_name(process_signal::terminate)) == "SIGTERM", "signal_name");
}
