#include <pace/http.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <memory>
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

using test_body = pace::task<void>(*)(pace::io_context&);
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
        run_test("chunk extensions, fragmentation and pipeline preservation", chunk_pipeline);
        run_test("request smuggling and resource-limit rejection", rejected_requests);
        run_test("client informational, EOF framing and malformed responses", response_framing);
        run_test("client validation and overlapping exchange protection", client_state);
        run_test("cancellation closes pending HTTP exchange", client_cancellation);
#ifdef PACE_HAS_TLS
        run_test("HTTPS verified HTTP/1.1 with chunked trailers", https_exchange);
#endif
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
