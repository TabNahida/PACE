#include <pace/pace.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
std::atomic_flag interrupted = ATOMIC_FLAG_INIT;
void on_signal(int) { interrupted.test_and_set(std::memory_order_relaxed); }

// Keep signal handlers minimal and join the watcher even when run() throws.
// std::thread also supports platforms whose libc++ does not provide jthread.
class signal_watcher {
    std::atomic<bool> finished_{false};
    std::thread worker_;
public:
    explicit signal_watcher(pace::runtime& runtime) : worker_([this, &runtime] {
        while (!finished_.load(std::memory_order_relaxed) &&
               !interrupted.test(std::memory_order_relaxed))
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (interrupted.test(std::memory_order_relaxed)) runtime.stop();
    }) {}
    ~signal_watcher() {
        finished_.store(true, std::memory_order_relaxed);
        worker_.join();
    }
};

pace::task<void> echo(pace::tcp_stream stream) {
    try {
        stream.set_no_delay();
        std::array<char, 16 * 1024> buffer;
        while (const auto count = co_await stream.read_some(buffer))
            co_await stream.write_all(std::string_view(buffer.data(), count));
    } catch (const std::system_error&) {
        // Peer disconnects and context cancellation finish this connection.
    }
}

pace::task<void> accept_connections(pace::tcp_listener& listener) {
    for (;;) {
        try {
            listener.context().spawn(echo(co_await listener.accept()));
        } catch (const std::system_error& error) {
            if (error.code() == std::errc::operation_canceled) co_return;
            throw;
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        const auto port = argc > 1 ? std::stoul(argv[1]) : 9000;
        const auto workers = argc > 2 ? std::stoul(argv[2]) : 1;
        if (port > 65535 || workers == 0) throw std::invalid_argument("invalid port or worker count");
#if defined(_WIN32)
        if (workers > 1) throw std::invalid_argument("this example uses SO_REUSEPORT; use one worker on Windows");
#endif
        pace::runtime runtime(workers);
        std::vector<std::unique_ptr<pace::tcp_listener>> listeners;
        for (std::size_t i = 0; i < runtime.size(); ++i) {
            auto& context = runtime.context(i);
            listeners.push_back(std::make_unique<pace::tcp_listener>(context,
                pace::ipv4_endpoint{"0.0.0.0", static_cast<std::uint16_t>(port)},
                pace::listen_options{.reuse_port = workers > 1}));
            context.spawn(accept_connections(*listeners.back()));
        }
        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);
        signal_watcher shutdown(runtime);
        std::cout << "PACE TCP echo listening on port " << port << " with " << workers
                  << " worker(s); Ctrl+C to stop\n";
        runtime.run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
