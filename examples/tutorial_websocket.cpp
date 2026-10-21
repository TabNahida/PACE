#include <pace/websocket.hpp>

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <iostream>

namespace {
std::atomic_flag interrupted = ATOMIC_FLAG_INIT;
void on_signal(int) { interrupted.test_and_set(std::memory_order_relaxed); }

pace::task<void> stop_on_signal(pace::io_context& io) {
    while (!interrupted.test(std::memory_order_relaxed))
        co_await io.sleep_for(std::chrono::milliseconds(20));
    io.stop();
}
pace::task<void> deadline(pace::io_context& io) {
    co_await io.sleep_for(std::chrono::seconds(5));
    throw std::runtime_error("WebSocket tutorial exceeded five seconds");
}
pace::task<void> echo(pace::tcp::stream transport) {
    try {
        transport.set_no_delay();
        pace::websocket::stream socket(std::move(transport), pace::websocket::role::server);
        // Applications may additionally check Origin and authentication here.
        co_await socket.accept({"echo"}, [](const auto& request) { return request.target == "/echo"; });
        while (auto message = co_await socket.receive())
            co_await socket.send(std::move(*message));
    } catch (const std::system_error&) {
        // Transport errors and cancellation belong to this connection.
    } catch (const pace::websocket::protocol_error&) {
        // Malformed upgrades/frames are rejected and this transport is closed.
    }
}
pace::task<void> serve(pace::tcp::listener& listener) {
    try {
        for (;;) listener.context().spawn(echo(co_await listener.accept()));
    } catch (const std::system_error& error) {
        if (error.code() != std::errc::operation_canceled && error.code() != std::errc::bad_file_descriptor) throw;
    }
}
pace::task<void> client(pace::io_context& io, pace::tcp::endpoint endpoint) {
    pace::tcp::stream transport(io);
    co_await transport.connect(std::move(endpoint));
    transport.set_no_delay();
    pace::websocket::stream socket(std::move(transport), pace::websocket::role::client);
    co_await socket.handshake("localhost", "/echo", {"echo"});
    co_await socket.ping("hello");
    co_await socket.send_text("Hello from PACE", 4); // Continuation frames are assembled by receive().
    const auto response = co_await socket.receive();
    if (!response || response->type != pace::websocket::message_type::text || response->data != "Hello from PACE")
        throw std::runtime_error("WebSocket echo mismatch");
    if (!(co_await socket.close_gracefully(1000, "finished")))
        throw std::runtime_error("WebSocket peer did not complete closing handshake");
    std::cout << "WebSocket echo verified: " << response->data << '\n';
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
            throw std::invalid_argument("usage: tutorial_websocket [selftest|serve|client] [port]");
        const auto port = argc > 2 ? parse_port(argv[2]) : std::uint16_t{9092};
        pace::io_context io;
        if (mode == "client") {
            io.spawn(client(io, {"127.0.0.1", port}));
            io.spawn(deadline(io));
            io.run();
        } else {
            pace::tcp::listener listener(io, {"127.0.0.1", mode == "selftest" ? std::uint16_t{0} : port});
            io.spawn(serve(listener));
            if (mode == "selftest") {
                io.spawn(client(io, listener.local_endpoint()));
                io.spawn(deadline(io));
            } else {
                std::signal(SIGINT, on_signal);
                std::signal(SIGTERM, on_signal);
                std::cout << "WebSocket echo at ws://127.0.0.1:" << listener.local_endpoint().port << "/echo (Ctrl-C to stop)" << std::endl;
                io.spawn(stop_on_signal(io));
            }
            io.run();
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
