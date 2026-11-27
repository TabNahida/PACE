#pragma once

#include <pace/gateway/services.hpp>
#include <pace/http/router.hpp>
#include <pace/http/types.hpp>
#include <pace/net/tcp.hpp>
#include <pace/runtime/run.hpp>
#include <pace/runtime/runtime.hpp>
#ifdef PACE_HAS_TLS
#include <pace/tls/stream.hpp>
#endif

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace pace::http
{
/// Bounds on what an HTTP/1 server or client receives. Zero allows none of a resource, not "unlimited".
struct limits
{
    /// Start line and header fields, including the final empty line.
    std::size_t max_header_bytes = std::size_t{16} * 1024;
    /// De-framed body bytes; a larger Content-Length is rejected before reading.
    std::size_t max_body_bytes = std::size_t{1024} * 1024;
    /// Server only: requests served on one connection; the last permitted response closes it.
    std::size_t max_requests_per_connection = 1000;
    /// Number of header fields.
    std::size_t max_header_fields = 100;
    /// Trailer field lines and the final empty line.
    std::size_t max_trailer_bytes = std::size_t{8} * 1024;
    /// Number of trailer fields.
    std::size_t max_trailer_fields = 32;
    /// One chunk-size line, including extensions and CRLF.
    std::size_t max_chunk_line_bytes = 1024;
    /// Chunks per message, including the terminating zero-size chunk.
    std::size_t max_chunks = 65536;
    /// Client only: 1xx responses skipped before the final response.
    std::size_t max_informational_responses = 8;
};

/// How a client closes a connection after the final response on it.
enum class client_close_policy
{
    /// Send the local FIN or TLS close_notify without waiting for the peer.
    no_wait,
    /// Wait for the peer to close, at most client_options::close_timeout.
    graceful
};

/// Client close behaviour. Applies only after a complete response ends the connection; a response framed by the
/// end of the connection still needs that EOF first. Invalid values throw std::invalid_argument.
struct client_options
{
    /// The close policy.
    client_close_policy close_policy = client_close_policy::no_wait;
    /// Nonnegative wait of the graceful policy; zero sends the close notification once without waiting.
    std::chrono::milliseconds close_timeout{100};
};

/// The validated head of a request, passed to server_options::admit before the body is read.
/// Every view and the span are valid only during that call.
struct request_head
{
    /// Method token.
    std::string_view method;
    /// Request target, not decoded.
    std::string_view target;
    /// "HTTP/1.0" or "HTTP/1.1".
    std::string_view version;
    /// Header fields.
    std::span<const header_field> headers;
    /// Declared Content-Length; empty for a chunked request or one without a length.
    std::optional<std::size_t> content_length;
    /// The body uses chunked transfer coding.
    bool chunked = false;
    /// The client asked to keep the connection open.
    bool keep_alive = true;
    /// The first header whose name matches ignoring ASCII case, or an empty view.
    [[nodiscard]] std::string_view header(std::string_view name) const noexcept;
};

/// The decision of server_options::admit for one request.
struct admission
{
    /// Body limit for this request only, replacing limits::max_body_bytes; empty keeps it, zero allows no body.
    std::optional<std::size_t> max_body_bytes;
    /// When set, this response is sent instead of reading the body and running the handler.
    std::optional<response> rejection;

    /// Accept the request, reading its body with `maximum` (or the default limit when empty).
    static admission accept(std::optional<std::size_t> maximum = {})
    {
        return {maximum, {}};
    }

    /// Send `result` without reading the body or running the handler, then close the connection.
    static admission reject(response result)
    {
        return {{}, std::move(result)};
    }
};

/// Produces the body of a streaming_response on demand. Owned by that response; destroyed on every path,
/// including disconnect, cancellation and write deadline expiry, after which it is not called again.
class body_source
{
public:
    /// Destroys the source.
    virtual ~body_source() = default;
    /// Fills at most destination.size() bytes and returns the positive count written; zero or a larger count
    /// before Content-Length is reached, or an exception, closes the connection. Runs synchronously on the
    /// connection's context, only after the previous bytes were written, and must return promptly.
    virtual std::size_t read(std::span<char> destination) = 0;
};

/// A response whose body is pulled from `source` and framed with Content-Length.
struct streaming_response
{
    /// Status, content type and header fields; body and trailers must be empty and chunked false.
    response head;
    /// Exact number of body bytes `source` will produce.
    std::uint64_t content_length = 0;
    /// The body producer; may be null when no byte is read (zero length, HEAD, 204, 205, 304).
    std::unique_ptr<body_source> source;
};

/// What a server_handler returns: a buffered response or a streamed one.
using server_response = std::variant<response, streaming_response>;
/// Handler of the server_options overloads; copied into every connection.
using server_handler = std::function<task<server_response>(request)>;

/// Options of the server_handler overloads. Negative timeouts or a zero buffer throw std::invalid_argument.
struct server_options
{
    /// Receive limits.
    limits bounds{};
    /// Called synchronously for every request after its head is validated and before its body is read.
    /// Empty accepts every request. Throwing yields a 500 and closes the connection.
    std::function<admission(const request_head&)> admit{};
    /// Absolute budget for receiving one request (head, admission, body), including the wait for it on a
    /// keep-alive connection. Zero disables it. Expiry closes the connection without a reply.
    std::chrono::milliseconds request_timeout{0};
    /// Absolute budget for writing one response, body_source calls included. Zero disables it.
    std::chrono::milliseconds write_timeout{0};
    /// Largest span passed to body_source::read; must not be zero.
    std::size_t response_buffer_bytes = std::size_t{64} * 1024;
};

/// Serves HTTP/1.0 and HTTP/1.1 requests on one connection, one at a time, until it closes. Owns the stream and
/// the handler. Handler exceptions become a 500 and close the connection; transport errors and cancellation end
/// the connection and the task completes normally.
task<void> serve_connection(tcp::stream stream, handler handle, limits bounds = {});
/// Accepts connections and serves each in its own task with a copy of the handler, which may run concurrently on
/// other worker contexts. Returns when the listener is closed or the task cancelled, after cancelling and joining
/// every accepted connection; other accept errors are thrown. The listener and its context must outlive the task.
task<void> serve(tcp::listener& listener, handler handle, limits bounds = {});
/// Like the listener overload, but creates and owns a listener bound to `endpoint` when the task starts; bind
/// errors are thrown from the task.
task<void> serve(io_context& context, tcp::endpoint endpoint, handler handle, limits bounds = {});

/// serve_connection with admission, per-request body limits, deadlines and streamed responses.
task<void> serve_connection(tcp::stream stream, server_handler handle, server_options options = {});
/// serve with admission, per-request body limits, deadlines and streamed responses.
task<void> serve(tcp::listener& listener, server_handler handle, server_options options = {});

/// A persistent HTTP/1 client connection: one exchange at a time, on the stream's context. The client, its stream
/// and that context must stay alive and unmoved until send() completes. Only the tcp::stream and tls::stream
/// instantiations exist.
template <class Transport>
class basic_client
{
public:
    /// Takes a connected stream (a TLS stream after its handshake). Throws std::invalid_argument for bad options.
    explicit basic_client(Transport stream, limits bounds = {}, client_options options = {});
    basic_client(const basic_client&) = delete;
    basic_client& operator=(const basic_client&) = delete;
    /// Closes the connection.
    ~basic_client() = default;
    /// Writes `outgoing` and reads one final response. Throws std::invalid_argument for an invalid request before
    /// writing, std::logic_error when another exchange runs, the connection is closed or the caller is on another
    /// context, http::protocol_error for a bad response and std::system_error for transport errors; the last two
    /// close the connection.
    task<response> send(request outgoing);

    /// The owned stream, for configuration while no exchange runs; do not read or write it directly.
    [[nodiscard]] Transport& transport() noexcept
    {
        return stream_;
    }

    /// Whether the stream is open and may be reused; does not probe the peer.
    [[nodiscard]] bool is_open() const noexcept
    {
        return stream_.is_open() && reusable_;
    }

    /// Closes the connection at once; idempotent. Call it on the owning context.
    void close() noexcept
    {
        reusable_ = false;
        stream_.close();
    }

private:
    Transport stream_;
    limits bounds_;
    client_options options_;
    std::string pending_;
    bool busy_ = false;
    bool reusable_ = true;
};

/// A persistent HTTP/1 client over TCP.
using client = basic_client<tcp::stream>;
extern template class basic_client<tcp::stream>;

/// Connects to a numeric endpoint, sends `outgoing` with keep_alive forced off, returns the response and closes.
/// A missing Host is set from the endpoint. No DNS, redirects or retries. `context` must outlive the task.
task<response> async_fetch(
    io_context& context,
    tcp::endpoint endpoint,
    request outgoing,
    limits bounds = {},
    client_options options = {}
);
/// async_fetch with a GET of `target`.
task<response> async_get(
    io_context& context,
    tcp::endpoint endpoint,
    std::string target = "/",
    limits bounds = {},
    client_options options = {}
);

/// Sends `outgoing` to an absolute http or https URL on one new connection and returns the response. The URL sets
/// the target and Host; a Host in `outgoing` throws std::invalid_argument. Host names need DNS support and https
/// needs TLS support (else std::system_error with not_supported). Everything happens when the task runs. No
/// redirects, retries, proxy, cookies or decompression. `context` must outlive the task.
task<response> async_fetch(
    io_context& context,
    std::string url,
    request outgoing,
    limits bounds = {},
    client_options options = {}
);
/// URL async_fetch with GET.
task<response> async_get(io_context& context, std::string url, limits bounds = {}, client_options options = {});
/// URL async_fetch with POST, `body` and a Content-Type of `content_type`.
task<response> async_post(
    io_context& context,
    std::string url,
    std::string body,
    std::string content_type = "application/octet-stream",
    limits bounds = {},
    client_options options = {}
);
/// URL async_fetch on the context that runs the task.
task<response> async_fetch(std::string url, request outgoing, limits bounds = {}, client_options options = {});
/// URL async_get on the context that runs the task.
task<response> async_get(std::string url, limits bounds = {}, client_options options = {});
/// URL async_post on the context that runs the task.
task<response> async_post(
    std::string url,
    std::string body,
    std::string content_type = "application/octet-stream",
    limits bounds = {},
    client_options options = {}
);

/// Runs URL async_fetch on a fresh context on this thread and returns the response. `wait` bounds the whole call
/// (30 s by default); expiry throws std::system_error with timed_out. Throws std::logic_error inside a running
/// PACE context.
inline response fetch(
    std::string url,
    request outgoing,
    blocking_options wait = {},
    limits bounds = {},
    client_options options = {}
)
{
    return blocking_call(
        [&](io_context& io) { return async_fetch(io, std::move(url), std::move(outgoing), bounds, options); },
        std::move(wait)
    );
}

/// Blocking URL GET; see fetch.
inline response get(std::string url, blocking_options wait = {}, limits bounds = {}, client_options options = {})
{
    return blocking_call(
        [&](io_context& io) { return async_get(io, std::move(url), bounds, options); },
        std::move(wait)
    );
}

/// Blocking URL POST of `body` with a Content-Type of `content_type`; see fetch.
inline response post(
    std::string url,
    std::string body,
    std::string content_type = "application/octet-stream",
    blocking_options wait = {},
    limits bounds = {},
    client_options options = {}
)
{
    return blocking_call(
        [&](io_context& io)
        { return async_post(io, std::move(url), std::move(body), std::move(content_type), bounds, options); },
        std::move(wait)
    );
}

/// A small HTTP/1 application: routes, static mounts and proxies on an internal router, served with serve, run or
/// run_workers. Configure it before serving; configuration is not synchronized. No signal handling.
class app
{
    router routes_;

public:
    /// Registers an exact route (router::add). Throws std::invalid_argument for an invalid or duplicate route.
    template <class Function>
    app& route(std::string method, std::string path, Function&& function)
    {
        routes_.add(std::move(method), std::move(path), std::forward<Function>(function));
        return *this;
    }

    /// route("GET", path, function).
    template <class Function>
    app& get(std::string path, Function&& function)
    {
        return route("GET", std::move(path), std::forward<Function>(function));
    }

    /// route("POST", path, function).
    template <class Function>
    app& post(std::string path, Function&& function)
    {
        return route("POST", std::move(path), std::forward<Function>(function));
    }

    /// Serves files under `directory` for paths under `prefix` with a static_files service. Throws
    /// std::invalid_argument for invalid options or prefix and std::system_error when the directory cannot be opened.
    app& mount(std::string prefix, std::filesystem::path directory, static_options options = {})
    {
        auto service = static_files(prefix, std::move(directory), std::move(options));
        routes_.mount(std::move(prefix), std::move(service));
        return *this;
    }

    /// Forwards requests under `prefix` to the upstream base URLs with a reverse_proxy service. Throws
    /// std::invalid_argument for invalid options, prefix or upstream URLs.
    app& proxy(std::string prefix, std::vector<std::string> upstreams, proxy_options options = {})
    {
        auto service = reverse_proxy(prefix, std::move(upstreams), options);
        routes_.mount(std::move(prefix), std::move(service));
        return *this;
    }

    /// proxy with a list of upstream base URLs.
    app& proxy(std::string prefix, std::initializer_list<std::string> upstreams, proxy_options options = {})
    {
        return proxy(std::move(prefix), std::vector<std::string>(upstreams), options);
    }

    /// proxy with one upstream base URL.
    app& proxy(std::string prefix, std::string upstream, proxy_options options = {})
    {
        return proxy(std::move(prefix), std::vector<std::string>{std::move(upstream)}, options);
    }

    /// Serves on `workers` contexts, one listener each (SO_REUSEPORT when workers > 1), context 0 on this thread and
    /// the others on new threads; returns when the runtime stops. Throws std::invalid_argument for zero workers and
    /// std::system_error on bind failure or where SO_REUSEPORT is missing.
    void run_workers(std::size_t workers, tcp::endpoint endpoint, limits bounds = {}) const;

    /// A task that binds `endpoint` when it starts and serves a snapshot of the routes; it does not borrow *this.
    task<void> serve(io_context& io, tcp::endpoint endpoint, limits bounds = {}) const
    {
        return http::serve(io, std::move(endpoint), routes_, bounds);
    }

    /// A task that serves a snapshot of the routes on `listener`, which must outlive it.
    task<void> serve(tcp::listener& listener, limits bounds = {}) const
    {
        return http::serve(listener, routes_, bounds);
    }

    /// Serves `endpoint` on a fresh context on this thread until that context stops. Throws std::logic_error inside
    /// a running PACE context and std::system_error on bind failure.
    void run(tcp::endpoint endpoint, limits bounds = {}) const
    {
        pace::run([&](io_context& io) { return serve(io, std::move(endpoint), bounds); });
    }

    /// run on 127.0.0.1:`port` (loopback only).
    void run(std::uint16_t port = 8080, limits bounds = {}) const
    {
        run({"127.0.0.1", port}, bounds);
    }

    /// A copy of the routes, for HTTP/2, HTTP/3, TLS or server_options servers.
    [[nodiscard]] router handler() const
    {
        return routes_;
    }
};

#ifdef PACE_HAS_TLS
/// A persistent HTTP/1 client over TLS; the negotiated ALPN protocol must be http/1.1 or none.
using secure_client = basic_client<tls::stream>;
extern template class basic_client<tls::stream>;
/// URL async_fetch over https with your TLS configuration (trust, verification, ALPN) instead of the default.
/// The URL must be https and still sets the certificate name and Host. `configuration` must outlive the task.
task<response> async_fetch(
    io_context& context,
    const tls::context& configuration,
    std::string url,
    request outgoing,
    limits bounds = {},
    client_options options = {}
);
/// URL async_get over https with your TLS configuration.
task<response> async_get(
    io_context& context,
    const tls::context& configuration,
    std::string url,
    limits bounds = {},
    client_options options = {}
);

/// Blocking fetch over https with your TLS configuration, which must outlive the call.
inline response fetch(
    const tls::context& configuration,
    std::string url,
    request outgoing,
    blocking_options wait = {},
    limits bounds = {},
    client_options options = {}
)
{
    return blocking_call(
        [&](io_context& io)
        { return async_fetch(io, configuration, std::move(url), std::move(outgoing), bounds, options); },
        std::move(wait)
    );
}

/// Blocking GET over https with your TLS configuration, which must outlive the call.
inline response get(
    const tls::context& configuration,
    std::string url,
    blocking_options wait = {},
    limits bounds = {},
    client_options options = {}
)
{
    return blocking_call(
        [&](io_context& io) { return async_get(io, configuration, std::move(url), bounds, options); },
        std::move(wait)
    );
}

/// serve_connection over a TLS stream whose handshake has completed; ALPN must be http/1.1 or none.
task<void> serve_connection(tls::stream stream, handler handle, limits bounds = {});
/// serve over TLS: each accepted connection runs its handshake in its own task before HTTP. `configuration` and
/// the listener must outlive the task.
task<void> serve(tcp::listener& listener, const tls::context& configuration, handler handle, limits bounds = {});
/// TLS serve_connection with server_options.
task<void> serve_connection(tls::stream stream, server_handler handle, server_options options = {});
/// TLS serve with server_options.
task<void> serve(
    tcp::listener& listener,
    const tls::context& configuration,
    server_handler handle,
    server_options options = {}
);
/// One-shot HTTPS exchange with a numeric endpoint. `peer_name` is the certificate name, SNI name and default
/// Host. `configuration` must outlive the task.
task<response> async_fetch(
    io_context& context,
    const tls::context& configuration,
    tcp::endpoint endpoint,
    std::string peer_name,
    request outgoing,
    limits bounds = {},
    client_options options = {}
);
/// One-shot HTTPS GET of `target`; see the async_fetch overload above.
task<response> async_get(
    io_context& context,
    const tls::context& configuration,
    tcp::endpoint endpoint,
    std::string peer_name,
    std::string target = "/",
    limits bounds = {},
    client_options options = {}
);
#endif
} // namespace pace::http
