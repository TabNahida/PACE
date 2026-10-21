#include <pace/websocket.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;
namespace ws = pace::websocket;
namespace {
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
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
    unsigned count = 0;
    void finish() { if (++count == 2) io.stop(); }
};
constexpr std::string_view request = "GET /chat HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
std::string frame(unsigned opcode, std::string_view payload, bool final = true, bool masked = true) {
    std::string result;
    result += static_cast<char>((final ? 0x80 : 0) | opcode);
    const auto mask = masked ? 0x80 : 0;
    if (payload.size() < 126) result += static_cast<char>(mask | payload.size());
    else if (payload.size() < 65536) {
        result += static_cast<char>(mask | 126);
        result += static_cast<char>(payload.size() >> 8); result += static_cast<char>(payload.size());
    } else {
        result += static_cast<char>(mask | 127);
        for (int shift = 56; shift >= 0; shift -= 8) result += static_cast<char>(static_cast<std::uint64_t>(payload.size()) >> shift);
    }
    constexpr std::string_view key = "test";
    if (masked) result += key;
    const auto start = result.size();
    result += payload;
    if (masked) for (std::size_t i = 0; i < payload.size(); ++i) result[start + i] ^= key[i % 4];
    return result;
}
pace::task<std::string> read_head(pace::tcp::stream& socket, std::string& pending) {
    for (;;) {
        const auto end = pending.find("\r\n\r\n");
        if (end != std::string::npos) {
            auto result = pending.substr(0, end + 4); pending.erase(0, end + 4); co_return result;
        }
        require(pending.size() < 16384, "raw HTTP head too large");
        std::array<char, 1024> bytes{};
        const auto count = co_await socket.read_some(bytes);
        require(count != 0, "raw peer ended before upgrade head");
        pending.append(bytes.data(), count);
    }
}
pace::task<std::string> take(pace::tcp::stream& socket, std::string& pending, std::size_t length) {
    while (pending.size() < length) {
        std::array<char, 2048> bytes{};
        const auto count = co_await socket.read_some(bytes);
        require(count != 0, "raw WebSocket frame truncated");
        pending.append(bytes.data(), count);
    }
    auto result = pending.substr(0, length); pending.erase(0, length); co_return result;
}
pace::task<std::pair<unsigned, std::string>> read_frame(pace::tcp::stream& socket, std::string& pending, bool expected_mask) {
    const auto prefix = co_await take(socket, pending, 2);
    const auto first = static_cast<unsigned char>(prefix[0]), second = static_cast<unsigned char>(prefix[1]);
    require(bool(second & 0x80) == expected_mask, "outgoing frame mask direction");
    std::size_t size = second & 127;
    if (size >= 126) {
        const auto extended = co_await take(socket, pending, size == 126 ? 2 : 8);
        size = 0; for (unsigned char byte : extended) size = (size << 8) | byte;
    }
    require(size <= 1024 * 1024, "raw frame too large");
    std::string mask;
    if (expected_mask) mask = co_await take(socket, pending, 4);
    auto payload = co_await take(socket, pending, size);
    if (expected_mask) for (std::size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i % 4];
    co_return std::pair<unsigned, std::string>{first, std::move(payload)};
}
std::string binary_payload() {
    std::string result(96 * 1024 + 7, '\0');
    for (std::size_t i = 0; i < result.size(); ++i) result[i] = static_cast<char>(i * 17 % 251);
    return result;
}
template<class Transport>
pace::task<void> echo_peer(Transport transport, completion& done) {
    ws::basic_stream<Transport> socket(std::move(transport), ws::role::server);
    const auto incoming = co_await socket.accept({"chat"}, [](const ws::upgrade_request& value) { return value.target == "/chat"; });
    require(incoming.target == "/chat" && socket.selected_protocol() == "chat", "server upgrade metadata/subprotocol");
    for (unsigned i = 0; i < 3; ++i) {
        auto value = co_await socket.receive();
        require(value.has_value(), "echo peer closed early");
        co_await socket.send(std::move(*value));
    }
    require(!(co_await socket.receive()), "expected WebSocket closing handshake");
    require(socket.peer_close_code() == 1000 && socket.peer_close_reason() == "finished", "peer close status lost");
    done.finish();
}
template<class Transport>
pace::task<void> client_peer(Transport transport, completion& done) {
    ws::basic_stream<Transport> socket(std::move(transport), ws::role::client);
    co_await socket.handshake("localhost", "/chat", {"chat", "other"});
    require(socket.selected_protocol() == "chat", "client subprotocol mismatch");
    co_await socket.ping(std::string("p\0ng", 4));
    const std::string unicode = "hello \xE2\x82\xAC \xF0\x9F\x8C\x8D";
    co_await socket.send_text(unicode, 1); // UTF-8 sequence split across fragments.
    const auto text = co_await socket.receive();
    require(text && text->type == ws::message_type::text && text->data == unicode, "fragmented UTF-8 echo mismatch");
    const auto binary = binary_payload();
    co_await socket.send_binary(binary, 30000);
    const auto response = co_await socket.receive();
    require(response && response->type == ws::message_type::binary && response->data == binary, "large binary echo mismatch");
    co_await socket.send_text("");
    const auto empty = co_await socket.receive();
    require(empty && empty->data.empty(), "empty WebSocket message lost");
    require(co_await socket.close_gracefully(1000, "finished", 1s), "peer did not complete WebSocket close");
    require(socket.peer_close_code() == 1000, "client close response code");
    done.finish();
}
pace::task<void> plain_server(pace::tcp::listener& listener, completion& done) {
    co_await echo_peer(co_await listener.accept(), done);
}
pace::task<void> plain_client(pace::io_context& io, pace::tcp::endpoint endpoint, completion& done) {
    pace::tcp::stream transport(io); co_await transport.connect(std::move(endpoint));
    co_await client_peer(std::move(transport), done);
}
void roundtrip() {
    pace::io_context io; pace::tcp::listener listener(io, {"127.0.0.1", 0}); completion done{io};
    io.spawn(plain_server(listener, done)); io.spawn(plain_client(io, listener.local_endpoint(), done)); io.run();
    require(done.count == 2, "plain WebSocket roundtrip incomplete");
}
#ifdef PACE_HAS_TLS
pace::task<void> secure_server(pace::tcp::listener& listener, const pace::tls::context& config, completion& done) {
    pace::tls::stream transport(co_await listener.accept(), config); co_await transport.handshake();
    co_await echo_peer(std::move(transport), done);
}
pace::task<void> secure_client(pace::io_context& io, pace::tcp::endpoint endpoint, const pace::tls::context& config, completion& done) {
    pace::tls::stream transport(io, config); co_await transport.connect(std::move(endpoint), "localhost");
    co_await client_peer(std::move(transport), done);
}
void secure_roundtrip(const std::filesystem::path& fixtures) {
    pace::tls::context server(pace::tls::role::server), client;
    server.use_certificate_chain((fixtures / "localhost.pem").string(), (fixtures / "localhost-key.pem").string());
    client.load_verify_file((fixtures / "ca.pem").string());
    server.set_alpn_protocols({"http/1.1"}); client.set_alpn_protocols({"http/1.1"});
    pace::io_context io; pace::tcp::listener listener(io, {"127.0.0.1", 0}); completion done{io};
    io.spawn(secure_server(listener, server, done)); io.spawn(secure_client(io, listener.local_endpoint(), client, done)); io.run();
    require(done.count == 2, "secure WebSocket roundtrip incomplete");
}
#endif

pace::task<void> fragmented_server(pace::tcp::listener& listener, completion& done) {
    ws::stream socket(co_await listener.accept(), ws::role::server);
    co_await socket.accept();
    auto text = co_await socket.receive();
    require(text && text->data == "\xE2\x82\xAC", "control frame disrupted UTF-8 fragments");
    co_await socket.send(std::move(*text));
    require(!(co_await socket.receive()), "raw close not processed");
    done.finish();
}
pace::task<void> fragmented_raw_client(pace::io_context& io, pace::tcp::endpoint endpoint, completion& done) {
    pace::tcp::stream socket(io); co_await socket.connect(std::move(endpoint));
    const std::string bytes = std::string(request) + frame(1, "\xE2", false) + frame(9, "ping") + frame(0, "\x82\xAC");
    co_await socket.write_all(bytes); // First frame coalesced with upgrade headers.
    std::string pending;
    const auto head = co_await read_head(socket, pending);
    require(head.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != std::string::npos,
            "RFC 6455 published SHA-1/base64 accept vector mismatch");
    const auto pong = co_await read_frame(socket, pending, false);
    require(pong.first == 0x8a && pong.second == "ping", "ping must produce matching pong during fragmentation");
    const auto text = co_await read_frame(socket, pending, false);
    require(text.first == 0x81 && text.second == "\xE2\x82\xAC", "raw text echo mismatch");
    const auto closing = frame(8, std::string("\x03\xE8", 2));
    co_await socket.write_all(closing);
    const auto close = co_await read_frame(socket, pending, false);
    require(close.first == 0x88 && close.second == std::string("\x03\xE8", 2), "close reply mismatch");
    socket.close(); done.finish();
}
void control_interleaving() {
    pace::io_context io; pace::tcp::listener listener(io, {"127.0.0.1", 0}); completion done{io};
    io.spawn(fragmented_server(listener, done)); io.spawn(fragmented_raw_client(io, listener.local_endpoint(), done)); io.run();
    require(done.count == 2, "control interleaving incomplete");
}

pace::task<void> invalid_frame_server(pace::tcp::listener& listener, completion& done, unsigned expected, ws::limits bounds) {
    ws::stream socket(co_await listener.accept(), ws::role::server, bounds);
    co_await socket.accept();
    bool rejected = false;
    try { (void)co_await socket.receive(); }
    catch (const ws::protocol_error& error) { rejected = error.close_code() == expected; }
    require(rejected && !socket.is_open(), "invalid frame was accepted or wrong close code returned");
    done.finish();
}
pace::task<void> invalid_raw_client(pace::io_context& io, pace::tcp::endpoint endpoint, completion& done, std::string wire) {
    pace::tcp::stream socket(io); co_await socket.connect(std::move(endpoint));
    co_await socket.write_all(request);
    std::string pending; (void)co_await read_head(socket, pending);
    co_await socket.write_all(wire);
    // The endpoint reports protocol_error independently of peer TCP reset timing.
    try { std::array<char, 256> buffer{}; while (co_await socket.read_some(buffer)) {} }
    catch (const std::system_error&) {}
    socket.close(); done.finish();
}
void bad_frame(std::string wire, unsigned expected = 1002, ws::limits bounds = {}) {
    pace::io_context io; pace::tcp::listener listener(io, {"127.0.0.1", 0}); completion done{io};
    io.spawn(invalid_frame_server(listener, done, expected, bounds));
    io.spawn(invalid_raw_client(io, listener.local_endpoint(), done, std::move(wire))); io.run();
    require(done.count == 2, "invalid-frame test incomplete");
}

pace::task<void> invalid_upgrade_server(pace::tcp::listener& listener, completion& done, bool deny) {
    ws::stream socket(co_await listener.accept(), ws::role::server);
    bool rejected = false;
    try { (void)co_await socket.accept({}, [deny](const auto&) { return !deny; }); }
    catch (const ws::protocol_error&) { rejected = true; }
    require(rejected && !socket.is_open(), "invalid upgrade accepted"); done.finish();
}
pace::task<void> invalid_upgrade_client(pace::io_context& io, pace::tcp::endpoint endpoint, completion& done,
                                       std::string wire, unsigned status) {
    pace::tcp::stream socket(io); co_await socket.connect(std::move(endpoint)); co_await socket.write_all(wire);
    std::string pending; const auto response = co_await read_head(socket, pending);
    require(response.starts_with("HTTP/1.1 " + std::to_string(status) + " "), "upgrade rejection HTTP status");
    socket.close(); done.finish();
}
void bad_upgrade(std::string wire, unsigned status = 400, bool deny = false) {
    pace::io_context io; pace::tcp::listener listener(io, {"127.0.0.1", 0}); completion done{io};
    io.spawn(invalid_upgrade_server(listener, done, deny));
    io.spawn(invalid_upgrade_client(io, listener.local_endpoint(), done, std::move(wire), status)); io.run();
    require(done.count == 2, "upgrade rejection incomplete");
}

pace::task<void> silent_close_server(pace::tcp::listener& listener, completion& done) {
    ws::stream socket(co_await listener.accept(), ws::role::server); co_await socket.accept();
    co_await socket.context().sleep_for(20ms);
    socket.close(); done.finish();
}
pace::task<void> timed_close_client(pace::io_context& io, pace::tcp::endpoint endpoint, completion& done) {
    pace::tcp::stream transport(io); co_await transport.connect(std::move(endpoint));
    ws::stream socket(std::move(transport), ws::role::client); co_await socket.handshake("localhost");
    require(!(co_await socket.close_gracefully(1000, "", 1ms)) && !socket.is_open(), "close timeout did not bound silent peer");
    done.finish();
}
void bounded_close() {
    pace::io_context io; pace::tcp::listener listener(io, {"127.0.0.1", 0}); completion done{io};
    io.spawn(silent_close_server(listener, done)); io.spawn(timed_close_client(io, listener.local_endpoint(), done)); io.run();
    require(done.count == 2, "bounded close incomplete");
}

pace::task<void> overlap_and_cancel(ws::stream& socket, bool& rejected) {
    try { co_await socket.ping("overlap"); }
    catch (const std::logic_error&) { rejected = true; }
    require(rejected && socket.is_open(), "overlapping send altered the active receive");
    socket.close(); // Owner stays alive until the pending receive resumes.
}
pace::task<void> canceled_client(pace::io_context& io, pace::tcp::endpoint endpoint, completion& done) {
    pace::tcp::stream transport(io); co_await transport.connect(std::move(endpoint));
    ws::stream socket(std::move(transport), ws::role::client); co_await socket.handshake("localhost");
    bool rejected = false, canceled = false;
    io.spawn(overlap_and_cancel(socket, rejected));
    try { (void)co_await socket.receive(); }
    catch (const std::system_error& error) { canceled = error.code() == std::errc::operation_canceled; }
    require(rejected && canceled && !socket.is_open(), "pending WebSocket read did not cancel safely");
    done.finish();
}
void cancellation_and_overlap() {
    pace::io_context io; pace::tcp::listener listener(io, {"127.0.0.1", 0}); completion done{io};
    io.spawn(silent_close_server(listener, done)); io.spawn(canceled_client(io, listener.local_endpoint(), done)); io.run();
    require(done.count == 2, "cancellation/overlap incomplete");
}

pace::task<void> deadline_lifetime_server(pace::tcp::listener& listener, completion& done) {
    ws::stream socket(co_await listener.accept(), ws::role::server); co_await socket.accept();
    require(!(co_await socket.receive()), "expected close in lifetime test");
    co_await socket.context().sleep_for(300ms); // Keep reactor alive after the client's deadline expires.
    done.finish();
}
pace::task<void> deadline_lifetime_client(pace::io_context& io, pace::tcp::endpoint endpoint, completion& done) {
    {
        pace::tcp::stream transport(io); co_await transport.connect(std::move(endpoint));
        ws::stream original(std::move(transport), ws::role::client);
        ws::stream socket(std::move(original));
        require(!original.is_open(), "moved-from WebSocket retained transport");
        co_await socket.handshake("localhost");
        require(co_await socket.close_gracefully(1000, "", 100ms), "lifetime close did not complete");
    } // Pending cooperative deadline must not retain a pointer to the destroyed transport.
    co_await io.sleep_for(200ms);
    done.finish();
}
void close_deadline_lifetime() {
    pace::io_context io; pace::tcp::listener listener(io, {"127.0.0.1", 0}); completion done{io};
    io.spawn(deadline_lifetime_server(listener, done));
    io.spawn(deadline_lifetime_client(io, listener.local_endpoint(), done)); io.run();
    require(done.count == 2, "close deadline lifetime incomplete");
}
} // namespace

int main(int argc, char** argv) {
    watchdog guard;
    try {
        roundtrip(); control_interleaving(); bounded_close(); cancellation_and_overlap(); close_deadline_lifetime();
        bad_frame(frame(1, "unmasked", true, false));
        auto reserved = frame(1, "reserved"); reserved[0] |= 0x40; bad_frame(reserved);
        bad_frame(frame(3, "reserved opcode"));
        bad_frame(frame(0, "orphan continuation"));
        bad_frame(frame(1, "start", false) + frame(1, "new"));
        bad_frame(frame(9, "fragmented ping", false));
        bad_frame(frame(9, std::string(126, 'x')));
        bad_frame(frame(8, "x"));
        bad_frame(frame(8, std::string("\x03\xED", 2))); // Forbidden 1005.
        bad_frame(frame(1, std::string("\xC0\xAF", 2)), 1007); // Overlong UTF-8.
        bad_frame(frame(1, std::string("\xED\xA0\x80", 3)), 1007); // Surrogate.
        bad_frame(frame(8, std::string("\x03\xE8\xFF", 3)), 1007);
        bad_frame(std::string("\x81\xFE\0\1", 4)); // Nonminimal length; no allocation/body read.
        bad_frame(std::string("\x81\xFF\x80\0\0\0\0\0\0\0", 10));
        bad_frame(frame(2, std::string(129, 'x')), 1009, {.max_frame_bytes = 128, .max_message_bytes = 128});
        bad_frame(frame(2, std::string(100, 'x'), false) + frame(0, std::string(100, 'x')), 1009,
                  {.max_frame_bytes = 128, .max_message_bytes = 128});
        bad_frame(frame(1, "", false) + frame(0, "", false) + frame(0, ""), 1009,
                  {.max_fragments = 2});
        bad_frame(frame(10, "") + frame(10, "") + frame(10, ""), 1008, {.max_control_frames = 2});
        auto wrong_version = std::string(request); wrong_version.replace(wrong_version.find("Version: 13"), 11, "Version: 12");
        bad_upgrade(std::move(wrong_version), 426);
        auto duplicate = std::string(request); duplicate.insert(duplicate.size() - 2, "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"); bad_upgrade(std::move(duplicate));
        auto bad_key = std::string(request); bad_key.replace(bad_key.find("dGhlIHNhbXBsZSBub25jZQ=="), 24, "invalid"); bad_upgrade(std::move(bad_key));
        bad_upgrade(std::string(request), 403, true);
#ifdef PACE_HAS_TLS
        const auto fixtures = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path(__FILE__).parent_path() / "certs";
        secure_roundtrip(fixtures);
#else
        (void)argc; (void)argv;
#endif
        std::cout << "WebSocket tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
