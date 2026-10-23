#include <pace/http.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <type_traits>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <memory>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
std::filesystem::path certificate_directory = std::filesystem::path(__FILE__).parent_path() / "certs";
using namespace std::chrono_literals;
using pace::http::request;
using pace::http::response;
void require(bool result, const char* message) { if (!result) throw std::runtime_error(message); }

request message(std::string method = "GET", std::string target = "/") {
    request value;
    value.method = std::move(method); value.target = std::move(target);
    value.headers.push_back({"Host", "localhost"});
    return value;
}

pace::task<response> route(request value) {
    if (value.target == "/host") co_return response{.body = std::string(value.header("Host"))};
    if (value.target == "/chunks") {
        response result{.body = value.body.empty() ? "chunk response" : value.body};
        result.chunked = true;
        result.trailers.push_back({"X-Checksum", "verified"});
        co_return result;
    }
    if (value.target == "/trailers") {
        require(value.chunked && value.trailers.size() == 1 && value.trailers[0].name == "X-Check" &&
                value.trailers[0].value == "yes", "request trailers were not parsed");
        co_return response{.body = value.body};
    }
    if (value.target == "/empty") co_return response{.status = 204, .body = "hidden"};
    if (value.target == "/reset") co_return response{.status = 205, .body = "hidden"};
    if (value.target == "/cached") co_return response{.status = 304, .body = "hidden"};
    co_return response{.body = value.body.empty() ? value.target : value.body};
}

pace::task<void> server(std::shared_ptr<pace::tcp::listener> listener, pace::http::limits bounds = {}) {
    co_await pace::http::serve(*listener, route, bounds);
}

pace::task<std::string> raw_exchange(pace::io_context& io, pace::tcp::endpoint endpoint, std::string bytes) {
    pace::tcp::stream socket(io);
    co_await socket.connect(std::move(endpoint));
    co_await socket.write_all(bytes);
    std::string result;
    std::array<char, 2048> buffer{};
    while (const auto count = co_await socket.read_some(buffer)) {
        result.append(buffer.data(), count);
        require(result.size() < 1024 * 1024, "raw response exceeded test bound");
    }
    co_return result;
}

pace::task<void> normal_client(pace::io_context& io) {
    auto listener = std::make_shared<pace::tcp::listener>(io, pace::tcp::endpoint{"127.0.0.1", 0});
    io.spawn(server(listener));
    const auto address = listener->local_endpoint();
    auto simple = co_await pace::http::get(io, address, "/simple");
    require(simple.status == 200 && simple.body == "/simple" && simple.close, "one-shot get failed");
    pace::tcp::stream socket(io);
    co_await socket.connect(address);
    pace::http::client client(std::move(socket));
    for (const auto* target : {"/", "/empty", "/reset", "/cached", "/chunks"}) {
        auto incoming = co_await client.send(message("HEAD", target));
        require(incoming.body.empty() && incoming.trailers.empty() && client.is_open(), "HEAD must preserve reuse without consuming body");
    }
    auto outgoing = message("POST", "/chunks");
    outgoing.body.assign("a\0b", 3);
    outgoing.chunked = true;
    outgoing.trailers.push_back({"X-Sent", "ok"});
    auto incoming = co_await client.send(outgoing);
    require(incoming.body == outgoing.body && incoming.chunked && incoming.trailers.size() == 1 &&
            incoming.trailers[0].value == "verified" && incoming.header("TRANSFER-ENCODING") == "chunked",
            "chunked client/server binary payload and trailers");
    outgoing = message("POST", "/trailers"); outgoing.body = "hello"; outgoing.chunked = true;
    outgoing.trailers.push_back({"X-Check", "yes"}); outgoing.keep_alive = false;
    incoming = co_await client.send(std::move(outgoing));
    require(incoming.body == "hello" && !client.is_open(), "request trailers/explicit close failed");
    bool rejected = false;
    try { co_await client.send(message()); } catch (const std::logic_error&) { rejected = true; }
    require(rejected, "closed HTTP client accepted another request");
}

pace::task<void> ipv6_http_authority(pace::io_context& io) {
    auto listener = std::make_shared<pace::tcp::listener>(io, pace::tcp::endpoint{"::1", 0});
    io.spawn(server(listener));
    const auto endpoint = listener->local_endpoint();
    const auto result = co_await pace::http::get(io, endpoint, "/host");
    require(result.body == "[::1]:" + std::to_string(endpoint.port), "IPv6 default Host must bracket the literal");
    auto outgoing = message("GET", "/host");
    outgoing.headers.front().value = "[::1]";
    const auto supplied = co_await pace::http::fetch(io, endpoint, outgoing);
    require(supplied.body == "[::1]", "supplied IPv6 Host was not preserved");
}

pace::task<void> chunk_pipeline(pace::io_context& io) {
    auto listener = std::make_shared<pace::tcp::listener>(io, pace::tcp::endpoint{"127.0.0.1", 0});
    io.spawn(server(listener));
    const auto request_bytes = std::string(
        "POST /trailers HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nTrailer: X-Check\r\n\r\n"
        "2;name=token;quoted=\"a\\\"b\"\r\nhe\r\n3 ; next=value\r\nllo\r\n0\r\nX-Check: yes\r\n\r\n"
        "GET /second HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    auto result = co_await raw_exchange(io, listener->local_endpoint(), request_bytes);
    const auto second = result.find("HTTP/1.1 200", 1);
    require(result.starts_with("HTTP/1.1 200") && second != std::string::npos &&
            result.find("\r\n\r\nhello") < second && result.ends_with("\r\n\r\n/second"),
            "chunked parser lost pipelined bytes");
    pace::tcp::stream fragmented(io);
    co_await fragmented.connect(listener->local_endpoint());
    const std::array<std::string_view, 5> pieces{
        "POST /trailers HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n",
        "5\r", "\nhello\r\n0\r\nX-Check:", " yes\r", "\n\r\n"};
    for (auto piece : pieces) { co_await fragmented.write_all(piece); co_await io.sleep_for(1ms); }
    result.clear(); std::array<char, 512> buffer{};
    while (const auto count = co_await fragmented.read_some(buffer)) result.append(buffer.data(), count);
    require(result.ends_with("\r\n\r\nhello"), "fragmented chunk/trailer parse failed");
}

pace::task<void> rejected_requests(pace::io_context& io) {
    auto listener = std::make_shared<pace::tcp::listener>(io, pace::tcp::endpoint{"127.0.0.1", 0});
    pace::http::limits bounds;
    bounds.max_body_bytes = 8; bounds.max_trailer_bytes = 48; bounds.max_trailer_fields = 1;
    bounds.max_chunk_line_bytes = 32; bounds.max_header_fields = 6; bounds.max_chunks = 3;
    io.spawn(server(listener, bounds));
    struct test_case { std::string extra, body; unsigned status; };
    const test_case cases[]{
        {"Transfer-Encoding: chunked\r\nContent-Length: 0\r\n", "0\r\n\r\n", 400},
        {"Transfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n", "0\r\n\r\n", 400},
        {"Transfer-Encoding: chunked, chunked\r\n", "0\r\n\r\n", 501},
        {"Transfer-Encoding: gzip\r\n", "", 501},
        {"Transfer-Encoding: chunked\r\n", "+1\r\na\r\n0\r\n\r\n", 400},
        {"Transfer-Encoding: chunked\r\n", "1\r\naXX0\r\n\r\n", 400},
        {"Transfer-Encoding: chunked\r\n", "9\r\n123456789\r\n0\r\n\r\n", 413},
        {"Transfer-Encoding: chunked\r\n", "ffffffffffffffffffffffff\r\n", 400},
        {"Transfer-Encoding: chunked\r\n", "1;bad=\"unfinished\r\na\r\n0\r\n\r\n", 400},
        {"Transfer-Encoding: chunked\r\n", "0\r\nContent-Length: 1\r\n\r\n", 400},
        {"Transfer-Encoding: chunked\r\nTrailer: Host\r\n", "0\r\n\r\n", 400},
        {"Transfer-Encoding: chunked\r\nConnection: X-Hidden\r\n", "0\r\nX-Hidden: yes\r\n\r\n", 400},
        {"Transfer-Encoding: chunked\r\n", "0\r\nX-A: one\r\nX-B: two\r\n\r\n", 431},
        {"Transfer-Encoding: chunked\r\n", "0\r\nX-A: " + std::string(48, 'x') + "\r\n\r\n", 431},
        {"Transfer-Encoding: chunked\r\n", "1;" + std::string(32, 'x') + "\r\na\r\n0\r\n\r\n", 431},
        {"Transfer-Encoding: chunked\r\n", "1\r\na\r\n1\r\nb\r\n1\r\nc\r\n0\r\n\r\n", 413},
        {"Content-Length: 0\r\nContent-Length: 0\r\n", "", 400},
        {"Content-Length: 0\r\nConnection: Content-Length\r\n", "", 400},
        {"Content-Length: 0\r\nTrailer: X-End\r\n", "", 400},
        {"X-A: a\r\nX-B: b\r\nX-C: c\r\nX-D: d\r\nX-E: e\r\nX-F: f\r\n", "", 431},
    };
    for (const auto& test : cases) {
        const auto bytes = "POST / HTTP/1.1\r\nHost: localhost\r\n" + test.extra + "\r\n" + test.body;
        const auto result = co_await raw_exchange(io, listener->local_endpoint(), bytes);
        require(result.starts_with("HTTP/1.1 " + std::to_string(test.status)), "rejected request returned wrong status");
        require(result.find("Connection: close\r\n") != std::string::npos, "bad framing connection stayed alive");
    }
}

pace::task<void> raw_server(std::shared_ptr<pace::tcp::listener> listener, std::string response_bytes,
                            bool fragment = false) {
    auto socket = co_await listener->accept();
    std::string head; std::array<char, 1024> buffer{};
    while (head.find("\r\n\r\n") == std::string::npos) {
        const auto count = co_await socket.read_some(buffer);
        if (!count) co_return;
        head.append(buffer.data(), count);
    }
    try {
        if (fragment) {
            for (std::size_t i = 0; i < response_bytes.size(); i += 3) {
                co_await socket.write_all(std::string_view(response_bytes).substr(i, 3));
                co_await socket.context().yield();
            }
        } else co_await socket.write_all(response_bytes);
        co_await socket.close_gracefully();
    } catch (const std::system_error&) { /* Invalid-response client may close immediately. */ }
}

pace::task<response> exchange_raw_response(pace::io_context& io, std::string response_bytes,
                                          pace::http::limits bounds = {}, std::string method = "GET") {
    auto listener = std::make_shared<pace::tcp::listener>(io, pace::tcp::endpoint{"127.0.0.1", 0});
    io.spawn(raw_server(listener, std::move(response_bytes), true));
    auto outgoing = message(std::move(method));
    co_return co_await pace::http::fetch(io, listener->local_endpoint(), std::move(outgoing), bounds);
}

pace::task<void> response_framing(pace::io_context& io) {
    auto result = co_await exchange_raw_response(io,
        "HTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\nHTTP/1.1 100 Continue\r\n\r\n"
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTrailer: X-End\r\n\r\n"
        "2\r\nhe\r\n3\r\nllo\r\n0\r\nX-End: yes\r\n\r\n");
    require(result.body == "hello" && result.trailers.size() == 1 && result.trailers[0].value == "yes",
            "informational/chunked response parsing failed");
    result = co_await exchange_raw_response(io, "HTTP/1.0 200 OK\r\n\r\nclose-delimited");
    require(result.close && result.body == "close-delimited", "EOF-delimited response failed");
    pace::http::limits bounds; bounds.max_body_bytes = 4;
    result = co_await exchange_raw_response(io, "HTTP/1.1 200 OK\r\nContent-Length: 99999\r\n\r\n", bounds, "HEAD");
    require(result.body.empty(), "HEAD incorrectly applied body size/read rules");
    result = co_await exchange_raw_response(io, "HTTP/1.1 304 Not Modified\r\nContent-Length: 99999\r\n\r\n", bounds);
    require(result.status == 304 && result.body.empty(), "304 consumed representation body");
    const std::string invalid[]{
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx",
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nConnection: Content-Length\r\n\r\nx",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 0\r\n\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nxx",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nx",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nHost: bad\r\n\r\n",
        "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 205 Reset Content\r\nContent-Length: 1\r\n\r\nx",
        "HTTP/1.1 101 Switching Protocols\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nlarge",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n",
        "HTTP/1.1 200 OK\r\n\r\ntoo large",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r",
        "HTTP/1.1 100 Continue\r\nContent-Length: 0\r\n\r\n",
    };
    for (const auto& bytes : invalid) {
        bool rejected = false;
        try { co_await exchange_raw_response(io, bytes, bounds); }
        catch (const pace::http::protocol_error&) { rejected = true; }
        require(rejected, "malformed/oversized response was accepted");
    }
    bounds.max_informational_responses = 1;
    bool rejected = false;
    try { co_await exchange_raw_response(io, "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Hint\r\n\r\n", bounds); }
    catch (const pace::http::protocol_error&) { rejected = true; }
    require(rejected, "unbounded informational responses accepted");
}

pace::task<void> duplicate_send(pace::http::client& client, bool& rejected) {
    try { co_await client.send(message()); }
    catch (const std::logic_error&) { rejected = true; }
}

pace::task<void> client_state(pace::io_context& io) {
    auto listener = std::make_shared<pace::tcp::listener>(io, pace::tcp::endpoint{"127.0.0.1", 0});
    io.spawn(server(listener));
    pace::tcp::stream stream(io); co_await stream.connect(listener->local_endpoint());
    pace::http::client client(std::move(stream));
    auto invalid = message(); invalid.headers.push_back({"Content-Length", "0"});
    bool rejected = false;
    try { co_await client.send(std::move(invalid)); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && client.is_open(), "invalid local request should not poison unused connection");
    rejected = false;
    io.spawn(duplicate_send(client, rejected));
    const auto result = co_await client.send(message());
    while (!rejected) co_await io.yield();
    require(result.status == 200 && client.is_open(), "overlap rejection poisoned active exchange");
    client.close();
}

pace::task<void> stalled_server(std::shared_ptr<pace::tcp::listener> listener) {
    auto socket = co_await listener->accept();
    std::array<char, 1024> buffer{};
    co_await socket.read_some(buffer);
    co_await socket.context().sleep_for(1h);
}
pace::task<void> stop_later(pace::io_context& io) {
    co_await io.sleep_for(5ms);
    io.stop();
}
pace::task<void> client_cancellation(pace::io_context& io) {
    auto listener = std::make_shared<pace::tcp::listener>(io, pace::tcp::endpoint{"127.0.0.1", 0});
    io.spawn(stalled_server(listener));
    pace::tcp::stream stream(io);
    co_await stream.connect(listener->local_endpoint());
    pace::http::client client(std::move(stream));
    io.spawn(stop_later(io));
    bool canceled = false;
    try { co_await client.send(message()); }
    catch (const std::system_error& error) { canceled = error.code() == std::errc::operation_canceled; }
    require(canceled && !client.is_open(), "canceling an HTTP exchange must poison and close its transport");
}

#ifdef PACE_HAS_TLS
std::string cert(const char* name) {
    return (certificate_directory / name).string();
}
pace::task<void> secure_server(std::shared_ptr<pace::tcp::listener> listener,
                               std::shared_ptr<pace::tls::context> configuration) {
    co_await pace::http::serve(*listener, *configuration, route);
}
pace::task<void> https_exchange(pace::io_context& io) {
    auto configuration = std::make_shared<pace::tls::context>(pace::tls::role::server);
    configuration->use_certificate_chain(cert("localhost.pem"), cert("localhost-key.pem"));
    configuration->set_alpn_protocols({"http/1.1"});
    auto listener = std::make_shared<pace::tcp::listener>(io, pace::tcp::endpoint{"127.0.0.1", 0});
    io.spawn(secure_server(listener, configuration));
    pace::tls::context trusted;
    trusted.load_verify_file(cert("ca.pem")); trusted.set_alpn_protocols({"http/1.1"});
    const auto result = co_await pace::http::get(io, trusted, listener->local_endpoint(), "localhost", "/chunks");
    require(result.body == "chunk response" && result.chunked && result.trailers.size() == 1,
            "HTTPS client/server chunked exchange failed");
    bool rejected = false;
    try { co_await pace::http::get(io, trusted, listener->local_endpoint(), "wrong.example", "/"); }
    catch (const std::system_error&) { rejected = true; }
    require(rejected, "HTTPS accepted a wrong hostname");
    // A failed TLS peer must not end the server's accept task.
    const auto again = co_await pace::http::get(io, trusted, listener->local_endpoint(), "localhost", "/again");
    require(again.body == "/again", "HTTPS handshake failure killed serving");

    auto h2_configuration = std::make_shared<pace::tls::context>(pace::tls::role::server);
    h2_configuration->use_certificate_chain(cert("localhost.pem"), cert("localhost-key.pem"));
    h2_configuration->set_alpn_protocols({"h2"});
    auto h2_listener = std::make_shared<pace::tcp::listener>(io, pace::tcp::endpoint{"127.0.0.1", 0});
    io.spawn(secure_server(h2_listener, h2_configuration));
    pace::tls::context wrong_protocol;
    wrong_protocol.load_verify_file(cert("ca.pem")); wrong_protocol.set_alpn_protocols({"h2"});
    rejected = false;
    try { co_await pace::http::get(io, wrong_protocol, h2_listener->local_endpoint(), "localhost"); }
    catch (const std::system_error& error) { rejected = error.code() == std::errc::protocol_error; }
    require(rejected, "HTTP/1.1 exchanged bytes on an h2-negotiated transport");
}
#endif


struct producer_state {
    std::size_t admissions = 0, handled = 0, reads = 0, destroyed = 0, largest_buffer = 0;
    std::uint64_t produced = 0;
};
constexpr std::uint64_t large_response_bytes = 4 * 1024 * 1024 + 113;
constexpr std::uint64_t stalled_response_bytes = 128 * 1024 * 1024;

class generated_body final : public pace::http::body_source {
public:
    generated_body(std::shared_ptr<producer_state> state, std::uint64_t length, unsigned failure = 0)
        : state_(std::move(state)), length_(length), failure_(failure) {}
    ~generated_body() override { ++state_->destroyed; }
    std::size_t read(std::span<char> output) override {
        ++state_->reads;
        state_->largest_buffer = std::max(state_->largest_buffer, output.size());
        if (failure_ == 1) return 0;
        if (failure_ == 2) throw std::runtime_error("producer read failure");
        if (failure_ == 3) return output.size() + 1;
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(output.size(), length_ - offset_));
        for (std::size_t index = 0; index < count; ++index)
            output[index] = static_cast<char>((offset_ + index) % 251);
        offset_ += count; state_->produced += count;
        return count;
    }
private:
    std::shared_ptr<producer_state> state_;
    std::uint64_t length_, offset_ = 0;
    unsigned failure_;
};

class open_file_body final : public pace::http::body_source {
public:
    explicit open_file_body(std::shared_ptr<producer_state> state) : state_(std::move(state)), file_(std::tmpfile()) {
        if (!file_) throw std::runtime_error("cannot open temporary streaming test file");
        std::array<char, 4096> block{};
        std::uint64_t offset = 0;
        while (offset < large_response_bytes) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(block.size(), large_response_bytes - offset));
            for (std::size_t i = 0; i < count; ++i) block[i] = static_cast<char>((offset + i) % 251);
            if (std::fwrite(block.data(), 1, count, file_) != count) {
                std::fclose(file_); file_ = nullptr;
                throw std::runtime_error("cannot prepare streaming test file");
            }
            offset += count;
        }
        std::rewind(file_);
    }
    ~open_file_body() override { std::fclose(file_); ++state_->destroyed; }
    std::size_t read(std::span<char> output) override {
        ++state_->reads;
        state_->largest_buffer = std::max(state_->largest_buffer, output.size());
        const auto count = std::fread(output.data(), 1, output.size(), file_);
        if (std::ferror(file_)) throw std::runtime_error("streaming test file read failed");
        state_->produced += count;
        return count;
    }
private:
    std::shared_ptr<producer_state> state_;
    std::FILE* file_;
};

pace::task<pace::http::server_response> admitted_route(std::shared_ptr<producer_state> state, request value) {
    ++state->handled;
    if (value.target == "/generated" || value.target == "/stalled" || value.target == "/broken" ||
        value.target == "/file" || value.target == "/bodyless" || value.target == "/invalid-stream" ||
        value.target == "/throw-source" || value.target == "/overrun-source" || value.target == "/huge") {
        const auto length = value.target == "/huge" ? std::numeric_limits<std::uint64_t>::max() :
                            value.target == "/stalled" ? stalled_response_bytes : large_response_bytes;
        pace::http::streaming_response result;
        result.content_length = length;
        if (value.target == "/file") result.source = std::make_unique<open_file_body>(state);
        else result.source = std::make_unique<generated_body>(state, length,
            value.target == "/broken" ? 1 : value.target == "/throw-source" ? 2 : value.target == "/overrun-source" ? 3 : 0);
        if (value.target == "/bodyless") result.head.status = 205;
        if (value.target == "/invalid-stream") result.head.body = "conflicting buffered body";
        co_return result;
    }
    co_return response{.body = value.body.empty() ? value.target : value.body};
}

struct http_fixture {
    std::shared_ptr<pace::tcp::listener> listener;
    std::shared_ptr<producer_state> state = std::make_shared<producer_state>();
#ifdef PACE_HAS_TLS
    std::shared_ptr<pace::tls::context> configuration;
    std::shared_ptr<pace::tls::context> trust;
#endif
};

pace::task<void> admitted_server(http_fixture fixture, pace::http::server_options options) {
    pace::http::server_handler handle = [state = fixture.state](request value) {
        return admitted_route(state, std::move(value));
    };
#ifdef PACE_HAS_TLS
    if (fixture.configuration) {
        co_await pace::http::serve(*fixture.listener, *fixture.configuration, handle, options);
        co_return;
    }
#endif
    co_await pace::http::serve(*fixture.listener, handle, options);
}

template<class Transport>
http_fixture start_admitted_server(pace::io_context& io, std::chrono::milliseconds request_timeout = 1s,
                                  std::chrono::milliseconds write_timeout = 2s,
                                  std::size_t response_buffer_bytes = 4096,
                                  std::shared_ptr<producer_state> state = {}) {
    http_fixture result;
    if (state) result.state = std::move(state);
    result.listener = std::make_shared<pace::tcp::listener>(io, pace::tcp::endpoint{"127.0.0.1", 0});
#ifdef PACE_HAS_TLS
    if constexpr (std::is_same_v<Transport, pace::tls::stream>) {
        result.configuration = std::make_shared<pace::tls::context>(pace::tls::role::server);
        result.configuration->use_certificate_chain(cert("localhost.pem"), cert("localhost-key.pem"));
        result.configuration->set_alpn_protocols({"http/1.1"});
        result.trust = std::make_shared<pace::tls::context>();
        result.trust->load_verify_file(cert("ca.pem"));
        result.trust->set_alpn_protocols({"http/1.1"});
    }
#endif
    pace::http::server_options options;
    options.bounds.max_body_bytes = 2;
    options.response_buffer_bytes = response_buffer_bytes;
    options.request_timeout = request_timeout;
    options.write_timeout = write_timeout;
    options.admit = [state = result.state](const pace::http::request_head& head) {
        ++state->admissions;
        require(head.header("HOST") == "localhost", "admission lost validated Host");
        if (head.target == "/unauthorized") {
            require(head.content_length == 1024 && !head.chunked, "admission lost parsed content length");
            return pace::http::admission::reject(response{.status = 401, .body = "unauthorized\n"});
        }
        if (head.target == "/throw-admission") throw std::runtime_error("admission failure");
        if (head.target == "/small") return pace::http::admission::accept(4);
        if (head.target == "/large") return pace::http::admission::accept(16);
        if (head.target == "/zero") return pace::http::admission::accept(0);
        return pace::http::admission::accept();
    };
    io.spawn(admitted_server(result, options));
    return result;
}

template<class Transport>
pace::task<Transport> admitted_connect(pace::io_context& io, const http_fixture& fixture) {
    if constexpr (std::is_same_v<Transport, pace::tcp::stream>) {
        Transport socket(io);
        co_await socket.connect(fixture.listener->local_endpoint());
        co_return socket;
    }
#ifdef PACE_HAS_TLS
    else {
        Transport socket(io, *fixture.trust);
        co_await socket.connect(fixture.listener->local_endpoint(), "localhost");
        co_return socket;
    }
#endif
}

template<class Transport>
pace::task<std::string> admitted_raw(pace::io_context& io, const http_fixture& fixture, std::string wire,
                                    bool expect_truncation = false) {
    auto socket = co_await admitted_connect<Transport>(io, fixture);
    co_await socket.write_all(wire);
    std::array<char, 2048> buffer{};
    std::string result;
    try {
        while (const auto count = co_await socket.read_some(buffer)) {
            result.append(buffer.data(), count);
            require(result.size() < 16384, "small response exceeded bound");
        }
    } catch (const std::system_error&) { if (!expect_truncation) throw; }
    co_return result;
}

template<class Transport>
pace::task<void> admission_scenarios(pace::io_context& io) {
    auto fixture = start_admitted_server<Transport>(io, 500ms);
    struct test_case { const char* wire; unsigned status; bool callback; };
    const test_case cases[]{
        {"POST /unauthorized HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1024\r\n\r\n", 401, true},
        {"POST /small HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\n", 413, true},
        {"POST /small HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n2\r\n", 413, true},
        {"POST /zero HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n1\r\n", 413, true},
        {"POST /default HTTP/1.1\r\nHost: localhost\r\nContent-Length: 3\r\n\r\n", 413, true},
        {"POST /throw-admission HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1\r\n\r\n", 500, true},
        {"POST /small HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n", 400, false},
        {"POST /small HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nContent-Length: 1\r\n\r\n", 400, false},
    };
    for (const auto& test : cases) {
        const auto callbacks = fixture.state->admissions;
        const auto result = co_await admitted_raw<Transport>(io, fixture, test.wire);
        require(result.starts_with("HTTP/1.1 " + std::to_string(test.status)), "head admission failed before withheld body upload");
        require(result.find("Connection: close\r\n") != std::string::npos, "early rejection must close");
        require(fixture.state->admissions == callbacks + test.callback, "admission called before framing validation");
        require(fixture.state->handled == 0, "rejected body reached final handler");
    }
    pace::http::basic_client<Transport> client(co_await admitted_connect<Transport>(io, fixture));
    for (auto target : {"/small", "/large", "/small", "/large", "/zero"}) {
        auto outgoing = message("POST", target);
        outgoing.body = target == std::string_view("/large") ? "0123456789abcdef" :
                        target == std::string_view("/zero") ? "" : "1234";
        outgoing.chunked = target == std::string_view("/large");
        const auto expected = outgoing.body.empty() ? std::string(target) : outgoing.body;
        const auto result = co_await client.send(std::move(outgoing));
        require(result.status == 200 && result.body == expected && client.is_open(), "keep-alive did not select fresh body limit");
    }
    client.close();
    // The larger previous route must not raise the next request's limit, even
    // when both heads and the accepted body arrived in one transport read.
    const auto pipelined = co_await admitted_raw<Transport>(io, fixture,
        "POST /large HTTP/1.1\r\nHost: localhost\r\nContent-Length: 16\r\n\r\n0123456789abcdef"
        "POST /small HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\n");
    require(pipelined.starts_with("HTTP/1.1 200") && pipelined.find("HTTP/1.1 413") != std::string::npos,
            "pipelined route inherited previous body limit");
}

template<class Transport>
pace::task<void> consume_large(pace::io_context& io, const http_fixture& fixture, const char* target) {
    auto socket = co_await admitted_connect<Transport>(io, fixture);
    co_await socket.write_all(std::string("GET ") + target + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    std::array<char, 8192> buffer{};
    std::string head;
    std::uint64_t received = 0;
    bool parsed_head = false;
    while (const auto count = co_await socket.read_some(buffer)) {
        std::string_view payload(buffer.data(), count);
        if (!parsed_head) {
            head.append(payload);
            const auto end = head.find("\r\n\r\n");
            require(head.size() < 16384, "streamed response head exceeded bound");
            if (end == std::string::npos) continue;
            require(head.starts_with("HTTP/1.1 200") &&
                    head.find("Content-Length: " + std::to_string(large_response_bytes) + "\r\n") != std::string::npos,
                    "streamed response has wrong framing");
            payload = std::string_view(head).substr(end + 4);
            parsed_head = true;
        }
        for (unsigned char byte : payload) {
            require(byte == received % 251, "streamed generated/file body corrupted");
            ++received;
        }
    }
    require(parsed_head && received == large_response_bytes, "streamed response was truncated");
}

template<class Transport>
pace::task<void> streaming_scenarios(pace::io_context& io) {
    auto fixture = start_admitted_server<Transport>(io);
    for (const auto* target : {"/generated", "/file"}) {
        const auto destroyed = fixture.state->destroyed;
        co_await consume_large<Transport>(io, fixture, target);
        require(fixture.state->destroyed == destroyed + 1 && fixture.state->largest_buffer <= 4096,
                "response source lifetime or buffering was unbounded");
    }
    pace::http::basic_client<Transport> client(co_await admitted_connect<Transport>(io, fixture));
    auto reads = fixture.state->reads;
    auto result = co_await client.send(message("HEAD", "/generated"));
    require(result.status == 200 && result.body.empty() && fixture.state->reads == reads &&
            result.header("Content-Length") == std::to_string(large_response_bytes), "HEAD pulled response producer");
    result = co_await client.send(message("GET", "/bodyless"));
    require(result.status == 205 && result.body.empty() && fixture.state->reads == reads, "bodyless status pulled response producer");
    result = co_await client.send(message("GET", "/invalid-stream"));
    require(result.status == 500 && result.close && fixture.state->reads == reads, "invalid streaming metadata was sent");
    for (const auto* target : {"/broken", "/throw-source", "/overrun-source"}) {
        const auto destroyed = fixture.state->destroyed;
        const auto broken = co_await admitted_raw<Transport>(io, fixture,
            std::string("GET ") + target + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", true);
        require(broken.starts_with("HTTP/1.1 200") && broken.ends_with("\r\n\r\n") &&
                fixture.state->destroyed == destroyed + 1, "source failure must close without another status line");
    }
    // Oversized local buffer configuration produces length_error, not an actual
    // allocation. It must only terminate this response, retaining accept service.
    auto huge = start_admitted_server<Transport>(io, 1s, 2s, std::numeric_limits<std::size_t>::max());
    const auto failed = co_await admitted_raw<Transport>(io, huge,
        "GET /huge HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", true);
    require(failed.starts_with("HTTP/1.1 200") && huge.state->destroyed == 1 && huge.state->reads == 0,
            "buffer allocation failure escaped connection scope");
    pace::http::basic_client<Transport> recovered(co_await admitted_connect<Transport>(io, huge));
    result = co_await recovered.send(message("GET", "/ok"));
    require(result.status == 200, "producer failure killed accepting service");
    recovered.close();
}

template<class Transport>
pace::task<void> disconnect_scenarios(pace::io_context& io) {
    auto fixture = start_admitted_server<Transport>(io);
    auto socket = co_await admitted_connect<Transport>(io, fixture);
    co_await socket.write_all("GET /stalled HTTP/1.1\r\nHost: localhost\r\n\r\n");
    std::array<char, 1024> buffer{};
    require(co_await socket.read_some(buffer) != 0, "stream never began");
    socket.close();
    for (unsigned i = 0; fixture.state->destroyed == 0 && i < 100; ++i) co_await io.sleep_for(5ms);
    require(fixture.state->destroyed == 1 && fixture.state->produced < stalled_response_bytes &&
            fixture.state->largest_buffer <= 4096, "disconnect did not release backpressured source");
}

template<class Transport>
pace::task<void> deadline_scenarios(pace::io_context& io) {
    auto fixture = start_admitted_server<Transport>(io, 40ms, 80ms);
    for (auto wire : {"POST /small HTTP/1.1\r\nHost:",
                     "POST /small HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\nx"}) {
        auto socket = co_await admitted_connect<Transport>(io, fixture);
        co_await socket.write_all(wire);
        std::array<char, 32> buffer{};
        bool closed = false;
        try { closed = co_await socket.read_some(buffer) == 0; }
        catch (const std::system_error&) { closed = true; }
        require(closed && fixture.state->handled == 0, "receive deadline failed on partial head/body");
    }
    auto socket = co_await admitted_connect<Transport>(io, fixture);
    co_await socket.write_all("GET /stalled HTTP/1.1\r\nHost: localhost\r\n\r\n");
    // Never read: writes must reach backpressure, then the write deadline must
    // destroy the producer without waiting for the peer or retaining a timer.
    for (unsigned i = 0; fixture.state->destroyed == 0 && i < 100; ++i) co_await io.sleep_for(5ms);
    require(fixture.state->destroyed == 1 && fixture.state->produced < stalled_response_bytes &&
            fixture.state->largest_buffer <= 4096, "write deadline did not cancel stalled response");
    socket.close();
    // Completed phase timers must not close a later phase/request on this socket.
    auto normal = start_admitted_server<Transport>(io, 80ms, 80ms);
    pace::http::basic_client<Transport> client(co_await admitted_connect<Transport>(io, normal));
    for (unsigned i = 0; i < 5; ++i) {
        const auto result = co_await client.send(message("GET", "/ok"));
        require(result.status == 200 && client.is_open(), "stale deadline closed keep-alive connection");
        co_await io.sleep_for(25ms);
    }
    client.close();
}


template<class Transport>
pace::task<void> context_stop_scenario(pace::io_context& io, std::shared_ptr<producer_state> state) {
    auto fixture = start_admitted_server<Transport>(io, 1s, 2s, 4096, std::move(state));
    auto socket = co_await admitted_connect<Transport>(io, fixture);
    co_await socket.write_all("GET /stalled HTTP/1.1\r\nHost: localhost\r\n\r\n");
    std::array<char, 1024> buffer{};
    require(co_await socket.read_some(buffer) != 0, "stream never began before context stop");
    io.stop();
    // run() drains the canceled server write after this client task returns.
}

using test_body = std::function<pace::task<void>(pace::io_context&)>;
pace::task<void> run_body(pace::io_context& io, test_body body, bool& completed) {
    co_await body(io); completed = true; io.stop();
}
void run_test(const char* name, test_body body) {
    pace::io_context io;
    bool completed = false, finished = false;
    std::atomic<bool> timed_out{false};
    std::mutex mutex; std::condition_variable condition;
    std::thread watchdog([&] {
        std::unique_lock lock(mutex);
        if (!condition.wait_for(lock, 15s, [&] { return finished; })) { timed_out = true; io.stop(); }
    });
    auto join = [&] { { std::lock_guard lock(mutex); finished = true; } condition.notify_one(); watchdog.join(); };
    try { io.spawn(run_body(io, body, completed)); io.run(); }
    catch (...) { join(); throw; }
    join();
    require(completed && !timed_out, "HTTP test timed out or did not finish");
    std::cout << "PASS " << name << '\n';
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc > 1) certificate_directory = argv[1];
        run_test("HTTP client/server, binary chunks, trailers and HEAD reuse", normal_client);
        run_test("HTTP IPv6 endpoint and bracketed Host authority", ipv6_http_authority);
        run_test("chunk extensions, fragmentation and pipeline preservation", chunk_pipeline);
        run_test("request smuggling and resource-limit rejection", rejected_requests);
        run_test("client informational, EOF framing and malformed responses", response_framing);
        run_test("client validation and overlapping exchange protection", client_state);
        run_test("cancellation closes pending HTTP exchange", client_cancellation);
        run_test("TCP validated admission, early rejection and per-route body limits", admission_scenarios<pace::tcp::stream>);
        run_test("TCP bounded generated/file response and bodyless producer handling", streaming_scenarios<pace::tcp::stream>);
        run_test("TCP disconnect cancels and releases response producer", disconnect_scenarios<pace::tcp::stream>);
        run_test("TCP request/write deadlines and keep-alive timer cancellation", deadline_scenarios<pace::tcp::stream>);
        auto stopped = std::make_shared<producer_state>();
        run_test("TCP context stop cancels streamed response", [stopped](pace::io_context& io) {
            return context_stop_scenario<pace::tcp::stream>(io, stopped);
        });
        require(stopped->destroyed == 1 && stopped->produced < stalled_response_bytes,
                "TCP context stop retained producer");
#ifdef PACE_HAS_TLS
        run_test("HTTPS verified HTTP/1.1 with chunked trailers", https_exchange);
        run_test("TLS validated admission, early rejection and per-route body limits", admission_scenarios<pace::tls::stream>);
        run_test("TLS bounded generated/file response and bodyless producer handling", streaming_scenarios<pace::tls::stream>);
        run_test("TLS disconnect cancels and releases response producer", disconnect_scenarios<pace::tls::stream>);
        run_test("TLS request/write deadlines and keep-alive timer cancellation", deadline_scenarios<pace::tls::stream>);
        stopped = std::make_shared<producer_state>();
        run_test("TLS context stop cancels streamed response", [stopped](pace::io_context& io) {
            return context_stop_scenario<pace::tls::stream>(io, stopped);
        });
        require(stopped->destroyed == 1 && stopped->produced < stalled_response_bytes,
                "TLS context stop retained producer");
#endif
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
