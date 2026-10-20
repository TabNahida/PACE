#include <pace/pace.hpp>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using namespace std::chrono_literals;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

pace::task<void> echo_connection(pace::tcp_stream stream) {
    try {
        std::array<char, 4096> buffer{};
        while (const auto size = co_await stream.read_some(buffer))
            co_await stream.write_all(std::string_view(buffer.data(), size));
    } catch (const std::system_error& error) {
        if (error.code() != std::errc::operation_canceled) throw;
    }
}

pace::task<void> echo_listener(std::shared_ptr<pace::tcp_listener> listener) {
    auto stream = co_await listener->accept();
    co_await echo_connection(std::move(stream));
}

pace::task<void> tcp_roundtrip(pace::io_context& context) {
    auto listener = std::make_shared<pace::tcp_listener>(context, pace::ipv4_endpoint{"127.0.0.1", 0});
    context.spawn(echo_listener(listener));
    pace::tcp_stream client(context);
    co_await client.connect(listener->local_endpoint());
    client.set_no_delay();
    std::string payload(96 * 1024, '\0');
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(i % 251);
    co_await client.write_all(payload);
    std::string received;
    std::array<char, 3071> buffer{};
    while (received.size() < payload.size()) {
        const auto size = co_await client.read_some(buffer);
        require(size != 0, "unexpected echo EOF");
        received.append(buffer.data(), size);
    }
    require(received == payload, "binary TCP roundtrip mismatch");
    pace::tcp_stream moved(std::move(client));
    require(!client.is_open() && moved.is_open(), "move must transfer socket ownership");
    moved.close();
    moved.close();
    bool bad_fd = false;
    try { co_await moved.read_some(buffer); }
    catch (const std::system_error& error) { bad_fd = error.code() == std::errc::bad_file_descriptor; }
    require(bad_fd, "closed stream should reject operations");
}

pace::task<void> connect_errors(pace::io_context& context) {
    pace::tcp_listener reserved(context, {"127.0.0.1", 0});
    auto endpoint = reserved.local_endpoint();
    reserved.close();
    pace::tcp_stream client(context);
    bool failed = false;
    try { co_await client.connect(endpoint); }
    catch (const std::system_error&) { failed = true; }
    require(failed, "connecting to an unbound port should fail");
    bool invalid = false;
    try { co_await client.connect({"not-an-address", 42}); }
    catch (const std::invalid_argument&) { invalid = true; }
    require(invalid, "numeric address validation");
}

pace::task<pace::http::response> http_handler(pace::http::request request) {
    require(request.header("HOST") == "example.test" || request.version == "HTTP/1.0", "case-insensitive header lookup");
    if (request.target == "/bad") {
        pace::http::response result{.body = "bad"};
        result.headers.push_back({"X-Injected", "a\r\nContent-Length: 0"});
        co_return result;
    }
    if (request.target == "/empty")
        co_return pace::http::response{.status = 204, .body = "must not appear"};
    if (request.target == "/echo") co_return pace::http::response{.body = std::move(request.body)};
    co_return pace::http::response{.body = request.target};
}

pace::task<void> http_listener(std::shared_ptr<pace::tcp_listener> listener, pace::http::limits bounds) {
    co_await pace::http::serve(*listener, http_handler, bounds);
}

// First validate the advertised HTTP frames, then require orderly closure.
// Reset, truncation and trailing bytes fail even for rejected requests.
bool complete_responses(std::string_view input, std::size_t expected, bool head_response = false) {
    for (std::size_t index = 0; index < expected; ++index) {
        const auto delimiter = input.find("\r\n\r\n");
        if (delimiter == std::string_view::npos) return false;
        const auto head = input.substr(0, delimiter + 2);
        require(head.starts_with("HTTP/1.1 ") && head.size() >= 14, "invalid HTTP response status line");
        unsigned status = 0;
        const auto [status_end, status_error] = std::from_chars(head.data() + 9, head.data() + 12, status);
        require(status_error == std::errc{} && status_end == head.data() + 12 && head[12] == ' ',
                "invalid HTTP response status code");
        constexpr std::string_view length_header = "\r\nContent-Length: ";
        const auto length_start = head.find(length_header);
        std::size_t length = 0;
        if (length_start != std::string_view::npos) {
            const auto value_start = length_start + length_header.size();
            const auto value_end = head.find("\r\n", value_start);
            require(value_end != std::string_view::npos && head.find(length_header, value_start) == std::string_view::npos,
                    "invalid or duplicate response Content-Length");
            const auto [end, error] = std::from_chars(head.data() + value_start, head.data() + value_end, length);
            require(error == std::errc{} && end == head.data() + value_end && length <= 1024 * 1024,
                    "invalid response Content-Length");
        }
        const bool bodyless = head_response || status == 204 || status == 304;
        require(bodyless || length_start != std::string_view::npos, "response lacks Content-Length framing");
        input.remove_prefix(delimiter + 4);
        const auto body_size = bodyless ? 0 : length;
        if (input.size() < body_size) return false;
        input.remove_prefix(body_size);
    }
    require(input.empty(), "unexpected bytes after the expected HTTP responses");
    return true;
}

void verify_response_framing() {
    constexpr std::string_view response = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
    for (std::size_t size = 0; size < response.size(); ++size)
        require(!complete_responses(response.substr(0, size), 1), "truncated HTTP response must remain incomplete");
    require(complete_responses(response, 1), "complete HTTP body must be accepted");
    require(!complete_responses(response, 2), "missing pipelined response must remain incomplete");
    require(complete_responses(std::string(response) + std::string(response), 2), "both pipeline frames are required");
    require(complete_responses("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n", 1, true), "HEAD framing has no payload");
    require(complete_responses("HTTP/1.1 204 No Content\r\n\r\n", 1), "204 framing has no payload");
}

pace::task<std::string> read_responses(pace::tcp_stream& stream, std::size_t expected = 1,
                                     bool head_response = false) {
    std::string output;
    std::array<char, 1024> buffer{};
    while (!complete_responses(output, expected, head_response)) {
        const auto size = co_await stream.read_some(buffer);
        require(size != 0, "EOF before complete HTTP response");
        output.append(buffer.data(), size);
        require(output.size() <= 1024 * 1024, "HTTP test response exceeds safety bound");
    }
    require(co_await stream.read_some(buffer) == 0, "unexpected trailing HTTP response bytes");
    co_return output;
}

pace::task<std::string> exchange(pace::io_context& context, pace::ipv4_endpoint endpoint,
                               std::string request, std::size_t expected = 1) {
    pace::tcp_stream stream(context);
    co_await stream.connect(std::move(endpoint));
    co_await stream.write_all(request);
    co_return co_await read_responses(stream, expected, request.starts_with("HEAD "));
}

pace::task<void> http_protocol(pace::io_context& context) {
    auto listener = std::make_shared<pace::tcp_listener>(context, pace::ipv4_endpoint{"127.0.0.1", 0});
    context.spawn(http_listener(listener, {.max_header_bytes = 256, .max_body_bytes = 32, .max_requests_per_connection = 2}));
    const auto endpoint = listener->local_endpoint();
    auto result = co_await exchange(context, endpoint,
        "POST /echo HTTP/1.1\r\nHost: example.test\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello");
    require(result.starts_with("HTTP/1.1 200") && result.ends_with("\r\n\r\nhello"), "HTTP body roundtrip");

    result = co_await exchange(context, endpoint,
        "GET /one HTTP/1.1\r\nHost: example.test\r\n\r\nGET /two HTTP/1.1\r\nHost: example.test\r\n\r\n", 2);
    auto second = result.find("HTTP/1.1 200", 1);
    require(second != std::string::npos && result.find("\r\n\r\n/one") != std::string::npos && result.ends_with("/two"),
            "HTTP pipelining must preserve request boundaries");
    require(result.find("Connection: keep-alive") < second && result.find("Connection: close", second) != std::string::npos,
            "HTTP request-count limit must close final response");

    pace::tcp_stream fragmented(context);
    co_await fragmented.connect(endpoint);
    co_await fragmented.write_all("POST /echo HTTP/1.1\r\nHo");
    co_await context.sleep_for(1ms);
    co_await fragmented.write_all("st: example.test\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhe");
    co_await context.sleep_for(1ms);
    co_await fragmented.write_all("llo");
    result = co_await read_responses(fragmented);
    require(result.ends_with("\r\n\r\nhello"), "fragmented HTTP head/body");

    struct invalid_case { std::string request; unsigned status; };
    const std::array cases{
        invalid_case{"GET / HTTP/1.1\r\n\r\n", 400},
        invalid_case{"GET / HTTP/1.1\r\nHost: example.test\r\nHost: other\r\n\r\n", 400},
        invalid_case{"POST / HTTP/1.1\r\nHost: example.test\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n", 400},
        invalid_case{"POST / HTTP/1.1\r\nHost: example.test\r\nContent-Length: +1\r\n\r\n", 400},
        invalid_case{"POST / HTTP/1.1\r\nHost: example.test\r\nTransfer-Encoding: chunked\r\n\r\n", 501},
        invalid_case{"POST / HTTP/1.1\r\nHost: example.test\r\nTransfer-Encoding: chunked\r\nContent-Length: 0\r\n\r\n", 400},
        invalid_case{"GET / HTTP/1.1\r\nHost: example.test\r\n bad: fold\r\n\r\n", 400},
        invalid_case{"GET / HTTP/1.1\r\nHost : example.test\r\n\r\n", 400},
        invalid_case{"GET / HTTP/1.1\r\nHost: example.test\nbad: value\r\n\r\n", 400},
        invalid_case{"POST / HTTP/1.1\r\nHost: example.test\r\nContent-Length: 33\r\n\r\n", 413},
        invalid_case{"POST / HTTP/1.1\r\nHost: example.test\r\nExpect: 100-continue\r\n\r\n", 417},
        invalid_case{"HEAD / HTTP/1.1\r\nHost: example.test\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n", 400},
        invalid_case{"HEAD / HTTP/1.1\r\nHost: example.test\r\nExpect: 100-continue\r\n\r\n", 417},
        invalid_case{"GET / HTTP/1.1\r\nHost: example.test\r\nX-Large: " + std::string(256, 'x') + "\r\n\r\n", 431},
        invalid_case{"HEAD / HTTP/1.1\r\nHost: example.test\r\nX-Large: " + std::string(256, 'x') + "\r\n\r\n", 431}
    };
    for (const auto& test : cases) {
        try {
            result = co_await exchange(context, endpoint, test.request);
        } catch (const std::exception& error) {
            throw std::runtime_error("HTTP error-response case (expected " + std::to_string(test.status) + "): " + error.what());
        }
        require(result.starts_with("HTTP/1.1 " + std::to_string(test.status)), "malformed request response status");
        require(result.find("Connection: close") != std::string::npos, "malformed request must close connection");
        if (test.request.starts_with("HEAD ")) require(result.ends_with("\r\n\r\n"), "HEAD errors forbid payload bytes");
    }
    result = co_await exchange(context, endpoint, "HEAD /head HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n");
    require(result.find("Content-Length: 5\r\n") != std::string::npos && result.ends_with("\r\n\r\n"), "HEAD response framing");
    result = co_await exchange(context, endpoint, "GET /empty HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n");
    require(result.starts_with("HTTP/1.1 204") && result.find("Content-Length") == std::string::npos && result.ends_with("\r\n\r\n"),
            "204 forbids body and Content-Length");
    result = co_await exchange(context, endpoint, "GET /legacy HTTP/1.0\r\n\r\n");
    require(result.ends_with("/legacy") && result.find("Connection: close") != std::string::npos, "HTTP/1.0 defaults to close");
    result = co_await exchange(context, endpoint, "GET /bad HTTP/1.1\r\nHost: example.test\r\n\r\n");
    require(result.starts_with("HTTP/1.1 500") && result.find("X-Injected") == std::string::npos, "response header injection must be rejected");
}

pace::task<void> stop_soon(pace::io_context& context) {
    co_await context.sleep_for(5ms);
    context.stop();
}

pace::task<void> stop_pending_accept(pace::io_context& context) {
    pace::tcp_listener listener(context, {"127.0.0.1", 0});
    context.spawn(stop_soon(context));
    bool canceled = false;
    try { auto unused = co_await listener.accept(); }
    catch (const std::system_error& error) { canceled = error.code() == std::errc::operation_canceled; }
    require(canceled, "stop must cancel a suspended accept");
}

pace::task<void> accept_idle(std::shared_ptr<pace::tcp_listener> listener) {
    auto stream = co_await listener->accept();
    std::array<char, 8> buffer;
    try { co_await stream.read_some(buffer); }
    catch (const std::system_error& error) {
        if (error.code() != std::errc::operation_canceled) throw;
    }
}

pace::task<void> stop_pending_read(pace::io_context& context) {
    auto listener = std::make_shared<pace::tcp_listener>(context, pace::ipv4_endpoint{"127.0.0.1", 0});
    context.spawn(accept_idle(listener));
    pace::tcp_stream client(context);
    co_await client.connect(listener->local_endpoint());
    context.spawn(stop_soon(context));
    bool canceled = false;
    std::array<char, 8> buffer;
    try { co_await client.read_some(buffer); }
    catch (const std::system_error& error) { canceled = error.code() == std::errc::operation_canceled; }
    require(canceled, "stop must cancel a suspended read");
}

pace::task<void> close_soon(std::shared_ptr<pace::tcp_stream> stream) {
    co_await stream->context().sleep_for(5ms);
    stream->close();
}

pace::task<void> close_pending_read(pace::io_context& context) {
    auto listener = std::make_shared<pace::tcp_listener>(context, pace::ipv4_endpoint{"127.0.0.1", 0});
    context.spawn(accept_idle(listener));
    auto client = std::make_shared<pace::tcp_stream>(context);
    co_await client->connect(listener->local_endpoint());
    context.spawn(close_soon(client));
    bool canceled = false;
    std::array<char, 8> buffer;
    try { co_await client->read_some(buffer); }
    catch (const std::system_error& error) { canceled = error.code() == std::errc::operation_canceled; }
    require(canceled && !context.stopped(), "socket close must cancel only its own suspended read");
}

struct graceful_close_result {
    bool finished = false;
    std::chrono::steady_clock::duration elapsed{};
};

pace::task<void> close_after_reply(std::shared_ptr<pace::tcp_listener> listener,
                                  std::shared_ptr<graceful_close_result> result) {
    auto stream = co_await listener->accept();
    co_await stream.write_all("done");
    const auto started = std::chrono::steady_clock::now();
    co_await stream.close_gracefully(5ms);
    result->elapsed = std::chrono::steady_clock::now() - started;
    result->finished = true;
    require(!stream.is_open(), "graceful close must release its socket");
}

pace::task<void> graceful_close_bounds(pace::io_context& context) {
    auto listener = std::make_shared<pace::tcp_listener>(context, pace::ipv4_endpoint{"127.0.0.1", 0});
    auto result = std::make_shared<graceful_close_result>();
    context.spawn(close_after_reply(listener, result));
    pace::tcp_stream client(context);
    co_await client.connect(listener->local_endpoint());
    co_await client.write_all("discard this pending input");
    std::array<char, 4> reply{};
    std::size_t received = 0;
    while (received < reply.size()) {
        const auto count = co_await client.read_some(std::span<char>(reply).subspan(received));
        require(count != 0, "graceful close truncated its final reply");
        received += count;
    }
    require(std::string_view(reply.data(), reply.size()) == "done", "graceful close reply mismatch");
    // Keep our send side open: the server must finish when its drain timeout
    // expires instead of waiting indefinitely for peer EOF.
    co_await context.sleep_for(25ms);
    require(result->finished && result->elapsed >= 5ms, "graceful close must honor its drain deadline");
    require(co_await client.read_some(reply) == 0, "drained connection must close without reset");
}

pace::task<void> hold_connection(std::shared_ptr<pace::tcp_listener> listener) {
    auto stream = co_await listener->accept();
    try { co_await stream.context().sleep_for(24h); }
    catch (const std::system_error& error) {
        if (error.code() != std::errc::operation_canceled) throw;
    }
}

pace::task<void> graceful_close_cancellation(pace::io_context& context) {
    auto listener = std::make_shared<pace::tcp_listener>(context, pace::ipv4_endpoint{"127.0.0.1", 0});
    context.spawn(hold_connection(listener));
    pace::tcp_stream client(context);
    co_await client.connect(listener->local_endpoint());
    context.spawn(stop_soon(context));
    bool canceled = false;
    try { co_await client.close_gracefully(1s); }
    catch (const std::system_error& error) { canceled = error.code() == std::errc::operation_canceled; }
    require(canceled && !client.is_open(), "canceled graceful close must release its socket");
}

using test_body = pace::task<void> (*)(pace::io_context&);
pace::task<void> run_body(pace::io_context& context, test_body body, bool& completed) {
    co_await body(context);
    completed = true;
    context.stop();
}

void run_test(const char* name, test_body body) {
    pace::io_context context;
    bool completed = false;
    std::mutex mutex;
    std::condition_variable condition;
    bool finished = false;
    std::atomic<bool> timed_out = false;
    std::thread watchdog([&] {
        std::unique_lock lock(mutex);
        if (!condition.wait_for(lock, 10s, [&] { return finished; })) {
            timed_out = true;
            context.stop();
        }
    });
    auto finish = [&] {
        { std::lock_guard lock(mutex); finished = true; }
        condition.notify_one();
        watchdog.join();
    };
    try {
        context.spawn(run_body(context, body, completed));
        context.run();
    } catch (...) { finish(); throw; }
    finish();
    require(!timed_out && completed, "network test timed out or did not complete");
    std::cout << "PASS " << name << '\n';
}
} // namespace

int main() {
    try {
        verify_response_framing();
        run_test("TCP binary roundtrip and ownership", tcp_roundtrip);
        run_test("connect and endpoint errors", connect_errors);
        run_test("HTTP framing, fragmentation, limits and keep-alive", http_protocol);
        run_test("stop cancels pending accept", stop_pending_accept);
        run_test("stop cancels pending read", stop_pending_read);
        run_test("socket close cancels pending read", close_pending_read);
        run_test("graceful close drains input and bounds peer wait", graceful_close_bounds);
        run_test("stop cancels graceful close", graceful_close_cancellation);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
