#include <pace/quic/connection.hpp>
#include <pace/runtime/operation.hpp>
#include "support/pace_test.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>

using namespace std::chrono_literals;

namespace
{
using pace::test::require;

std::filesystem::path certificates()
{
    return pace::test::certificates();
}

// Interop peer mode runs outside doctest and its per-test-case watchdog, so the
// peer keeps its own bound: abort when one exchange takes longer than 60 s.
struct watchdog
{
    const char* scenario;
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    std::thread thread{[this]
                       {
                           std::unique_lock lock(mutex);
                           if (!changed.wait_for(lock, 60s, [&] { return done; }))
                           {
                               std::cerr << "QUIC watchdog timed out in " << scenario << std::endl;
                               std::abort();
                           }
                       }};

    explicit watchdog(const char* name)
        : scenario(name)
    {
    }

    ~watchdog()
    {
        {
            std::lock_guard lock(mutex);
            done = true;
        }
        changed.notify_one();
        thread.join();
        if (std::uncaught_exceptions())
        {
            std::cerr << "QUIC scenario failed: " << scenario << '\n';
        }
    }
};

struct completion
{
    pace::io_context& io;
    unsigned count = 0, expected = 2;

    void arrive()
    {
        if (++count == expected)
        {
            io.stop();
        }
    }
};

struct configurations
{
    pace::quic::context server{pace::quic::role::server}, client;

    explicit configurations(const std::filesystem::path& fixtures)
    {
        server.use_certificate_chain((fixtures / "localhost.pem").string(), (fixtures / "localhost-key.pem").string());
        client.load_verify_file((fixtures / "ca.pem").string());
        server.set_alpn_protocols({"pace-test", "fallback"});
        client.set_alpn_protocols({"fallback", "pace-test"});
    }
};

pace::task<std::string> read_to_end(pace::quic::stream& stream)
{
    std::string result;
    std::array<char, 8191> buffer{};
    while (auto count = co_await stream.read_some(pace::as_writable_bytes(buffer)))
    {
        result.append(buffer.data(), count);
        require(result.size() <= 512 * 1024, "QUIC test message grew without bound");
    }
    co_return result;
}

std::string payload(unsigned index)
{
    std::string result(192 * 1024 + 7 + index, '\0');
    for (std::size_t i = 0; i < result.size(); ++i)
    {
        result[i] = static_cast<char>((i * 31 + index) % 251);
    }
    return result;
}

pace::task<void> echo_stream(pace::quic::stream stream, completion& done)
{
    require(!stream.is_unidirectional() && !stream.is_local(), "server accepted wrong stream direction");
    auto message = co_await read_to_end(stream);
    require(message.size() >= 192 * 1024, "short QUIC binary request");
    co_await stream.write_all(pace::as_bytes(message));
    stream.finish_write();
    // Retain the transport until its bytes have actually been acknowledged.
    while (stream.pending_write_bytes())
    {
        co_await stream.wait_events();
    }
    done.arrive();
}

pace::task<void> request_stream(pace::quic::stream stream, unsigned index, completion& done)
{
    require(stream.is_local() && !stream.is_unidirectional(), "client stream metadata mismatch");
    auto message = payload(index);
    co_await stream.write_all(pace::as_bytes(message));
    stream.finish_write();
    require(co_await read_to_end(stream) == message, "QUIC binary stream roundtrip mismatch");
    done.arrive();
}

pace::task<void> server_roundtrip(pace::quic::listener& listener, completion& done)
{
    auto connection = co_await listener.accept();
    require(connection.selected_protocol() == "pace-test", "server ALPN preference not applied");
    std::optional<pace::quic::stream> uni;
    unsigned ids = 0;
    for (unsigned i = 0; i < 5; ++i)
    {
        auto stream = co_await connection.accept_stream();
        if (stream.is_unidirectional())
        {
            require(!uni, "duplicate uni stream");
            uni.emplace(std::move(stream));
        }
        else
        {
            require(
                stream.id() < 16 && stream.id() % 4 == 0 && !(ids & (1U << (stream.id() / 4))),
                "unexpected/duplicate client bidirectional stream ID"
            );
            ids |= 1U << (stream.id() / 4);
            listener.context().spawn(echo_stream(std::move(stream), done));
        }
    }
    require(uni && ids == 15 && !uni->is_local(), "missing concurrent streams");
    require(co_await read_to_end(*uni) == "client uni", "unidirectional request mismatch");
    auto reply = co_await connection.open_stream(pace::quic::stream_type::unidirectional);
    co_await reply.write_all(pace::as_bytes("server uni"));
    reply.finish_write();
    while (reply.pending_write_bytes())
    {
        co_await reply.wait_events();
    }
    done.arrive();
}

pace::task<void> client_roundtrip(
    pace::quic::connection& connection,
    pace::udp::endpoint endpoint,
    std::string name,
    completion& done
)
{
    auto& io = connection.context();
    co_await connection.connect(endpoint, std::move(name));
    require(connection.selected_protocol() == "pace-test", "client ALPN mismatch");
    for (unsigned i = 0; i < 4; ++i)
    {
        io.spawn(request_stream(co_await connection.open_stream(), i, done));
    }
    auto stream = co_await connection.open_stream(pace::quic::stream_type::unidirectional);
    co_await stream.write_all(pace::as_bytes("client uni"));
    stream.finish_write();
    auto reply = co_await connection.accept_stream();
    require(reply.is_unidirectional() && !reply.is_local(), "missing server uni stream");
    require(co_await read_to_end(reply) == "server uni", "unidirectional response mismatch");
    done.arrive();
}

pace::task<void> lossy_relay(pace::udp::socket& socket, pace::udp::endpoint server, unsigned& dropped)
{
    std::array<char, 65536> bytes{};
    std::optional<pace::udp::endpoint> client;
    unsigned to_server = 0, to_client = 0;
    try
    {
        for (;;)
        {
            auto packet = co_await socket.receive_from(pace::as_writable_bytes(bytes));
            const bool reply = packet.sender.port == server.port;
            if (!reply)
            {
                client = packet.sender;
            }
            auto& count = reply ? to_client : to_server;
            ++count;
            // Drop the first Initial to exercise PTO, then lose application
            // packets in both directions to exercise retransmission/ACK storage.
            if ((!reply && count == 1) || count % (reply ? 19 : 17) == 0)
            {
                ++dropped;
                continue;
            }
            if (reply && !client)
            {
                continue;
            }
            co_await socket.send_to(
                pace::as_bytes(std::string_view(bytes.data(), packet.size)),
                reply ? *client : server
            );
        }
    }
    catch (const std::system_error& error)
    {
        if (error.code() != std::errc::operation_canceled)
        {
            throw;
        }
    }
}

void roundtrip(
    const std::filesystem::path& fixtures,
    std::string name,
    const char* bind = "127.0.0.1",
    bool loss = false
)
{
    pace::io_context io;
    configurations config(fixtures);
    if (name == "::1")
    {
        config.server.use_certificate_chain(
            (fixtures / "a.example.pem").string(),
            (fixtures / "a.example-key.pem").string()
        );
        config.client.load_verify_file((fixtures / "sni-ca.pem").string());
    }
    pace::quic::listener listener(io, {bind, 0}, config.server);
    pace::quic::connection client(io, config.client);
    completion done{io, 0, 10};
    pace::udp::socket relay(io, {bind, 0});
    unsigned dropped = 0;
    if (loss)
    {
        io.spawn(lossy_relay(relay, listener.local_endpoint(), dropped));
    }
    io.spawn(server_roundtrip(listener, done));
    io.spawn(
        client_roundtrip(client, loss ? relay.local_endpoint() : listener.local_endpoint(), std::move(name), done)
    );
    io.run();
    require(done.count == 10, "QUIC concurrent streams did not complete");
    if (loss)
    {
        require(dropped > 10, "loss simulation did not exercise retransmission");
    }
}

pace::task<void> accept_failure(pace::quic::listener& listener)
{
    try
    {
        auto connection = co_await listener.accept(2s);
    }
    catch (const std::system_error&)
    {
    } // Peer rejection or stop must unwind.
}

pace::task<void> rejecting_client(
    pace::io_context& io,
    const pace::quic::context& config,
    pace::udp::endpoint endpoint,
    std::string name,
    bool& rejected
)
{
    pace::quic::connection connection(io, config);
    try
    {
        co_await connection.connect(endpoint, std::move(name), 2s);
    }
    catch (const std::system_error& error)
    {
        rejected = error.code() == std::errc::protocol_error || error.code() == std::errc::connection_aborted;
    }
    require(rejected && !connection.is_open(), "unverified QUIC peer was accepted or left open");
    io.stop();
}

void rejects(
    const std::filesystem::path& fixtures,
    std::string name,
    bool trusted,
    bool expired,
    bool alpn,
    const char* bind = "127.0.0.1"
)
{
    pace::io_context io;
    pace::quic::context server(pace::quic::role::server), client;
    server.use_certificate_chain(
        (fixtures / (expired ? "expired.pem" : "localhost.pem")).string(),
        (fixtures / "localhost-key.pem").string()
    );
    if (trusted)
    {
        client.load_verify_file((fixtures / "ca.pem").string());
    }
    if (alpn)
    {
        client.set_alpn_protocols({"unsupported-protocol"});
    }
    pace::quic::listener listener(io, {bind, 0}, server);
    bool rejected = false;
    io.spawn(accept_failure(listener));
    io.spawn(rejecting_client(io, client, listener.local_endpoint(), std::move(name), rejected));
    io.run();
    require(rejected, "QUIC failed to reject bad credentials");
}

pace::task<void> stop_soon(pace::io_context& io)
{
    co_await io.sleep_for(5ms);
    io.stop();
}

pace::task<void> no_reply(pace::io_context& io, pace::udp::endpoint endpoint, bool cancel, bool& completed)
{
    pace::quic::context config;
    pace::quic::connection connection(io, config);
    try
    {
        co_await connection.connect(endpoint, "localhost", cancel ? 1h : 15ms);
    }
    catch (const std::system_error& error)
    {
        completed = error.code() == (cancel ? std::errc::operation_canceled : std::errc::timed_out);
    }
    require(completed && !connection.is_open(), "QUIC handshake timeout/cancellation failed");
    io.stop();
}

void deadlines(bool cancel)
{
    pace::io_context io;
    pace::udp::socket blackhole(io, {"127.0.0.1", 0});
    bool done = false;
    io.spawn(no_reply(io, blackhole.local_endpoint(), cancel, done));
    if (cancel)
    {
        io.spawn(stop_soon(io));
    }
    io.run();
    require(done, "QUIC deadline operation did not unwind");
}

pace::task<void> closing_server(pace::quic::listener& listener, bool close_listener)
{
    auto connection = co_await listener.accept();
    auto stream = co_await connection.accept_stream();
    std::array<char, 1> byte{};
    require(co_await stream.read_some(pace::as_writable_bytes(byte)) == 1, "QUIC close synchronization failed");
    if (close_listener)
    {
        listener.close();
        bool canceled = false;
        try
        {
            co_await stream.read_some(pace::as_writable_bytes(byte));
        }
        catch (const std::system_error& e)
        {
            canceled = e.code() == std::errc::operation_canceled;
        }
        require(canceled && !connection.is_open(), "listener close must invalidate accepted streams");
        listener.context().stop();
    }
    else
    {
        connection.close(42);
    }
}

pace::task<void> closed_client(
    pace::io_context& io,
    const pace::quic::context& config,
    pace::udp::endpoint endpoint,
    bool listener_closed,
    bool& done
)
{
    pace::quic::connection connection(io, config);
    co_await connection.connect(endpoint, "localhost");
    auto stream = co_await connection.open_stream();
    co_await stream.write_all(pace::as_bytes("x"));
    try
    {
        char byte;
        co_await stream.read_some(pace::as_writable_bytes(std::span<char>(&byte, 1)));
    }
    catch (const std::system_error& e)
    {
        done = e.code() == (listener_closed ? std::errc::operation_canceled : std::errc::connection_aborted);
    }
    require(done, "QUIC close did not cancel pending read");
    io.stop();
}

void closure(const std::filesystem::path& fixtures, bool listener_closed)
{
    pace::io_context io;
    configurations config(fixtures);
    pace::quic::listener listener(io, {"127.0.0.1", 0}, config.server);
    bool done = false;
    io.spawn(closing_server(listener, listener_closed));
    io.spawn(closed_client(io, config.client, listener.local_endpoint(), listener_closed, done));
    io.run();
    require(done, "QUIC close test did not unwind");
}

pace::task<void> conflicting_reader(pace::quic::stream& stream, bool& rejected)
{
    std::array<char, 1> bytes{};
    try
    {
        co_await stream.read_some(pace::as_writable_bytes(bytes));
    }
    catch (const std::logic_error&)
    {
        rejected = true;
    }
    require(rejected, "overlapping QUIC stream reads must reject");
    stream.reset(7);
}

pace::task<void> reset_client(
    pace::io_context& io,
    const pace::quic::context& config,
    pace::udp::endpoint endpoint,
    bool& rejected,
    bool& canceled
)
{
    pace::quic::connection connection(io, config);
    co_await connection.connect(endpoint, "localhost");
    auto stream = co_await connection.open_stream();
    co_await stream.write_all(pace::as_bytes("x"));
    io.spawn(conflicting_reader(stream, rejected));
    try
    {
        char byte;
        co_await stream.read_some(pace::as_writable_bytes(std::span<char>(&byte, 1)));
    }
    catch (const std::system_error& error)
    {
        canceled = error.code() == std::errc::operation_canceled;
    }
    require(canceled, "QUIC stream reset did not unwind its pending read");
    io.stop();
}

pace::task<void> waiting_server(pace::quic::listener& listener)
{
    try
    {
        auto connection = co_await listener.accept();
        auto stream = co_await connection.accept_stream();
        co_await read_to_end(stream);
    }
    catch (const std::system_error&)
    {
    }
}

void serialization_and_reset(const std::filesystem::path& fixtures)
{
    pace::io_context io;
    configurations config(fixtures);
    pace::quic::listener listener(io, {"127.0.0.1", 0}, config.server);
    bool rejected = false, canceled = false;
    io.spawn(waiting_server(listener));
    io.spawn(reset_client(io, config.client, listener.local_endpoint(), rejected, canceled));
    io.run();
    require(rejected && canceled, "QUIC operation serialization failed");
}

pace::task<void> isolated_client(
    pace::quic::connection& connection,
    pace::udp::endpoint endpoint,
    bool first,
    completion& done
);

pace::task<void> isolated_server(
    pace::quic::listener& listener,
    pace::quic::connection& second_client,
    completion& done
)
{
    auto first = co_await listener.accept();
    auto first_stream = co_await first.accept_stream();
    require(co_await read_to_end(first_stream) == "first", "first connection sync failed");
    listener.context().spawn(isolated_client(second_client, listener.local_endpoint(), false, done));
    auto second = co_await listener.accept();
    first.close(9);
    require(second.is_open() && listener.is_open(), "closing a peer closed the shared listener");
    auto stream = co_await second.accept_stream();
    require(co_await read_to_end(stream) == "second", "second connection lost after first close");
    co_await stream.write_all(pace::as_bytes("survived"));
    stream.finish_write();
    while (stream.pending_write_bytes())
    {
        co_await stream.wait_events();
    }
    done.arrive();
}

pace::task<void> isolated_client(
    pace::quic::connection& connection,
    pace::udp::endpoint endpoint,
    bool first,
    completion& done
)
{
    co_await connection.connect(endpoint, "localhost");
    auto stream = co_await connection.open_stream();
    co_await stream.write_all(pace::as_bytes(first ? "first" : "second"));
    stream.finish_write();
    if (first)
    {
        bool closed = false;
        try
        {
            co_await read_to_end(stream);
        }
        catch (const std::system_error& error)
        {
            closed = error.code() == std::errc::connection_aborted;
        }
        require(closed, "first connection did not receive close");
    }
    else
    {
        require(co_await read_to_end(stream) == "survived", "sibling connection failed");
    }
    done.arrive();
}

void connection_isolation(const std::filesystem::path& fixtures)
{
    pace::io_context io;
    configurations config(fixtures);
    pace::quic::listener listener(io, {"127.0.0.1", 0}, config.server);
    pace::quic::connection first(io, config.client), second(io, config.client);
    completion done{io, 0, 3};
    io.spawn(isolated_server(listener, second, done));
    io.spawn(isolated_client(first, listener.local_endpoint(), true, done));
    io.run();
    require(done.count == 3, "shared-listener isolation did not complete");
}

pace::task<void> close_acceptor(pace::quic::listener& listener)
{
    co_await listener.context().yield();
    listener.close();
}

pace::task<void> pending_accept(pace::quic::listener& listener, bool& canceled)
{
    try
    {
        auto connection = co_await listener.accept();
    }
    catch (const std::system_error& error)
    {
        canceled = error.code() == std::errc::operation_canceled;
    }
    require(canceled && !listener.is_open(), "listener close did not cancel pending accept");
    listener.context().stop();
}

pace::task<void> local_accept_deadline(pace::quic::listener& listener)
{
    bool timed_out = false;
    try
    {
        co_await pace::with_deadline(pace::deadline::after(5ms), [&]() { return listener.accept(); });
    }
    catch (const std::system_error& error)
    {
        timed_out = error.code() == std::errc::timed_out;
    }
    require(timed_out && listener.is_open(), "local deadline must unwind only the pending QUIC accept");
    listener.context().stop();
}

void local_cancellation(const std::filesystem::path& fixtures)
{
    pace::io_context io;
    configurations config(fixtures);
    pace::quic::listener listener(io, {"127.0.0.1", 0}, config.server);
    io.spawn(local_accept_deadline(listener));
    io.run();
}

void accept_cancellation(const std::filesystem::path& fixtures)
{
    pace::io_context io;
    configurations config(fixtures);
    pace::quic::listener listener(io, {"127.0.0.1", 0}, config.server);
    bool canceled = false;
    io.spawn(pending_accept(listener, canceled));
    io.spawn(close_acceptor(listener));
    io.run();
    require(canceled, "QUIC accept failed to unwind");
}

pace::task<void> recycle_server(
    pace::quic::listener& listener,
    std::optional<pace::quic::connection>& peer,
    completion& done
)
{
    auto& connection = peer.emplace(co_await listener.accept());
    for (unsigned i = 0; i < 160; ++i)
    {
        auto stream = co_await connection.accept_stream();
        require(
            co_await read_to_end(stream) == std::string(48 * 1024, static_cast<char>(i)),
            "recycled stream data mismatch"
        );
    }
    done.arrive();
}

pace::task<void> recycle_client(pace::quic::connection& connection, pace::udp::endpoint endpoint, completion& done)
{
    co_await connection.connect(endpoint, "localhost");
    for (unsigned i = 0; i < 160; ++i)
    {
        auto stream = co_await connection.open_stream(pace::quic::stream_type::unidirectional);
        const std::string message(48 * 1024, static_cast<char>(i));
        co_await stream.write_all(pace::as_bytes(message));
        stream.finish_write();
        while (stream.pending_write_bytes())
        {
            co_await stream.wait_events();
        }
    }
    done.arrive();
}

void recycle_streams(const std::filesystem::path& fixtures)
{
    pace::io_context io;
    configurations config(fixtures);
    pace::quic::listener listener(io, {"127.0.0.1", 0}, config.server);
    pace::quic::connection client(io, config.client);
    completion done{io};
    // The last stream can reach EOF before the peer receives its final ACK.
    // Keep both transports alive until the two tasks have finished.
    std::optional<pace::quic::connection> server;
    io.spawn(recycle_server(listener, server, done));
    io.spawn(recycle_client(client, listener.local_endpoint(), done));
    io.run();
    require(done.count == 2, "stream or connection flow-control credit was not recycled");
}

pace::task<void> interop_server(pace::quic::listener& listener)
{
    auto connection = co_await listener.accept();
    auto stream = co_await connection.accept_stream();
    auto message = co_await read_to_end(stream);
    require(message == payload(0), "cross-backend request mismatch");
    co_await stream.write_all(pace::as_bytes(message));
    stream.finish_write();
    while (stream.pending_write_bytes())
    {
        co_await stream.wait_events();
    }
    listener.context().stop();
}

pace::task<void> interop_client(pace::quic::connection& connection, pace::udp::endpoint endpoint)
{
    co_await connection.connect(endpoint, "localhost");
    auto stream = co_await connection.open_stream();
    const auto message = payload(0);
    co_await stream.write_all(pace::as_bytes(message));
    stream.finish_write();
    require(co_await read_to_end(stream) == message, "cross-backend response mismatch");
    // Allow the delayed final ACK to leave before stopping the UDP reactor.
    co_await connection.context().sleep_for(100ms);
    connection.context().stop();
}

void interop(const std::filesystem::path& fixtures, bool server, uint16_t port)
{
    watchdog guard{__func__};
    pace::io_context io;
    configurations config(fixtures);
    if (server)
    {
        pace::quic::listener listener(io, {"127.0.0.1", 0}, config.server);
        std::cout << listener.local_endpoint().port << std::endl;
        io.spawn(interop_server(listener));
        io.run();
    }
    else
    {
        pace::quic::connection client(io, config.client);
        io.spawn(interop_client(client, {"127.0.0.1", port}));
        io.run();
    }
}

// Cross-backend interop peer for tests/quic/quic_backend_interop.py; main() calls it instead of doctest:
//   --interop-server       listen on 127.0.0.1, print the port and a newline to stdout, serve one exchange
//   --interop-client PORT  connect to 127.0.0.1:PORT and run one exchange
// Returns 0 on success; prints the error to stderr and returns 1 on failure.
int run_interop_peer(int argc, char** argv)
{
    try
    {
        const bool server = std::string_view(argv[1]) == "--interop-server";
        if (!server && argc != 3)
        {
            throw std::invalid_argument("interop client requires port");
        }
        interop(certificates(), server, server ? 0 : static_cast<uint16_t>(std::stoul(argv[2])));
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

const pace::test::command interop_server_command{"--interop-server", run_interop_peer};
const pace::test::command interop_client_command{"--interop-client", run_interop_peer};
} // namespace

TEST_CASE("QUIC concurrent streams round trip with a DNS name")
{
    roundtrip(certificates(), "localhost");
}

TEST_CASE("QUIC concurrent streams round trip with an IPv4 address name")
{
    roundtrip(certificates(), "127.0.0.1");
}

TEST_CASE("QUIC round trip recovers from packet loss in both directions")
{
    roundtrip(certificates(), "localhost", "127.0.0.1", true);
}

TEST_CASE("QUIC concurrent streams round trip over IPv6 with a DNS name")
{
    roundtrip(certificates(), "localhost", "::1");
}

TEST_CASE("QUIC concurrent streams round trip over IPv6 with an IPv6 address name")
{
    roundtrip(certificates(), "::1", "::1");
}

TEST_CASE("QUIC rejects a certificate that does not cover the IPv6 address name")
{
    rejects(certificates(), "::1", true, false, false, "::1");
}

TEST_CASE("QUIC rejects a certificate for the wrong name")
{
    rejects(certificates(), "wrong.example", true, false, false);
}

TEST_CASE("QUIC rejects an untrusted certificate")
{
    rejects(certificates(), "localhost", false, false, false);
}

TEST_CASE("QUIC rejects an expired certificate")
{
    rejects(certificates(), "localhost", true, true, false);
}

TEST_CASE("QUIC rejects a peer without a common ALPN protocol")
{
    rejects(certificates(), "localhost", true, false, true);
}

TEST_CASE("QUIC handshake times out against a silent peer")
{
    deadlines(false);
}

TEST_CASE("QUIC handshake is canceled when the io_context stops")
{
    deadlines(true);
}

TEST_CASE("QUIC connection close aborts the peer's pending read")
{
    closure(certificates(), false);
}

TEST_CASE("QUIC listener close cancels accepted streams and the peer's pending read")
{
    closure(certificates(), true);
}

TEST_CASE("QUIC rejects overlapping stream reads and a stream reset unwinds the pending read")
{
    serialization_and_reset(certificates());
}

TEST_CASE("QUIC recycles stream and connection flow-control credit")
{
    recycle_streams(certificates());
}

TEST_CASE("closing one QUIC connection leaves its sibling and the shared listener open")
{
    connection_isolation(certificates());
}

TEST_CASE("QUIC listener close cancels a pending accept")
{
    accept_cancellation(certificates());
}

TEST_CASE("a local deadline unwinds only the pending QUIC accept")
{
    local_cancellation(certificates());
}
