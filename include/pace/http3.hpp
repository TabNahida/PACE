#pragma once
#include <pace/http_types.hpp>
#include <pace/quic.hpp>

#ifdef PACE_HAS_HTTP3
namespace pace::http3 {
struct limits {
    std::size_t max_header_bytes = 16 * 1024;
    std::size_t max_header_fields = 100;
    std::size_t max_body_bytes = 1024 * 1024;
    std::size_t max_concurrent_streams = 32;
    std::size_t max_requests_per_connection = 1000;
    // Cooperative application tasks per connection; 1 serializes handlers.
    // The effective cap is min(this value, max_concurrent_streams).
    std::size_t max_concurrent_handlers = 32;
};

// RFC 9114 over an authenticated QUIC connection with negotiated ALPN "h3".
// Owns the connection for a multiplexed batch; results preserve input order.
// Bodies are buffered within the per-stream limits. No CONNECT or server push.
task<std::vector<http::response>> async_exchange(quic::connection transport, std::string authority,
                                          std::vector<http::request> requests, limits bounds = {});
task<void> serve_connection(quic::connection transport, http::handler handle, limits bounds = {});
// The QUIC listener must use a server context configured with certificates and
// ALPN {"h3"}. Each connection owns its control and QPACK streams.
task<void> serve(quic::listener& listener, http::handler handle, limits bounds = {});
// One GET over an owned connection. For multiplexing, use async_exchange() with a batch.
inline task<http::response> async_get(quic::connection transport, std::string authority,
                         std::string target = "/", limits bounds = {}) {
    std::vector<http::request> requests(1);
    requests.front().method = "GET";
    requests.front().target = std::move(target);
    auto replies = co_await async_exchange(std::move(transport), std::move(authority), std::move(requests), bounds);
    co_return std::move(replies.front());
}

// URL clients use the same names/ownership contract as HTTP/1. Each call owns one
// connection. HTTP/2 http:// uses prior knowledge; HTTP/3 requires https://.
task<http::response> async_fetch(io_context&, std::string url, http::request, limits bounds = {});
inline task<http::response> async_get(io_context& io, std::string url, limits bounds = {}) {
    http::request request; request.method = "GET";
    return async_fetch(io, std::move(url), std::move(request), bounds);
}
inline task<http::response> async_post(io_context& io, std::string url, std::string body,
        std::string content_type = "application/octet-stream", limits bounds = {}) {
    http::request request; request.method = "POST"; request.body = std::move(body);
    request.headers.push_back({"Content-Type", std::move(content_type)});
    return async_fetch(io, std::move(url), std::move(request), bounds);
}
inline task<http::response> async_fetch(std::string url, http::request request, limits bounds = {}) {
    co_return co_await async_fetch(this_context(), std::move(url), std::move(request), bounds);
}
inline task<http::response> async_get(std::string url, limits bounds = {}) {
    co_return co_await async_get(this_context(), std::move(url), bounds);
}
inline task<http::response> async_post(std::string url, std::string body,
        std::string content_type = "application/octet-stream", limits bounds = {}) {
    co_return co_await async_post(this_context(), std::move(url), std::move(body), std::move(content_type), bounds);
}
inline http::response fetch(std::string url, http::request request, blocking_options wait = {}, limits bounds = {}) {
    return pace::detail::blocking_call([&](io_context& io) { return async_fetch(io, std::move(url), std::move(request), bounds); }, wait);
}
inline http::response get(std::string url, blocking_options wait = {}, limits bounds = {}) {
    return pace::detail::blocking_call([&](io_context& io) { return async_get(io, std::move(url), bounds); }, wait);
}
inline http::response post(std::string url, std::string body, std::string content_type = "application/octet-stream",
        blocking_options wait = {}, limits bounds = {}) {
    return pace::detail::blocking_call([&](io_context& io) { return async_post(io, std::move(url), std::move(body), std::move(content_type), bounds); }, wait);
}
task<http::response> async_fetch(io_context&, const quic::context&, std::string url, http::request, limits bounds = {});
inline task<http::response> async_get(io_context& io, const quic::context& trust, std::string url, limits bounds = {}) {
    http::request request; request.method = "GET";
    return async_fetch(io, trust, std::move(url), std::move(request), bounds);
}
inline http::response fetch(const quic::context& trust, std::string url, http::request request,
        blocking_options wait = {}, limits bounds = {}) {
    return pace::detail::blocking_call([&](io_context& io) { return async_fetch(io, trust, std::move(url), std::move(request), bounds); }, wait);
}
inline http::response get(const quic::context& trust, std::string url, blocking_options wait = {}, limits bounds = {}) {
    return pace::detail::blocking_call([&](io_context& io) { return async_get(io, trust, std::move(url), bounds); }, wait);
}
} // namespace pace::http3
#endif
