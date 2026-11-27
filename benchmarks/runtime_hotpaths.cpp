#include <pace/net/udp.hpp>
#include "runtime/poller.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>
#ifndef _WIN32
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace
{
using clock_type = std::chrono::steady_clock;
using samples = std::vector<std::int64_t>;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::int64_t elapsed(clock_type::time_point start)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now() - start).count();
}

samples sparse_cancel(std::size_t count, std::size_t iterations)
{
    pace::io_context io;
    std::vector<pace::cancellation_source> sources(count);
    std::vector<pace::io_context::cancellation_handle> background;
    background.reserve(count);
    pace::io_context::cancellation_handle active;
    samples times;
    times.reserve(iterations);
    clock_type::time_point start;
    std::function<void()> next;
    next = [&]
    {
        pace::cancellation_source source;
        active = io.on_cancel(
            source.token(),
            [&](std::error_code error)
            {
                const auto time = elapsed(start);
                require(error == std::errc::operation_canceled, "wrong cancellation result");
                times.push_back(time);
                if (times.size() < iterations)
                {
                    io.post(next);
                }
                else
                {
                    background.clear();
                    io.stop();
                }
            }
        );
        start = clock_type::now();
        require(source.request_cancellation(), "duplicate cancellation request");
    };
    io.post(
        [&]
        {
            for (const auto& source : sources)
            {
                background.push_back(io.on_cancel(
                    source.token(),
                    [](std::error_code) { throw std::runtime_error("unrelated token was canceled"); }
                ));
            }
            next();
        }
    );
    io.run();
    return times;
}

samples ready_batch(std::size_t count, std::size_t iterations)
{
    pace::io_context io;
    pace::udp::socket sender(io);
    std::vector<pace::udp::socket> receivers;
    std::vector<sockaddr_in> addresses;
    receivers.reserve(count);
    addresses.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        auto& receiver = receivers.emplace_back(io, pace::udp::endpoint{});
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(receiver.local_endpoint().port);
        addresses.push_back(address);
    }
    pace::poller poller(false, io.backend());
    std::array<pace::poll_event, 256> events;
    std::vector<bool> seen(count);
    samples times;
    times.reserve(iterations);
    for (std::size_t trial = 0; trial < iterations; ++trial)
    {
        std::fill(seen.begin(), seen.end(), false);
        for (std::size_t i = 0; i < count; ++i)
        {
            poller.update(receivers[i].native_handle(), true, false, i + 1);
            require(
                ::sendto(
                    sender.native_handle(),
                    "x",
                    1,
                    0,
                    reinterpret_cast<const sockaddr*>(&addresses[i]),
                    sizeof(addresses[i])
                ) == 1,
                "datagram send failed"
            );
        }
        std::size_t delivered = 0;
        std::int64_t poll_ns = 0;
        while (delivered < count)
        {
            const auto start = clock_type::now();
            const auto batch = poller.wait(events, 1000);
            poll_ns += elapsed(start);
            require(batch > 0 && batch <= count - delivered, "incomplete readiness batch");
            for (std::size_t i = 0; i < batch; ++i)
            {
                const auto& event = events[i];
                require(
                    event.token && event.token <= count && event.readable && !event.writable && !event.error,
                    "invalid readiness event"
                );
                const auto index = static_cast<std::size_t>(event.token - 1);
                require(!seen[index], "duplicate readiness event");
                seen[index] = true;
                char byte = 0;
                require(
                    ::recv(receivers[index].native_handle(), &byte, 1, 0) == 1 && byte == 'x',
                    "invalid datagram payload"
                );
            }
            delivered += batch;
        }
        times.push_back(poll_ns);
    }
    return times;
}

pace::task<void> count_root(std::size_t& executed)
{
    ++executed;
    co_return;
}

samples inbox_burst(std::size_t count, std::size_t iterations)
{
    samples times;
    times.reserve(iterations);
    for (std::size_t trial = 0; trial < iterations; ++trial)
    {
        pace::io_context io;
        std::size_t executed = 0;
        for (std::size_t i = 0; i < count; ++i)
        {
            io.spawn(count_root(executed));
        }
        clock_type::time_point start;
        auto timer = io.call_after(
            std::chrono::milliseconds(0),
            [&]
            {
                times.push_back(elapsed(start));
                io.stop();
            }
        );
        start = clock_type::now();
        io.run();
        require(executed == count, "accepted submissions were lost");
    }
    return times;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        require(argc == 5, "usage: pace_runtime_hotpaths sparse_cancel|ready_batch|inbox_burst size iterations warmup");
        const std::string_view workload(argv[1]);
        const auto size = std::stoul(argv[2]);
        const auto iterations = std::stoul(argv[3]);
        const auto warmup = std::stoul(argv[4]);
        require(iterations > 0 && iterations <= 10000 && warmup <= 10000 && size <= 65536, "invalid workload size");
        const auto measure = workload == "sparse_cancel" ? sparse_cancel
                             : workload == "ready_batch" ? ready_batch
                             : workload == "inbox_burst" ? inbox_burst
                                                         : nullptr;
        require(measure && (workload != "ready_batch" || (size > 0 && size <= 256)), "invalid workload");
        auto times = measure(size, iterations + warmup);
        require(times.size() == iterations + warmup, "incomplete measurement");
        times.erase(times.begin(), times.begin() + warmup);
        std::sort(times.begin(), times.end());
        std::cout << "{\"workload\":\"" << workload << "\",\"size\":" << size << ",\"iterations\":" << iterations
                  << ",\"p50_ns\":" << times[(times.size() - 1) / 2]
                  << ",\"p99_ns\":" << times[(times.size() * 99 + 99) / 100 - 1] << ",\"validated\":true}\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
