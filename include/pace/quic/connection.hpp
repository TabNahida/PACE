#pragma once
#ifdef PACE_HAS_QUIC
#include <pace/net/buffer.hpp>
#include <pace/net/stream_io.hpp>
#include <pace/net/udp.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <vector>

namespace pace::quic
{
/// Library-internal access to a connection's transport; not for application use.
struct quic_driver;
} // namespace pace::quic

namespace pace::quic
{
/// Which side of a handshake a `quic::context` configures.
enum class role
{
    client,
    server
};

/// Direction of a stream opened with `connection::open_stream()`.
enum class stream_type
{
    bidirectional,
    unidirectional
};

/// Opaque implementation state, defined in the library sources.
struct configuration;
/// Opaque implementation state, defined in the library sources.
struct engine;
/// Opaque implementation state, defined in the library sources.
struct connection_state;
/// Opaque implementation state, defined in the library sources.
struct stream_state;

/// Shared QUIC/TLS 1.3 configuration: role, trust, certificate and ALPN list. QUIC runs on POSIX only
/// (Linux, macOS, FreeBSD), over ngtcp2 or OpenSSL's native QUIC; both backends expose this API.
/// Copies share one state. Configure it fully before the first connection or listener uses it, and do not
/// change it while any of them is alive. Clients always verify the peer; there is no insecure mode.
class context
{
public:
    /// Creates a configuration for `mode` with the system trust store and ALPN `{"pace-quic"}`.
    /// Throws std::invalid_argument for an invalid role, std::system_error(protocol_error) if OpenSSL fails.
    explicit context(role mode = role::client);
    /// Adds the PEM certificates in `path` to the trusted roots. Throws std::system_error(protocol_error).
    void load_verify_file(const std::string& path);
    /// Loads a PEM certificate chain and its unencrypted PEM private key (server side).
    /// Throws std::system_error(protocol_error) if a file cannot be loaded or the key does not match.
    void use_certificate_chain(const std::string& chain, const std::string& key);
    /// Replaces the ALPN list. Each identifier is 1-255 bytes and the encoded list at most 65535 bytes;
    /// throws std::invalid_argument otherwise, including for an empty list. Servers pick by their own order.
    void set_alpn_protocols(const std::vector<std::string>& protocols);

private:
    std::shared_ptr<configuration> state_;
    friend class connection;
    friend class listener;
};

/// One QUIC stream of a connection. Move-only; the handle keeps its connection's state alive.
/// At most one read and one write may be outstanding at a time; all calls run on the connection's context.
class stream
{
public:
    stream(stream&&) noexcept;
    stream& operator=(stream&&) noexcept;
    /// Releases the handle. Queued output is not waited for; keep a handle until it is acknowledged.
    ~stream();
    stream(const stream&) = delete;
    stream& operator=(const stream&) = delete;
    /// The QUIC stream ID.
    std::uint64_t id() const noexcept;
    /// True for a unidirectional stream.
    bool is_unidirectional() const noexcept;
    /// True if this endpoint opened the stream.
    bool is_local() const noexcept;
    /// The io_context that owns the connection.
    io_context& context() const noexcept;
    /// Reads at least one byte into `buffer`; returns 0 at the peer's FIN (or for an empty buffer).
    /// Throws std::logic_error on an overlapping read or on the sending-only side of a unidirectional stream,
    /// std::system_error on cancellation, reset or connection failure.
    task<std::size_t> read_some(std::span<std::byte> buffer);
    /// Completes when the transport has accepted every byte of `bytes`; does not wait for acknowledgement.
    /// Throws std::logic_error on an overlapping write, after finish_write() or on a receive-only stream.
    task<void> write_all(std::span<const std::byte> bytes);
    /// finish_write() as a byte_stream operation: queues FIN and completes at once.
    task<void> shutdown_write();
    /// reset() as a byte_stream operation: abandons both directions at once.
    void close() noexcept;

    /// Waits for transport progress on the connection; spurious wakeups are permitted. Recheck state after it.
    task<void> wait_events();
    /// Reads without suspending, for multiplexed protocol drivers: nullopt means no data yet, 0 means FIN,
    /// otherwise the number of bytes copied.
    std::optional<std::size_t> try_read(std::span<std::byte> buffer);
    /// Queues as many bytes as the flow-control windows allow without suspending; 0 means would-block.
    std::size_t try_write(std::span<const std::byte> bytes);
    /// Bytes written but not yet acknowledged by the peer. Does not cover the FIN itself.
    std::uint64_t pending_write_bytes() const;
    /// Queues FIN on the sending direction; the receive direction stays usable.
    /// Throws std::logic_error on the receive-only side of a unidirectional stream.
    void finish_write();
    /// Abandons the stream with `application_error` and wakes its pending operations.
    void reset(std::uint64_t application_error = 0) noexcept;

private:
    explicit stream(std::shared_ptr<stream_state> state);
    std::shared_ptr<stream_state> state_;
    friend class connection;
};

/// A client or accepted QUIC connection. Move-only. All members run on the owning io_context.
/// One connect(), one open and one accept may be outstanding at a time; an overlap throws std::logic_error.
class connection
{
public:
    /// Creates an unconnected client bound to an ephemeral UDP port on `io`.
    /// Throws std::invalid_argument if `config` is not a client configuration.
    connection(io_context& io, const quic::context& config);
    connection(connection&&) noexcept;
    connection& operator=(connection&&) noexcept;
    /// Releases the handle. Streams keep the connection's state alive until they are released too.
    ~connection();
    connection(const connection&) = delete;
    connection& operator=(const connection&) = delete;
    /// Performs the QUIC/TLS 1.3 handshake with `peer` (a numeric address) and verifies that the certificate
    /// matches `expected_name` (a DNS name, sent as SNI, or an IP address). A nonpositive `timeout` allows one
    /// immediate attempt. Throws std::invalid_argument for an empty name or port 0,
    /// std::system_error(timed_out) at the deadline, std::system_error(protocol_error) on a TLS/QUIC failure.
    /// A failed connect closes the connection.
    task<void> connect(
        udp::endpoint peer,
        std::string expected_name,
        std::chrono::milliseconds timeout = std::chrono::seconds(10)
    );
    /// Opens a stream, waiting while the peer's stream limit is reached.
    task<stream> open_stream(stream_type type = stream_type::bidirectional);
    /// Opens a stream without suspending; nullopt while the peer's stream limit is reached.
    std::optional<stream> try_open_stream(stream_type type = stream_type::bidirectional);
    /// Waits for the next stream the peer opened. A stream becomes visible when its first data or FIN arrives.
    task<stream> accept_stream();
    /// Takes the next peer-opened stream without suspending; nullopt if none is waiting.
    std::optional<stream> try_accept_stream();
    /// Waits for transport progress on this connection; spurious wakeups are permitted.
    task<void> wait_events();
    /// The io_context that owns the connection.
    io_context& context() const noexcept;
    /// The ALPN protocol negotiated by the handshake, or an empty string before it.
    std::string selected_protocol() const;
    /// False after close, a connection failure, or when the owning context has stopped.
    bool is_open() const noexcept;
    /// Sends CONNECTION_CLOSE best-effort, discards unacknowledged output and invalidates every stream of this
    /// connection. Pending operations fail. Other connections on the same listener are not affected.
    void close(std::uint64_t application_error = 0) noexcept;

private:
    explicit connection(std::shared_ptr<connection_state> state);
    std::shared_ptr<connection_state> state_;
    friend class listener;
    friend struct quic_driver;
};

/// A bound UDP socket that accepts QUIC connections. Not copyable or movable; it must outlive its accepted
/// connections. Only one accept() may be outstanding at a time.
class listener
{
public:
    /// Binds `local` (a numeric address; IPv6 endpoints are IPv6-only) and starts receiving.
    /// Throws std::invalid_argument if `config` is not a server configuration, std::system_error if binding fails.
    listener(io_context& io, udp::endpoint local, const quic::context& config);
    /// Closes the listener; see close().
    ~listener();
    listener(const listener&) = delete;
    listener& operator=(const listener&) = delete;
    /// Waits for a new connection and completes its handshake. `handshake_timeout` starts when the connection
    /// is taken from the accept queue, not while waiting for a peer. A handshake failure throws, but the listener
    /// stays usable while is_open() is true.
    task<connection> accept(std::chrono::milliseconds handshake_timeout = std::chrono::seconds(10));
    /// The bound address, with the port the system chose for port 0.
    udp::endpoint local_endpoint() const;
    /// The io_context that owns the listener.
    io_context& context() const noexcept;
    /// False after close(), an engine failure, or when the owning context has stopped.
    bool is_open() const noexcept;
    /// Cancels a pending accept() and invalidates every connection accepted on this listener.
    void close() noexcept;

private:
    std::shared_ptr<engine> state_;
};

/// Binds a listener on `io`; same as the listener constructor.
inline listener listen(io_context& io, udp::endpoint local, const quic::context& configuration)
{
    return {io, std::move(local), configuration};
}

/// Binds a listener on the current context. Must be called inside a running io_context.
inline listener listen(udp::endpoint local, const quic::context& configuration)
{
    return listen(this_context(), std::move(local), configuration);
}

/// Creates a client connection on `io` and completes connection::connect(). The task holds a copy of
/// `configuration`, so the caller's object need not outlive it.
inline task<connection> connect(
    io_context& io,
    quic::context configuration,
    udp::endpoint peer,
    std::string expected_name,
    std::chrono::milliseconds timeout = std::chrono::seconds(10)
)
{
    connection result(io, configuration);
    co_await result.connect(std::move(peer), std::move(expected_name), timeout);
    co_return result;
}

/// Same as connect(io_context&, ...) on the current context.
inline task<connection> connect(
    quic::context configuration,
    udp::endpoint peer,
    std::string expected_name,
    std::chrono::milliseconds timeout = std::chrono::seconds(10)
)
{
    co_return co_await connect(
        this_context(),
        std::move(configuration),
        std::move(peer),
        std::move(expected_name),
        timeout
    );
}
} // namespace pace::quic
#endif
