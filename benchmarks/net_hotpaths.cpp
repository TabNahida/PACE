#include <pace/net/relay.hpp>
#include <pace/net/udp.hpp>
#include "net/socket_address.hpp"
#include "runtime/poller.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace
{
using clock_type = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr std::size_t warmup = 32;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

double poll_time(pace::io_context& io, std::size_t iterations, std::size_t batch)
{
    pace::poller poller(false, io.backend());
    auto sender = pace::udp::open({.context = &io});
    std::vector<pace::udp::socket> sockets;
    std::vector<pace::net::socket_address> targets;
    constexpr std::size_t registrations = 192;
    for (std::size_t i = 0; i < registrations; ++i)
    {
        auto& socket = sockets.emplace_back(io, pace::udp::endpoint{"127.0.0.1", 0});
        if (i < batch)
        {
            targets.push_back(pace::net::parse_socket_address("127.0.0.1", socket.local_endpoint().port));
        }
        poller.update(socket.native_handle(), true, false, i + 1);
    }

    std::array<pace::poll_event, 256> events;
    std::vector<bool> seen(batch);
    std::vector<clock_type::duration> samples;
    samples.reserve(iterations);
    for (std::size_t iteration = 0; iteration < warmup + iterations; ++iteration)
    {
        for (auto& target : targets)
        {
            require(::sendto(sender.native_handle(), "x", 1, 0, target.data(), target.length) == 1, "UDP marker send");
        }
        std::size_t delivered = 0;
        clock_type::duration elapsed{};
        std::fill(seen.begin(), seen.end(), false);
        const auto deadline = clock_type::now() + 5s;
        while (delivered < batch)
        {
            const auto started = clock_type::now();
            require(started < deadline, "poll delivery timed out");
            const auto count = poller.wait(events, 100);
            elapsed += clock_type::now() - started;
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto& event = events[i];
                require(event.token >= 1 && event.token <= batch && event.readable && !event.error, "bad poll event");
                const auto index = static_cast<std::size_t>(event.token - 1);
                require(!seen[index], "duplicate poll event");
                seen[index] = true;
                char byte = 0;
                const auto fd = sockets[index].native_handle();
                require(::recv(fd, &byte, 1, 0) == 1 && byte == 'x', "bad UDP marker");
                poller.update(fd, true, false, event.token);
                ++delivered;
            }
        }
        if (iteration >= warmup)
        {
            samples.push_back(elapsed);
        }
    }
    const auto median = samples.begin() + samples.size() / 2;
    std::nth_element(samples.begin(), median, samples.end());
    return std::chrono::duration<double, std::nano>(*median).count() / batch;
}

pace::task<void> reply(std::shared_ptr<pace::udp::socket> peer, std::size_t count)
{
    auto other = pace::udp::bind(peer->context(), {"127.0.0.1", 0});
    std::array<char, 16> buffer;
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto request = co_await peer->receive_from(pace::as_writable_bytes(buffer));
        require(request.size == 1 && buffer[0] == 'x', "bad UDP request");
        for (unsigned j = 0; j < 8; ++j)
        {
            co_await other.send_to(pace::as_bytes("other"), request.sender);
        }
        co_await peer->send_to(pace::as_bytes("reply"), request.sender);
    }
}

pace::task<double> udp_time(pace::io_context& io, std::size_t iterations)
{
    auto peer = std::make_shared<pace::udp::socket>(io, pace::udp::endpoint{"127.0.0.1", 0});
    const auto target = peer->local_endpoint();
    io.spawn(reply(peer, warmup + iterations));
    auto started = clock_type::now();
    for (std::size_t i = 0; i < warmup + iterations; ++i)
    {
        if (i == warmup)
        {
            started = clock_type::now();
        }
        const auto result = co_await pace::udp::async_exchange(target, "x", {.context = &io});
        require(result.data == "reply" && !result.truncated && result.sender.port == target.port, "bad UDP exchange");
    }
    co_return std::chrono::duration<double, std::nano>(clock_type::now() - started).count() / iterations;
}

pace::task<void> manual_pump(pace::tcp::stream& from, pace::tcp::stream& to)
{
    std::array<std::byte, 16 * 1024> buffer{};
    while (const auto count = co_await from.read_some(buffer))
    {
        co_await to.write_all(std::span(buffer).first(count));
    }
    co_await to.shutdown_write();
}

pace::task<double> relay_time(pace::io_context& io, std::string_view workload, std::size_t iterations)
{
    auto listener = pace::tcp::listen(io, {"127.0.0.1", 0});
    auto client = co_await pace::tcp::connect(listener.local_endpoint(), {.context = &io});
    auto left = co_await listener.accept();
    auto right = co_await pace::tcp::connect(listener.local_endpoint(), {.context = &io});
    auto server = co_await listener.accept();
    const std::string payload(16 * 1024, 'x');
    std::vector<std::byte> received(payload.size());
    clock_type::duration elapsed{};
    co_await pace::with_task_group(
        [&](pace::task_group& group) -> pace::task<void>
        {
            if (workload == "relay-manual")
            {
                (void)group.spawn(manual_pump(left, right));
                (void)group.spawn(manual_pump(right, left));
            }
            else
            {
                (void)group.spawn(pace::relay(left, right, {.splice = workload == "relay-splice"}));
            }
            auto started = clock_type::now();
            for (std::size_t i = 0; i < warmup + iterations; ++i)
            {
                if (i == warmup)
                {
                    started = clock_type::now();
                }
                co_await client.write_all(pace::as_bytes(payload));
                co_await server.read_exact(received);
            }
            elapsed = clock_type::now() - started;
            require(pace::as_text(received) == payload, "bad relay data");
            co_await client.shutdown_write();
            co_await server.shutdown_write();
        }
    );
    co_return std::chrono::duration<double, std::nano>(elapsed).count() / iterations;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        require(
            argc == 3,
            "usage: pace_net_hotpaths poll-dense|poll-sparse|udp|relay-manual|relay-copy|relay-splice iterations"
        );
        const std::string_view workload(argv[1]);
        const auto iterations = std::stoul(argv[2]);
        require(iterations > 0 && iterations <= 100000, "out-of-range iterations");
        std::string_view backend;
        const double result = pace::run(
            [&](pace::io_context& io) -> pace::task<double>
            {
                backend = pace::backend_name(io.backend());
                if (workload == "udp")
                {
                    co_return co_await pace::with_deadline(pace::deadline::after(20s), udp_time(io, iterations));
                }
                if (workload == "relay-manual" || workload == "relay-copy" || workload == "relay-splice")
                {
                    co_return co_await relay_time(io, workload, iterations);
                }
                require(workload == "poll-dense" || workload == "poll-sparse", "unknown workload");
                co_return poll_time(io, iterations, workload == "poll-dense" ? 128 : 1);
            }
        );
        std::cout << "{\"backend\":\"" << backend << "\",\"workload\":\"" << workload
                  << "\",\"iterations\":" << iterations << ",\"ns_per_operation\":" << result << "}\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
