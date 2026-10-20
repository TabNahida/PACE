// PACE-only worker/execution scaling. Separate from the Asio/libuv comparison.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>
#endif

#include <pace/runtime.hpp>
#include <pace/tcp.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <future>
#include <iomanip>
#include <iostream>
#include <latch>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using clock_type = std::chrono::steady_clock;
std::int64_t nanoseconds(clock_type::time_point begin, clock_type::time_point end) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
}

struct options {
    std::string mode = "runtime";
    std::size_t workers = 1, clients = 8, requests = 2000, warmup = 200, bytes = 64;
    std::size_t timeout_ms = 5000, trial = 1;
};

class cluster {
public:
    explicit cluster(const options& config)
        : mode_(config.mode), remaining_(config.workers), ready_(ready_signal_.get_future().share()) {
        if (mode_ == "external") {
            for (std::size_t i = 0; i < config.workers; ++i)
                external_.push_back(std::make_unique<pace::io_context>());
        } else {
            runtime_ = std::make_unique<pace::runtime>(config.workers);
        }
        for (std::size_t i = 0; i < config.workers; ++i) {
            auto& io = context(i);
            listeners_.push_back(std::make_unique<pace::tcp::listener>(io, pace::tcp::endpoint{"127.0.0.1", 0}));
            ports_.push_back(listeners_.back()->local_endpoint().port);
            io.spawn(accept_loop(*listeners_.back()));
            io.post([this] {
                if (remaining_.fetch_sub(1) == 1) {
                    launch_ns_ = nanoseconds(launch_begin_, clock_type::now());
                    finish_ready(nullptr);
                }
            });
        }
    }

    void run() {
        launch_begin_ = clock_type::now();
        if (mode_ == "runtime") runtime_->run();
        else if (mode_ == "caller") runtime_->run_on_current_thread();
        else {
            std::vector<std::thread> threads;
            try {
                threads.reserve(external_.size());
                for (auto& io : external_) {
                    threads.emplace_back([this, pointer = io.get()] {
                        try { pointer->run(); }
                        catch (...) { fail(std::current_exception()); }
                    });
                }
            } catch (...) { fail(std::current_exception()); }
            for (auto& thread : threads) thread.join();
            check_error();
        }
    }

    void stop() noexcept {
        if (runtime_) runtime_->stop();
        else for (auto& io : external_) io->stop();
    }
    void fail(std::exception_ptr error) {
        {
            std::lock_guard lock(error_mutex_);
            if (!error_) error_ = error;
        }
        stop();
        finish_ready(error);
    }
    void check_error() {
        std::lock_guard lock(error_mutex_);
        if (error_) std::rethrow_exception(error_);
    }
    void wait_ready() { ready_.get(); }
    std::uint16_t port(std::size_t index) const { return ports_.at(index); }
    std::int64_t launch_ns() const noexcept { return launch_ns_; }

private:
    pace::io_context& context(std::size_t index) {
        return runtime_ ? runtime_->context(index) : *external_.at(index);
    }
    void finish_ready(std::exception_ptr error) {
        std::call_once(ready_once_, [this, error] {
            if (error) ready_signal_.set_exception(error);
            else ready_signal_.set_value();
        });
    }
    static pace::task<void> echo(pace::tcp::stream socket) {
        socket.set_no_delay();
        std::array<char, 64 * 1024> buffer{};
        for (;;) {
            const auto size = co_await socket.read_some(buffer);
            if (!size) co_return;
            co_await socket.write_all({buffer.data(), size});
        }
    }
    static pace::task<void> accept_loop(pace::tcp::listener& listener) {
        for (;;) listener.context().spawn(echo(co_await listener.accept()));
    }

    std::string mode_;
    std::unique_ptr<pace::runtime> runtime_;
    std::vector<std::unique_ptr<pace::io_context>> external_;
    std::vector<std::unique_ptr<pace::tcp::listener>> listeners_;
    std::vector<std::uint16_t> ports_;
    std::atomic<std::size_t> remaining_;
    std::promise<void> ready_signal_;
    std::shared_future<void> ready_;
    std::once_flag ready_once_;
    clock_type::time_point launch_begin_{};
    std::int64_t launch_ns_{};
    std::mutex error_mutex_;
    std::exception_ptr error_;
};

#ifdef _WIN32
using socket_type = SOCKET;
constexpr socket_type invalid_socket = INVALID_SOCKET;
void close_socket(socket_type fd) { ::closesocket(fd); }
std::string socket_error() { return std::to_string(WSAGetLastError()); }
bool interrupted() { return WSAGetLastError() == WSAEINTR; }
#else
using socket_type = int;
constexpr socket_type invalid_socket = -1;
void close_socket(socket_type fd) { ::close(fd); }
std::string socket_error() { return std::strerror(errno); }
bool interrupted() { return errno == EINTR; }
#endif
struct socket_guard {
    socket_type fd = invalid_socket;
    ~socket_guard() { if (fd != invalid_socket) close_socket(fd); }
};

void connect_client(socket_type socket, std::uint16_t port, std::size_t timeout_ms) {
    const int enabled = 1;
    if (::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
        reinterpret_cast<const char*>(&enabled), sizeof(enabled)) != 0)
        throw std::runtime_error("TCP_NODELAY: " + socket_error());
#ifdef _WIN32
    const DWORD timeout = static_cast<DWORD>(timeout_ms);
#else
    timeval timeout{};
    timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(timeout_ms / 1000);
    timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>((timeout_ms % 1000) * 1000);
#endif
    for (const int option : {SO_RCVTIMEO, SO_SNDTIMEO}) {
        if (::setsockopt(socket, SOL_SOCKET, option, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) != 0)
            throw std::runtime_error("client timeout: " + socket_error());
    }
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &endpoint.sin_addr);
    if (::connect(socket, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) != 0)
        throw std::runtime_error("client connect: " + socket_error());
}

std::uint64_t exchange(socket_type socket, std::vector<char>& payload,
                       std::vector<char>& received, std::uint64_t sequence) {
    std::memcpy(payload.data(), &sequence, sizeof(sequence));
    std::size_t position = 0;
    while (position < payload.size()) {
        const auto count = ::send(socket, payload.data() + position, static_cast<int>(payload.size() - position), 0);
        if (count < 0 && interrupted()) continue;
        if (count <= 0) throw std::runtime_error("client send: " + socket_error());
        position += static_cast<std::size_t>(count);
    }
    position = 0;
    while (position < received.size()) {
        const auto count = ::recv(socket, received.data() + position, static_cast<int>(received.size() - position), 0);
        if (count < 0 && interrupted()) continue;
        if (count <= 0) throw std::runtime_error("client receive/EOF: " + socket_error());
        position += static_cast<std::size_t>(count);
    }
    if (received != payload) throw std::runtime_error("echo payload mismatch");
    std::uint64_t actual{};
    std::memcpy(&actual, received.data(), sizeof(actual));
    return actual;
}

struct client_result {
    std::vector<double> latencies;
    std::uint64_t checksum{};
    std::exception_ptr error;
};

void benchmark(const options& config) {
    const auto setup_begin = clock_type::now();
    cluster service(config);
    const auto setup_ns = nanoseconds(setup_begin, clock_type::now());
    std::vector<client_result> results(config.clients);
    std::exception_ptr coordinator_error;
    std::int64_t warmup_ns{}, steady_ns{};
    clock_type::time_point shutdown_begin{};
    std::thread coordinator([&] {
        try {
            service.wait_ready();
            const auto warmup_begin = clock_type::now();
            std::latch ready(static_cast<std::ptrdiff_t>(config.clients));
            std::promise<void> start_signal;
            const auto start = start_signal.get_future().share();
            std::atomic<bool> abort{false};
            std::vector<std::thread> clients;
            try {
                clients.reserve(config.clients);
                for (std::size_t id = 0; id < config.clients; ++id) {
                    clients.emplace_back([&, id] {
                        bool signaled = false;
                        try {
                            socket_guard socket{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
                            if (socket.fd == invalid_socket) throw std::runtime_error("client socket: " + socket_error());
                            connect_client(socket.fd, service.port(id % config.workers), config.timeout_ms);
                            std::vector<char> payload(config.bytes), received(config.bytes);
                            for (std::size_t i = 0; i < config.bytes; ++i)
                                payload[i] = static_cast<char>((i * 31 + id * 17) % 251);
                            auto& result = results[id];
                            result.latencies.reserve(config.requests);
                            const auto tag = static_cast<std::uint64_t>(id) << 48;
                            for (std::size_t i = 0; i < config.warmup; ++i)
                                exchange(socket.fd, payload, received, (std::uint64_t{1} << 63) | tag | i);
                            ready.count_down();
                            signaled = true;
                            start.wait();
                            if (abort.load()) return;
                            for (std::size_t i = 0; i < config.requests; ++i) {
                                const auto begin = clock_type::now();
                                const auto response = exchange(socket.fd, payload, received, tag | i);
                                result.latencies.push_back(std::chrono::duration<double, std::micro>(clock_type::now() - begin).count());
                                result.checksum += (id + 1) * ((response & ((std::uint64_t{1} << 48) - 1)) + 1);
                            }
                        } catch (...) {
                            results[id].error = std::current_exception();
                            if (!signaled) ready.count_down();
                        }
                    });
                }
            } catch (...) {
                abort.store(true);
                start_signal.set_value();
                service.stop();
                for (auto& client : clients) client.join();
                throw;
            }
            ready.wait();
            warmup_ns = nanoseconds(warmup_begin, clock_type::now());
            for (const auto& result : results) if (result.error) abort.store(true);
            const auto begin = clock_type::now();
            start_signal.set_value();
            for (auto& client : clients) client.join();
            steady_ns = nanoseconds(begin, clock_type::now());
            for (const auto& result : results) if (result.error) std::rethrow_exception(result.error);
        } catch (...) { coordinator_error = std::current_exception(); }
        shutdown_begin = clock_type::now();
        service.stop();
    });
    try { service.run(); }
    catch (...) { service.fail(std::current_exception()); }
    const auto shutdown_end = clock_type::now();
    coordinator.join();
    service.check_error();
    if (coordinator_error) std::rethrow_exception(coordinator_error);

    std::vector<double> samples;
    std::vector<std::size_t> per_worker(config.workers);
    std::uint64_t checksum{};
    samples.reserve(config.clients * config.requests);
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& result = results[i];
        checksum += result.checksum;
        per_worker[i % config.workers] += result.latencies.size();
        samples.insert(samples.end(), result.latencies.begin(), result.latencies.end());
    }
    const std::uint64_t expected_checksum = (config.clients * (config.clients + 1) / 2)
        * (static_cast<std::uint64_t>(config.requests) * (config.requests + 1) / 2);
    if (samples.size() != config.clients * config.requests || checksum != expected_checksum)
        throw std::runtime_error("incomplete request count/checksum");
    for (auto completed : per_worker)
        if (completed != config.clients / config.workers * config.requests)
            throw std::runtime_error("unbalanced worker completion count");
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&](double fraction) {
        return samples[static_cast<std::size_t>(std::ceil(fraction * samples.size())) - 1];
    };
    const double seconds = static_cast<double>(steady_ns) / 1e9;
    const double throughput = static_cast<double>(samples.size()) / seconds;
    const auto launch_ns = service.launch_ns();
    std::cout << std::fixed << std::setprecision(12)
        << "{\"benchmark\":\"tcp_worker_scaling\",\"mode\":\"" << config.mode
        << "\",\"trial\":" << config.trial << ",\"worker_count\":" << config.workers
        << ",\"server_threads\":" << config.workers
        << ",\"spawned_server_threads\":" << (config.mode == "caller" ? config.workers - 1 : config.workers)
        << ",\"caller_runs_reactor\":" << (config.mode == "caller" ? "true" : "false")
        << ",\"client_threads\":" << config.clients << ",\"connections\":" << config.clients
        << ",\"connections_per_worker\":" << config.clients / config.workers
        << ",\"listener_count\":" << config.workers << ",\"pipeline\":1,\"payload_bytes\":" << config.bytes
        << ",\"requests_per_client\":" << config.requests << ",\"warmup_per_client\":" << config.warmup
        << ",\"socket_timeout_ms\":" << config.timeout_ms
        << ",\"completed_requests\":" << samples.size() << ",\"checksum\":" << checksum
        << ",\"per_worker_completed_requests\":[";
    for (std::size_t i = 0; i < per_worker.size(); ++i) {
        if (i) std::cout << ',';
        std::cout << per_worker[i];
    }
    std::cout << "],\"setup_ns\":" << setup_ns << ",\"launch_ns\":" << launch_ns
        << ",\"startup_ns\":" << setup_ns + launch_ns << ",\"client_setup_warmup_ns\":" << warmup_ns
        << ",\"steady_ns\":" << steady_ns << ",\"shutdown_ns\":" << nanoseconds(shutdown_begin, shutdown_end)
        << ",\"seconds\":" << seconds << ",\"requests_per_second\":" << throughput
        << ",\"roundtrip_mib_per_second\":" << throughput * config.bytes * 2 / (1024 * 1024)
        << ",\"p50_us\":" << percentile(0.50) << ",\"p99_us\":" << percentile(0.99)
        << ",\"max_us\":" << samples.back() << ",\"validated\":true,\"instrumented\":false}\n";
}

void list_capabilities() {
    std::cout << "{\"modes\":[\"runtime\",\"caller\",\"external\"],\"cpp_standard\":";
#ifdef _MSVC_LANG
    std::cout << _MSVC_LANG;
#else
    std::cout << __cplusplus;
#endif
    std::cout << ",\"compiler\":\"";
#if defined(__clang__)
    std::cout << "Clang " << __clang_major__ << '.' << __clang_minor__ << '.' << __clang_patchlevel__;
#elif defined(_MSC_VER)
    std::cout << "MSVC " << _MSC_VER;
#elif defined(__GNUC__)
    std::cout << "GCC " << __GNUC__ << '.' << __GNUC_MINOR__ << '.' << __GNUC_PATCHLEVEL__;
#else
    std::cout << "unknown";
#endif
    std::cout << "\",\"ndebug\":";
#ifdef NDEBUG
    std::cout << "true";
#else
    std::cout << "false";
#endif
    std::cout << ",\"profiling_enabled\":";
#ifdef PACE_ENABLE_PROFILING
    std::cout << "true";
#else
    std::cout << "false";
#endif
    std::cout << "}\n";
}

std::size_t integer(std::string_view value) {
    if (value.empty() || value.find_first_not_of("0123456789") != std::string_view::npos)
        throw std::invalid_argument("expected an unsigned integer");
    return std::stoull(std::string(value));
}
} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA winsock{};
    if (::WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return 1;
    struct cleanup { ~cleanup() { ::WSACleanup(); } } cleanup_guard;
#else
    ::signal(SIGPIPE, SIG_IGN);
#endif
    try {
        options config;
        for (int i = 1; i < argc; ++i) {
            const std::string_view argument = argv[i];
            if (argument == "--list") { list_capabilities(); return 0; }
            if (argument == "--help") {
                std::cout << "pace_scaling --mode runtime|caller|external --workers N [--clients 8] "
                    "[--requests 2000] [--warmup 200] [--bytes 64] [--timeout-ms 5000] [--trial 1]\n";
                return 0;
            }
            if (++i == argc) throw std::invalid_argument("missing option value");
            if (argument == "--mode") { config.mode = argv[i]; continue; }
            const auto value = integer(argv[i]);
            if (argument == "--workers") config.workers = value;
            else if (argument == "--clients") config.clients = value;
            else if (argument == "--requests") config.requests = value;
            else if (argument == "--warmup") config.warmup = value;
            else if (argument == "--bytes") config.bytes = value;
            else if (argument == "--timeout-ms") config.timeout_ms = value;
            else if (argument == "--trial") config.trial = value;
            else throw std::invalid_argument("unknown option");
        }
#ifdef PACE_ENABLE_PROFILING
        throw std::invalid_argument("worker scaling requires an uninstrumented build (--profiling=n)");
#endif
        if ((config.mode != "runtime" && config.mode != "caller" && config.mode != "external") ||
            config.workers < 1 || config.workers > 64 || config.clients < config.workers || config.clients > 256 ||
            config.clients % config.workers != 0 || config.requests < 1 || config.requests > 10000000 ||
            config.clients * config.requests > 20000000 || config.warmup > 1000000 || config.bytes < 8 ||
            config.bytes > 1048576 || config.timeout_ms < 1 || config.timeout_ms > 600000 || config.trial < 1)
            throw std::invalid_argument("out-of-range workload; clients must be a positive multiple of workers");
        benchmark(config);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "scaling benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
