// Build this diagnostic against each revision's pace_net and pace_runtime.
#include <pace/tcp.hpp>
#include <pace/udp.hpp>
#include <chrono>
#include <iostream>
#include <semaphore>
#include <thread>

namespace {
using clock_type = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct cancellation_batch {
    static constexpr unsigned count = 4096;
    pace::io_context io{};
    pace::cancellation_source source{};
    std::counting_semaphore<2> armed{0};
    unsigned started = 0, finished = 0;
    clock_type::time_point begin;

    pace::task<void> sleeper(unsigned id) {
        if (++started == count) io.post([this] { armed.release(); });
        try {
            co_await io.sleep_for(1h + std::chrono::seconds(id * 7919 % count));
        } catch (const std::system_error& error) {
            if (error.code() != std::errc::operation_canceled) throw;
            if (++finished == count) io.stop();
            co_return;
        }
        throw std::logic_error("unexpected timer expiry");
    }

    auto measure() {
        for (unsigned i = 0; i < count; ++i)
            io.spawn(pace::detail::task_access::bind(sleeper(i), source.token()));
        std::jthread requester([this] {
            armed.acquire();
            begin = clock_type::now();
            source.request_cancellation();
        });
        try { io.run(); }
        catch (...) { armed.release(); throw; }
        requester.join();
        if (finished != count) throw std::logic_error("missing completion");
        return clock_type::now() - begin;
    }
};

auto measure_sockets() {
    pace::io_context io;
    const auto begin = clock_type::now();
    for (unsigned i = 0; i < 60000; ++i) {
        pace::udp::socket udp(io);
        pace::tcp::stream tcp(io);
    }
    return clock_type::now() - begin;
}
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string_view name = argv[1];
    if (name != "cancel" && name != "socket") return 2;
    auto elapsed = name == "cancel" ? cancellation_batch{}.measure() : measure_sockets();
    std::cout << std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() << '\n';
}
