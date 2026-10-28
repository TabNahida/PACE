#include <pace/pace.hpp>

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

pace::http::router make_routes(pace::io_context& io) {
    pace::http::router routes;
    routes.get("/health", [](pace::http::request) {
        return pace::http::response{.body = "ok\n"};
    });
    routes.post("/echo", [](pace::http::request request) {
        return pace::http::response{.body = std::move(request.body),
                                    .content_type = "application/octet-stream"};
    });
    routes.get("/slow", [&io](pace::http::request) -> pace::task<pace::http::response> {
        co_await io.sleep_for(std::chrono::milliseconds(5));
        co_return pace::http::response{.body = "done\n"};
    });
    return routes;
}

pace::task<void> check_response(pace::io_context& io, pace::tcp::endpoint endpoint) {
    const auto response = co_await pace::http::async_get(io, endpoint, "/health?probe=1");
    if (response.status != 200 || response.body != "ok\n")
        throw std::runtime_error("HTTP response mismatch");
    const auto delayed = co_await pace::http::async_get(io, endpoint, "/slow");
    if (delayed.body != "done\n") throw std::runtime_error("async route mismatch");
    std::cout << "HTTP /health and /slow verified\n";
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
        io.spawn(pace::http::serve(listener, make_routes(io), bounds));
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
