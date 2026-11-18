#pragma once
#include <pace/http_types.hpp>
#include <pace/tcp.hpp>
#include <pace/tls.hpp>

#ifdef PACE_HAS_HTTP2
namespace pace::http2 {
struct limits {
    std::size_t max_header_bytes = 16 * 1024;
    std::size_t max_header_fields = 100;
    std::size_t max_body_bytes = 1024 * 1024;
    std::size_t max_concurrent_streams = 100;
    std::size_t max_requests_per_connection = 1000;
    // Cooperative application tasks per connection; 1 serializes handlers.
    // The effective cap is min(this value, max_concurrent_streams).
    std::size_t max_concurrent_handlers = 100;
};

// HTTP/2 prior knowledge on cleartext TCP. Owns the connection for one batch;
// requests are submitted together and results preserve input order. Bodies are
// buffered with per-stream bounds. No HTTP/1 Upgrade or CONNECT tunnel support.
task<std::vector<http::response>> async_exchange(tcp::stream transport, std::string authority,
                                          std::vector<http::request> requests, limits bounds = {});
task<void> serve_connection(tcp::stream transport, http::handler handle, limits bounds = {});
task<void> serve(tcp::listener& listener, http::handler handle, limits bounds = {});

#ifdef PACE_HAS_TLS
// Transport must have completed a verified TLS handshake with ALPN "h2".
task<std::vector<http::response>> async_exchange(tls::stream transport, std::string authority,
                                          std::vector<http::request> requests, limits bounds = {});
task<void> serve_connection(tls::stream transport, http::handler handle, limits bounds = {});
// Configure certificates and ALPN {"h2"} before entering serve(). Configuration
// and listener must outlive the server. Each accepted stream performs handshake.
task<void> serve(tcp::listener& listener, const tls::context& configuration,
                 http::handler handle, limits bounds = {});
#endif
// One GET over an owned connection. For multiplexing, use async_exchange() with a batch.
template<class Transport>
requires (std::is_same_v<Transport, tcp::stream>
#ifdef PACE_HAS_TLS
          || std::is_same_v<Transport, tls::stream>
#endif
         )
task<http::response> async_get(Transport transport, std::string authority,
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
#ifdef PACE_HAS_TLS
task<http::response> async_fetch(io_context&, const tls::context&, std::string url, http::request, limits bounds = {});
inline task<http::response> async_get(io_context& io, const tls::context& trust, std::string url, limits bounds = {}) {
    http::request request; request.method = "GET";
    return async_fetch(io, trust, std::move(url), std::move(request), bounds);
}
inline http::response fetch(const tls::context& trust, std::string url, http::request request,
        blocking_options wait = {}, limits bounds = {}) {
    return pace::detail::blocking_call([&](io_context& io) { return async_fetch(io, trust, std::move(url), std::move(request), bounds); }, wait);
}
inline http::response get(const tls::context& trust, std::string url, blocking_options wait = {}, limits bounds = {}) {
    return pace::detail::blocking_call([&](io_context& io) { return async_get(io, trust, std::move(url), bounds); }, wait);
}
#endif
} // namespace pace::http2
#endif
