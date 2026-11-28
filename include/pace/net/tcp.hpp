#pragma once

#include <pace/net/buffer.hpp>
#include <pace/net/client_options.hpp>
#include <pace/net/ip.hpp>
#include <pace/net/socket_options.hpp>
#include <pace/net/stream_io.hpp>
#include <pace/runtime/blocking.hpp>
#include <pace/runtime/io_context.hpp>
#include <pace/runtime/platform.hpp>
#include <pace/runtime/task.hpp>
#include <pace/runtime/this_context.hpp>
#include <pace/runtime/unique_function.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace pace::tcp
{
/// A TCP address: numeric, unbracketed IPv4/IPv6 (resolve host names through pace::dns) and a port.
/// Invalid input throws std::invalid_argument when the endpoint is used. Use scope_id for an IPv6 interface
/// index; textual %zone suffixes are rejected.
struct endpoint
{
    /// Numeric IPv4 or IPv6 address.
    std::string address = "127.0.0.1";
    /// Port; 0 on a listener picks a free port.
    std::uint16_t port = 0;
    /// Must match `address` unless unspecified; inspection reports the concrete family.
    address_family family = address_family::unspecified;
    /// IPv6 interface index; must be 0 for IPv4.
    std::uint32_t scope_id = 0;
};

/// Options for listener construction.
struct listen_options
{
    /// SO_REUSEPORT_LB or SO_REUSEPORT; throws std::system_error(operation_not_supported) where neither exists.
    bool reuse_port = false;
    /// Pending-connection queue length; must be positive (std::invalid_argument otherwise).
    int backlog = 1024;
    /// Sets IPV6_V6ONLY on an IPv6 endpoint. Set false for explicit dual-stack; must stay true on IPv4.
    bool ipv6_only = true;
    socket_options socket{};
};

/// Move-only, context-affine TCP socket. The context must outlive the socket.
/// One read and one write may be outstanding concurrently. Buffers must remain
/// valid until their operation completes. close() must run on the owning loop
/// (or before/after run()); it cancels outstanding operations before closing.
/// Move a socket only when it has no outstanding operations.
class stream
{
public:
    /// Creates an unconnected IPv4 socket whose family follows the first connect(). Throws std::system_error.
    explicit stream(io_context& context);
    /// Creates an unconnected socket of a fixed family (`unspecified` behaves like the one-argument form).
    stream(io_context& context, address_family family);
    /// Takes ownership of an idle native IPv4/IPv6 TCP stream on success. On failure the caller owns fd.
    static stream adopt(io_context& context, native_socket_type fd);
    /// Closes the socket.
    ~stream();
    /// Moves the handle; only while idle. The source keeps its context but owns no handle; move assignment
    /// closes the destination's previous socket first.
    stream(stream&& other) noexcept;
    stream& operator=(stream&& other) noexcept;
    stream(const stream&) = delete;
    stream& operator=(const stream&) = delete;

    /// Connects to a numeric endpoint; one attempt per stream (a second throws std::logic_error). Throws
    /// std::system_error on failure, including address_family_not_supported for an explicit-family mismatch.
    task<void> connect(endpoint endpoint, connect_options options = {});
    task<result<void>> connect(as_result_t, endpoint destination, connect_options options = {});
    /// Completes when at least one byte was read and returns the count; 0 with a nonempty buffer means EOF. The
    /// buffer is borrowed until completion. One read may be outstanding (std::logic_error otherwise).
    task<std::size_t> read_some(std::span<std::byte> buffer);
    task<result<std::size_t>> read_some(as_result_t, std::span<std::byte> buffer);
    /// Fills the buffer or throws std::system_error(connection_reset) on premature EOF. A failed read may have
    /// filled a prefix; no bytes beyond buffer.size() are consumed.
    task<void> read_exact(std::span<std::byte> buffer);
    /// Owned convenience result with a pre-allocation bound (1 MiB by default); throws std::length_error
    /// before allocating when `size > maximum`.
    task<std::string> read_exact(std::size_t size, std::size_t maximum = std::size_t{1024} * 1024);
    /// Completes when the OS has accepted every byte of `bytes`, which stay borrowed until then. One write may be
    /// outstanding (std::logic_error otherwise). Text: `write_all(pace::as_bytes(text))`.
    task<void> write_all(std::span<const std::byte> bytes);
    task<result<void>> write_all(as_result_t, std::span<const std::byte> bytes);
    /// Sends FIN (shutdown(SHUT_WR)): the peer reads EOF while this side can still read. Completes at once; throws
    /// std::system_error. Do not overlap a write.
    task<void> shutdown_write();

    /// Sets or clears TCP_NODELAY. Call before run() or on the owning loop; throws std::system_error.
    void set_no_delay(bool enabled = true);
    /// Half-close the send side, then discard peer input until EOF or a bound, and close the socket.
    /// This lets queued output reach peers before unread input can cause a reset. Reaching a bound is normal
    /// completion. Must have no other operations outstanding; errors and cancellation close it and propagate.
    task<void> close_gracefully(
        std::chrono::milliseconds timeout = std::chrono::milliseconds(100),
        std::size_t discard_limit = std::size_t{64} * 1024
    );
    /// Idempotent. Cancels pending waits and closes the handle; keep the object alive until they resume.
    void close() noexcept;

    /// The borrowed native socket; do not close it or change its blocking mode.
    [[nodiscard]] native_socket_type native_handle() const noexcept
    {
        return fd_;
    }

    /// The owning context.
    [[nodiscard]] io_context& context() const noexcept
    {
        return *context_;
    }

    /// Whether the stream owns a handle; not whether the connection is healthy.
    [[nodiscard]] bool is_open() const noexcept
    {
        return fd_ != invalid_socket;
    }

    /// The local address and port of the native socket. Throws std::system_error.
    [[nodiscard]] endpoint local_endpoint() const;
    /// The connected peer's address and port. Throws std::system_error.
    [[nodiscard]] endpoint remote_endpoint() const;
    /// Destination before NAT redirect (Linux netfilter or BSD/macOS PF). Requires a matching state entry.
    /// For a Linux TPROXY socket, local_endpoint() already contains the unmodified destination.
    [[nodiscard]] endpoint original_destination() const;

private:
    template <bool Report>
    task<result_access::value<Report, void>> connect_impl(
        endpoint destination,
        connect_options options,
        std::span<const std::byte> first_write = {}
    );
    friend task<std::string> async_exchange(endpoint, std::string, std::size_t, client_options);
    template <bool Report>
    task<result_access::value<Report, std::size_t>> read_impl(std::span<std::byte> buffer);
    template <bool Report>
    task<result_access::value<Report, void>> write_impl(std::span<const std::byte> bytes);
    friend class listener;
    friend struct splice_access;
    // A short read commonly drains a socket. Wait for level readiness before
    // the next attempt instead of issuing a predictable failing recv syscall.
    bool read_may_be_ready_ = true;
    stream(io_context& context, native_socket_type fd, address_family family) noexcept;
    io_context* context_;
    native_socket_type fd_ = invalid_socket;
    address_family family_ = address_family::ipv4;
    bool automatic_family_ = true;
    bool connect_started_ = false;
    bool no_delay_ = false;
    bool reading_ = false;
    bool writing_ = false;
};

/// Connects a new stream on the running context, under the supplied client budget.
task<stream> connect(endpoint destination, client_options options = {});
task<result<stream>> connect(as_result_t, endpoint destination, client_options options = {});

/// Move-only, context-affine listening socket. The context must outlive it; one accept() may be outstanding.
class listener
{
public:
    /// Creates, binds and listens synchronously. Throws std::invalid_argument for invalid input and
    /// std::system_error for OS failures.
    listener(io_context& context, endpoint endpoint, listen_options options = {});
    /// Takes ownership of an idle native IPv4/IPv6 TCP listener on success. On failure the caller owns fd.
    static listener adopt(io_context& context, native_socket_type fd);
    /// Closes the listener; accepted streams are not affected.
    ~listener();
    /// Moves the handle; only while no accept() is pending. Move assignment closes the previous socket.
    listener(listener&& other) noexcept;
    listener& operator=(listener&& other) noexcept;
    listener(const listener&) = delete;
    listener& operator=(const listener&) = delete;

    /// Waits for one connection and returns it as a stream on the listener's context. Skips aborted connections
    /// and backs off on descriptor exhaustion; other failures throw std::system_error.
    task<stream> accept();
    task<result<stream>> accept(as_result_t);
    /// The bound address and actual port. Throws std::system_error.
    [[nodiscard]] endpoint local_endpoint() const;

    /// The owning context.
    [[nodiscard]] io_context& context() const noexcept
    {
        return *context_;
    }

    /// The borrowed native socket.
    [[nodiscard]] native_socket_type native_handle() const noexcept
    {
        return fd_;
    }

    /// Idempotent. Cancels a pending accept() and closes the handle.
    void close() noexcept;

private:
    template <bool Report>
    task<result_access::value<Report, stream>> accept_impl();
    listener(io_context& context, native_socket_type fd, address_family family) noexcept;
    io_context* context_;
    native_socket_type fd_ = invalid_socket;
    address_family family_ = address_family::ipv4;
    unsigned accept_attempts_ = 0;
    cancellation_source* accept_backoff_ = nullptr;
    bool accepting_ = false;
};

/// Bind immediately. Current-context form is for a running task/callback.
inline listener listen(io_context& io, endpoint local, listen_options options = {})
{
    return {io, std::move(local), options};
}

/// As above, on pace::this_context() at the call; throws std::logic_error when no context is running.
inline listener listen(endpoint local, listen_options options = {})
{
    return listen(this_context(), std::move(local), options);
}

/// Sends one request and reads an exactly framed reply bounded by options.max_reply_size.
task<std::string> async_exchange(
    endpoint peer,
    std::string request,
    std::size_t reply_size,
    client_options options = {}
);
/// Blocking exchange on a fresh context, with a default total timeout of 30 seconds.
std::string exchange(endpoint peer, std::string request, std::size_t reply_size, client_options options = {});
} // namespace pace::tcp
