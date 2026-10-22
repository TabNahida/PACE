#include <pace/tls.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct watchdog {
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    std::thread thread{[this] {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, 30s, [&] { return done; })) std::abort();
    }};
    ~watchdog() { { std::lock_guard lock(mutex); done = true; } changed.notify_one(); thread.join(); }
};
struct finished {
    pace::io_context& io;
    unsigned count = 0;
    void arrive() { if (++count == 2) io.stop(); }
};

pace::task<void> echo_server(pace::tcp::listener& listener, const pace::tls::context& config,
                            std::size_t size, finished& done) {
    pace::tls::stream socket(co_await listener.accept(), config);
    co_await socket.handshake();
    require(socket.selected_protocol() == "h2", "server ALPN preference was not selected");
    std::array<char, 4093> bytes{};
    std::size_t received = 0;
    while (received < size) {
        const auto count = co_await socket.read_some(bytes);
        require(count > 0, "TLS echo received early EOF");
        received += count;
        co_await socket.write_all(std::string_view(bytes.data(), count));
    }
    require(received == size, "TLS echo received too many bytes");
    co_await socket.shutdown();
    require(!socket.is_open(), "TLS shutdown did not close transport");
    done.arrive();
}
pace::task<void> echo_client(pace::tls::stream& socket, pace::tcp::endpoint endpoint,
                            std::string name, std::size_t size, finished& done) {
    co_await socket.connect(std::move(endpoint), std::move(name));
    require(socket.selected_protocol() == "h2", "client ALPN negotiation mismatch");
    std::string message(size, '\0');
    for (std::size_t i = 0; i < size; ++i) message[i] = static_cast<char>(i * 31 % 251);
    co_await socket.write_all(std::span<const std::byte>(reinterpret_cast<const std::byte*>(message.data()), message.size()));
    std::string received(size, '\0');
    std::size_t offset = 0;
    while (offset < size) {
        const auto count = co_await socket.read_some(std::span<std::byte>(
            reinterpret_cast<std::byte*>(received.data() + offset), size - offset));
        require(count > 0, "TLS client received early EOF");
        offset += count;
    }
    require(message == received, "TLS record binary payload mismatch");
    char byte;
    require(co_await socket.read_some(std::span<char>(&byte, 1)) == 0, "close_notify was not EOF");
    co_await socket.shutdown();
    done.arrive();
}
void verified_roundtrip(const std::filesystem::path& fixtures, std::string name) {
    pace::io_context io;
    pace::tls::context server(pace::tls::role::server);
    server.use_certificate_chain((fixtures / "localhost.pem").string(), (fixtures / "localhost-key.pem").string());
    server.set_alpn_protocols({"h2", "http/1.1"});
    // The stream keeps its configuration alive after the public context dies.
    pace::tls::stream client = [&] {
        pace::tls::context config;
        config.load_verify_file((fixtures / "ca.pem").string());
        config.set_alpn_protocols({"http/1.1", "h2"});
        return pace::tls::stream(io, config);
    }();
    pace::tcp::listener listener(io, {"127.0.0.1", 0});
    finished done{io};
    constexpr std::size_t payload = 96 * 1024 + 13;
    io.spawn(echo_server(listener, server, payload, done));
    io.spawn(echo_client(client, listener.local_endpoint(), std::move(name), payload, done));
    io.run();
    require(done.count == 2, "TLS roundtrip did not complete both peers");
}

pace::task<void> rejecting_server(pace::tcp::listener& listener, const pace::tls::context& config,
                                 finished& done, bool& rejected) {
    pace::tls::stream socket(co_await listener.accept(), config);
    try { co_await socket.handshake(); }
    catch (const std::system_error& error) { rejected = error.code() == std::errc::protocol_error; }
    require(rejected && !socket.is_open(), "failed TLS server handshake did not close");
    done.arrive();
}
pace::task<void> rejecting_client(pace::io_context& io, const pace::tls::context& config,
                                 pace::tcp::endpoint endpoint, std::string name,
                                 finished& done, bool& rejected) {
    pace::tls::stream socket(io, config);
    try { co_await socket.connect(std::move(endpoint), std::move(name)); }
    catch (const std::system_error& error) { rejected = error.code() == std::errc::protocol_error; }
    require(rejected && !socket.is_open(), "unverified TLS connection was accepted or left open");
    done.arrive();
}
void rejected_handshake(const std::filesystem::path& fixtures, std::string name,
                        bool trust, bool expired, bool bad_alpn) {
    pace::io_context io;
    pace::tls::context server(pace::tls::role::server), client;
    server.use_certificate_chain((fixtures / (expired ? "expired.pem" : "localhost.pem")).string(),
                                 (fixtures / "localhost-key.pem").string());
    if (trust) client.load_verify_file((fixtures / "ca.pem").string());
    if (bad_alpn) { server.set_alpn_protocols({"h2"}); client.set_alpn_protocols({"http/1.1"}); }
    pace::tcp::listener listener(io, {"127.0.0.1", 0});
    finished done{io};
    bool server_rejected = false, client_rejected = false;
    io.spawn(rejecting_server(listener, server, done, server_rejected));
    io.spawn(rejecting_client(io, client, listener.local_endpoint(), std::move(name), done, client_rejected));
    io.run();
    require(done.count == 2 && server_rejected && client_rejected, "TLS failure was not contained on both peers");
}

pace::task<void> silent_server(pace::tcp::listener& listener) {
    auto socket = co_await listener.accept();
    co_await listener.context().sleep_for(5ms);
    listener.context().stop();
}
pace::task<void> canceled_handshake(pace::io_context& io, const pace::tls::context& config,
                                   pace::tcp::endpoint endpoint, bool& canceled) {
    pace::tls::stream socket(io, config);
    try { co_await socket.connect(std::move(endpoint), "localhost"); }
    catch (const std::system_error& error) { canceled = error.code() == std::errc::operation_canceled; }
    require(canceled && !socket.is_open(), "handshake cancellation must close and unwind");
}
void handshake_cancellation() {
    pace::io_context io;
    pace::tls::context client;
    pace::tcp::listener listener(io, {"127.0.0.1", 0});
    bool canceled = false;
    io.spawn(silent_server(listener));
    io.spawn(canceled_handshake(io, client, listener.local_endpoint(), canceled));
    io.run();
    require(canceled, "pending handshake was not canceled");
}

pace::task<void> truncated_server(pace::tcp::listener& listener, const pace::tls::context& config,
                                 finished& done) {
    pace::tls::stream socket(co_await listener.accept(), config);
    co_await socket.handshake();
    co_await socket.write_all("authenticated data");
    socket.close(); // Deliberately omit close_notify.
    done.arrive();
}
pace::task<void> truncated_client(pace::io_context& io, const pace::tls::context& config,
                                 pace::tcp::endpoint endpoint, finished& done) {
    pace::tls::stream socket(io, config);
    co_await socket.connect(std::move(endpoint), "localhost");
    bool rejected = false;
    std::string received;
    try {
        std::array<char, 128> bytes{};
        for (;;) {
            const auto count = co_await socket.read_some(bytes);
            require(count != 0, "unclean TLS EOF must not be accepted as authenticated EOF");
            received.append(bytes.data(), count);
        }
    } catch (const std::system_error& error) { rejected = error.code() == std::errc::protocol_error; }
    require(rejected && received == "authenticated data", "TLS truncation handling lost record or error");
    done.arrive();
}
void truncation(const std::filesystem::path& fixtures) {
    pace::io_context io;
    pace::tls::context server(pace::tls::role::server), client;
    server.use_certificate_chain((fixtures / "localhost.pem").string(), (fixtures / "localhost-key.pem").string());
    client.load_verify_file((fixtures / "ca.pem").string());
    pace::tcp::listener listener(io, {"127.0.0.1", 0});
    finished done{io};
    io.spawn(truncated_server(listener, server, done));
    io.spawn(truncated_client(io, client, listener.local_endpoint(), done));
    io.run();
    require(done.count == 2, "truncation test did not finish");
}

pace::task<void> conflicting_reader(pace::tls::stream& socket, bool& rejected) {
    std::array<char, 1> bytes{};
    try { co_await socket.read_some(bytes); }
    catch (const std::logic_error&) { rejected = true; }
    require(rejected && socket.is_open(), "overlapping TLS operation must reject without closing the active operation");
    socket.context().stop();
}
pace::task<void> waiting_tls_server(pace::tcp::listener& listener, const pace::tls::context& config,
                                   bool& canceled) {
    pace::tls::stream socket(co_await listener.accept(), config);
    try {
        co_await socket.handshake();
        char byte;
        co_await socket.read_some(std::span<char>(&byte, 1));
    } catch (const std::system_error& error) { canceled = error.code() == std::errc::operation_canceled; }
    require(canceled && !socket.is_open(), "server TLS read was not canceled");
}
pace::task<void> waiting_tls_client(pace::io_context& io, const pace::tls::context& config,
                                   pace::tcp::endpoint endpoint, bool& rejected, bool& canceled) {
    pace::tls::stream socket(io, config);
    co_await socket.connect(std::move(endpoint), "localhost");
    io.spawn(conflicting_reader(socket, rejected));
    try {
        char byte;
        co_await socket.read_some(std::span<char>(&byte, 1));
    } catch (const std::system_error& error) { canceled = error.code() == std::errc::operation_canceled; }
    require(canceled && !socket.is_open(), "client TLS read was not canceled");
}
void read_cancellation_and_serialization(const std::filesystem::path& fixtures) {
    pace::io_context io;
    pace::tls::context server(pace::tls::role::server), client;
    server.use_certificate_chain((fixtures / "localhost.pem").string(), (fixtures / "localhost-key.pem").string());
    client.load_verify_file((fixtures / "ca.pem").string());
    pace::tcp::listener listener(io, {"127.0.0.1", 0});
    bool server_canceled = false, client_canceled = false, rejected = false;
    io.spawn(waiting_tls_server(listener, server, server_canceled));
    io.spawn(waiting_tls_client(io, client, listener.local_endpoint(), rejected, client_canceled));
    io.run();
    require(server_canceled && client_canceled && rejected, "TLS active operation contract failed");
}

pace::task<void> shutdown_peer(pace::tcp::listener& listener, const pace::tls::context& config,
                               bool& client_finished, const bool& shutdown_started, bool cancel_shutdown) {
    pace::tls::stream socket(co_await listener.accept(), config);
    co_await socket.handshake();
    // Send a record so the client knows both handshakes have completed.
    co_await socket.write_all("ready");
    if (cancel_shutdown) {
        while (!shutdown_started) co_await listener.context().sleep_for(1ms);
        co_await listener.context().sleep_for(5ms);
        listener.context().stop();
    } else {
        // Deliberately do not consume/respond to close_notify.
        while (!client_finished) co_await listener.context().sleep_for(1ms);
        listener.context().stop();
    }
}
pace::task<void> bounded_shutdown_client(pace::io_context& io, const pace::tls::context& config,
                                        pace::tcp::endpoint endpoint, bool& finished, bool& shutdown_started,
                                        bool cancel_shutdown) {
    pace::tls::stream socket(io, config);
    co_await socket.connect(std::move(endpoint), "localhost");
    std::array<char, 5> ready{};
    std::size_t offset = 0;
    while (offset < ready.size()) {
        const auto count = co_await socket.read_some(std::span<char>(ready).subspan(offset));
        require(count > 0, "shutdown peer closed before synchronization");
        offset += count;
    }
    require(std::string_view(ready.data(), ready.size()) == "ready", "shutdown synchronization record mismatch");
    shutdown_started = true;
    bool canceled = false;
    try { co_await socket.shutdown(cancel_shutdown ? 1h : 3ms); }
    catch (const std::system_error& error) { canceled = error.code() == std::errc::operation_canceled; }
    require(canceled == cancel_shutdown && !socket.is_open(), "bounded/canceled TLS shutdown did not close");
    finished = true;
}
void shutdown_bounds(const std::filesystem::path& fixtures, bool cancel_shutdown) {
    pace::io_context io;
    pace::tls::context server(pace::tls::role::server), client;
    server.use_certificate_chain((fixtures / "localhost.pem").string(), (fixtures / "localhost-key.pem").string());
    client.load_verify_file((fixtures / "ca.pem").string());
    pace::tcp::listener listener(io, {"127.0.0.1", 0});
    bool finished = false, shutdown_started = false;
    io.spawn(shutdown_peer(listener, server, finished, shutdown_started, cancel_shutdown));
    io.spawn(bounded_shutdown_client(io, client, listener.local_endpoint(), finished, shutdown_started, cancel_shutdown));
    io.run();
    require(finished, "TLS bounded shutdown failed to finish");
}

pace::task<void> missing_peer_name(pace::io_context& io, const pace::tls::context& config) {
    pace::tls::stream socket(io, config);
    bool rejected = false;
    try { co_await socket.connect({"127.0.0.1", 1}, ""); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && !socket.is_open(), "anonymous TLS client must be rejected before connecting");
    io.stop();
}
void configuration_errors(const std::filesystem::path& fixtures) {
    pace::tls::context config;
    bool rejected = false;
    try { config.set_alpn_protocols({""}); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "empty ALPN identifier must reject");
    rejected = false;
    try { config.load_verify_file((fixtures / "missing-ca.pem").string()); }
    catch (const std::system_error&) { rejected = true; }
    require(rejected, "missing trust file must reject");
    pace::io_context io;
    io.spawn(missing_peer_name(io, config));
    io.run();
}
} // namespace

int main(int argc, char** argv) {
    watchdog guard;
    try {
        const auto fixtures = argc > 1 ? std::filesystem::path(argv[1]) :
            std::filesystem::path(__FILE__).parent_path() / "certs";
        verified_roundtrip(fixtures, "localhost");
        verified_roundtrip(fixtures, "127.0.0.1");
        rejected_handshake(fixtures, "wrong.example", true, false, false);
        rejected_handshake(fixtures, "localhost", false, false, false);
        rejected_handshake(fixtures, "localhost", true, true, false);
        rejected_handshake(fixtures, "localhost", true, false, true);
        handshake_cancellation();
        truncation(fixtures);
        read_cancellation_and_serialization(fixtures);
        shutdown_bounds(fixtures, false);
        shutdown_bounds(fixtures, true);
        configuration_errors(fixtures);
        std::cout << "TLS tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
