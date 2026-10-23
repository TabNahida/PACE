#include <pace/http3.hpp>
#include <array>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
using namespace std::chrono_literals;
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
struct watchdog {
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    std::thread thread{[this] {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, 45s, [&] { return done; })) std::abort();
    }};
    ~watchdog() { { std::lock_guard lock(mutex); done = true; } changed.notify_one(); thread.join(); }
};
struct fixture {
    pace::io_context io;
    pace::quic::context server{pace::quic::role::server}, client;
    std::unique_ptr<pace::quic::listener> listener;
    fixture(const std::filesystem::path& certs, std::string protocol = "h3") {
        server.use_certificate_chain((certs/"localhost.pem").string(), (certs/"localhost-key.pem").string());
        client.load_verify_file((certs/"ca.pem").string());
        server.set_alpn_protocols({protocol}); client.set_alpn_protocols({protocol});
        listener = std::make_unique<pace::quic::listener>(io, pace::udp::endpoint{"127.0.0.1", 0}, server);
    }
};
std::string payload() {
    std::string value(256 * 1024 + 7, '\0');
    for (std::size_t i = 0; i < value.size(); ++i) value[i] = static_cast<char>(i % 251);
    return value;
}
pace::task<pace::http::response> echo(pace::http::request request) {
    require(request.version == "HTTP/3", "request version");
    require(request.header("host") == "localhost", "authority mapped to host");
    pace::http::response result;
    if (request.target == "/fail") throw std::runtime_error("application failure");
    if (request.target == "/reset") { result.status = 205; result.body = "must be suppressed"; }
    else if (request.target == "/head") result.body = "HEAD metadata";
    else if (request.target == "/large") {
        require(request.body == payload(), "large request flow control");
        require(request.trailers.size() == 1 && request.trailers[0].value == "request", "request trailers");
        result.body = std::move(request.body);
        result.trailers.push_back({"x-trailer", "response"});
    } else result.body = "ok";
    co_return result;
}
std::vector<pace::http::request> requests() {
    std::vector<pace::http::request> batch;
    pace::http::request large;
    large.method = "POST"; large.target = "/large"; large.body = payload();
    large.headers.push_back({"content-type", "application/octet-stream"});
    large.trailers.push_back({"x-trailer", "request"}); batch.push_back(std::move(large));
    for (const auto* path : {"/head", "/reset", "/fail", "/"}) {
        pace::http::request request;
        request.method = std::string_view(path) == "/head" ? "HEAD" : "GET";
        request.target = path; batch.push_back(std::move(request));
    }
    return batch;
}
std::vector<pace::http::request> one_request() {
    std::vector<pace::http::request> batch(1); batch[0].method = "GET"; batch[0].target = "/"; return batch;
}
pace::task<void> batch_client(fixture& test, bool& complete) {
    pace::quic::connection connection(test.io, test.client);
    co_await connection.connect(test.listener->local_endpoint(), "localhost");
    auto replies = co_await pace::http3::exchange(std::move(connection), "localhost", requests());
    require(replies.size() == 5, "batch result count");
    for (const auto& reply : replies) require(reply.version == "HTTP/3", "response version");
    require(replies[0].body == payload(), "large response flow control");
    require(replies[0].trailers.size() == 1 && replies[0].trailers[0].value == "response", "response trailers");
    require(replies[1].status == 200 && replies[1].body.empty() && replies[1].header("content-length") == "13", "HEAD response");
    require(replies[2].status == 205 && replies[2].body.empty(), "205 body suppression");
    require(replies[3].status == 500, "handler failure isolation");
    require(replies[4].body == "ok", "batch input ordering");
    complete = true; test.io.stop();
}
void roundtrip(const std::filesystem::path& certs) {
    fixture test(certs); bool complete = false;
    test.io.spawn(pace::http3::serve(*test.listener, echo));
    test.io.spawn(batch_client(test, complete)); test.io.run(); require(complete, "roundtrip incomplete");
}
pace::task<void> stream_credit_client(fixture& test, bool& complete) {
    pace::quic::connection connection(test.io, test.client);
    co_await connection.connect(test.listener->local_endpoint(), "localhost");
    std::vector<pace::http::request> batch(130);
    for (auto& request : batch) { request.method = "GET"; request.target = "/"; }
    pace::http3::limits limits; limits.max_concurrent_streams = batch.size();
    const auto replies = co_await pace::http3::exchange(std::move(connection), "localhost", std::move(batch), limits);
    require(replies.size() == 130, "stream credit response count");
    for (const auto& reply : replies) require(reply.status == 200 && reply.body == "ok", "stream credit response");
    complete = true; test.io.stop();
}
void stream_credit(const std::filesystem::path& certs) {
    fixture test(certs); bool complete = false;
    pace::http3::limits limits; limits.max_concurrent_streams = 130;
    test.io.spawn(pace::http3::serve(*test.listener, echo, limits));
    test.io.spawn(stream_credit_client(test, complete)); test.io.run(); require(complete, "stream credit incomplete");
}

// Independent RFC 9114 frames and RFC 9204 static-table QPACK. These peers do
// not use nghttp3; this prevents a paired implementation bug passing roundtrip.
std::string bytes(std::initializer_list<unsigned> values) {
    std::string result; for (const auto value : values) result.push_back(static_cast<char>(value)); return result;
}
pace::task<std::vector<pace::quic::stream>> raw_controls(pace::quic::connection& connection) {
    std::vector<pace::quic::stream> streams;
    for (const auto type : {0u, 2u, 3u}) {
        streams.push_back(co_await connection.open_stream(pace::quic::stream_type::unidirectional));
        const auto wire = type == 0 ? bytes({0, 4, 0}) : bytes({type});
        co_await streams.back().write_all(wire);
    }
    co_return streams;
}
pace::task<std::string> read_all(pace::quic::stream& stream) {
    std::string result; std::array<char, 4096> buffer{};
    while (const auto size = co_await stream.read_some(buffer)) result.append(buffer.data(), size);
    co_return result;
}
pace::task<void> raw_server(fixture& test, bool malformed) {
    auto connection = co_await test.listener->accept();
    auto controls = co_await raw_controls(connection);
    std::vector<pace::quic::stream> peer_controls;
    for (;;) {
        auto stream = co_await connection.accept_stream();
        if (stream.is_unidirectional()) { peer_controls.push_back(std::move(stream)); continue; }
        const auto request = co_await read_all(stream);
        require(!request.empty() && request[0] == 1, "client emitted HEADERS frame");
        // QPACK prefix 0,0 + indexed static :status=200 (25). Optional
        // content-length=5 then DATA="wire" exposes truncated-message handling.
        auto response = malformed ? bytes({1, 6, 0, 0, 0xd9, 0x54, 1, '5', 0, 4})
                                  : bytes({1, 3, 0, 0, 0xd9, 0, 4});
        response += "wire";
        co_await stream.write_all(response); stream.finish_write();
        // Keep the connection and critical streams alive until the client closes.
        try { for (;;) co_await connection.wait_events(); }
        catch (const std::system_error&) { co_return; }
    }
}
pace::task<void> raw_server_client(fixture& test, bool malformed, bool& complete) {
    pace::quic::connection connection(test.io, test.client);
    co_await connection.connect(test.listener->local_endpoint(), "localhost");
    bool rejected = false;
    try {
        auto replies = co_await pace::http3::exchange(std::move(connection), "localhost", one_request());
        require(!malformed && replies.size() == 1 && replies[0].status == 200 && replies[0].body == "wire", "independent server response");
    } catch (const pace::http::protocol_error&) { rejected = true; }
    require(rejected == malformed, "malformed content-length not rejected");
    complete = true; test.io.stop();
}
pace::task<void> bounded_client(fixture& test, bool& complete) {
    pace::quic::connection connection(test.io, test.client);
    co_await connection.connect(test.listener->local_endpoint(), "localhost");
    pace::http3::limits limits; limits.max_body_bytes = 3;
    bool rejected = false;
    try { (void)co_await pace::http3::exchange(std::move(connection), "localhost", one_request(), limits); }
    catch (const std::length_error&) { rejected = true; }
    require(rejected, "incoming response body limit");
    complete = true; test.io.stop();
}
std::uint64_t varint(const std::string& data, std::size_t& cursor) {
    require(cursor < data.size(), "truncated frame varint");
    const auto first = static_cast<unsigned char>(data[cursor++]);
    const auto length = 1u << (first >> 6); std::uint64_t value = first & 63;
    for (unsigned i = 1; i < length; ++i) { require(cursor < data.size(), "truncated frame varint"); value = (value << 8) | static_cast<unsigned char>(data[cursor++]); }
    return value;
}
pace::task<void> raw_client(fixture& test, bool& complete) {
    pace::quic::connection connection(test.io, test.client);
    co_await connection.connect(test.listener->local_endpoint(), "localhost");
    auto controls = co_await raw_controls(connection);
    auto stream = co_await connection.open_stream();
    // GET https://localhost/ using static indexed GET/https/path and a
    // literal :authority value with static name reference.
    auto request = bytes({1, 16, 0, 0, 0xd1, 0xd7, 0xc1, 0x50, 9}); request += "localhost";
    co_await stream.write_all(request); stream.finish_write();
    const auto response = co_await read_all(stream);
    std::size_t cursor = 0; bool headers = false; std::string body;
    while (cursor < response.size()) {
        const auto type = varint(response, cursor), length = varint(response, cursor);
        require(length <= response.size() - cursor, "truncated HTTP3 frame");
        if (type == 1) headers = true;
        if (type == 0) body.append(response, cursor, static_cast<std::size_t>(length));
        cursor += static_cast<std::size_t>(length);
    }
    require(headers && body == "ok", "independent client response");
    connection.close(); complete = true; test.io.stop();
}
void interop(const std::filesystem::path& certs) {
    for (bool malformed : {false, true}) {
        fixture test(certs); bool complete = false;
        test.io.spawn(raw_server(test, malformed)); test.io.spawn(raw_server_client(test, malformed, complete));
        test.io.run(); require(complete, "independent server incomplete");
    }
    fixture test(certs); bool complete = false;
    test.io.spawn(pace::http3::serve(*test.listener, echo)); test.io.spawn(raw_client(test, complete));
    test.io.run(); require(complete, "independent client incomplete");
}
pace::task<void> validation(fixture& test, bool& complete) {
    for (unsigned variant = 0; variant < 6; ++variant) {
        auto batch = one_request(); pace::http3::limits limits;
        switch (variant) {
        case 0: batch[0].headers.push_back({"connection", "close"}); break;
        case 1: batch[0].target = "/bad\r\n"; break;
        case 2: batch[0].method = "CONNECT"; break;
        case 3: batch[0].trailers.push_back({"content-length", "1"}); break;
        case 4: limits.max_header_bytes = 1; break;
        case 5: limits.max_concurrent_streams = 0; break;
        }
        bool rejected = false;
        try { (void)co_await pace::http3::exchange(pace::quic::connection(test.io, test.client), "localhost", std::move(batch), limits); }
        catch (const std::invalid_argument&) { rejected = true; }
        catch (const std::length_error&) { rejected = true; }
        require(rejected, "invalid HTTP3 request reached transport");
    }
    complete = true; test.io.stop();
}
pace::task<void> silent_server(fixture& test, bool stop) {
    auto connection = co_await test.listener->accept();
    if (stop) { co_await test.io.sleep_for(10ms); test.io.stop(); }
    else { try { for (;;) co_await connection.wait_events(); } catch (const std::system_error&) {} }
}
pace::task<void> rejection_client(fixture& test, bool cancellation, bool& complete) {
    pace::quic::connection connection(test.io, test.client);
    co_await connection.connect(test.listener->local_endpoint(), "localhost");
    bool rejected = false;
    try { (void)co_await pace::http3::exchange(std::move(connection), "localhost", one_request()); }
    catch (const std::invalid_argument&) { rejected = !cancellation; }
    catch (const std::system_error& error) { rejected = cancellation && error.code() == std::errc::operation_canceled; }
    require(rejected, "HTTP3 ALPN/cancellation rejection");
    complete = true; test.io.stop();
}
pace::task<void> rejected_peer_then_valid(fixture& test, const std::filesystem::path& certs, bool& complete) {
    pace::quic::context wrong_protocol;
    wrong_protocol.load_verify_file((certs/"ca.pem").string());
    wrong_protocol.set_alpn_protocols({"wrong"});
    bool rejected = false;
    try {
        pace::quic::connection bad(test.io, wrong_protocol);
        co_await bad.connect(test.listener->local_endpoint(), "localhost", 100ms);
    } catch (const std::system_error&) { rejected = true; }
    require(rejected, "wrong-ALPN handshake unexpectedly succeeded");
    pace::quic::connection connection(test.io, test.client);
    co_await connection.connect(test.listener->local_endpoint(), "localhost");
    const auto replies = co_await pace::http3::exchange(std::move(connection), "localhost", one_request());
    require(replies.size() == 1 && replies[0].body == "ok", "failed handshake stopped listener");
    complete = true; test.io.stop();
}
void errors(const std::filesystem::path& certs) {
    { fixture test(certs); bool complete = false; test.io.spawn(validation(test, complete)); test.io.run(); require(complete, "validation incomplete"); }
    { fixture test(certs); bool complete = false;
      test.io.spawn(raw_server(test, false)); test.io.spawn(bounded_client(test, complete));
      test.io.run(); require(complete, "incoming limit check incomplete"); }
    { fixture test(certs); bool complete = false;
      test.io.spawn(pace::http3::serve(*test.listener, echo)); test.io.spawn(rejected_peer_then_valid(test, certs, complete));
      test.io.run(); require(complete, "failed handshake isolation incomplete"); }
    for (bool cancellation : {false, true}) {
        fixture test(certs, cancellation ? "h3" : "other"); bool complete = false;
        test.io.spawn(silent_server(test, cancellation)); test.io.spawn(rejection_client(test, cancellation, complete));
        test.io.run(); require(complete, "rejection incomplete");
    }
}
} // namespace
int main(int argc, char** argv) {
    watchdog deadline;
    try {
        const auto certs = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path(__FILE__).parent_path()/"certs";
        roundtrip(certs); stream_credit(certs); interop(certs); errors(certs);
        std::cout << "HTTP/3 encrypted multiplexing, flow control, trailers, independent wire peers, limits, ALPN and cancellation passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
