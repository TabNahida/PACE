#pragma once

#include <pace/net/tcp.hpp>
#ifdef PACE_HAS_TLS
#include <pace/tls/stream.hpp>
#endif

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace pace::websocket
{
/// Which side of the HTTP/1.1 Upgrade handshake a stream performs.
enum class role
{
    client,
    server
};
/// Data frame opcode of a whole message. Text must be valid UTF-8.
enum class message_type
{
    text,
    binary
};

/// One complete application message, reassembled from its fragments.
struct message
{
    /// Text or binary.
    message_type type = message_type::text;
    /// The payload; may contain NUL bytes.
    std::string data;
};

/// Per-stream bounds, checked by the constructor: max_header_bytes must be 64 bytes to 1 MiB, the others nonzero,
/// and max_frame_bytes at most max_message_bytes; otherwise it throws std::invalid_argument.
struct limits
{
    /// HTTP Upgrade request or response head.
    std::size_t max_header_bytes = std::size_t{16} * 1024;
    /// Payload of one data frame, sent or received.
    std::size_t max_frame_bytes = std::size_t{64} * 1024;
    /// Payload of one reassembled message.
    std::size_t max_message_bytes = std::size_t{1024} * 1024;
    /// Data frames in one message.
    std::size_t max_fragments = 1024;
    /// Ping, pong and close frames handled by one receive() call; bounds control-frame floods.
    std::size_t max_control_frames = 1024;
};

/// What a client asked for in its Upgrade request, as seen by a server's accept().
struct upgrade_request
{
    /// Request target: origin-form path and query.
    std::string target;
    /// Host field.
    std::string host;
    /// Origin field, or empty.
    std::string origin;
    /// Offered subprotocols, in the client's order.
    std::vector<std::string> protocols;
};

/// A WebSocket protocol violation, by the peer or in the upgrade exchange. After the upgrade the stream sends the
/// matching close frame (best effort); in every case it closes its transport.
class protocol_error : public std::runtime_error
{
public:
    /// Creates an error carrying close status `code`.
    protocol_error(std::uint16_t code, const std::string& message)
        : std::runtime_error(message)
        , code_(code)
    {
    }

    /// The RFC 6455 close status: 1002 protocol, 1007 invalid UTF-8, 1008 policy (control flood or a rejected
    /// upgrade), 1009 too big.
    std::uint16_t close_code() const noexcept
    {
        return code_;
    }

private:
    std::uint16_t code_;
};

/// An RFC 6455 WebSocket over an owned, connected `tcp::stream` or handshaken `tls::stream`.
/// Only one operation may be active at a time, sends and receive included; an overlap throws std::logic_error and
/// leaves the active operation running. Every member runs on the transport's io_context. Move only while idle.
template <class Transport>
class basic_stream
{
public:
    /// Takes ownership of `transport` before any HTTP bytes have been read from it.
    /// Throws std::invalid_argument for an invalid role or limits.
    explicit basic_stream(Transport transport, role mode, limits bounds = {});
    /// Closes the transport without a closing handshake.
    ~basic_stream();
    basic_stream(basic_stream&&) noexcept;
    basic_stream& operator=(basic_stream&&) noexcept;
    basic_stream(const basic_stream&) = delete;
    basic_stream& operator=(const basic_stream&) = delete;

    /// Client: sends the Upgrade request with Host `host` and origin-form `target`, offering `protocols`, and
    /// validates the server's answer. Throws std::invalid_argument for an invalid host, target or protocol list,
    /// websocket::protocol_error for a rejected or invalid answer.
    task<void> handshake(std::string host, std::string target = "/", std::vector<std::string> protocols = {});
    /// Server: reads the Upgrade request, selects the first of `supported_protocols` the client offered, asks
    /// `authorize` (if set; it runs synchronously and must not block) and answers 101, or 400, 403 or 426 and
    /// throws websocket::protocol_error. Returns the request's target, Host, Origin and offered protocols.
    task<upgrade_request> accept(
        std::vector<std::string> supported_protocols = {},
        std::function<bool(const upgrade_request&)> authorize = {}
    );

    /// Returns the next complete message. Reassembles fragments, answers ping, consumes pong, and returns nullopt
    /// after a peer close (having sent the close reply and closed the transport). EOF without a close frame and any
    /// protocol violation throw websocket::protocol_error.
    task<std::optional<message>> receive();
    /// Sends `value` in frames of `fragment_bytes` (0, or a value above max_frame_bytes, means max_frame_bytes).
    /// The data is owned across suspension. Throws std::invalid_argument for invalid UTF-8 text or a message over
    /// the limits, std::logic_error after close.
    task<void> send(message value, std::size_t fragment_bytes = 0);

    /// Sends a text message; see send().
    task<void> send_text(std::string text, std::size_t fragment_bytes = 0)
    {
        return send({message_type::text, std::move(text)}, fragment_bytes);
    }

    /// Sends a binary message; see send().
    task<void> send_binary(std::string bytes, std::size_t fragment_bytes = 0)
    {
        return send({message_type::binary, std::move(bytes)}, fragment_bytes);
    }

    /// Sends a ping. The payload is at most 125 bytes; throws std::invalid_argument otherwise.
    task<void> ping(std::string payload = {});
    /// Sends an unsolicited pong. The payload is at most 125 bytes; throws std::invalid_argument otherwise.
    task<void> pong(std::string payload = {});
    /// Starts the closing handshake; call receive() to process the peer's reply. The reason must be valid UTF-8 of
    /// at most 123 bytes. No data may be sent afterwards.
    task<void> send_close(std::uint16_t code = 1000, std::string reason = {});
    /// Sends close (unless already sent), discards incoming messages until the peer's close frame, then closes the
    /// transport in every case. Returns true if the peer's close frame arrived, false if `timeout` expired first;
    /// other errors propagate.
    task<bool> close_gracefully(
        std::uint16_t code = 1000,
        std::string reason = {},
        std::chrono::milliseconds timeout = std::chrono::milliseconds(100)
    );
    /// Closes the transport immediately, without a closing handshake. Pending operations fail.
    void close() noexcept;
    /// True until the transport is closed.
    [[nodiscard]] bool is_open() const noexcept;
    /// The io_context that owns the transport.
    [[nodiscard]] io_context& context() const noexcept;
    /// The negotiated subprotocol, or an empty string.
    [[nodiscard]] std::string selected_protocol() const;
    /// The status code of the peer's close frame; empty if none arrived or it carried no status.
    [[nodiscard]] std::optional<std::uint16_t> peer_close_code() const;
    /// The reason text of the peer's close frame, or an empty string.
    [[nodiscard]] std::string peer_close_reason() const;

private:
    struct implementation;
    std::unique_ptr<implementation> impl_;
};

/// WebSocket over plain TCP (`ws`).
using stream = basic_stream<tcp::stream>;
extern template class basic_stream<tcp::stream>;
#ifdef PACE_HAS_TLS
/// WebSocket over TLS (`wss`); the TLS handshake must be complete, with ALPN empty or "http/1.1".
using secure_stream = basic_stream<tls::stream>;
extern template class basic_stream<tls::stream>;
#endif
/// Client: wraps a connected TCP or handshaken TLS `transport` and performs handshake(). `authority` is the Host
/// value (with the port when needed), not a URL. The task owns all arguments.
template <class Transport>
task<basic_stream<Transport>> upgrade(
    Transport transport,
    std::string authority,
    std::string target = "/",
    std::vector<std::string> protocols = {},
    limits bounds = {}
)
{
    basic_stream<Transport> result(std::move(transport), role::client, bounds);
    co_await result.handshake(std::move(authority), std::move(target), std::move(protocols));
    co_return result;
}

/// Server: wraps an accepted TCP or handshaken TLS `transport` and performs basic_stream::accept(). Returns only
/// the stream; call the member accept() to also get the upgrade_request. An empty `authorize` accepts every request.
template <class Transport>
task<basic_stream<Transport>> accept(
    Transport transport,
    std::vector<std::string> protocols = {},
    std::function<bool(const upgrade_request&)> authorize = {},
    limits bounds = {}
)
{
    basic_stream<Transport> result(std::move(transport), role::server, bounds);
    co_await result.accept(std::move(protocols), std::move(authorize));
    co_return result;
}

/// Client: connects TCP to `peer` (a numeric address) on `io`, then performs upgrade().
inline task<stream> connect(
    io_context& io,
    tcp::endpoint peer,
    std::string authority,
    std::string target = "/",
    std::vector<std::string> protocols = {},
    limits bounds = {}
)
{
    auto transport = co_await tcp::connect(io, std::move(peer));
    co_return co_await upgrade(
        std::move(transport),
        std::move(authority),
        std::move(target),
        std::move(protocols),
        bounds
    );
}

/// connect() on the current context.
inline task<stream> connect(
    tcp::endpoint peer,
    std::string authority,
    std::string target = "/",
    std::vector<std::string> protocols = {},
    limits bounds = {}
)
{
    co_return co_await connect(
        this_context(),
        std::move(peer),
        std::move(authority),
        std::move(target),
        std::move(protocols),
        bounds
    );
}
} // namespace pace::websocket
