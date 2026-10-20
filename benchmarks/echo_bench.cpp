// One identical, independent TCP client workload for every server backend.
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

#include <pace/net.hpp>
#ifdef PACE_HAS_LIBUV
#include <uv.h>
#endif
#ifdef PACE_HAS_ASIO
#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#include <asio.hpp>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
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
constexpr std::size_t server_buffer_size = 64 * 1024;

class server {
public:
    virtual ~server() = default;
    virtual std::uint16_t port() const = 0;
    virtual void stop() noexcept = 0;
    void check_error() {
        std::lock_guard lock(error_mutex_);
        if (error_) std::rethrow_exception(error_);
    }
protected:
    void remember_error(std::exception_ptr error) noexcept {
        std::lock_guard lock(error_mutex_);
        if (!error_) error_ = error;
    }
private:
    std::mutex error_mutex_;
    std::exception_ptr error_;
};

class pace_server final : public server {
public:
    pace_server() : listener_(context_, {"127.0.0.1", 0}) {
        context_.spawn(accept_loop());
        worker_ = std::thread([this] {
            try { context_.run(); }
            catch (...) { remember_error(std::current_exception()); }
        });
    }
    ~pace_server() override { stop(); }
    std::uint16_t port() const override { return listener_.local_endpoint().port; }
    void stop() noexcept override {
        context_.stop();
        if (worker_.joinable()) worker_.join();
    }
private:
    pace::task<void> echo(pace::tcp_stream socket) {
        socket.set_no_delay();
        std::array<char, server_buffer_size> buffer{};
        for (;;) {
            const auto size = co_await socket.read_some(buffer);
            if (!size) co_return;
            co_await socket.write_all({buffer.data(), size});
        }
    }
    pace::task<void> accept_loop() {
        for (;;) context_.spawn(echo(co_await listener_.accept()));
    }
    pace::io_context context_;
    pace::tcp_listener listener_;
    std::thread worker_;
};

#ifdef PACE_HAS_ASIO
class asio_server final : public server {
public:
    asio_server() : acceptor_(context_, {asio::ip::address_v4::loopback(), 0}) {
        asio::co_spawn(context_, accept_loop(), [this](std::exception_ptr error) {
            if (error) remember_error(error);
        });
        worker_ = std::thread([this] {
            try { context_.run(); }
            catch (...) { remember_error(std::current_exception()); }
        });
    }
    ~asio_server() override { stop(); }
    std::uint16_t port() const override { return acceptor_.local_endpoint().port(); }
    void stop() noexcept override {
        context_.stop();
        if (worker_.joinable()) worker_.join();
    }
private:
    asio::awaitable<void> echo(asio::ip::tcp::socket socket) {
        socket.set_option(asio::ip::tcp::no_delay(true));
        std::array<char, server_buffer_size> buffer{};
        for (;;) {
            std::error_code error;
            const auto size = co_await socket.async_read_some(asio::buffer(buffer),
                asio::redirect_error(asio::use_awaitable, error));
            if (error == asio::error::eof) co_return;
            if (error) throw std::system_error(error);
            co_await asio::async_write(socket, asio::buffer(buffer.data(), size), asio::use_awaitable);
        }
    }
    asio::awaitable<void> accept_loop() {
        for (;;) {
            auto socket = co_await acceptor_.async_accept(asio::use_awaitable);
            asio::co_spawn(context_, echo(std::move(socket)), [this](std::exception_ptr error) {
                if (error) remember_error(error);
            });
        }
    }
    asio::io_context context_{1};
    asio::ip::tcp::acceptor acceptor_;
    std::thread worker_;
};
#endif

#ifdef PACE_HAS_LIBUV
class uv_server final : public server {
    struct peer {
        uv_tcp_t socket{};
        uv_write_t write{};
        std::array<char, server_buffer_size> buffer{};
        uv_server* owner;
    };
public:
    uv_server() {
        check(uv_loop_init(&loop_));
        check(uv_tcp_init(&loop_, &listener_));
        listener_.data = this;
        sockaddr_in endpoint{};
        check(uv_ip4_addr("127.0.0.1", 0, &endpoint));
        check(uv_tcp_bind(&listener_, reinterpret_cast<const sockaddr*>(&endpoint), 0));
        check(uv_listen(reinterpret_cast<uv_stream_t*>(&listener_), 1024, on_connection));
        int size = sizeof(endpoint);
        check(uv_tcp_getsockname(&listener_, reinterpret_cast<sockaddr*>(&endpoint), &size));
        port_ = ntohs(endpoint.sin_port);
        check(uv_async_init(&loop_, &shutdown_, on_shutdown));
        shutdown_.data = this;
        worker_ = std::thread([this] { uv_run(&loop_, UV_RUN_DEFAULT); });
    }
    ~uv_server() override { stop(); }
    std::uint16_t port() const override { return port_; }
    void stop() noexcept override {
        if (!worker_.joinable()) return;
        uv_async_send(&shutdown_);
        worker_.join();
        uv_loop_close(&loop_);
    }
private:
    static void check(int result) {
        if (result < 0) throw std::runtime_error(std::string("libuv: ") + uv_strerror(result));
    }
    void error(int code) noexcept {
        try { check(code); }
        catch (...) { remember_error(std::current_exception()); }
    }
    static void close_peer(uv_handle_t* handle) {
        delete static_cast<peer*>(handle->data);
    }
    static void disconnect(peer* connection) {
        auto* handle = reinterpret_cast<uv_handle_t*>(&connection->socket);
        if (!uv_is_closing(handle)) uv_close(handle, close_peer);
    }
    static void allocate(uv_handle_t* handle, std::size_t, uv_buf_t* buffer) {
        auto* connection = static_cast<peer*>(handle->data);
        *buffer = uv_buf_init(connection->buffer.data(), static_cast<unsigned>(connection->buffer.size()));
    }
    static void on_write(uv_write_t* request, int status) {
        auto* connection = static_cast<peer*>(request->data);
        if (uv_is_closing(reinterpret_cast<uv_handle_t*>(&connection->socket))) return;
        if (status < 0) {
            connection->owner->error(status);
            disconnect(connection);
            return;
        }
        const auto result = uv_read_start(reinterpret_cast<uv_stream_t*>(&connection->socket), allocate, on_read);
        if (result < 0) { connection->owner->error(result); disconnect(connection); }
    }
    static void on_read(uv_stream_t* stream, std::ptrdiff_t size, const uv_buf_t*) {
        auto* connection = static_cast<peer*>(stream->data);
        if (size == 0) return;
        if (size < 0) {
            if (size != UV_EOF) connection->owner->error(static_cast<int>(size));
            disconnect(connection);
            return;
        }
        uv_read_stop(stream);
        auto buffer = uv_buf_init(connection->buffer.data(), static_cast<unsigned>(size));
        const auto result = uv_write(&connection->write, stream, &buffer, 1, on_write);
        if (result < 0) { connection->owner->error(result); disconnect(connection); }
    }
    static void on_connection(uv_stream_t* listener, int status) {
        auto* owner = static_cast<uv_server*>(listener->data);
        if (status < 0) { owner->error(status); return; }
        auto* connection = new (std::nothrow) peer{};
        if (!connection) { owner->error(UV_ENOMEM); return; }
        connection->owner = owner;
        connection->socket.data = connection;
        connection->write.data = connection;
        auto result = uv_tcp_init(&owner->loop_, &connection->socket);
        if (result < 0) { owner->error(result); delete connection; return; }
        connection->socket.data = connection;
        result = uv_accept(listener, reinterpret_cast<uv_stream_t*>(&connection->socket));
        if (result >= 0) result = uv_tcp_nodelay(&connection->socket, 1);
        if (result >= 0) result = uv_read_start(reinterpret_cast<uv_stream_t*>(&connection->socket), allocate, on_read);
        if (result < 0) { owner->error(result); disconnect(connection); }
    }
    static void on_shutdown(uv_async_t* handle) {
        auto* owner = static_cast<uv_server*>(handle->data);
        uv_walk(&owner->loop_, [](uv_handle_t* current, void* argument) {
            auto* self = static_cast<uv_server*>(argument);
            if (uv_is_closing(current)) return;
            if (current->type == UV_TCP && current != reinterpret_cast<uv_handle_t*>(&self->listener_))
                uv_close(current, close_peer);
            else uv_close(current, nullptr);
        }, owner);
    }
    uv_loop_t loop_{};
    uv_tcp_t listener_{};
    uv_async_t shutdown_{};
    std::uint16_t port_{};
    std::thread worker_;
};
#endif

#ifdef _WIN32
using socket_type = SOCKET;
constexpr socket_type invalid_socket = INVALID_SOCKET;
void close_socket(socket_type fd) { closesocket(fd); }
std::string socket_error() { return std::to_string(WSAGetLastError()); }
#else
using socket_type = int;
constexpr socket_type invalid_socket = -1;
void close_socket(socket_type fd) { ::close(fd); }
std::string socket_error() { return std::strerror(errno); }
#endif

struct socket_guard {
    socket_type fd = invalid_socket;
    ~socket_guard() { if (fd != invalid_socket) close_socket(fd); }
};

void exchange(socket_type socket, std::vector<char>& payload, std::vector<char>& received,
              std::uint64_t sequence) {
    std::memcpy(payload.data(), &sequence, sizeof(sequence));
    std::size_t position = 0;
    while (position < payload.size()) {
        const auto count = ::send(socket, payload.data() + position,
                                  static_cast<int>(payload.size() - position), 0);
        if (count <= 0) throw std::runtime_error("client send: " + socket_error());
        position += static_cast<std::size_t>(count);
    }
    position = 0;
    while (position < received.size()) {
        const auto count = ::recv(socket, received.data() + position,
                                  static_cast<int>(received.size() - position), 0);
        if (count <= 0) throw std::runtime_error("client recv/EOF: " + socket_error());
        position += static_cast<std::size_t>(count);
    }
    if (payload != received) throw std::runtime_error("echo payload mismatch");
}

struct options {
    std::string backend = "pace";
    std::size_t clients = 8;
    std::size_t requests = 5000;
    std::size_t warmup = 500;
    std::size_t bytes = 64;
    unsigned timeout_ms = 5000;
    unsigned trial = 1;
};

std::unique_ptr<server> make_server(const std::string& backend) {
    if (backend == "pace") return std::make_unique<pace_server>();
#ifdef PACE_HAS_ASIO
    if (backend == "asio") return std::make_unique<asio_server>();
#endif
#ifdef PACE_HAS_LIBUV
    if (backend == "libuv") return std::make_unique<uv_server>();
#endif
    throw std::invalid_argument("backend not built: " + backend);
}

void benchmark(const options& config) {
    auto service = make_server(config.backend);
    const auto port = service->port();
    std::latch ready(static_cast<std::ptrdiff_t>(config.clients));
    std::promise<void> start_signal;
    auto start = start_signal.get_future().share();
    std::vector<std::thread> workers;
    workers.reserve(config.clients);
    std::vector<std::vector<double>> latencies(config.clients);
    std::vector<std::exception_ptr> errors(config.clients);
    for (std::size_t id = 0; id < config.clients; ++id) {
        workers.emplace_back([&, id] {
            bool ready_signaled = false;
            try {
                socket_guard socket{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
                if (socket.fd == invalid_socket) throw std::runtime_error("client socket: " + socket_error());
                const int enabled = 1;
                if (::setsockopt(socket.fd, IPPROTO_TCP, TCP_NODELAY,
                    reinterpret_cast<const char*>(&enabled), sizeof(enabled)) != 0)
                    throw std::runtime_error("TCP_NODELAY: " + socket_error());
#ifdef _WIN32
                const DWORD timeout = config.timeout_ms;
#else
                timeval timeout{};
                timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(config.timeout_ms / 1000);
                timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>((config.timeout_ms % 1000) * 1000);
#endif
                for (const int option : {SO_RCVTIMEO, SO_SNDTIMEO}) {
                    if (::setsockopt(socket.fd, SOL_SOCKET, option,
                        reinterpret_cast<const char*>(&timeout), sizeof(timeout)) != 0)
                        throw std::runtime_error("client timeout: " + socket_error());
                }
                sockaddr_in endpoint{};
                endpoint.sin_family = AF_INET;
                endpoint.sin_port = htons(port);
                ::inet_pton(AF_INET, "127.0.0.1", &endpoint.sin_addr);
                if (::connect(socket.fd, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) != 0)
                    throw std::runtime_error("client connect: " + socket_error());
                std::vector<char> payload(config.bytes), received(config.bytes);
                for (std::size_t i = 0; i < payload.size(); ++i)
                    payload[i] = static_cast<char>((i * 31 + id * 17) % 251);
                auto& samples = latencies[id];
                samples.reserve(config.requests);
                const auto connection_tag = static_cast<std::uint64_t>(id) << 48;
                for (std::size_t i = 0; i < config.warmup; ++i)
                    exchange(socket.fd, payload, received, (std::uint64_t{1} << 63) | connection_tag | i);
                ready.count_down();
                ready_signaled = true;
                start.wait();
                for (std::size_t i = 0; i < config.requests; ++i) {
                    const auto before = clock_type::now();
                    exchange(socket.fd, payload, received, connection_tag | i);
                    samples.push_back(std::chrono::duration<double, std::micro>(clock_type::now() - before).count());
                }
            } catch (...) {
                errors[id] = std::current_exception();
                if (!ready_signaled) ready.count_down();
            }
        });
    }
    ready.wait();
    const auto begin = clock_type::now();
    start_signal.set_value();
    for (auto& worker : workers) worker.join();
    const auto elapsed = std::chrono::duration<double>(clock_type::now() - begin).count();
    // Check before shutdown: cancellation is not a benchmark failure.
    service->check_error();
    service->stop();
    for (const auto& error : errors) if (error) std::rethrow_exception(error);
    std::vector<double> merged;
    merged.reserve(config.clients * config.requests);
    for (auto& samples : latencies) merged.insert(merged.end(), samples.begin(), samples.end());
    std::sort(merged.begin(), merged.end());
    const auto percentile = [&](double fraction) {
        return merged[static_cast<std::size_t>(std::ceil(fraction * merged.size())) - 1];
    };
    const double throughput = merged.size() / elapsed;
    std::cout << std::fixed << std::setprecision(9)
        << "{\"benchmark\":\"tcp_echo\",\"backend\":\"" << config.backend
        << "\",\"trial\":" << config.trial << ",\"server_threads\":1,\"client_threads\":" << config.clients
        << ",\"connections\":" << config.clients << ",\"pipeline\":1,\"payload_bytes\":" << config.bytes
        << ",\"requests_per_client\":" << config.requests << ",\"warmup_per_client\":" << config.warmup
        << ",\"completed_requests\":" << merged.size() << ",\"seconds\":" << elapsed
        << ",\"requests_per_second\":" << throughput
        << ",\"roundtrip_mib_per_second\":" << throughput * config.bytes * 2 / (1024 * 1024)
        << ",\"p50_us\":" << percentile(0.50) << ",\"p99_us\":" << percentile(0.99)
        << ",\"max_us\":" << merged.back() << ",\"payload_verified\":true}\n";
}

void list_backends() {
    std::cout << "{\"backends\":[\"pace\"";
#ifdef PACE_HAS_ASIO
    std::cout << ",\"asio\"";
#endif
#ifdef PACE_HAS_LIBUV
    std::cout << ",\"libuv\"";
#endif
    std::cout << "],\"asio_version\":";
#ifdef PACE_HAS_ASIO
    std::cout << ASIO_VERSION;
#else
    std::cout << "null";
#endif
    std::cout << ",\"libuv_version\":";
#ifdef PACE_HAS_LIBUV
    std::cout << '"' << uv_version_string() << '"';
#else
    std::cout << "null";
#endif
    std::cout << ",\"cpp_standard\":";
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
    std::cout << "}\n";
}

std::size_t integer(std::string_view input) {
    if (input.empty() || input.find_first_not_of("0123456789") != std::string_view::npos)
        throw std::invalid_argument("expected positive integer");
    return std::stoull(std::string(input));
}
} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return 1;
    struct winsock_guard { ~winsock_guard() { WSACleanup(); } } guard;
#else
    ::signal(SIGPIPE, SIG_IGN);
#endif
    try {
        options config;
        for (int i = 1; i < argc; ++i) {
            const std::string_view argument = argv[i];
            if (argument == "--list") { list_backends(); return 0; }
            if (argument == "--help") {
                std::cout << "pace_bench --backend pace|asio|libuv [--clients 8] [--requests 5000] "
                    "[--warmup 500] [--bytes 64] [--timeout-ms 5000] [--trial 1]\n";
                return 0;
            }
            if (i + 1 == argc) throw std::invalid_argument("missing option value");
            const std::string_view value = argv[++i];
            if (argument == "--backend") config.backend = value;
            else if (argument == "--clients") config.clients = integer(value);
            else if (argument == "--requests") config.requests = integer(value);
            else if (argument == "--warmup") config.warmup = integer(value);
            else if (argument == "--bytes") config.bytes = integer(value);
            else if (argument == "--timeout-ms") config.timeout_ms = static_cast<unsigned>(integer(value));
            else if (argument == "--trial") config.trial = static_cast<unsigned>(integer(value));
            else throw std::invalid_argument("unknown option: " + std::string(argument));
        }
        if (!config.clients || config.clients > 256 || !config.requests || config.requests > 10000000 ||
            config.clients * config.requests > 20000000 || config.warmup > 1000000 ||
            config.bytes < 8 || config.bytes > 1048576 || config.timeout_ms < 100 || config.timeout_ms > 60000)
            throw std::invalid_argument("out-of-range benchmark configuration");
        benchmark(config);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
