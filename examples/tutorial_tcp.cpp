#include <pace/pace.hpp>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
std::atomic_flag interrupted = ATOMIC_FLAG_INIT;
void on_signal(int) { interrupted.test_and_set(std::memory_order_relaxed); }

pace::task<void> stop_on_signal(pace::io_context& io) {
    while (!interrupted.test(std::memory_order_relaxed))
        co_await io.sleep_for(std::chrono::milliseconds(20));
    io.stop();
}

pace::task<void> echo(pace::tcp::stream socket) {
    try {
        socket.set_no_delay();
        std::array<char, 4096> buffer{};
        while (const auto count = co_await socket.read_some(buffer))
            co_await socket.write_all(std::string_view(buffer.data(), count));
    } catch (const std::system_error&) {
        // Peer errors and shutdown are local to this connection.
    }
}

pace::task<void> accept_connections(pace::tcp::listener& listener) {
    try {
        for (;;) listener.context().spawn(echo(co_await listener.accept()));
    } catch (const std::system_error& error) {
        if (error.code() != std::errc::operation_canceled &&
            error.code() != std::errc::bad_file_descriptor) throw;
    }
}

pace::task<void> client(pace::io_context& io, pace::tcp::endpoint endpoint,
                        std::string message) {
    pace::tcp::stream socket(io);
    co_await socket.connect(std::move(endpoint));
    socket.set_no_delay();
    co_await socket.write_all(message);
    std::string response(message.size(), '\0');
    std::size_t received = 0;
    while (received < response.size()) {
        const auto count = co_await socket.read_some(
            std::span<char>(response.data() + received, response.size() - received));
        if (!count) throw std::runtime_error("EOF before complete echo");
        received += count;
    }
    if (response != message) throw std::runtime_error("echo payload mismatch");
    co_await socket.close_gracefully();
    std::cout << "TCP echo verified: " << response << '\n';
    io.stop();
}

std::uint16_t parse_port(std::string_view value) {
    unsigned port = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), port);
    if (error != std::errc{} || end != value.data() + value.size() || port == 0 || port > 65535)
        throw std::invalid_argument("port must be 1..65535");
    return static_cast<std::uint16_t>(port);
}
} // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view mode = argc > 1 ? argv[1] : "selftest";
        if (argc > 3 || (mode != "selftest" && mode != "serve" && mode != "client"))
            throw std::invalid_argument("usage: tutorial_tcp [selftest|serve|client] [port]");
        const auto port = argc > 2 ? parse_port(argv[2]) : std::uint16_t{9090};
        pace::io_context io;
        if (mode == "client") {
            io.spawn(client(io, {"127.0.0.1", port}, "Hello from PACE"));
            io.run();
        } else {
            pace::tcp::listener listener(io, {"127.0.0.1", mode == "selftest" ? std::uint16_t{0} : port});
            io.spawn(accept_connections(listener));
            if (mode == "selftest") {
                io.spawn(client(io, listener.local_endpoint(), "Hello from PACE"));
            } else {
                std::signal(SIGINT, on_signal);
                std::signal(SIGTERM, on_signal);
                std::cout << "TCP echo on 127.0.0.1:" << listener.local_endpoint().port << " (Ctrl-C to stop)\n";
                io.spawn(stop_on_signal(io));
            }
            io.run(); // Listener outlives all accepted work and is destroyed first.
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
