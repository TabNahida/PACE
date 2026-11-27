// Cross-context primitives: channel throughput and latency, on() round trips, snapshot reads.
//
//     pace_cross_context <workload> <iterations> <trials> [capacity]
//
// Workloads (each runs on a fresh pace::runtime; thread start-up and shutdown are outside the timed interval):
//   channel_coroutine  a coroutine on context 1 sends `iterations` values with co_await send() to a channel owned by
//                      context 0, whose coroutine receives them; ns per value, capacity `capacity` (default 1024)
//   channel_thread     the same, with a plain thread calling try_send() and yielding while the channel is full
//   channel_ping_pong  one value bounces between two contexts through two channels; ns per round trip
//   on_round_trip      a coroutine on context 0 awaits on(context 1, trivial task) `iterations` times in sequence
//   snapshot_get       a coroutine reads snapshot::get() `iterations` times on one of 4 contexts; ns per read
// Prints one JSON object with the median and range of ns per operation across trials.
#include <pace/runtime.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
using bench_clock = std::chrono::steady_clock;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

double per_operation(bench_clock::time_point begin, bench_clock::time_point end, std::uint64_t operations)
{
    return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count())
           / static_cast<double>(operations);
}

double channel_coroutine(std::uint64_t iterations, std::size_t capacity)
{
    pace::runtime workers(2);
    pace::channel<std::uint64_t> ch(workers.context(0), capacity);
    bench_clock::time_point begin;
    bench_clock::time_point end;
    std::uint64_t sum = 0;
    auto receiver = [&]() -> pace::task<void>
    {
        for (std::uint64_t i = 0; i < iterations; ++i)
        {
            auto value = co_await ch.receive();
            sum += *value;
        }
        end = bench_clock::now();
        workers.stop();
    };
    auto sender = [&]() -> pace::task<void>
    {
        begin = bench_clock::now();
        for (std::uint64_t i = 0; i < iterations; ++i)
        {
            require(co_await ch.send(i) == pace::send_status::sent, "send failed");
        }
    };
    workers.context(0).spawn(receiver());
    workers.context(1).spawn(sender());
    workers.run();
    require(sum == iterations * (iterations - 1) / 2, "lost values");
    return per_operation(begin, end, iterations);
}

double channel_thread(std::uint64_t iterations, std::size_t capacity)
{
    pace::runtime workers(1);
    pace::channel<std::uint64_t> ch(workers.context(0), capacity);
    bench_clock::time_point begin;
    bench_clock::time_point end;
    std::uint64_t sum = 0;
    std::atomic<bool> started{false};
    auto receiver = [&]() -> pace::task<void>
    {
        started = true;
        for (std::uint64_t i = 0; i < iterations; ++i)
        {
            auto value = co_await ch.receive();
            sum += *value;
        }
        end = bench_clock::now();
        workers.stop();
    };
    workers.context(0).spawn(receiver());
    std::thread producer(
        [&]
        {
            while (!started)
            {
                std::this_thread::yield();
            }
            begin = bench_clock::now();
            for (std::uint64_t i = 0; i < iterations; ++i)
            {
                while (ch.try_send(i) == pace::send_status::full)
                {
                    std::this_thread::yield();
                }
            }
        }
    );
    workers.run();
    producer.join();
    require(sum == iterations * (iterations - 1) / 2, "lost values");
    return per_operation(begin, end, iterations);
}

double channel_ping_pong(std::uint64_t iterations, std::size_t)
{
    pace::runtime workers(2);
    pace::channel<std::uint64_t> ping(workers.context(1), 1);
    pace::channel<std::uint64_t> pong(workers.context(0), 1);
    bench_clock::time_point begin;
    bench_clock::time_point end;
    auto server = [&]() -> pace::task<void>
    {
        while (auto value = co_await ping.receive())
        {
            require(pong.try_send(*value + 1) == pace::send_status::sent, "pong full");
        }
    };
    auto client = [&]() -> pace::task<void>
    {
        begin = bench_clock::now();
        for (std::uint64_t i = 0; i < iterations; ++i)
        {
            require(ping.try_send(i) == pace::send_status::sent, "ping full");
            auto reply = co_await pong.receive();
            require(reply && *reply == i + 1, "wrong reply");
        }
        end = bench_clock::now();
        ping.close();
        workers.stop();
    };
    workers.context(1).spawn(server());
    workers.context(0).spawn(client());
    workers.run();
    return per_operation(begin, end, iterations);
}

pace::task<std::uint64_t> identity(std::uint64_t value)
{
    co_return value;
}

double on_round_trip(std::uint64_t iterations, std::size_t)
{
    pace::runtime workers(2);
    auto& target = workers.context(1);
    bench_clock::time_point begin;
    bench_clock::time_point end;
    std::uint64_t sum = 0;
    auto caller = [&]() -> pace::task<void>
    {
        begin = bench_clock::now();
        for (std::uint64_t i = 0; i < iterations; ++i)
        {
            sum += co_await pace::on(target, identity(i));
        }
        end = bench_clock::now();
        workers.stop();
    };
    workers.context(0).spawn(caller());
    workers.run();
    require(sum == iterations * (iterations - 1) / 2, "lost results");
    return per_operation(begin, end, iterations);
}

double snapshot_get(std::uint64_t iterations, std::size_t)
{
    pace::runtime workers(4);
    pace::snapshot<std::uint64_t> value(workers, std::make_shared<const std::uint64_t>(1));
    bench_clock::time_point begin;
    bench_clock::time_point end;
    std::uint64_t sum = 0;
    auto reader = [&]() -> pace::task<void>
    {
        begin = bench_clock::now();
        for (std::uint64_t i = 0; i < iterations; ++i)
        {
            sum += *value.get();
        }
        end = bench_clock::now();
        workers.stop();
        co_return;
    };
    // The last context: get() scans the context list, so this is its slowest position.
    workers.context(3).spawn(reader());
    workers.run();
    require(sum == iterations, "wrong reads");
    return per_operation(begin, end, iterations);
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        require(
            argc == 4 || argc == 5,
            "usage: pace_cross_context channel_coroutine|channel_thread|channel_ping_pong|on_round_trip|snapshot_get "
            "iterations trials [capacity]"
        );
        const std::string_view workload(argv[1]);
        const auto iterations = std::stoull(argv[2]);
        const auto trials = std::stoull(argv[3]);
        const std::size_t capacity = argc == 5 ? std::stoull(argv[4]) : 1024;
        require(iterations > 0 && trials > 0 && trials <= 1000 && capacity > 0, "invalid configuration");
        const auto measure = workload == "channel_coroutine"   ? channel_coroutine
                             : workload == "channel_thread"    ? channel_thread
                             : workload == "channel_ping_pong" ? channel_ping_pong
                             : workload == "on_round_trip"     ? on_round_trip
                             : workload == "snapshot_get"      ? snapshot_get
                                                               : nullptr;
        require(measure != nullptr, "unknown workload");
        measure(std::min<std::uint64_t>(iterations, 10000), capacity); // Warm up allocators and page tables.
        std::vector<double> samples;
        samples.reserve(trials);
        for (std::uint64_t i = 0; i < trials; ++i)
        {
            samples.push_back(measure(iterations, capacity));
        }
        std::sort(samples.begin(), samples.end());
        std::cout << R"({"benchmark":"cross_context","workload":")" << workload << R"(","iterations":)" << iterations
                  << R"(,"trials":)" << trials << R"(,"capacity":)" << capacity << R"(,"median_ns_per_operation":)"
                  << samples[samples.size() / 2] << R"(,"min_ns_per_operation":)" << samples.front()
                  << R"(,"max_ns_per_operation":)" << samples.back() << R"(,"validated":true})" << '\n';
    }
    catch (const std::exception& error)
    {
        std::cerr << "cross-context benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
