#pragma once

#include <pace/http/types.hpp>
#include <pace/net/tcp.hpp>
#include <pace/tls/stream.hpp>

#ifdef PACE_HAS_HTTP2
namespace pace::http2
{
/// Per-connection bounds for clients and servers. Every violation fails the connection.
/// Header byte and field limits and max_requests_per_connection must be nonzero; the two concurrency limits must
/// be 1-10000. Otherwise the call that receives them throws std::invalid_argument.
struct limits
{
    /// Decoded header bytes per stream (name + value + 32 per field), initial headers and trailers together.
    std::size_t max_header_bytes = std::size_t{16} * 1024;
    /// Header fields per stream.
    std::size_t max_header_fields = 100;
    /// Buffered body bytes per stream and direction; 0 allows only empty bodies.
    std::size_t max_body_bytes = std::size_t{1024} * 1024;
    /// Streams open at once; also the largest client batch.
    std::size_t max_concurrent_streams = 100;
    /// Requests over the life of one connection.
    std::size_t max_requests_per_connection = 1000;
    /// Cooperative handler tasks per server connection; 1 serializes handlers.
    /// The effective cap is min(this value, max_concurrent_streams).
    std::size_t max_concurrent_handlers = 100;
};

/// Sends `requests` as one batch of streams over HTTP/2 prior knowledge on cleartext TCP and returns the
/// responses in request order. Owns `transport` and closes it after the batch. `authority` becomes `:authority`.
/// Bodies are buffered. Throws std::invalid_argument for an empty authority, an empty or oversized batch, or an
/// invalid request (including CONNECT); throws if any stream fails, is reset or is incomplete (no partial batch).
task<std::vector<http::response>> async_exchange(
    tcp::stream transport,
    std::string authority,
    std::vector<http::request> requests,
    limits bounds = {}
);
/// Serves one cleartext prior-knowledge connection until the peer finishes or it fails. Owns `transport`.
/// A handler exception becomes a 500 response; connection errors and cancellation propagate.
task<void> serve_connection(tcp::stream transport, http::handler handle, limits bounds = {});
/// Accepts cleartext connections and serves each one until `listener` is closed or the call is canceled.
/// A failing connection ends only itself. Returns after cancelling and joining every accepted connection.
/// `listener` must outlive the call.
task<void> serve(tcp::listener& listener, http::handler handle, limits bounds = {});

#ifdef PACE_HAS_TLS
/// TLS form of async_exchange(). `transport` must have completed a verified handshake that negotiated ALPN "h2";
/// throws std::runtime_error otherwise.
task<std::vector<http::response>> async_exchange(
    tls::stream transport,
    std::string authority,
    std::vector<http::request> requests,
    limits bounds = {}
);
/// TLS form of serve_connection(). `transport` must have completed a handshake that negotiated ALPN "h2".
task<void> serve_connection(tls::stream transport, http::handler handle, limits bounds = {});
/// Accepts TCP connections, performs each TLS handshake and serves the connection. Configure certificates and
/// ALPN {"h2"} before the call; `configuration` and `listener` must outlive it. A failed handshake ends only
/// that connection.
task<void> serve(tcp::listener& listener, const tls::context& configuration, http::handler handle, limits bounds = {});
#endif
/// One GET of `target` over an owned TCP or TLS connection; a batch of one for async_exchange().
template <class Transport>
requires(
    std::is_same_v<Transport, tcp::stream>
#ifdef PACE_HAS_TLS
    || std::is_same_v<Transport, tls::stream>
#endif
)
task<http::response> async_get(Transport transport, std::string authority, std::string target = "/", limits bounds = {})
{
    std::vector<http::request> requests(1);
    requests.front().method = "GET";
    requests.front().target = std::move(target);
    auto replies = co_await async_exchange(std::move(transport), std::move(authority), std::move(requests), bounds);
    co_return std::move(replies.front());
}

/// Sends `request` to `url` over a new connection and returns the response. `http://` uses prior knowledge;
/// `https://` uses TLS with system trust and ALPN "h2" (and needs --tls=y, otherwise it throws
/// std::system_error(not_supported)). The URL supplies the target and authority; the request must not set Host
/// or pseudo-headers. Same contract as the HTTP/1 URL client.
task<http::response> async_fetch(io_context&, std::string url, http::request, limits bounds = {});

/// GET `url`; see async_fetch().
inline task<http::response> async_get(io_context& io, std::string url, limits bounds = {})
{
    http::request request;
    request.method = "GET";
    return async_fetch(io, std::move(url), std::move(request), bounds);
}

/// POST `body` with `content_type` to `url`; see async_fetch().
inline task<http::response> async_post(
    io_context& io,
    std::string url,
    std::string body,
    std::string content_type = "application/octet-stream",
    limits bounds = {}
)
{
    http::request request;
    request.method = "POST";
    request.body = std::move(body);
    request.headers.push_back({"Content-Type", std::move(content_type)});
    return async_fetch(io, std::move(url), std::move(request), bounds);
}

/// async_fetch() on the current context.
inline task<http::response> async_fetch(std::string url, http::request request, limits bounds = {})
{
    co_return co_await async_fetch(this_context(), std::move(url), std::move(request), bounds);
}

/// async_get() on the current context.
inline task<http::response> async_get(std::string url, limits bounds = {})
{
    co_return co_await async_get(this_context(), std::move(url), bounds);
}

/// async_post() on the current context.
inline task<http::response> async_post(
    std::string url,
    std::string body,
    std::string content_type = "application/octet-stream",
    limits bounds = {}
)
{
    co_return co_await async_post(this_context(), std::move(url), std::move(body), std::move(content_type), bounds);
}

/// Blocking async_fetch() on a private io_context. Throws std::logic_error if called inside a PACE context.
inline http::response fetch(std::string url, http::request request, blocking_options wait = {}, limits bounds = {})
{
    return blocking_call(
        [&](io_context& io) { return async_fetch(io, std::move(url), std::move(request), bounds); },
        std::move(wait)
    );
}

/// Blocking async_get(); see fetch().
inline http::response get(std::string url, blocking_options wait = {}, limits bounds = {})
{
    return blocking_call([&](io_context& io) { return async_get(io, std::move(url), bounds); }, std::move(wait));
}

/// Blocking async_post(); see fetch().
inline http::response post(
    std::string url,
    std::string body,
    std::string content_type = "application/octet-stream",
    blocking_options wait = {},
    limits bounds = {}
)
{
    return blocking_call(
        [&](io_context& io)
        { return async_post(io, std::move(url), std::move(body), std::move(content_type), bounds); },
        std::move(wait)
    );
}
#ifdef PACE_HAS_TLS
/// async_fetch() with the caller's TLS configuration, which must set ALPN {"h2"} and must outlive the call.
/// Requires an `https://` URL; throws std::invalid_argument otherwise.
task<http::response> async_fetch(io_context&, const tls::context&, std::string url, http::request, limits bounds = {});

/// GET with the caller's TLS configuration; see async_fetch(io_context&, const tls::context&, ...).
inline task<http::response> async_get(io_context& io, const tls::context& trust, std::string url, limits bounds = {})
{
    http::request request;
    request.method = "GET";
    return async_fetch(io, trust, std::move(url), std::move(request), bounds);
}

/// Blocking fetch with the caller's TLS configuration.
inline http::response fetch(
    const tls::context& trust,
    std::string url,
    http::request request,
    blocking_options wait = {},
    limits bounds = {}
)
{
    return blocking_call(
        [&](io_context& io) { return async_fetch(io, trust, std::move(url), std::move(request), bounds); },
        std::move(wait)
    );
}

/// Blocking GET with the caller's TLS configuration.
inline http::response get(const tls::context& trust, std::string url, blocking_options wait = {}, limits bounds = {})
{
    return blocking_call([&](io_context& io) { return async_get(io, trust, std::move(url), bounds); }, std::move(wait));
}
#endif
} // namespace pace::http2
#endif
