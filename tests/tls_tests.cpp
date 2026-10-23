#include <pace/tls.hpp>

#include <array>
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
        if (starttls) co_await socket.handshake(test.name);
        else co_await socket.connect(endpoint, test.name);
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
        sni_and_application_trust(fixtures, false);
        sni_and_application_trust(fixtures, true);
        configuration_freeze_and_atomicity(fixtures);
        shared_sni_threads(fixtures);
        ipv6_application_trust(fixtures, false);
        ipv6_application_trust(fixtures, true);
        std::cout << "TLS tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
