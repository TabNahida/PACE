#include <pace/runtime.hpp>
#include "support/pace_test.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using pace::test::require;

namespace
{
template <class Function>
bool throws_code(Function&& function, std::errc code)
{
    try
    {
        function();
    }
    catch (const std::system_error& error)
    {
        return error.code() == code;
    }
    return false;
}

template <class Exception, class Function>
bool throws_type(Function&& function)
{
    try
    {
        function();
    }
    catch (const Exception&)
    {
        return true;
    }
    return false;
}

// The test repeats a stress round PACE_STRESS_ROUNDS times (default 1); run the binary in a loop for more.
int stress_rounds()
{
    const char* value = std::getenv("PACE_STRESS_ROUNDS");
    return value ? std::max(1, static_cast<int>(std::strtol(value, nullptr, 10))) : 1;
}

pace::task<void> receive_into(pace::channel<int>& ch, std::optional<int>& out, std::error_code& error)
{
    try
    {
        out = co_await ch.receive();
    }
    catch (const std::system_error& failure)
    {
        error = failure.code();
    }
}

pace::task<void> send_into(pace::channel<int>& ch, int value, pace::send_status& out, std::error_code& error)
{
    try
    {
        out = co_await ch.send(value);
    }
    catch (const std::system_error& failure)
    {
        error = failure.code();
    }
}

// Awaits `work` and records the code of a std::system_error it throws.
pace::task<void> record_failure(pace::task<void> work, std::error_code& out)
{
    try
    {
        co_await std::move(work);
    }
    catch (const std::system_error& failure)
    {
        out = failure.code();
    }
}

// Lets every coroutine that is ready on this context run until it suspends.
pace::task<void> settle()
{
    for (int i = 0; i < 4; ++i)
    {
        co_await pace::yield();
    }
}
} // namespace

TEST_CASE("channel queues in order, rejects or drops when full, and drains after close")
{
    pace::run(
        []() -> pace::task<void>
        {
            auto& io = pace::this_context();
            pace::channel<int> ch(io, 2);
            require(ch.capacity() == 2 && ch.size() == 0 && !ch.closed(), "fresh channel");
            require(ch.try_send(1) == pace::send_status::sent, "first send");
            require(ch.try_send(2) == pace::send_status::sent, "second send");
            require(ch.try_send(3) == pace::send_status::full, "reject policy refuses a full channel");
            require(ch.size() == 2, "size counts queued values");
            auto first = co_await ch.receive();
            require(first && *first == 1, "receive takes the oldest value");
            require(ch.try_receive() == 2, "try_receive takes the next value");
            require(!ch.try_receive(), "try_receive on an empty channel");

            pace::channel<int> lossy(io, 2, pace::overflow_policy::drop_oldest);
            require(lossy.try_send(1) == pace::send_status::sent, "lossy first");
            require(lossy.try_send(2) == pace::send_status::sent, "lossy second");
            require(lossy.try_send(3) == pace::send_status::dropped_oldest, "drop_oldest makes room");
            require(co_await lossy.send(4) == pace::send_status::dropped_oldest, "awaited send never waits");
            require(lossy.try_receive() == 3 && lossy.try_receive() == 4, "the oldest values were dropped");

            require(ch.try_send(5) == pace::send_status::sent, "send before close");
            ch.close();
            ch.close();
            require(ch.closed(), "closed");
            require(ch.try_send(6) == pace::send_status::closed, "try_send after close");
            require(co_await ch.send(7) == pace::send_status::closed, "send after close");
            auto drained = co_await ch.receive();
            require(drained && *drained == 5, "queued values survive close");
            require(!(co_await ch.receive()), "closed and empty ends the channel");

            pace::channel<std::unique_ptr<int>> owned(io, 1);
            auto item = std::make_unique<int>(1);
            require(owned.try_send(std::move(item)) == pace::send_status::sent && !item, "moved on success");
            item = std::make_unique<int>(2);
            require(owned.try_send(std::move(item)) == pace::send_status::full && item, "kept on failure");

            require(
                throws_type<std::invalid_argument>([&] { pace::channel<int> zero(io, 0); }),
                "zero capacity is rejected"
            );
            bool off_owner = false;
            std::thread([&] { off_owner = throws_type<std::logic_error>([&] { (void)ch.try_receive(); }); }).join();
            require(off_owner, "try_receive off the owner");
            require(throws_type<std::logic_error>([&] { (void)ch.blocking_send(1); }), "blocking_send on a context");

            pace::channel<int> copy = lossy;
            require(copy.try_send(8) == pace::send_status::sent && lossy.try_receive() == 8, "copies share");
        }
    );
}

TEST_CASE("channel suspends and resumes senders and receivers on one context")
{
    pace::run(
        []() -> pace::task<void>
        {
            auto& io = pace::this_context();
            pace::channel<int> ch(io, 1);
            co_await pace::with_task_group(
                [&](pace::task_group& group) -> pace::task<void>
                {
                    std::optional<int> received;
                    std::error_code receive_error;
                    auto receiving = group.spawn(receive_into(ch, received, receive_error));
                    co_await settle();
                    require(!received, "receive waits for a value");
                    require(co_await ch.send(1) == pace::send_status::sent, "send hands to the receiver");
                    co_await std::move(receiving);
                    require(received == 1 && !receive_error, "receiver got the value");

                    require(ch.try_send(2) == pace::send_status::sent, "fill");
                    pace::send_status statuses[2]{};
                    std::error_code errors[2];
                    auto a = group.spawn(send_into(ch, 3, statuses[0], errors[0]));
                    auto b = group.spawn(send_into(ch, 4, statuses[1], errors[1]));
                    co_await settle();
                    require(ch.size() == 1, "full channel holds its capacity");
                    require(ch.try_receive() == 2, "receive makes room");
                    require(ch.try_receive() == 3, "the first waiting sender refilled the slot");
                    require(ch.try_receive() == 4, "then the second");
                    co_await std::move(a);
                    co_await std::move(b);
                    require(statuses[0] == pace::send_status::sent && statuses[1] == pace::send_status::sent, "sent");

                    require(ch.try_send(5) == pace::send_status::sent, "fill again");
                    pace::send_status closed_status{};
                    std::error_code closed_error;
                    auto waiting = group.spawn(send_into(ch, 6, closed_status, closed_error));
                    co_await settle();
                    ch.close();
                    co_await std::move(waiting);
                    require(closed_status == pace::send_status::closed && !closed_error, "close ends waiting sends");
                    require(ch.try_receive() == 5 && !ch.try_receive(), "the waiting value was not queued");
                }
            );

            pace::channel<int> empty(io, 1);
            std::optional<int> received;
            std::error_code error;
            co_await pace::with_task_group(
                [&](pace::task_group& group) -> pace::task<void>
                {
                    auto receiving = group.spawn(receive_into(empty, received, error));
                    co_await settle();
                    bool second = false;
                    try
                    {
                        (void)co_await empty.receive();
                    }
                    catch (const std::logic_error&)
                    {
                        second = true;
                    }
                    require(second, "one receive at a time");
                    empty.close();
                    co_await std::move(receiving);
                }
            );
            require(!received && !error, "close ends a pending receive");
        }
    );
}

TEST_CASE("channel cancellation leaves the value unsent and the channel consistent")
{
    pace::run(
        []() -> pace::task<void>
        {
            auto& io = pace::this_context();
            pace::channel<int> ch(io, 1);
            require(ch.try_send(0) == pace::send_status::sent, "fill");
            pace::cancellation_source source;
            pace::send_status status{};
            std::error_code error;
            std::error_code boundary;
            co_await pace::with_task_group(
                [&](pace::task_group& group) -> pace::task<void>
                {
                    auto sending = group.spawn(record_failure(
                        pace::with_cancellation(source.token(), [&] { return send_into(ch, 1, status, error); }),
                        boundary
                    ));
                    co_await settle();
                    source.request_cancellation();
                    co_await std::move(sending);
                }
            );
            require(error == std::errc::operation_canceled, "cancelled send throws");
            require(boundary == std::errc::operation_canceled, "and so does its boundary");
            require(ch.try_receive() == 0 && !ch.try_receive(), "the cancelled value was not queued");
            require(ch.try_send(2) == pace::send_status::sent, "the channel still works");
            require(ch.try_receive() == 2, "and delivers");

            pace::channel<int> silent(io, 1);
            bool timed_out = false;
            try
            {
                (void)co_await pace::with_deadline(
                    pace::deadline::after(1ms),
                    [&]() -> pace::task<std::optional<int>> { co_return co_await silent.receive(); }
                );
            }
            catch (const std::system_error& failure)
            {
                timed_out = failure.code() == std::errc::timed_out;
            }
            require(timed_out, "a receive deadline throws timed_out");
            require(silent.try_send(3) == pace::send_status::sent, "a later send queues normally");
            auto next = co_await silent.receive();
            require(next == 3, "and a later receive gets it");

            pace::cancellation_source cancelled;
            cancelled.request_cancellation();
            bool threw = false;
            try
            {
                (void)co_await pace::with_cancellation(
                    cancelled.token(),
                    [&]() -> pace::task<std::optional<int>> { co_return co_await silent.receive(); }
                );
            }
            catch (const std::system_error& failure)
            {
                threw = failure.code() == std::errc::operation_canceled;
            }
            require(threw, "an already cancelled receive throws");
        }
    );
}

TEST_CASE("channel owner stop cancels the receive and closes the channel")
{
    pace::io_context io;
    pace::channel<int> ch(io, 1);
    std::optional<int> received;
    std::error_code receive_error;
    auto receiver = [&]() -> pace::task<void> { co_await receive_into(ch, received, receive_error); };
    auto stopper = [&]() -> pace::task<void>
    {
        co_await settle();
        io.stop();
        co_return;
    };
    io.spawn(receiver());
    io.spawn(stopper());
    io.run();
    require(receive_error == std::errc::operation_canceled, "a pending receive fails when the owner stops");
    require(ch.closed(), "the owner's shutdown closes the channel");
    require(ch.try_send(1) == pace::send_status::closed, "sends after the shutdown report closed");
    require(ch.blocking_send(1) == pace::send_status::closed, "blocking sends too");

    pace::channel<int> late(io, 1);
    require(late.closed(), "a channel created after the owner shut down starts closed");

    auto owner = std::make_unique<pace::io_context>();
    pace::channel<int> orphan(*owner, 1);
    require(orphan.try_send(1) == pace::send_status::sent, "sends before the owner runs are queued");
    owner.reset();
    require(orphan.closed() && orphan.try_send(2) == pace::send_status::closed, "a destroyed owner closes it");
    require(orphan.size() == 1, "queued values stay until the last handle goes");
}

TEST_CASE("channel senders on other contexts wait for room and resume on their own context")
{
    for (int round = 0; round < stress_rounds(); ++round)
    {
        pace::runtime workers(3);
        auto& owner = workers.context(0);
        pace::channel<int> ch(owner, 1);
        std::atomic<int> finished{0};
        std::atomic<bool> wrong_context{false};
        constexpr int per_sender = 500;
        auto sender = [&](pace::io_context& here, int base) -> pace::task<void>
        {
            for (int i = 0; i < per_sender; ++i)
            {
                require(co_await ch.send(base + i) == pace::send_status::sent, "sent under backpressure");
                if (pace::io_context::current() != &here)
                {
                    wrong_context = true;
                }
            }
            ++finished;
        };
        auto receiver = [&]() -> pace::task<void>
        {
            std::vector<int> next{0, 100000};
            for (int i = 0; i < 2 * per_sender; ++i)
            {
                auto value = co_await ch.receive();
                require(value.has_value(), "values until both senders finish");
                auto& expected = next[*value >= 100000 ? 1 : 0];
                require(*value == expected, "each sender's values arrive in order");
                ++expected;
                require(ch.size() <= ch.capacity(), "bounded");
            }
            workers.stop();
        };
        owner.spawn(receiver());
        workers.context(1).spawn(sender(workers.context(1), 0));
        workers.context(2).spawn(sender(workers.context(2), 100000));
        workers.run();
        require(finished == 2 && !wrong_context, "both senders finished on their own contexts");
    }
}

TEST_CASE("channel stress: N workers and threads, cancellation, no lost wake-ups, bounded memory")
{
    for (int round = 0; round < stress_rounds(); ++round)
    {
        constexpr std::size_t worker_count = 4;
        constexpr int coroutines_per_worker = 2;
        constexpr int per_producer = 1000;
        constexpr std::size_t capacity = 8;
        constexpr int thread_producers = 2;
        constexpr int producers = static_cast<int>(worker_count) * coroutines_per_worker + thread_producers;
        pace::runtime workers(worker_count + 1);
        auto& owner = workers.context(0);

        struct item
        {
            int producer;
            int sequence;
        };

        pace::channel<item> ch(owner, capacity);
        std::atomic<std::int64_t> delivered{0};
        std::atomic<int> remaining{producers};
        std::atomic<std::int64_t> received_sum{0};
        auto producer_done = [&]
        {
            if (--remaining == 0)
            {
                ch.close();
            }
        };
        auto send_one = [&](item value, bool& sent) -> pace::task<void>
        { sent = co_await ch.send(value) == pace::send_status::sent; };
        auto producer = [&](int id) -> pace::task<void>
        {
            for (int i = 0; i < per_producer; ++i)
            {
                bool sent = false;
                if (i % 5 == 0)
                {
                    // A short deadline cancels some of these sends while they wait; the value counts only if the
                    // send itself reported it sent.
                    try
                    {
                        co_await pace::with_deadline(pace::deadline::after(20us), send_one({id, i}, sent));
                    }
                    catch (const std::system_error& failure)
                    {
                        require(failure.code() == std::errc::timed_out, "only the deadline cancels");
                    }
                }
                else
                {
                    co_await send_one({id, i}, sent);
                    require(sent, "an uncancelled send is delivered");
                }
                if (sent)
                {
                    delivered += i;
                }
            }
            producer_done();
        };
        auto receiver = [&]() -> pace::task<void>
        {
            std::vector<int> last(producers, -1);
            std::int64_t sum = 0;
            while (auto value = co_await ch.receive())
            {
                require(value->sequence > last[value->producer], "per-producer order");
                last[value->producer] = value->sequence;
                sum += value->sequence;
                require(ch.size() <= capacity, "bounded queue");
            }
            received_sum = sum;
            workers.stop();
        };
        owner.spawn(receiver());
        for (std::size_t w = 0; w < worker_count; ++w)
        {
            for (int c = 0; c < coroutines_per_worker; ++c)
            {
                workers.context(w + 1).spawn(producer(static_cast<int>(w) * coroutines_per_worker + c));
            }
        }
        std::vector<std::thread> threads;
        for (int t = 0; t < thread_producers; ++t)
        {
            const int id = static_cast<int>(worker_count) * coroutines_per_worker + t;
            threads.emplace_back(
                [&, id, t]
                {
                    for (int i = 0; i < per_producer; ++i)
                    {
                        pace::send_status status;
                        if (t == 0)
                        {
                            status = ch.blocking_send({id, i});
                        }
                        else
                        {
                            while ((status = ch.try_send({id, i})) == pace::send_status::full)
                            {
                                std::this_thread::yield();
                            }
                        }
                        if (status != pace::send_status::sent)
                        {
                            break;
                        }
                        delivered += i;
                    }
                    producer_done();
                }
            );
        }
        workers.run();
        for (auto& thread : threads)
        {
            thread.join();
        }
        require(received_sum == delivered, "every delivered value was received exactly once");
    }
}

TEST_CASE("blocking_send waits for room and honours its token")
{
    pace::io_context io;
    pace::channel<int> ch(io, 1);
    require(ch.blocking_send(1) == pace::send_status::sent, "room available");
    pace::cancellation_source source;
    std::atomic<bool> cancelled{false};
    std::thread blocked(
        [&]
        { cancelled = throws_code([&] { (void)ch.blocking_send(2, source.token()); }, std::errc::operation_canceled); }
    );
    // The value is either waiting or about to; cancellation ends it either way.
    source.request_cancellation();
    blocked.join();
    require(cancelled, "cancellation ends a blocked send");
    require(
        throws_code(
            [&] { (void)ch.blocking_send(3, pace::cancellation_source(pace::deadline::after(1ms)).token()); },
            std::errc::timed_out
        ),
        "a deadline ends a blocked send"
    );

    std::atomic<bool> sent{false};
    std::thread waiter([&] { sent = ch.blocking_send(4) == pace::send_status::sent; });
    auto receive_two = [&]() -> pace::task<void>
    {
        auto first = co_await ch.receive();
        require(first == 1, "first value");
        auto second = co_await ch.receive();
        require(second == 4, "the blocked thread's value");
        io.stop();
    };
    io.spawn(receive_two());
    io.run();
    waiter.join();
    require(sent, "the blocked thread was released with sent");
}

TEST_CASE("a sender's own context stop cancels its waiting send")
{
    pace::runtime workers(2);
    pace::channel<int> ch(workers.context(0), 1);
    require(ch.try_send(0) == pace::send_status::sent, "fill");
    std::error_code error;
    pace::send_status status{};
    auto sender = [&]() -> pace::task<void>
    {
        co_await send_into(ch, 1, status, error);
        workers.stop();
    };
    auto stopper = [&]() -> pace::task<void>
    {
        co_await settle();
        pace::this_context().stop();
    };
    workers.context(1).spawn(sender());
    workers.context(1).spawn(stopper());
    workers.run();
    require(error == std::errc::operation_canceled, "the stopped sender's send was cancelled");
}

TEST_CASE("snapshot publishes to every context without blocking readers")
{
    for (int round = 0; round < stress_rounds(); ++round)
    {
        struct config
        {
            int version;
            std::atomic<int>& live;

            config(int number, std::atomic<int>& counter)
                : version(number)
                , live(counter)
            {
                ++live;
            }

            config(const config&) = delete;
            config& operator=(const config&) = delete;

            ~config()
            {
                --live;
            }
        };

        std::atomic<int> live{0};
        constexpr int final_version = 2000;
        constexpr std::size_t worker_count = 4;
        {
            pace::runtime workers(worker_count);
            pace::snapshot<config> current(workers, std::make_shared<const config>(0, live));
            require(current.load()->version == 0, "initial value");
            require(throws_type<std::logic_error>([&] { (void)current.get(); }), "get off the contexts");
            std::atomic<std::size_t> readers_done{0};
            std::atomic<std::uint64_t> reads{0};
            auto reader = [&]() -> pace::task<void>
            {
                int last = -1;
                std::uint64_t count = 0;
                while (last != final_version)
                {
                    const auto& value = current.get();
                    require(value && value->version >= last, "a context sees versions in order");
                    last = value->version;
                    ++count;
                    co_await pace::yield();
                }
                reads += count;
                if (++readers_done == worker_count)
                {
                    workers.stop();
                }
            };
            for (std::size_t i = 0; i < worker_count; ++i)
            {
                workers.context(i).spawn(reader());
            }
            std::thread publisher(
                [&]
                {
                    for (int v = 1; v <= final_version; ++v)
                    {
                        current.publish(std::make_shared<const config>(v, live));
                    }
                }
            );
            workers.run();
            publisher.join();
            require(current.load()->version == final_version, "load sees the last publish");
            require(reads >= worker_count, "readers ran");
        }
        require(live == 0, "every published value was released");
    }
}

TEST_CASE("snapshot publish on a context is visible there at once")
{
    pace::runtime workers(2);
    auto& first = workers.context(0);
    auto& second = workers.context(1);
    pace::snapshot<int> value({&first, &second}, std::make_shared<const int>(1));
    require(
        throws_type<std::invalid_argument>([] { pace::snapshot<int> none(std::vector<pace::io_context*>{}); }),
        "empty list"
    );
    require(
        throws_type<std::invalid_argument>([&] { pace::snapshot<int> twice({&first, &first}); }),
        "repeated context"
    );
    auto writer = [&]() -> pace::task<void>
    {
        require(*value.get() == 1, "initial");
        value.publish(std::make_shared<const int>(2));
        require(*value.get() == 2, "the publisher's context sees its write immediately");
        auto seen = co_await pace::on(
            second,
            [&]() -> pace::task<int>
            {
                // The refresh was posted before this work, so it ran first.
                co_return *value.get();
            }
        );
        require(seen == 2, "the other context refreshed on its next turn");
        workers.stop();
    };
    first.spawn(writer());
    workers.run();
}

namespace
{
pace::task<int> doubled(int value)
{
    co_return 2 * value;
}
} // namespace

TEST_CASE("on runs work on the target and resumes the caller on its own context")
{
    pace::runtime workers(2);
    auto& home = workers.context(0);
    auto& away = workers.context(1);
    auto caller = [&]() -> pace::task<void>
    {
        auto value = co_await pace::on(
            away,
            [&]() -> pace::task<int>
            {
                require(pace::io_context::current() == &away, "work runs on the target");
                co_return 42;
            }
        );
        require(value == 42 && pace::io_context::current() == &home, "result on the caller's context");
        require(co_await pace::on(away, doubled(21)) == 42, "the task overload");
        auto with_context =
            co_await pace::on(away, [&](pace::io_context& io) -> pace::task<bool> { co_return &io == &away; });
        require(with_context, "a factory taking io_context& gets the target");
        bool rethrown = false;
        try
        {
            co_await pace::on(away, []() -> pace::task<void> { throw std::runtime_error("work failed"); });
        }
        catch (const std::runtime_error& error)
        {
            rethrown = std::string(error.what()) == "work failed" && pace::io_context::current() == &home;
        }
        require(rethrown, "exceptions propagate to the caller's context");
        auto inline_value = co_await pace::on(
            home,
            [&]() -> pace::task<int>
            {
                require(pace::io_context::current() == &home, "same context runs inline");
                co_return 7;
            }
        );
        require(inline_value == 7, "same-context result");
        auto nested = co_await pace::on(
            away,
            [&]() -> pace::task<int>
            {
                co_return co_await pace::on(home, []() -> pace::task<int> { co_return 5; });
            }
        );
        require(nested == 5, "on() nests across contexts");
        workers.stop();
    };
    home.spawn(caller());
    workers.run();
}

TEST_CASE("on propagates the caller's cancellation and deadline to the work")
{
    pace::runtime workers(2);
    auto& home = workers.context(0);
    auto& away = workers.context(1);
    std::atomic<bool> started{false};
    std::atomic<int> work_saw{0};
    auto long_work = [&]() -> pace::task<void>
    {
        started = true;
        try
        {
            co_await pace::sleep_for(1h);
        }
        catch (const std::system_error& error)
        {
            work_saw = error.code() == std::errc::operation_canceled ? 1 : error.code() == std::errc::timed_out ? 2 : 3;
            throw;
        }
    };
    auto caller = [&]() -> pace::task<void>
    {
        pace::cancellation_source source;
        std::error_code outcome;
        co_await pace::with_task_group(
            [&](pace::task_group& group) -> pace::task<void>
            {
                auto call = group.spawn(record_failure(
                    pace::with_cancellation(source.token(), [&] { return pace::on(away, long_work); }),
                    outcome
                ));
                while (!started)
                {
                    co_await pace::yield();
                }
                source.request_cancellation();
                co_await std::move(call);
            }
        );
        require(outcome == std::errc::operation_canceled && work_saw == 1, "cancellation reaches the work");

        started = false;
        bool timed_out = false;
        try
        {
            co_await pace::with_deadline(pace::deadline::after(2ms), [&] { return pace::on(away, long_work); });
        }
        catch (const std::system_error& error)
        {
            timed_out = error.code() == std::errc::timed_out;
        }
        require(timed_out && work_saw == 2, "the deadline reaches the work as timed_out");

        away.stop();
        bool refused = false;
        try
        {
            co_await pace::on(away, []() -> pace::task<void> { co_return; });
        }
        catch (const std::system_error& error)
        {
            refused = error.code() == std::errc::operation_canceled;
        }
        require(refused, "a stopped target refuses the work");
        workers.stop();
    };
    home.spawn(caller());
    workers.run();
}

TEST_CASE("on fails with operation_canceled when the target is destroyed first")
{
    pace::run(
        []() -> pace::task<void>
        {
            auto target = std::make_unique<pace::io_context>();
            std::error_code outcome;
            int live = 0;
            struct probe
            {
                int& count;

                explicit probe(int& counter)
                    : count(counter)
                {
                    ++count;
                }

                probe(const probe& other)
                    : count(other.count)
                {
                    ++count;
                }

                probe(probe&& other) noexcept
                    : count(other.count)
                {
                    ++count;
                }

                probe& operator=(const probe&) = delete;
                probe& operator=(probe&&) = delete;

                ~probe()
                {
                    --count;
                }
            };
            co_await pace::with_task_group(
                [&](pace::task_group& group) -> pace::task<void>
                {
                    auto call = group.spawn(
                        [&]() -> pace::task<void>
                        {
                            try
                            {
                                co_await pace::on(
                                    *target,
                                    [witness = probe(live)]() -> pace::task<void> { co_return; }
                                );
                            }
                            catch (const std::system_error& error)
                            {
                                outcome = error.code();
                            }
                        }
                    );
                    co_await settle();
                    target.reset(); // Never ran: its inbox, with the work, is destroyed.
                    co_await std::move(call);
                }
            );
            require(outcome == std::errc::operation_canceled, "the caller resumes with operation_canceled");
            require(live == 0, "the factory was destroyed");
        }
    );
}

TEST_CASE("on: the caller's context stop cancels the work")
{
    pace::runtime workers(2);
    auto& home = workers.context(0);
    auto& away = workers.context(1);
    std::atomic<int> work_saw{0};
    std::atomic<int> caller_saw{0};
    auto work = [&]() -> pace::task<void>
    {
        home.stop();
        try
        {
            co_await pace::sleep_for(1h);
        }
        catch (const std::system_error& error)
        {
            work_saw = error.code() == std::errc::operation_canceled ? 1 : 2;
            workers.stop();
            throw;
        }
    };
    auto caller = [&]() -> pace::task<void>
    {
        try
        {
            co_await pace::on(away, work);
        }
        catch (const std::system_error& error)
        {
            caller_saw = error.code() == std::errc::operation_canceled ? 1 : 2;
        }
    };
    home.spawn(caller());
    workers.run();
    require(work_saw == 1, "the caller's stop cancelled the work");
    // The caller resumes if its context is still draining when the work finishes; otherwise its frame is destroyed.
    require(caller_saw != 2, "a resumed caller sees operation_canceled");
}

TEST_CASE("on stress: concurrent round trips between all contexts")
{
    for (int round = 0; round < stress_rounds(); ++round)
    {
        constexpr std::size_t worker_count = 4;
        constexpr int calls = 300;
        pace::runtime workers(worker_count);
        std::atomic<std::size_t> finished{0};
        auto caller = [&](std::size_t index) -> pace::task<void>
        {
            auto& here = workers.context(index);
            std::int64_t sum = 0;
            for (int i = 0; i < calls; ++i)
            {
                auto& target = workers.context((index + 1 + static_cast<std::size_t>(i)) % worker_count);
                sum += co_await pace::on(
                    target,
                    [&target, i]() -> pace::task<int>
                    {
                        require(pace::io_context::current() == &target, "work on its target");
                        co_return i;
                    }
                );
                require(pace::io_context::current() == &here, "resumed at home");
            }
            require(sum == static_cast<std::int64_t>(calls) * (calls - 1) / 2, "every result arrived");
            if (++finished == worker_count)
            {
                workers.stop();
            }
        };
        for (std::size_t i = 0; i < worker_count; ++i)
        {
            workers.context(i).spawn(caller(i));
        }
        workers.run();
    }
}

TEST_CASE("runtime::shard_for maps keys to fixed contexts")
{
    pace::runtime workers(4);
    std::set<std::size_t> used;
    for (int key = 0; key < 1000; ++key)
    {
        const auto index = workers.shard_index(key);
        require(index < workers.size(), "in range");
        require(workers.shard_index(key) == index, "stable");
        require(&workers.shard_for(key) == &workers.context(index), "shard_for is context(shard_index)");
        used.insert(index);
    }
    require(used.size() == workers.size(), "keys spread over every context");
    require(workers.shard_index(std::string("user-42")) == workers.shard_index(std::string("user-42")), "strings");
    std::set<std::size_t> aligned;
    for (std::uintptr_t key = 0; key < 64; ++key)
    {
        aligned.insert(workers.shard_index(key * 64));
    }
    require(aligned.size() == workers.size(), "strided keys spread too");
}
