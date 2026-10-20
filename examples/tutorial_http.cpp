#include <pace/pace.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
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

pace::task<pace::http::response> route(pace::http::request request) {
    if ((request.method == "GET" || request.method == "HEAD") && request.target == "/health")
        co_return pace::http::response{.body = "ok\n"};
    if (request.method == "POST" && request.target == "/echo")
        co_return pace::http::response{.body = std::move(request.body),
                                      .content_type = "application/octet-stream"};
    co_return pace::http::response{.status = 404, .body = "Not Found\n"};
}

pace::task<void> check_response(pace::io_context& io, pace::tcp::endpoint endpoint) {
    pace::tcp::stream socket(io);
    co_await socket.connect(std::move(endpoint));
    const std::string request = "GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    co_await socket.write_all(request);
    std::string response;
    std::array<char, 1024> buffer{};
    while (const auto count = co_await socket.read_some(buffer)) {
        response.append(buffer.data(), count);
        if (response.size() > 8192) throw std::runtime_error("unexpectedly large tutorial response");
    }
    if (!response.starts_with("HTTP/1.1 200 OK\r\n") ||
        response.find("Content-Length: 3\r\n") == std::string::npos ||
        !response.ends_with("\r\n\r\nok\n"))
        throw std::runtime_error("HTTP response mismatch");
    socket.close();
    std::cout << "HTTP /health verified\n";
    io.stop();
}
} // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view mode = argc > 1 ? argv[1] : "selftest";
        if (argc > 2 || (mode != "selftest" && mode != "serve"))
            throw std::invalid_argument("usage: tutorial_http [selftest|serve]");
        pace::io_context io;
        pace::tcp::listener listener(io, {"127.0.0.1", mode == "selftest" ? std::uint16_t{0} : std::uint16_t{8080}});
        const pace::http::limits bounds{.max_header_bytes = 8192, .max_body_bytes = 65536,
                                       .max_requests_per_connection = 100};
        io.spawn(pace::http::serve(listener, route, bounds));
        if (mode == "selftest") {
            io.spawn(check_response(io, listener.local_endpoint()));
        } else {
            std::signal(SIGINT, on_signal);
            std::signal(SIGTERM, on_signal);
            std::cout << "HTTP on http://127.0.0.1:8080 (Ctrl-C to stop)\n";
            io.spawn(stop_on_signal(io));
        }
        io.run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
