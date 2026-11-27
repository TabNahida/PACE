#pragma once
#include <pace/http/types.hpp>
#include <pace/quic/connection.hpp>

#ifdef PACE_HAS_HTTP3
namespace pace::http3
{
/// Per-connection bounds for clients and servers, applied to incoming and outgoing messages.
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
    /// Request streams open at once; also the largest client batch.
    std::size_t max_concurrent_streams = 32;
    /// Requests over the life of one connection.
    std::size_t max_requests_per_connection = 1000;
    /// Cooperative handler tasks per server connection; 1 serializes handlers.
    /// The effective cap is min(this value, max_concurrent_streams).
    std::size_t max_concurrent_handlers = 32;
};

/// Sends `requests` as one batch of request streams over an established QUIC connection that negotiated ALPN
/// "h3" and returns the responses in request order. Owns `transport` and closes it after the batch. Bodies are
/// buffered. Throws std::invalid_argument for another ALPN, an empty authority, an empty or oversized batch, or
/// an invalid request (including CONNECT); throws if any stream is refused, reset, truncated or malformed.
task<std::vector<http::response>> async_exchange(
    quic::connection transport,
    std::string authority,
    std::vector<http::request> requests,
    limits bounds = {}
);
/// Serves one established connection that negotiated ALPN "h3" until the peer finishes or it fails.
/// A handler exception becomes a 500 response; connection errors and cancellation propagate.
task<void> serve_connection(quic::connection transport, http::handler handle, limits bounds = {});
/// Accepts connections on `listener` and serves each one until the listener is closed or the call is canceled.
/// The listener's server context needs a certificate and ALPN {"h3"}. A failed handshake or connection ends
/// only itself. Returns after cancelling and joining every accepted connection. `listener` must outlive the call.
task<void> serve(quic::listener& listener, http::handler handle, limits bounds = {});

/// One GET of `target` over an owned connection; a batch of one for async_exchange().
inline task<http::response> async_get(
    quic::connection transport,
    std::string authority,
    std::string target = "/",
    limits bounds = {}
)
{
    std::vector<http::request> requests(1);
    requests.front().method = "GET";
    requests.front().target = std::move(target);
    auto replies = co_await async_exchange(std::move(transport), std::move(authority), std::move(requests), bounds);
    co_return std::move(replies.front());
}

/// Sends `request` to an `https://` `url` over a new QUIC connection (system trust, ALPN "h3") and returns the
/// response. Connects to the first resolved address only. Throws std::invalid_argument for another scheme.
/// The URL supplies the target and authority; the request must not set Host or pseudo-headers.
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

/// async_fetch() with the caller's QUIC client configuration, which must set ALPN {"h3"} and must outlive the
/// call.
task<http::response> async_fetch(io_context&, const quic::context&, std::string url, http::request, limits bounds = {});

/// GET with the caller's QUIC configuration; see async_fetch(io_context&, const quic::context&, ...).
inline task<http::response> async_get(io_context& io, const quic::context& trust, std::string url, limits bounds = {})
{
    http::request request;
    request.method = "GET";
    return async_fetch(io, trust, std::move(url), std::move(request), bounds);
}

/// Blocking fetch with the caller's QUIC configuration.
inline http::response fetch(
    const quic::context& trust,
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

/// Blocking GET with the caller's QUIC configuration.
inline http::response get(const quic::context& trust, std::string url, blocking_options wait = {}, limits bounds = {})
{
    return blocking_call([&](io_context& io) { return async_get(io, trust, std::move(url), bounds); }, std::move(wait));
}
} // namespace pace::http3
#endif
