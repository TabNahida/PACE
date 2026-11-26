#include <pace/tls.hpp>
#include <pace/detail/reactor_signal.hpp>
#include <pace/quic.hpp>
#include <openssl/pem.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

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

enum class replay_prefix { none, complete_hello, mid_record };

pace::task<std::string> read_client_hello_prefix(pace::tcp::stream& socket, replay_prefix replay) {
    std::string prefix = co_await socket.read_exact(5);
    require(prefix[0] == '\x16', "expected TLS handshake record");
    const auto record_size = (static_cast<unsigned char>(prefix[3]) << 8) |
                             static_cast<unsigned char>(prefix[4]);
    require(record_size > 4 && record_size <= 16384, "unexpected ClientHello record size");
    const bool complete = replay == replay_prefix::complete_hello;
    prefix += co_await socket.read_exact(complete ? record_size : record_size / 2);
    require(prefix[5] == '\x01', "expected ClientHello message");
    if (complete) {
        const auto hello_size = (static_cast<unsigned char>(prefix[6]) << 16) |
                                (static_cast<unsigned char>(prefix[7]) << 8) |
                                static_cast<unsigned char>(prefix[8]);
        require(hello_size + 4 == record_size, "test client did not send a complete ClientHello in its first record");
    }
    co_return prefix;
}

pace::task<void> echo_server(pace::tcp::listener& listener, const pace::tls::context& config,
                            std::size_t size, finished& done, replay_prefix replay) {
    auto transport = co_await listener.accept();
    std::string prefix;
    if (replay != replay_prefix::none) prefix = co_await read_client_hello_prefix(transport, replay);
    pace::tls::stream socket(std::move(transport), config, prefix);
    std::string{}.swap(prefix); // The TLS stream owns its copy before the handshake starts.
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
void verified_roundtrip(const std::filesystem::path& fixtures, std::string name,
                        replay_prefix replay = replay_prefix::none) {
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
    io.spawn(echo_server(listener, server, payload, done, replay));
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

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read TLS test fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

pace::tls::context sni_configuration(const std::filesystem::path& fixtures,
                                     pace::tls::sni_policy missing = pace::tls::sni_policy::use_default,
                                     pace::tls::sni_policy unknown = pace::tls::sni_policy::use_default,
                                     bool broken_localhost = false) {
    pace::tls::context server(pace::tls::role::server);
    server.use_certificate_chain((fixtures / "localhost.pem").string(), (fixtures / "localhost-key.pem").string());
    server.set_alpn_protocols({"default"});
    for (const auto* name : {"a.example", "b.example"}) {
        pace::tls::context selected(pace::tls::role::server);
        selected.use_certificate_chain((fixtures / (std::string(name) + ".pem")).string(),
                                       (fixtures / (std::string(name) + "-key.pem")).string());
        selected.set_alpn_protocols({name});
        server.add_server_name(name, selected);
        if (std::string_view(name) == "a.example") server.add_server_name("route.example", selected);
        // selected's public object dies here; routing owns its immutable state.
    }
    if (broken_localhost) {
        pace::tls::context without_certificate(pace::tls::role::server);
        server.add_server_name("localhost", without_certificate);
    }
    server.set_server_name_policy(missing, unknown);
    return server;
}

pace::tls::context application_trust(const std::filesystem::path& fixtures) {
    pace::tls::context client(pace::tls::role::client, pace::tls::trust_source::application);
    // Both roots come from owned memory, never OpenSSL's defaults or a temp file.
    auto pem = read_file(fixtures / "sni-ca.pem") + read_file(fixtures / "ca.pem");
    client.load_verify_pem(pem);
    client.load_verify_pem(pem); // Adding existing roots is harmless.
    client.set_alpn_protocols({"a.example", "b.example", "default"});
    return client;
}

struct group_finished {
    pace::io_context& io;
    unsigned total;
    unsigned completed = 0;
    unsigned server_success = 0;
    unsigned server_failure = 0;
    void arrive() { if (++completed == total) io.stop(); }
};
struct client_case {
    std::string name;
    std::string protocol;
    bool accept = true;
    std::optional<std::string> server_name = std::nullopt;
};

template<class Stream>
pace::task<void> read_exact(Stream& stream, std::span<char> bytes) {
    while (!bytes.empty()) {
        const auto count = co_await stream.read_some(bytes);
        require(count != 0, "early EOF in TLS test exchange");
        bytes = bytes.subspan(count);
    }
}

pace::task<void> selected_server(pace::tcp::stream transport, const pace::tls::context& configuration,
                                 bool starttls, group_finished& done, bool own_configuration = false,
                                 std::filesystem::path fixtures = {}) {
    if (starttls) {
        std::array<char, 10> command{};
        co_await read_exact(transport, command);
        require(std::string_view(command.data(), command.size()) == "STARTTLS\r\n", "STARTTLS command mismatch");
        co_await transport.write_all("220 ready\r\n");
    }
    // This path also proves that the root mapping (not only its targets) can be
    // destroyed before ClientHello arrives while the stream retains everything.
    auto socket = [&] {
        if (own_configuration) {
            auto temporary = sni_configuration(fixtures);
            return pace::tls::stream(std::move(transport), temporary);
        }
        return pace::tls::stream(std::move(transport), configuration);
    }();
    bool accepted = false;
    try { co_await socket.handshake(); accepted = true; }
    catch (const std::system_error& error) {
        require(error.code() == std::errc::protocol_error && !socket.is_open(),
                "SNI failure must only close its connection");
    }
    if (accepted) {
        ++done.server_success;
        std::array<char, 1> byte{};
        co_await read_exact(socket, byte);
        co_await socket.write_all(std::string_view(byte.data(), byte.size()));
        co_await socket.shutdown();
    } else ++done.server_failure;
    done.arrive();
}

pace::task<void> selected_acceptor(pace::tcp::listener& listener, const pace::tls::context& config,
                                   unsigned count, bool starttls, group_finished& done,
                                   bool own_configuration, std::filesystem::path fixtures) {
    for (unsigned i = 0; i < count; ++i)
        listener.context().spawn(selected_server(co_await listener.accept(), config, starttls, done,
                                                own_configuration, fixtures));
}
pace::task<void> selected_client(pace::io_context& io, const pace::tls::context& config,
                                 pace::tcp::endpoint endpoint, client_case test, bool starttls,
                                 group_finished& done) {
    pace::tcp::stream transport(io);
    if (starttls) {
        co_await transport.connect(endpoint);
        co_await transport.write_all("STARTTLS\r\n");
        std::array<char, 11> ready{};
        co_await read_exact(transport, ready);
        require(std::string_view(ready.data(), ready.size()) == "220 ready\r\n", "STARTTLS reply mismatch");
    }
    pace::tls::stream socket(std::move(transport), config);
    bool accepted = false;
    try {
        if (starttls) {
            if (test.server_name) co_await socket.handshake(test.name, *test.server_name);
            else co_await socket.handshake(test.name);
        } else {
            if (test.server_name) co_await socket.connect(endpoint, test.name, *test.server_name);
            else co_await socket.connect(endpoint, test.name);
        }
        accepted = true;
    } catch (const std::system_error& error) {
        require(error.code() == std::errc::protocol_error && !socket.is_open(),
                "SNI/custom trust rejection must close this client");
    }
    require(accepted == test.accept, "unexpected SNI/custom trust handshake result");
    if (accepted) {
        require(socket.selected_protocol() == test.protocol, "SNI selected the wrong context/ALPN");
        co_await socket.write_all("x");
        std::array<char, 1> reply{};
        co_await read_exact(socket, reply);
        require(reply[0] == 'x', "SNI data exchange mismatch");
        co_await socket.shutdown();
    }
    done.arrive();
}

void handshake_group(const pace::tls::context& server, const pace::tls::context& client,
                     std::vector<client_case> cases, bool starttls,
                     bool own_configuration = false, const std::filesystem::path& fixtures = {},
                     std::string bind_address = "127.0.0.1") {
    pace::io_context io;
    pace::tcp::listener listener(io, {std::move(bind_address), 0});
    group_finished done{io, static_cast<unsigned>(cases.size() * 2)};
    io.spawn(selected_acceptor(listener, server, static_cast<unsigned>(cases.size()), starttls, done,
                               own_configuration, fixtures));
    unsigned accepted = 0;
    for (const auto& test : cases) {
        accepted += test.accept;
        io.spawn(selected_client(io, client, listener.local_endpoint(), test, starttls, done));
    }
    io.run();
    require(done.completed == done.total && done.server_success == accepted &&
            done.server_failure == cases.size() - accepted,
            "SNI failures affected other connections or did not reach both peers");
}

void sni_and_application_trust(const std::filesystem::path& fixtures, bool starttls) {
    auto client = application_trust(fixtures);
    auto server = sni_configuration(fixtures);
    handshake_group(server, client,
                    {{"a.example", "a.example"}, {"b.example", "b.example"},
                     {"A.Example", "a.example"}, {"b.example", "b.example"},
                     {"localhost", "default"}, {"127.0.0.1", "default"}}, starttls);
    handshake_group(server, client, {{"b.example", "b.example"}}, starttls, true, fixtures);

    auto strict = sni_configuration(fixtures, pace::tls::sni_policy::reject, pace::tls::sni_policy::reject);
    handshake_group(strict, client, {{"127.0.0.1", "", false}, {"localhost", "", false},
                                     {"a.example", "a.example"}, {"b.example", "b.example"}}, starttls);
    auto missing_only = sni_configuration(fixtures, pace::tls::sni_policy::reject);
    handshake_group(missing_only, client, {{"127.0.0.1", "", false}, {"localhost", "default"}}, starttls);
    auto unknown_only = sni_configuration(fixtures, pace::tls::sni_policy::use_default, pace::tls::sni_policy::reject);
    handshake_group(unknown_only, client, {{"127.0.0.1", "default"}, {"localhost", "", false}}, starttls);

    auto broken = sni_configuration(fixtures, pace::tls::sni_policy::use_default,
                                    pace::tls::sni_policy::use_default, true);
    // The default certificate is valid for localhost, but an explicitly selected
    // context without a certificate must fail, never silently fall back.
    handshake_group(broken, client, {{"localhost", "", false}, {"a.example", "a.example"},
                                     {"b.example", "b.example"}}, starttls);

    pace::tls::context untrusted(pace::tls::role::client, pace::tls::trust_source::application);
    handshake_group(server, untrusted, {{"a.example", "", false}}, starttls);
    untrusted = pace::tls::context(pace::tls::role::client, pace::tls::trust_source::application);
    untrusted.load_verify_pem(read_file(fixtures / "ca.pem")); // Trust an unrelated CA only.
    handshake_group(server, untrusted, {{"a.example", "", false}}, starttls);
    handshake_group(server, client, {{"wrong.example", "", false}}, starttls);
}

template<class Function>
void rejects_frozen(Function operation) {
    bool rejected = false;
    try { operation(); } catch (const std::logic_error&) { rejected = true; }
    require(rejected, "frozen TLS configuration accepted a setter");
}

void configuration_freeze_and_atomicity(const std::filesystem::path& fixtures) {
    pace::tls::context server(pace::tls::role::server), target(pace::tls::role::server);
    target.use_certificate_chain((fixtures / "a.example.pem").string(), (fixtures / "a.example-key.pem").string());
    target.set_alpn_protocols({"a.example"});
    bool rejected = false;
    try { target.use_certificate_chain((fixtures / "b.example.pem").string(), (fixtures / "a.example-key.pem").string()); }
    catch (const std::system_error&) { rejected = true; }
    require(rejected, "mismatched certificate/key was accepted");
    server.add_server_name("a.example", target);
    rejects_frozen([&] { target.set_alpn_protocols({"changed"}); });
    rejects_frozen([&] { target.load_verify_file((fixtures / "ca.pem").string()); });
    rejects_frozen([&] { target.load_verify_pem(read_file(fixtures / "ca.pem")); });
    rejects_frozen([&] { target.use_certificate_chain((fixtures / "localhost.pem").string(), (fixtures / "localhost-key.pem").string()); });
    rejects_frozen([&] { target.set_server_name_policy(pace::tls::sni_policy::reject, pace::tls::sni_policy::reject); });

    for (const auto& name : {"a.example", "A.EXAMPLE", "*.example", "127.0.0.1", "-a.example", "a..example", "a.example."}) {
        rejected = false;
        try { server.add_server_name(name, target); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "duplicate/invalid SNI name was accepted");
    }
    rejected = false;
    try { server.add_server_name("self.example", server); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "self-referential SNI context was accepted");
    pace::tls::context outer(pace::tls::role::server);
    rejected = false;
    try { outer.add_server_name("nested.example", server); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "nested SNI table was accepted");

    auto client = application_trust(fixtures);
    // Valid A certificate survives the failed B/key change.
    handshake_group(server, client, {{"a.example", "a.example"}}, false);
    rejects_frozen([&] { server.add_server_name("b.example", target); });
    rejects_frozen([&] { client.load_verify_pem(read_file(fixtures / "ca.pem")); });

    pace::tls::context empty(pace::tls::role::client, pace::tls::trust_source::application);
    for (const auto& pem : {std::string{}, std::string(" "), std::string("not PEM"),
                            read_file(fixtures / "sni-ca.pem") + "trailing garbage"}) {
        rejected = false;
        try { empty.load_verify_pem(pem); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "invalid PEM trust bundle was accepted");
    }
    // A valid prefix in an invalid bundle must not partially add trust.
    handshake_group(server, empty, {{"a.example", "", false}}, false);
}

void shared_sni_threads(const std::filesystem::path& fixtures) {
    auto server = sni_configuration(fixtures);
    auto client = application_trust(fixtures);
    std::array<std::exception_ptr, 4> failures{};
    std::array<std::thread, 4> threads;
    for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i] = std::thread([&, i] {
            try {
                handshake_group(server, client, {{"a.example", "a.example"}, {"b.example", "b.example"},
                                                 {"a.example", "a.example"}, {"b.example", "b.example"}}, i % 2);
            } catch (...) { failures[i] = std::current_exception(); }
        });
    }
    for (auto& thread : threads) thread.join();
    for (const auto& failure : failures) if (failure) std::rethrow_exception(failure);
}

void ipv6_application_trust(const std::filesystem::path& fixtures, bool starttls) {
    pace::tls::context server(pace::tls::role::server);
    server.use_certificate_chain((fixtures / "a.example.pem").string(), (fixtures / "a.example-key.pem").string());
    server.set_alpn_protocols({"a.example"});
    auto client = application_trust(fixtures);
    // ::1 is an IP SAN, not a DNS name/SNI. The client transport starts as IPv4
    // and connect switches its native descriptor before the socket BIO is used.
    handshake_group(server, client, {{"::1", "a.example"}, {"::2", "", false},
                                     {"a.example", "a.example"}, {"b.example", "", false}},
                    starttls, false, {}, "::1");
}

void independent_client_sni(const std::filesystem::path& fixtures, bool starttls) {
    auto server = sni_configuration(fixtures, pace::tls::sni_policy::use_default, pace::tls::sni_policy::reject);
    auto client = application_trust(fixtures);
    handshake_group(server, client,
                    {{"a.example", "a.example", true, "route.example"},
                     {"::1", "a.example", true, "route.example"},
                     {"b.example", "", false, "route.example"},
                     {"localhost", "default", true, ""},
                     {"localhost", "", false},
                     {"a.example", "", false, ""}}, starttls);
}

pace::tls::sha256_digest decode_pin(std::string_view hex) {
    pace::tls::sha256_digest pin{};
    require(hex.size() == 2 * pin.size(), "invalid test pin length");
    for (auto& byte : pin) {
        unsigned value = 0;
        const auto end = hex.data() + 2;
        const auto result = std::from_chars(hex.data(), end, value, 16);
        require(result.ec == std::errc{} && result.ptr == end, "invalid test pin hex");
        byte = static_cast<std::byte>(value);
        hex.remove_prefix(2);
    }
    return pin;
}

std::string fixture_der(const std::filesystem::path& path) {
    const auto pem = read_file(path);
    std::unique_ptr<BIO, decltype(&BIO_free)> input(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
    require(bool(input), "create test PEM reader");
    std::unique_ptr<X509, decltype(&X509_free)> certificate(PEM_read_bio_X509(input.get(), nullptr, nullptr, nullptr), X509_free);
    require(bool(certificate), "decode test certificate");
    const int size = i2d_X509(certificate.get(), nullptr);
    require(size > 0, "size test DER certificate");
    std::string der(static_cast<std::size_t>(size), '\0');
    auto* output = reinterpret_cast<unsigned char*>(der.data());
    require(i2d_X509(certificate.get(), &output) == size, "encode test DER certificate");
    return der;
}

void client_verification_modes(const std::filesystem::path& fixtures) {
    using namespace pace::tls;
    context server(role::server);
    server.use_certificate_chain((fixtures / "localhost-chain.pem").string(), (fixtures / "localhost-key.pem").string());
    server.set_alpn_protocols({"h2"});
    auto make_client = [] {
        context client(role::client, trust_source::application);
        client.set_alpn_protocols({"h2"});
        return client;
    };

    // Independently computed from the certificate and SPKI DER with openssl dgst.
    for (const auto& [type, hex] : std::array{
            std::pair{pin_type::leaf_certificate, "c3684a2f2b7036538c5984641852cbbacd67923d0ced6d36e1cb360b1935d2dd"},
            std::pair{pin_type::subject_public_key_info, "94ceb5d2a0b41214783f4425e3b3afe36c195c272a7f93b0b0db54ba8314aefa"}}) {
        auto client = make_client();
        auto pin = decode_pin(hex);
        client.set_verify_pin(type, pin);
        handshake_group(server, client, {{"", "h2"}, {"localhost", "h2"}, {"127.0.0.1", "h2"},
                                         {"wrong.example", "", false}, {"127.0.0.2", "", false}}, false);

        auto bad_pin = make_client();
        bad_pin.load_verify_file((fixtures / "ca.pem").string());
        pin.front() ^= std::byte{1};
        bad_pin.set_verify_pin(type, pin);
        handshake_group(server, bad_pin, {{"localhost", "", false}}, false);
    }

    const std::array expected_chain{fixture_der(fixtures / "localhost.pem"), fixture_der(fixtures / "ca.pem")};
    unsigned calls = 0;
    auto callback_client = make_client();
    callback_client.set_verify_callback([&](std::span<const std::string> chain) {
        ++calls;
        return std::ranges::equal(chain, expected_chain);
    });
    handshake_group(server, callback_client, {{"localhost", "h2"}, {"wrong.example", "", false}}, false);
    require(calls == 1, "verification callback ran before the peer identity check");

    auto rejecting_client = make_client();
    rejecting_client.load_verify_file((fixtures / "ca.pem").string());
    rejecting_client.set_verify_callback([](std::span<const std::string>) { return false; });
    handshake_group(server, rejecting_client, {{"localhost", "", false}}, false);
    auto throwing_client = make_client();
    throwing_client.set_verify_callback([](std::span<const std::string>) -> bool { throw std::runtime_error("verifier failure"); });
    handshake_group(server, throwing_client, {{"localhost", "", false}}, false);

    auto insecure_client = make_client();
    insecure_client.insecure_skip_verify();
    handshake_group(server, insecure_client, {{"", "h2"}, {"wrong.example", "h2"}}, true);

    rejects_frozen([&] { callback_client.set_verify_pin(pin_type::leaf_certificate, {}); });
    rejects_frozen([&] { callback_client.set_verify_callback([](std::span<const std::string>) { return true; }); });
    rejects_frozen([&] { insecure_client.insecure_skip_verify(); });
}

struct pipe_state {
    explicit pipe_state(pace::io_context& context, std::size_t capacity = 1024)
        : io(context), changed(context), capacity(capacity) {}
    pace::io_context& io;
    pace::detail::reactor_signal changed;
    const std::size_t capacity;
    std::array<std::string, 2> inbound;
    std::array<bool, 2> closed{};
    unsigned shutdown_calls = 0;
};

template<bool HasShutdown = false>
class pipe_stream {
public:
    pipe_stream(std::shared_ptr<pipe_state> state, std::size_t side) : state_(std::move(state)), side_(side) {}
    pipe_stream(pipe_stream&&) noexcept = default;
    pace::io_context& context() const noexcept { return state_->io; }
    bool is_open() const noexcept { return !state_->closed[side_]; }
    void close() noexcept {
        if (!is_open()) return;
        state_->closed[side_] = true;
        state_->changed.notify();
    }
    // This adapter uses shutdown for cleanup and relies on close to release the pipe.
    pace::task<void> shutdown(std::chrono::milliseconds) requires HasShutdown {
        ++state_->shutdown_calls;
        co_return;
    }
    pace::task<std::size_t> read_some(std::span<char> buffer) {
        auto& queue = state_->inbound[side_];
        while (is_open() && queue.empty() && !state_->closed[1 - side_]) co_await state_->changed.wait();
        check_open();
        // Split record headers and payloads even when more bytes are available.
        const auto count = std::min({queue.size(), buffer.size(), std::size_t(131)});
        std::copy_n(queue.data(), count, buffer.data());
        queue.erase(0, count);
        state_->changed.notify();
        co_return count;
    }
    pace::task<void> write_all(std::string_view bytes) {
        auto& queue = state_->inbound[1 - side_];
        const auto capacity = state_->capacity;
        while (!bytes.empty()) {
            while (is_open() && queue.size() == capacity && !state_->closed[1 - side_])
                co_await state_->changed.wait();
            check_open();
            if (state_->closed[1 - side_]) throw std::system_error(std::make_error_code(std::errc::broken_pipe));
            const auto count = std::min(capacity - queue.size(), bytes.size());
            queue.append(bytes.data(), count);
            bytes.remove_prefix(count);
            state_->changed.notify();
        }
    }
private:
    void check_open() const {
        if (!is_open()) throw std::system_error(std::make_error_code(std::errc::operation_canceled));
    }
    std::shared_ptr<pipe_state> state_;
    std::size_t side_;
};

struct transport_contexts {
    pace::tls::context server{pace::tls::role::server};
    pace::tls::context client{pace::tls::role::client, pace::tls::trust_source::application};
    explicit transport_contexts(const std::filesystem::path& fixtures) {
        server.use_certificate_chain((fixtures / "localhost.pem").string(), (fixtures / "localhost-key.pem").string());
        server.set_alpn_protocols({"test-echo"});
        client.load_verify_file((fixtures / "ca.pem").string());
        client.set_alpn_protocols({"test-echo"});
    }
};

constexpr std::size_t transport_payload_size = 96 * 1024 + 13;

pace::task<void> transport_echo_server(pace::tls::stream stream, finished& done) {
    co_await stream.handshake();
    require(stream.selected_protocol() == "test-echo", "server generic TLS ALPN mismatch");
    const auto bytes = co_await pace::read_exact(stream, transport_payload_size);
    co_await stream.write_all(bytes);
    co_await stream.shutdown(2s);
    require(!stream.is_open(), "generic TLS server shutdown did not close the stream");
    done.arrive();
}

pace::task<void> transport_echo_client(pace::tls::stream stream, finished& done) {
    co_await stream.handshake("localhost");
    require(stream.selected_protocol() == "test-echo", "client generic TLS ALPN mismatch");
    std::string bytes(transport_payload_size, '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>(i * 31 % 251);
    co_await stream.write_all(bytes);
    require((co_await pace::read_exact(stream, bytes.size())) == bytes, "generic TLS echo changed the payload");
    std::array<char, 1> tail{};
    require((co_await stream.read_some(tail)) == 0, "generic TLS peer did not send close_notify");
    co_await stream.shutdown(2s);
    require(!stream.is_open(), "generic TLS client shutdown did not close the stream");
    done.arrive();
}

template<bool HasShutdown>
pace::task<void> pipe_echo_server(pipe_stream<HasShutdown> transport, const pace::tls::context& config, finished& done) {
    // Read a record header and part of its body before wrapping the byte stream.
    std::array<char, 7> prefix{};
    co_await pace::read_exact(transport, std::span<char>(prefix));
    co_await transport_echo_server(
        pace::tls::stream(std::move(transport), config, std::string_view(prefix.data(), prefix.size())), done);
}

template<bool HasShutdown = false>
void memory_transport(const transport_contexts& contexts, std::size_t capacity) {
    pace::io_context io;
    auto pipe = std::make_shared<pipe_state>(io, capacity);
    finished done{io};
    const auto deadline = io.call_after(5s, [] { throw std::runtime_error("in-memory TLS roundtrip stalled"); });
    io.spawn(pipe_echo_server(pipe_stream<HasShutdown>(pipe, 0), contexts.server, done));
    io.spawn(transport_echo_client(pace::tls::stream(pipe_stream<HasShutdown>(pipe, 1), contexts.client), done));
    io.run();
    require(done.count == 2, "in-memory TLS roundtrip incomplete");
    require(pipe->shutdown_calls == (HasShutdown ? 2u : 0u), "unexpected in-memory shutdown hook calls");
    require(pipe->closed[0] && pipe->closed[1], "TLS shutdown left the in-memory transport open");
}

pace::task<void> withhold_transport_shutdown(pace::tls::stream stream) {
    co_await stream.handshake();
    co_await stream.write_all("ready");
    try { co_await stream.context().sleep_for(1h); }
    catch (const std::system_error& error) {
        require(error.code() == std::errc::operation_canceled, "unexpected in-memory peer error");
    }
}

pace::task<void> shutdown_memory_client(pace::tls::stream stream, bool& finished) {
    co_await stream.handshake("localhost");
    require((co_await pace::read_exact(stream, 5)) == "ready", "in-memory peer did not complete the handshake");
    co_await stream.shutdown(5ms);
    require(!stream.is_open(), "timed-out in-memory shutdown left the stream open");
    finished = true;
    stream.context().stop();
}

void memory_shutdown_timeout(const transport_contexts& contexts) {
    pace::io_context io;
    auto pipe = std::make_shared<pipe_state>(io);
    bool finished = false;
    io.spawn(withhold_transport_shutdown(pace::tls::stream(pipe_stream(pipe, 0), contexts.server)));
    io.spawn(shutdown_memory_client(pace::tls::stream(pipe_stream(pipe, 1), contexts.client), finished));
    io.run();
    require(finished && pipe->closed[1], "in-memory shutdown timeout did not unwind");
}

void simultaneous_memory_shutdown(const transport_contexts& contexts) {
    pace::io_context io;
    // One direction cannot buffer even one encrypted close_notify record.
    auto pipe = std::make_shared<pipe_state>(io, 16);
    finished done{io};
    unsigned handshakes = 0;
    const auto peer = [&](std::size_t side, const pace::tls::context& config, std::string peer_name) -> pace::task<void> {
        pace::tls::stream stream(pipe_stream<true>(pipe, side), config);
        co_await stream.handshake(std::move(peer_name));
        if (++handshakes == 2) pipe->changed.notify();
        while (handshakes != 2) co_await pipe->changed.wait();
        co_await stream.shutdown(100ms);
        done.arrive();
    };
    io.spawn(peer(0, contexts.server, {}));
    io.spawn(peer(1, contexts.client, "localhost"));
    io.run();
    require(done.count == 2 && pipe->closed[0] && pipe->closed[1], "simultaneous TLS shutdown did not close the pipe");
    require(pipe->shutdown_calls == 2, "simultaneous TLS shutdown timed out before the cleanup hooks");
}

void memory_transport_truncation(const transport_contexts& contexts) {
    pace::io_context io;
    auto pipe = std::make_shared<pipe_state>(io);
    finished done{io};
    const auto server = [&]() -> pace::task<void> {
        pace::tls::stream stream(pipe_stream(pipe, 0), contexts.server);
        co_await stream.handshake();
        co_await stream.write_all("record");
        stream.close();
        done.arrive();
    };
    const auto client = [&]() -> pace::task<void> {
        pace::tls::stream stream(pipe_stream(pipe, 1), contexts.client);
        co_await stream.handshake("localhost");
        require((co_await pace::read_exact(stream, 6)) == "record", "in-memory TLS lost an authenticated record");
        std::array<char, 1> byte{};
        bool rejected = false;
        try { (void)co_await stream.read_some(byte); }
        catch (const std::system_error& error) { rejected = error.code() == std::errc::protocol_error; }
        require(rejected && !stream.is_open(), "in-memory TLS accepted unauthenticated EOF");
        done.arrive();
    };
    io.spawn(server());
    io.spawn(client());
    io.run();
    require(done.count == 2, "in-memory TLS truncation test did not finish");
}

pace::task<void> nested_tls_server(pace::tcp::listener& listener, const transport_contexts& contexts, finished& done) {
    auto outer = co_await pace::tls::accept(co_await listener.accept(), contexts.server);
    co_await transport_echo_server(pace::tls::stream(std::move(outer), contexts.server), done);
}

pace::task<void> nested_tls_client(pace::tcp::listener& listener, const transport_contexts& contexts, finished& done) {
    auto outer = co_await pace::tls::connect(contexts.client, listener.local_endpoint(), "localhost");
    co_await transport_echo_client(pace::tls::stream(std::move(outer), contexts.client), done);
}

void nested_tls_transport(const transport_contexts& contexts) {
    pace::io_context io;
    pace::tcp::listener listener(io, {"127.0.0.1", 0});
    finished done{io};
    io.spawn(nested_tls_server(listener, contexts, done));
    io.spawn(nested_tls_client(listener, contexts, done));
    io.run();
    require(done.count == 2, "nested TLS roundtrip incomplete");
}

#ifdef PACE_HAS_QUIC
pace::task<void> quic_tls_server(pace::quic::listener& listener, std::optional<pace::quic::connection>& connection,
                                const pace::tls::context& config, finished& done) {
    connection.emplace(co_await listener.accept());
    co_await transport_echo_server(pace::tls::stream(co_await connection->accept_stream(), config), done);
}

pace::task<void> quic_tls_client(pace::quic::connection& connection, pace::udp::endpoint peer,
                                const pace::tls::context& config, finished& done) {
    co_await connection.connect(std::move(peer), "localhost");
    co_await transport_echo_client(pace::tls::stream(co_await connection.open_stream(), config), done);
}

void quic_transport(const std::filesystem::path& fixtures, const transport_contexts& contexts) {
    pace::io_context io;
    pace::quic::context server(pace::quic::role::server), client;
    server.use_certificate_chain((fixtures / "localhost.pem").string(), (fixtures / "localhost-key.pem").string());
    client.load_verify_file((fixtures / "ca.pem").string());
    pace::quic::listener listener(io, {"127.0.0.1", 0}, server);
    pace::quic::connection client_connection(io, client);
    std::optional<pace::quic::connection> server_connection;
    finished done{io};
    io.spawn(quic_tls_server(listener, server_connection, contexts.server, done));
    io.spawn(quic_tls_client(client_connection, listener.local_endpoint(), contexts.client, done));
    io.run();
    require(done.count == 2, "TLS over QUIC roundtrip incomplete");
}
#endif

} // namespace

int main(int argc, char** argv) {
    watchdog guard;
    try {
        const auto fixtures = argc > 1 ? std::filesystem::path(argv[1]) :
            std::filesystem::path(__FILE__).parent_path() / "certs";
        verified_roundtrip(fixtures, "localhost");
        verified_roundtrip(fixtures, "127.0.0.1");
        verified_roundtrip(fixtures, "localhost", replay_prefix::complete_hello);
        verified_roundtrip(fixtures, "localhost", replay_prefix::mid_record);
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
        sni_and_application_trust(fixtures, false);
        sni_and_application_trust(fixtures, true);
        configuration_freeze_and_atomicity(fixtures);
        shared_sni_threads(fixtures);
        ipv6_application_trust(fixtures, false);
        ipv6_application_trust(fixtures, true);
        independent_client_sni(fixtures, false);
        independent_client_sni(fixtures, true);
        client_verification_modes(fixtures);
        const transport_contexts contexts(fixtures);
        memory_transport(contexts, 64);
        memory_transport<true>(contexts, 1024);
        memory_shutdown_timeout(contexts);
        simultaneous_memory_shutdown(contexts);
        memory_transport_truncation(contexts);
        nested_tls_transport(contexts);
#ifdef PACE_HAS_QUIC
        quic_transport(fixtures, contexts);
#endif
        std::cout << "TLS tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
