#include <pace/quic.hpp>

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
struct completion {
    pace::io_context& io;
    unsigned count = 0, expected = 2;
    void arrive() { if (++count == expected) io.stop(); }
};
struct configurations {
    pace::quic::context server{pace::quic::role::server}, client;
    explicit configurations(const std::filesystem::path& fixtures) {
        server.use_certificate_chain((fixtures / "localhost.pem").string(), (fixtures / "localhost-key.pem").string());
        client.load_verify_file((fixtures / "ca.pem").string());
        server.set_alpn_protocols({"pace-test", "fallback"});
        client.set_alpn_protocols({"fallback", "pace-test"});
    }
};
pace::task<std::string> read_to_end(pace::quic::stream& stream) {
    std::string result;
    std::array<char, 8191> buffer{};
    while (auto count = co_await stream.read_some(buffer)) {
        result.append(buffer.data(), count);
        require(result.size() <= 512 * 1024, "QUIC test message grew without bound");
    }
    co_return result;
}
std::string payload(unsigned index) {
    std::string result(192 * 1024 + 7 + index, '\0');
    for (std::size_t i = 0; i < result.size(); ++i) result[i] = static_cast<char>((i * 31 + index) % 251);
    return result;
}
pace::task<void> echo_stream(pace::quic::stream stream, completion& done) {
    require(!stream.is_unidirectional() && !stream.is_local(), "server accepted wrong stream direction");
    auto message = co_await read_to_end(stream);
    require(message.size() >= 192 * 1024, "short QUIC binary request");
    co_await stream.write_all(message);
    stream.finish_write();
    // Retain the transport until its bytes have actually been acknowledged.
    while (stream.pending_write_bytes()) co_await stream.wait_events();
    done.arrive();
}
pace::task<void> request_stream(pace::quic::stream stream, unsigned index, completion& done) {
    require(stream.is_local() && !stream.is_unidirectional(), "client stream metadata mismatch");
    auto message = payload(index);
    co_await stream.write_all(message);
    stream.finish_write();
    require(co_await read_to_end(stream) == message, "QUIC binary stream roundtrip mismatch");
    done.arrive();
}
pace::task<void> server_roundtrip(pace::quic::listener& listener, completion& done) {
    auto connection = co_await listener.accept();
    require(connection.selected_protocol() == "pace-test", "server ALPN preference not applied");
    std::optional<pace::quic::stream> uni;
    unsigned ids = 0;
    for (unsigned i = 0; i < 5; ++i) {
        auto stream = co_await connection.accept_stream();
        if (stream.is_unidirectional()) { require(!uni, "duplicate uni stream"); uni.emplace(std::move(stream)); }
        else {
            require(stream.id() < 16 && stream.id() % 4 == 0 && !(ids & (1U << (stream.id() / 4))), "unexpected/duplicate client bidirectional stream ID");
            ids |= 1U << (stream.id() / 4);
            listener.context().spawn(echo_stream(std::move(stream), done));
        }
    }
    require(uni && ids == 15 && !uni->is_local(), "missing concurrent streams");
    require(co_await read_to_end(*uni) == "client uni", "unidirectional request mismatch");
    auto reply = co_await connection.open_stream(pace::quic::stream_type::unidirectional);
    co_await reply.write_all("server uni"); reply.finish_write();
    while (reply.pending_write_bytes()) co_await reply.wait_events();
    done.arrive();
}
pace::task<void> client_roundtrip(pace::quic::connection& connection,
                                  pace::udp::endpoint endpoint, std::string name, completion& done) {
    auto& io = connection.context();
    co_await connection.connect(endpoint, std::move(name));
    require(connection.selected_protocol() == "pace-test", "client ALPN mismatch");
    for (unsigned i = 0; i < 4; ++i)
        io.spawn(request_stream(co_await connection.open_stream(), i, done));
    auto stream = co_await connection.open_stream(pace::quic::stream_type::unidirectional);
    co_await stream.write_all("client uni"); stream.finish_write();
    auto reply = co_await connection.accept_stream();
    require(reply.is_unidirectional() && !reply.is_local(), "missing server uni stream");
    require(co_await read_to_end(reply) == "server uni", "unidirectional response mismatch");
    done.arrive();
}
void roundtrip(const std::filesystem::path& fixtures, std::string name, const char* bind = "127.0.0.1") {
    pace::io_context io; configurations config(fixtures);
    if (name == "::1") {
        config.server.use_certificate_chain((fixtures / "a.example.pem").string(), (fixtures / "a.example-key.pem").string());
        config.client.load_verify_file((fixtures / "sni-ca.pem").string());
    }
    pace::quic::listener listener(io, {bind, 0}, config.server);
    pace::quic::connection client(io, config.client);
    completion done{io, 0, 10};
    io.spawn(server_roundtrip(listener, done));
    io.spawn(client_roundtrip(client, listener.local_endpoint(), std::move(name), done));
    io.run(); require(done.count == 10, "QUIC concurrent streams did not complete");
}
pace::task<void> accept_failure(pace::quic::listener& listener) {
    try { auto connection = co_await listener.accept(100ms); }
    catch (const std::system_error&) {} // Peer rejection or stop must unwind.
}
pace::task<void> rejecting_client(pace::io_context& io, const pace::quic::context& config,
                                  pace::udp::endpoint endpoint, std::string name, bool& rejected) {
    pace::quic::connection connection(io, config);
    try { co_await connection.connect(endpoint, std::move(name), 200ms); }
    catch (const std::system_error& error) {
        rejected = error.code() == std::errc::protocol_error || error.code() == std::errc::connection_aborted;
    }
    require(rejected && !connection.is_open(), "unverified QUIC peer was accepted or left open");
    io.stop();
}
void rejects(const std::filesystem::path& fixtures, std::string name, bool trusted, bool expired, bool alpn,
             const char* bind = "127.0.0.1") {
    pace::io_context io;
    pace::quic::context server(pace::quic::role::server), client;
    server.use_certificate_chain((fixtures / (expired ? "expired.pem" : "localhost.pem")).string(), (fixtures / "localhost-key.pem").string());
    if (trusted) client.load_verify_file((fixtures / "ca.pem").string());
    if (alpn) client.set_alpn_protocols({"unsupported-protocol"});
    pace::quic::listener listener(io, {bind, 0}, server);
    bool rejected = false;
    io.spawn(accept_failure(listener));
    io.spawn(rejecting_client(io, client, listener.local_endpoint(), std::move(name), rejected));
    io.run(); require(rejected, "QUIC failed to reject bad credentials");
}
pace::task<void> stop_soon(pace::io_context& io) { co_await io.sleep_for(5ms); io.stop(); }
pace::task<void> no_reply(pace::io_context& io, pace::udp::endpoint endpoint, bool cancel, bool& completed) {
    pace::quic::context config; pace::quic::connection connection(io, config);
    try { co_await connection.connect(endpoint, "localhost", cancel ? 1h : 15ms); }
    catch (const std::system_error& error) {
        completed = error.code() == (cancel ? std::errc::operation_canceled : std::errc::timed_out);
    }
    require(completed && !connection.is_open(), "QUIC handshake timeout/cancellation failed");
    io.stop();
}
void deadlines(bool cancel) {
    pace::io_context io; pace::udp::socket blackhole(io, {"127.0.0.1", 0}); bool done = false;
    io.spawn(no_reply(io, blackhole.local_endpoint(), cancel, done));
    if (cancel) io.spawn(stop_soon(io));
    io.run(); require(done, "QUIC deadline operation did not unwind");
}
pace::task<void> closing_server(pace::quic::listener& listener, bool close_listener) {
    auto connection = co_await listener.accept();
    auto stream = co_await connection.accept_stream();
    std::array<char, 1> byte{};
    require(co_await stream.read_some(byte) == 1, "QUIC close synchronization failed");
    if (close_listener) {
        listener.close();
        bool canceled = false;
        try { co_await stream.read_some(byte); }
        catch (const std::system_error& e) { canceled = e.code() == std::errc::operation_canceled; }
        require(canceled && !connection.is_open(), "listener close must invalidate accepted streams");
        listener.context().stop();
    } else connection.close(42);
}
pace::task<void> closed_client(pace::io_context& io, const pace::quic::context& config,
                              pace::udp::endpoint endpoint, bool listener_closed, bool& done) {
    pace::quic::connection connection(io, config);
    co_await connection.connect(endpoint, "localhost");
    auto stream = co_await connection.open_stream(); co_await stream.write_all("x");
    try { char byte; co_await stream.read_some(std::span<char>(&byte, 1)); }
    catch (const std::system_error& e) {
        done = e.code() == (listener_closed ? std::errc::operation_canceled : std::errc::connection_aborted);
    }
    require(done, "QUIC close did not cancel pending read"); io.stop();
}
void closure(const std::filesystem::path& fixtures, bool listener_closed) {
    pace::io_context io; configurations config(fixtures);
    pace::quic::listener listener(io, {"127.0.0.1", 0}, config.server); bool done = false;
    io.spawn(closing_server(listener, listener_closed));
    io.spawn(closed_client(io, config.client, listener.local_endpoint(), listener_closed, done));
    io.run(); require(done, "QUIC close test did not unwind");
}
pace::task<void> conflicting_reader(pace::quic::stream& stream, bool& rejected) {
    std::array<char, 1> bytes{};
    try { co_await stream.read_some(bytes); }
    catch (const std::logic_error&) { rejected = true; }
    require(rejected, "overlapping QUIC stream reads must reject");
    stream.reset(7);
}
pace::task<void> reset_client(pace::io_context& io, const pace::quic::context& config,
                              pace::udp::endpoint endpoint, bool& rejected, bool& canceled) {
    pace::quic::connection connection(io, config); co_await connection.connect(endpoint, "localhost");
    auto stream = co_await connection.open_stream(); co_await stream.write_all("x");
    io.spawn(conflicting_reader(stream, rejected));
    try { char byte; co_await stream.read_some(std::span<char>(&byte, 1)); }
    catch (const std::system_error& error) { canceled = error.code() == std::errc::operation_canceled; }
    require(canceled, "QUIC stream reset did not unwind its pending read"); io.stop();
}
pace::task<void> waiting_server(pace::quic::listener& listener) {
    try {
        auto connection = co_await listener.accept(); auto stream = co_await connection.accept_stream();
        co_await read_to_end(stream);
    } catch (const std::system_error&) {}
}
void serialization_and_reset(const std::filesystem::path& fixtures) {
    pace::io_context io; configurations config(fixtures);
    pace::quic::listener listener(io, {"127.0.0.1", 0}, config.server); bool rejected = false, canceled = false;
    io.spawn(waiting_server(listener));
    io.spawn(reset_client(io, config.client, listener.local_endpoint(), rejected, canceled));
    io.run(); require(rejected && canceled, "QUIC operation serialization failed");
}
pace::task<void> isolated_client(pace::quic::connection& connection, pace::udp::endpoint endpoint,
                                bool first, completion& done);
pace::task<void> isolated_server(pace::quic::listener& listener, pace::quic::connection& second_client, completion& done) {
    auto first = co_await listener.accept();
    auto first_stream = co_await first.accept_stream();
    require(co_await read_to_end(first_stream) == "first", "first connection sync failed");
    listener.context().spawn(isolated_client(second_client, listener.local_endpoint(), false, done));
    auto second = co_await listener.accept();
    first.close(9);
    require(second.is_open() && listener.is_open(), "closing a peer closed the shared listener");
    auto stream = co_await second.accept_stream();
    require(co_await read_to_end(stream) == "second", "second connection lost after first close");
    co_await stream.write_all("survived"); stream.finish_write();
    while (stream.pending_write_bytes()) co_await stream.wait_events();
    done.arrive();
}
pace::task<void> isolated_client(pace::quic::connection& connection, pace::udp::endpoint endpoint,
                                bool first, completion& done) {
    co_await connection.connect(endpoint, "localhost");
    auto stream = co_await connection.open_stream();
    co_await stream.write_all(first ? "first" : "second"); stream.finish_write();
    if (first) {
        bool closed = false;
        try { co_await read_to_end(stream); }
        catch (const std::system_error& error) { closed = error.code() == std::errc::connection_aborted; }
        require(closed, "first connection did not receive close");
    } else require(co_await read_to_end(stream) == "survived", "sibling connection failed");
    done.arrive();
}
void connection_isolation(const std::filesystem::path& fixtures) {
    pace::io_context io; configurations config(fixtures);
    pace::quic::listener listener(io, {"127.0.0.1", 0}, config.server);
    pace::quic::connection first(io, config.client), second(io, config.client);
    completion done{io, 0, 3};
    io.spawn(isolated_server(listener, second, done));
    io.spawn(isolated_client(first, listener.local_endpoint(), true, done));
    io.run(); require(done.count == 3, "shared-listener isolation did not complete");
}
pace::task<void> close_acceptor(pace::quic::listener& listener) {
    co_await listener.context().yield(); listener.close();
}
pace::task<void> pending_accept(pace::quic::listener& listener, bool& canceled) {
    try { auto connection = co_await listener.accept(); }
    catch (const std::system_error& error) { canceled = error.code() == std::errc::operation_canceled; }
    require(canceled && !listener.is_open(), "listener close did not cancel pending accept");
    listener.context().stop();
}
void accept_cancellation(const std::filesystem::path& fixtures) {
    pace::io_context io; configurations config(fixtures);
    pace::quic::listener listener(io, {"127.0.0.1", 0}, config.server); bool canceled = false;
    io.spawn(pending_accept(listener, canceled)); io.spawn(close_acceptor(listener));
    io.run(); require(canceled, "QUIC accept failed to unwind");
}
} // namespace
int main(int argc, char** argv) {
    watchdog guard;
    try {
        const auto fixtures = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path(__FILE__).parent_path() / "certs";
        roundtrip(fixtures, "localhost");
        roundtrip(fixtures, "127.0.0.1");
        roundtrip(fixtures, "localhost", "::1");
        roundtrip(fixtures, "::1", "::1");
        rejects(fixtures, "::1", true, false, false, "::1");
        rejects(fixtures, "wrong.example", true, false, false);
        rejects(fixtures, "localhost", false, false, false);
        rejects(fixtures, "localhost", true, true, false);
        rejects(fixtures, "localhost", true, false, true);
        deadlines(false); deadlines(true);
        closure(fixtures, false); closure(fixtures, true);
        serialization_and_reset(fixtures);
        connection_isolation(fixtures); accept_cancellation(fixtures);
        std::cout << "QUIC tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
