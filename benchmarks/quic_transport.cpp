// Linux loopback benchmark. Both peers share one reactor and one process.
#include <pace/quic.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace pace;
using Clock = std::chrono::steady_clock;
constexpr size_t bulk = 32 * 1024 * 1024;
constexpr size_t chunk = 16384;
constexpr int samples = 1024, warmup = 32;

task<void> read_checked(quic::stream& s, size_t remaining, char expected)
{
    std::array<char, chunk> buffer;
    while (remaining)
    {
        auto n = co_await s.read_some(std::span(buffer).first(std::min(remaining, buffer.size())));
        if (!n || !std::all_of(buffer.begin(), buffer.begin() + n, [expected](char c) { return c == expected; }))
        {
            throw std::runtime_error("payload mismatch or early FIN");
        }
        remaining -= n;
    }
}

task<void> server(quic::listener& listener, bool latency)
{
    auto connection = co_await listener.accept();
    auto stream = co_await connection.accept_stream();
    if (latency)
    {
        for (int i = 0; i < samples + warmup; ++i)
        {
            co_await read_checked(stream, 64, 'x');
            co_await stream.write_all("a");
        }
    }
    else
    {
        for (auto size : {chunk * 64, bulk})
        {
            co_await read_checked(stream, size, 'x');
            co_await stream.write_all("a");
        }
    }
    while (stream.pending_write_bytes())
    {
        co_await stream.wait_events();
    }
}

task<void> client(quic::connection& connection, udp::endpoint peer, bool latency)
{
    co_await connection.connect(peer, "localhost");
    auto stream = co_await connection.open_stream();
    const std::string bytes(latency ? 64 : chunk, 'x');
    std::vector<double> rtt;
    if (latency)
    {
        rtt.reserve(samples);
        for (int i = -warmup; i < samples; ++i)
        {
            auto start = Clock::now();
            co_await stream.write_all(bytes);
            co_await read_checked(stream, 1, 'a');
            if (i >= 0)
            {
                rtt.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
            }
        }
        std::sort(rtt.begin(), rtt.end());
        std::cout << rtt[(samples * 50 + 99) / 100 - 1] << ',' << rtt[(samples * 99 + 99) / 100 - 1];
    }
    else
    {
        for (auto size : {chunk * 64, bulk})
        {
            auto start = Clock::now();
            for (size_t sent = 0; sent < size; sent += chunk)
            {
                co_await stream.write_all(bytes);
            }
            co_await read_checked(stream, 1, 'a');
            if (size == bulk)
            {
                std::cout << (bulk / 1048576.) / std::chrono::duration<double>(Clock::now() - start).count();
            }
        }
    }
    connection.context().stop();
}

int main(int argc, char** argv)
{
    if (argc != 2 || (std::string_view(argv[1]) != "latency" && std::string_view(argv[1]) != "bulk"))
    {
        return 2;
    }
    const bool latency = std::string_view(argv[1]) == "latency";
    io_context io;
    quic::context tls_server(quic::role::server), tls_client;
    tls_server.use_certificate_chain("tests/certs/localhost.pem", "tests/certs/localhost-key.pem");
    tls_client.load_verify_file("tests/certs/ca.pem");
    quic::listener listener(io, {"127.0.0.1", 0}, tls_server);
    quic::connection connection(io, tls_client);
    io.spawn(server(listener, latency));
    io.spawn(client(connection, listener.local_endpoint(), latency));
    io.run();
    // VmHWM starts with this executable's address space (ru_maxrss can
    // include the launcher's pre-exec high-water mark on Linux).
    std::ifstream status("/proc/self/status");
    for (std::string field; status >> field;)
    {
        if (field == "VmHWM:")
        {
            size_t kib;
            if (!(status >> kib))
            {
                return 1;
            }
            std::cout << ',' << kib << '\n';
            return 0;
        }
        std::getline(status, field);
    }
    return 1;
}
