#include <pace/tcp.hpp>
#include <pace/pace.hpp>
#include <pace/detail/socket_address.hpp>
#ifndef _WIN32
#include <netinet/tcp.h>
#endif

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
#include <type_traits>

static_assert(std::is_same_v<pace::tcp_stream, pace::tcp::stream>);
static_assert(std::is_same_v<pace::tcp_listener, pace::tcp::listener>);
static_assert(std::is_same_v<pace::ipv4_endpoint, pace::tcp::endpoint>);
static_assert(std::is_same_v<pace::listen_options, pace::tcp::listen_options>);

namespace {
using namespace std::chrono_literals;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

pace::task<void> echo_connection(pace::tcp::stream stream) {
    try {
        std::array<char, 4096> buffer{};
        while (const auto size = co_await stream.read_some(buffer))
            co_await stream.write_all(std::string_view(buffer.data(), size));
    } catch (const std::system_error& error) {
        if (error.code() != std::errc::operation_canceled) throw;
    }
}

pace::task<void> echo_listener(std::shared_ptr<pace::tcp::listener> listener) {
    auto stream = co_await listener->accept();
    co_await echo_connection(std::move(stream));
}

pace::task<void> tcp_roundtrip(pace::io_context& context) {
    auto listener = std::make_shared<pace::tcp::listener>(context, pace::tcp::endpoint{"127.0.0.1", 0});
    context.spawn(echo_listener(listener));
    pace::tcp::stream client(context);
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
    pace::tcp_stream legacy(std::move(client));
    pace::tcp::stream moved(std::move(legacy));
    require(!client.is_open() && !legacy.is_open() && moved.is_open(), "new and compatibility APIs share socket ownership");
    moved.close();
    moved.close();
    bool bad_fd = false;
    try { co_await moved.read_some(buffer); }
    catch (const std::system_error& error) { bad_fd = error.code() == std::errc::bad_file_descriptor; }
    require(bad_fd, "closed stream should reject operations");
}

pace::task<void> connect_errors(pace::io_context& context) {
    pace::tcp::listener reserved(context, {"127.0.0.1", 0});
    auto endpoint = reserved.local_endpoint();
    reserved.close();
    pace::tcp::stream client(context);
    bool failed = false;
    try { co_await client.connect(endpoint); }
    catch (const std::system_error&) { failed = true; }
    require(failed, "connecting to an unbound port should fail");
    bool invalid = false;
    try { co_await client.connect({"not-an-address", 42}); }
    catch (const std::invalid_argument&) { invalid = true; }
    require(invalid, "numeric address validation");
    invalid = false;
    pace::tcp::endpoint malformed{std::string("127.0.0.1\0unexpected", 20), 42};
    try { co_await client.connect(std::move(malformed)); }
    catch (const std::invalid_argument&) { invalid = true; }
    require(invalid, "embedded NUL address validation");
}

pace::task<void> endpoint_exchange(pace::io_context& io, pace::tcp::listener& listener,
                                  pace::tcp::endpoint destination, bool explicit_family = false) {
    pace::tcp::stream client(io, explicit_family ? pace::address_family::ipv6 : pace::address_family::unspecified);
    client.set_no_delay();
    const auto original_handle = client.native_handle();
    co_await client.connect(std::move(destination));
    if (explicit_family) require(client.native_handle() == original_handle, "explicit family changed native handle");
    int no_delay = 0;
    pace::detail::socket_address_length option_length = sizeof(no_delay);
    require(::getsockopt(client.native_handle(), IPPROTO_TCP, TCP_NODELAY,
                        reinterpret_cast<char*>(&no_delay), &option_length) == 0 && no_delay != 0,
            "TCP_NODELAY was lost while selecting IPv6");
    auto accepted = co_await listener.accept();
    const auto client_local = client.local_endpoint(), client_remote = client.remote_endpoint();
    const auto server_local = accepted.local_endpoint(), server_remote = accepted.remote_endpoint();
    require(client_local.family == client_remote.family && client_local.scope_id == 0,
            "client endpoint family/scope");
    require(server_local.family == listener.local_endpoint().family && server_remote.family == server_local.family,
            "accepted endpoint family");
    require(client_remote.port == server_local.port && client_local.port == server_remote.port,
            "local/remote TCP port inspection");
    if (client_local.family != server_remote.family) {
        require(server_remote.family == pace::address_family::ipv6 && server_remote.address == "::ffff:127.0.0.1",
                "dual-stack peer must retain IPv4-mapped IPv6 identity");
    } else require(client_local.address == server_remote.address, "TCP peer address inspection");
    const std::string payload("IPv6\0payload", 12);
    co_await client.write_all(payload);
    std::array<char, 64> buffer{};
    std::size_t received = 0;
    while (received < payload.size()) {
        const auto size = co_await accepted.read_some(std::span<char>(buffer).subspan(received));
        require(size != 0, "IPv6 premature EOF"); received += size;
    }
    require(std::string_view(buffer.data(), received) == payload, "IPv6 binary transfer");
    co_await accepted.write_all("ok");
    received = 0;
    while (received < 2) {
        const auto size = co_await client.read_some(std::span<char>(buffer).subspan(received));
        require(size != 0, "IPv6 reply EOF"); received += size;
    }
    require(std::string_view(buffer.data(), received) == "ok", "IPv6 reply");
    pace::tcp::stream moved(std::move(accepted));
    require(moved.remote_endpoint().address == server_remote.address, "IPv6 move lost peer endpoint");
    moved.close();
    bool closed = false;
    try { (void)moved.remote_endpoint(); }
    catch (const std::system_error& error) { closed = error.code() == std::errc::bad_file_descriptor; }
    require(closed, "closed peer endpoint inspection");
}

pace::task<void> ipv6_loopback(pace::io_context& io) {
    pace::tcp::listener listener(io, {"::1", 0});
    const auto local = listener.local_endpoint();
    require(local.address == "::1" && local.port != 0 && local.family == pace::address_family::ipv6 && local.scope_id == 0,
            "IPv6 listener endpoint");
    co_await endpoint_exchange(io, listener, local);
    co_await endpoint_exchange(io, listener, local, true);
    pace::tcp::stream fixed(io, pace::address_family::ipv4);
    bool rejected = false;
    try { co_await fixed.connect(local); }
    catch (const std::system_error& error) { rejected = error.code() == std::errc::address_family_not_supported; }
    require(rejected, "explicit TCP family mismatch");
}

pace::task<void> ipv6_wildcards(pace::io_context& io) {
    pace::tcp::listener only_v6(io, {"::", 0});
    auto local = only_v6.local_endpoint();
    require(local.address == "::" && local.family == pace::address_family::ipv6, "IPv6 wildcard inspection");
    int only = 0;
    pace::detail::socket_address_length length = sizeof(only);
    require(::getsockopt(only_v6.native_handle(), IPPROTO_IPV6, IPV6_V6ONLY,
                        reinterpret_cast<char*>(&only), &length) == 0 && only == 1,
            "IPv6 listeners must default to IPV6_V6ONLY");
    // A separate IPv4 listener must be able to own the same port.
    pace::tcp::listener v4(io, {"0.0.0.0", local.port});
    co_await endpoint_exchange(io, only_v6, {"::1", local.port});
    co_await endpoint_exchange(io, v4, {"127.0.0.1", local.port});

    pace::tcp::listener dual(io, {"::", 0}, {.ipv6_only = false});
    local = dual.local_endpoint();
    co_await endpoint_exchange(io, dual, {"::1", local.port});
    co_await endpoint_exchange(io, dual, {"127.0.0.1", local.port});
    // Explicit IPv4-mapped numeric destinations also work on an IPv6 stream.
    co_await endpoint_exchange(io, dual, {"::ffff:127.0.0.1", local.port}, true);
}

pace::task<void> ipv6_address_validation(pace::io_context& io) {
    using pace::address_family;
    for (const pace::tcp::endpoint& endpoint : {
            pace::tcp::endpoint{"::1", 0, address_family::ipv4},
            pace::tcp::endpoint{"127.0.0.1", 0, address_family::ipv6},
            pace::tcp::endpoint{"127.0.0.1", 0, address_family::unspecified, 1},
            pace::tcp::endpoint{"::1%1", 0}, pace::tcp::endpoint{"fe80::1%lo", 0},
            pace::tcp::endpoint{"[::1]", 0}, pace::tcp::endpoint{"::gg", 0},
            pace::tcp::endpoint{"::1", 0, static_cast<address_family>(99)},
            pace::tcp::endpoint{std::string("::1\0suffix", 10), 0}}) {
        bool rejected = false;
        try { pace::tcp::listener listener(io, endpoint); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "invalid IPv6 listener endpoint accepted");
        pace::tcp::stream client(io);
        rejected = false;
        try { co_await client.connect(endpoint); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "invalid IPv6 connect endpoint accepted");
    }
    bool rejected = false;
    try { pace::tcp::listener listener(io, {"127.0.0.1", 0}, {.ipv6_only = false}); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "dual-stack option on IPv4 must be rejected");
    // Link-local reachability depends on host interfaces. The shared conversion
    // must preserve its interface index without a DNS/interface-name lookup.
    const auto native = pace::detail::parse_socket_address("fe80::1234", 321, address_family::ipv6, 42);
    const auto scoped = pace::detail::socket_endpoint<pace::tcp::endpoint>(native.data(), native.length);
    require(scoped.address == "fe80::1234" && scoped.port == 321 && scoped.scope_id == 42 &&
            scoped.family == address_family::ipv6, "IPv6 scope must survive native endpoint conversion");
}

pace::task<void> short_read_server(std::shared_ptr<pace::tcp::listener> listener,
                                   std::shared_ptr<bool> tail_sent) {
    auto stream = co_await listener->accept();
    co_await stream.write_all("A");
    std::array<char, 1> acknowledgement{};
    require(co_await stream.read_some(acknowledgement) == 1 && acknowledgement[0] == '!',
            "short-read acknowledgement");
    co_await stream.write_all("BC");
    *tail_sent = true;
}

pace::task<void> short_read_rearm(pace::io_context& context) {
    auto listener = std::make_shared<pace::tcp::listener>(context, pace::tcp::endpoint{"127.0.0.1", 0});
    auto tail_sent = std::make_shared<bool>(false);
    context.spawn(short_read_server(listener, tail_sent));
    pace::tcp::stream client(context);
    co_await client.connect(listener->local_endpoint());
    std::array<char, 64> buffer{};
    require(co_await client.read_some(buffer) == 1 && buffer[0] == 'A', "initial short read");
    // A zero-size operation must finish even when the peer is awaiting our ack.
    require(co_await client.read_some(std::span<char>{}) == 0, "empty read after short read");
    pace::tcp::stream moved(std::move(client));
    pace::tcp::stream assigned(context);
    assigned = std::move(moved);
    co_await assigned.write_all("!");
    while (!*tail_sent) co_await context.yield();
    // Data is already queued before rearming. A level subscription must find it;
    // a full one-byte read must also preserve the unread remainder.
    require(co_await assigned.read_some(std::span<char>(buffer).first(1)) == 1 && buffer[0] == 'B',
            "queued readiness after short read and moves");
    require(co_await assigned.read_some(buffer) == 1 && buffer[0] == 'C', "read remainder after full buffer");
    require(co_await assigned.read_some(buffer) == 0, "EOF after short read");
}

#ifndef PACE_CORE_ONLY_TESTS
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

pace::task<void> http_listener(std::shared_ptr<pace::tcp::listener> listener, pace::http::limits bounds) {
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

pace::task<std::string> read_responses(pace::tcp::stream& stream, std::size_t expected = 1,
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

pace::task<std::string> exchange(pace::io_context& context, pace::tcp::endpoint endpoint,
                               std::string request, std::size_t expected = 1) {
    pace::tcp::stream stream(context);
    co_await stream.connect(std::move(endpoint));
    co_await stream.write_all(request);
    co_return co_await read_responses(stream, expected, request.starts_with("HEAD "));
}

pace::task<void> http_protocol(pace::io_context& context) {
    auto listener = std::make_shared<pace::tcp::listener>(context, pace::tcp::endpoint{"127.0.0.1", 0});
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

    pace::tcp::stream fragmented(context);
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
        invalid_case{"POST / HTTP/1.1\r\nHost: example.test\r\nTransfer-Encoding: gzip\r\n\r\n", 501},
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

#endif

pace::task<void> stop_soon(pace::io_context& context) {
    co_await context.sleep_for(5ms);
    context.stop();
}

pace::task<void> stop_pending_accept(pace::io_context& context) {
    pace::tcp::listener listener(context, {"127.0.0.1", 0});
    context.spawn(stop_soon(context));
    bool canceled = false;
    try { auto unused = co_await listener.accept(); }
    catch (const std::system_error& error) { canceled = error.code() == std::errc::operation_canceled; }
    require(canceled, "stop must cancel a suspended accept");
}

pace::task<void> accept_idle(std::shared_ptr<pace::tcp::listener> listener) {
    auto stream = co_await listener->accept();
    std::array<char, 8> buffer;
    try { co_await stream.read_some(buffer); }
    catch (const std::system_error& error) {
        if (error.code() != std::errc::operation_canceled) throw;
    }
}

pace::task<void> stop_pending_read(pace::io_context& context) {
    auto listener = std::make_shared<pace::tcp::listener>(context, pace::tcp::endpoint{"127.0.0.1", 0});
    context.spawn(accept_idle(listener));
    pace::tcp::stream client(context);
    co_await client.connect(listener->local_endpoint());
    context.spawn(stop_soon(context));
    bool canceled = false;
    std::array<char, 8> buffer;
    try { co_await client.read_some(buffer); }
    catch (const std::system_error& error) { canceled = error.code() == std::errc::operation_canceled; }
    require(canceled, "stop must cancel a suspended read");
}

pace::task<void> close_soon(std::shared_ptr<pace::tcp::stream> stream) {
    co_await stream->context().sleep_for(5ms);
    stream->close();
}

pace::task<void> close_pending_read(pace::io_context& context) {
    auto listener = std::make_shared<pace::tcp::listener>(context, pace::tcp::endpoint{"127.0.0.1", 0});
    context.spawn(accept_idle(listener));
    auto client = std::make_shared<pace::tcp::stream>(context);
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

pace::task<void> close_after_reply(std::shared_ptr<pace::tcp::listener> listener,
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
    auto listener = std::make_shared<pace::tcp::listener>(context, pace::tcp::endpoint{"127.0.0.1", 0});
    auto result = std::make_shared<graceful_close_result>();
    context.spawn(close_after_reply(listener, result));
    pace::tcp::stream client(context);
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

pace::task<void> graceful_close_timer_cleanup(pace::io_context& context) {
    pace::tcp::listener listener(context, {"::1", 0});
    {
        pace::tcp::stream client(context);
        co_await client.connect(listener.local_endpoint());
        auto accepted = co_await listener.accept();
        client.close();
        co_await accepted.close_gracefully(10ms);
        require(!accepted.is_open(), "peer EOF must finish graceful close");
    }
    // The deadline captured the destroyed stream and coroutine frame. It must
    // already have been removed, including when EOF completed synchronously.
    co_await context.sleep_for(25ms);
    co_await endpoint_exchange(context, listener, listener.local_endpoint());
}

pace::task<void> hold_connection(std::shared_ptr<pace::tcp::listener> listener) {
    auto stream = co_await listener->accept();
    try { co_await stream.context().sleep_for(24h); }
    catch (const std::system_error& error) {
        if (error.code() != std::errc::operation_canceled) throw;
    }
}

pace::task<void> graceful_close_cancellation(pace::io_context& context) {
    auto listener = std::make_shared<pace::tcp::listener>(context, pace::tcp::endpoint{"127.0.0.1", 0});
    context.spawn(hold_connection(listener));
    pace::tcp::stream client(context);
    co_await client.connect(listener->local_endpoint());
    context.spawn(stop_soon(context));
    bool canceled = false;
    try { co_await client.close_gracefully(1s); }
    catch (const std::system_error& error) { canceled = error.code() == std::errc::operation_canceled; }
    require(canceled && !client.is_open(), "canceled graceful close must release its socket");
}

pace::task<void> exact_reads(pace::io_context& io) {
    auto listener = pace::tcp::listen({"127.0.0.1", 0});
    auto client = co_await pace::tcp::connect(listener.local_endpoint());
    auto peer = co_await listener.accept();
    auto owned_write = pace::write(peer, std::string("ab\0cdef", 7));
    co_await io.yield();
    co_await std::move(owned_write);
    require((co_await pace::read_exact(client, 3)) == std::string("ab\0", 3), "owned exact binary read");
    std::array<std::byte, 2> pair{};
    co_await pace::read_exact(client, std::span<std::byte>(pair));
    require(pair[0] == std::byte{'c'} && pair[1] == std::byte{'d'}, "exact byte span overconsumed");
    bool bounded = false;
    try { (void)co_await pace::read_exact(client, 1025, 1024); }
    catch (const std::length_error&) { bounded = true; }
    require(bounded && (co_await client.read_exact(2)) == "ef", "bounded read consumed bytes");
    require((co_await client.read_exact(0)).empty(), "empty member exact read blocked");
    require((co_await pace::read_exact(client, 0)).empty(), "empty shared exact read blocked");
    co_await peer.write_all("x");
    peer.close();
    bool eof = false;
    try { (void)co_await pace::read_exact(client, 2); }
    catch (const std::system_error& error) { eof = error.code() == std::errc::connection_reset; }
    require(eof, "exact read silently returned a truncated message");
    auto pending = co_await pace::tcp::connect(io, listener.local_endpoint());
    auto waiting_peer = co_await listener.accept();
    io.spawn(stop_soon(io));
    bool canceled = false;
    try { (void)co_await pace::read_exact(pending, 1); }
    catch (const std::system_error& error) { canceled = error.code() == std::errc::operation_canceled; }
    require(canceled, "exact read lost cancellation");
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
#ifndef PACE_CORE_ONLY_TESTS
        verify_response_framing();
#endif
        run_test("TCP binary roundtrip and ownership", tcp_roundtrip);
        run_test("easy connect, bounded exact reads, EOF and cancellation", exact_reads);
        run_test("connect and endpoint errors", connect_errors);
        run_test("IPv6 loopback, endpoint inspection and family selection", ipv6_loopback);
        run_test("IPv6 wildcard, IPv6-only and dual-stack listeners", ipv6_wildcards);
        run_test("IPv6 family, scope and malformed endpoint validation", ipv6_address_validation);
        run_test("short-read rearm, queued bytes, move, empty buffer and EOF", short_read_rearm);
#ifndef PACE_CORE_ONLY_TESTS
        run_test("HTTP framing, fragmentation, limits and keep-alive", http_protocol);
#endif
        run_test("stop cancels pending accept", stop_pending_accept);
        run_test("stop cancels pending read", stop_pending_read);
        run_test("socket close cancels pending read", close_pending_read);
        run_test("graceful close drains input and bounds peer wait", graceful_close_bounds);
        run_test("completed IPv6 graceful close removes its deadline", graceful_close_timer_cleanup);
        run_test("stop cancels graceful close", graceful_close_cancellation);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
