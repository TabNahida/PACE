#include <pace/websocket.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef PACE_WEBSOCKET_DIAGNOSTICS
#include <sys/socket.h>
#include <sys/random.h>
namespace diagnostic {
bool active = false;
std::uint64_t reads = 0, writes = 0, random = 0;
}
extern "C" ssize_t __real_recv(int, void*, size_t, int);
extern "C" ssize_t __wrap_recv(int fd, void* data, size_t size, int flags) {
    if (diagnostic::active) ++diagnostic::reads;
    return __real_recv(fd, data, size, flags);
}
extern "C" ssize_t __real_send(int, const void*, size_t, int);
extern "C" ssize_t __wrap_send(int fd, const void* data, size_t size, int flags) {
    if (diagnostic::active) ++diagnostic::writes;
    return __real_send(fd, data, size, flags);
}
extern "C" ssize_t __real_getrandom(void*, size_t, unsigned);
extern "C" ssize_t __wrap_getrandom(void* data, size_t size, unsigned flags) {
    if (diagnostic::active) ++diagnostic::random;
    return __real_getrandom(data, size, flags);
}
#endif

namespace {
using clock_type = std::chrono::steady_clock;
struct run {
    pace::io_context io;
    pace::tcp::listener listener{io, {"127.0.0.1", 0}};
    std::size_t bytes = 64, iterations = 100000, fragments = 0, warmup = 1000;
    unsigned completed = 0;
    double elapsed = 0;
    std::vector<double> latencies;
    void done() { if (++completed == 2) io.stop(); }
};
pace::task<void> server(run& test) {
    auto socket = co_await test.listener.accept();
    socket.set_no_delay(true);
    pace::websocket::stream stream(std::move(socket), pace::websocket::role::server);
    (void)co_await stream.accept();
    for (std::size_t i = 0; i < test.warmup + test.iterations; ++i) {
        auto message = co_await stream.receive();
        if (!message || message->type != pace::websocket::message_type::binary || message->data.size() != test.bytes)
            throw std::runtime_error("invalid WebSocket request");
        co_await stream.send(std::move(*message), test.fragments);
    }
    // Wait for the client to finish validating the last response before teardown.
    if (co_await stream.receive()) throw std::runtime_error("expected close frame");
    test.done();
}
pace::task<void> client(run& test) {
    pace::tcp::stream socket(test.io);
    socket.set_no_delay(true);
    co_await socket.connect(test.listener.local_endpoint());
    pace::websocket::stream stream(std::move(socket), pace::websocket::role::client);
    co_await stream.handshake("localhost");
    std::string expected(test.bytes, '\0');
    for (std::size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<char>(i % 251);
    test.latencies.reserve(test.iterations);
    clock_type::time_point start;
    for (std::size_t i = 0; i < test.warmup + test.iterations; ++i) {
        // Change the sequence every message so stale/duplicated echoes cannot pass.
        for (unsigned byte = 0; byte < 8; ++byte) expected[byte] = static_cast<char>(static_cast<std::uint64_t>(i) >> (8 * byte));
        if (i == test.warmup) {
#ifdef PACE_WEBSOCKET_DIAGNOSTICS
            diagnostic::active = true;
#endif
            start = clock_type::now();
        }
        const auto begin = clock_type::now();
        co_await stream.send_binary(expected, test.fragments);
        auto message = co_await stream.receive();
        const auto end = clock_type::now();
        if (!message || message->type != pace::websocket::message_type::binary || message->data != expected)
            throw std::runtime_error("invalid WebSocket echo");
        if (i >= test.warmup) test.latencies.push_back(std::chrono::duration<double, std::micro>(end - begin).count());
    }
    test.elapsed = std::chrono::duration<double>(clock_type::now() - start).count();
#ifdef PACE_WEBSOCKET_DIAGNOSTICS
    diagnostic::active = false;
#endif
    if (!(co_await stream.close_gracefully())) throw std::runtime_error("close handshake timed out");
    test.done();
}
std::size_t number(const char* value) {
    std::size_t end;
    const std::string input(value);
    if (input.empty() || input.front() == '-') throw std::invalid_argument("invalid unsigned option");
    const auto result = std::stoull(input, &end);
    if (end != input.size() || result > 10000000) throw std::invalid_argument("invalid numeric option");
    return static_cast<std::size_t>(result);
}
}
int main(int argc, char** argv) {
    try {
        run test;
        for (int i = 1; i < argc; i += 2) {
            if (i + 1 == argc) throw std::invalid_argument("options require values");
            const std::string key(argv[i]); const auto value = number(argv[i + 1]);
            if (key == "--bytes") test.bytes = value;
            else if (key == "--iterations") test.iterations = value;
            else if (key == "--fragment-bytes") test.fragments = value;
            else if (key == "--warmup") test.warmup = value;
            else throw std::invalid_argument("unknown option: " + key);
        }
        if (test.bytes < 8 || test.bytes > 1024 * 1024 || !test.iterations ||
            (test.fragments && (test.bytes - 1) / std::min<std::size_t>(test.fragments, 65536) >= 1024))
            throw std::invalid_argument("configuration exceeds WebSocket limits");
        test.io.spawn(server(test)); test.io.spawn(client(test)); test.io.run();
        if (test.completed != 2 || test.latencies.size() != test.iterations) throw std::runtime_error("incomplete benchmark");
        std::sort(test.latencies.begin(), test.latencies.end());
        const auto percentile = [&](double p) { return test.latencies[static_cast<std::size_t>((test.latencies.size() - 1) * p)]; };
        std::cout << std::fixed << std::setprecision(6)
            << "{\"bytes\":" << test.bytes << ",\"fragment_bytes\":" << test.fragments
            << ",\"iterations\":" << test.iterations << ",\"warmup\":" << test.warmup
            << ",\"seconds\":" << test.elapsed << ",\"roundtrips_per_second\":" << test.iterations / test.elapsed
            << ",\"p50_us\":" << percentile(.50) << ",\"p95_us\":" << percentile(.95) << ",\"p99_us\":" << percentile(.99)
#ifdef PACE_WEBSOCKET_DIAGNOSTICS
            << ",\"instrumented\":true,\"recv_calls\":" << diagnostic::reads << ",\"send_calls\":" << diagnostic::writes
            << ",\"getrandom_calls\":" << diagnostic::random
#else
            << ",\"instrumented\":false"
#endif
            << ",\"validated\":true}\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
