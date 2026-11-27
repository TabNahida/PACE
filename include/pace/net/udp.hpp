#pragma once

#include <pace/net/buffer.hpp>
#include <pace/net/ip.hpp>
#include <pace/runtime/blocking.hpp>
#include <pace/runtime/io_context.hpp>
#include <pace/runtime/platform.hpp>
#include <pace/runtime/task.hpp>
#include <pace/runtime/this_context.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace pace::udp
{
/// A UDP address with the same rules as tcp::endpoint: numeric, unbracketed IPv4/IPv6 and a port.
/// Invalid input, including %zone suffixes, throws std::invalid_argument when the endpoint is used.
struct endpoint
{
    /// Numeric IPv4 or IPv6 address.
    std::string address = "127.0.0.1";
    /// Port; 0 when binding picks a free port.
    std::uint16_t port = 0;
    /// Must match `address` unless unspecified; inspection reports the concrete family.
    address_family family = address_family::unspecified;
    /// IPv6 interface index; must be 0 for IPv4.
    std::uint32_t scope_id = 0;
};

/// Options for binding a socket.
struct bind_options
{
    /// Sets IPV6_V6ONLY on an IPv6 endpoint. Set false for explicit dual-stack; must stay true on IPv4.
    bool ipv6_only = true;
};

/// Result of receive_from().
struct datagram
{
    /// Bytes copied; a zero-length datagram is not EOF.
    std::size_t size = 0;
    /// Source address and port.
    endpoint sender;
    /// The datagram was larger than the buffer; its remainder was discarded.
    bool truncated = false;
};

/// Owns the received bytes. An empty data string is a valid datagram, not EOF.
struct packet
{
    /// The received bytes.
    std::string data;
    /// Source address and port.
    endpoint sender;
    /// The datagram was larger than the capacity; its remainder was discarded.
    bool truncated = false;
};

/// Numeric IPv4/IPv6, move-only, context-affine. One send and one receive may be
/// outstanding; a second operation in one direction throws std::logic_error. Keep the object and borrowed
/// buffers alive until completion. Move only while idle. Close on the owning loop (or outside run()), then allow
/// canceled operations to unwind before destroying the object. Context outlives it.
class socket
{
public:
    /// Unbound socket; starts as IPv4 and switches to IPv6 on the first IPv6 bind or send_to.
    explicit socket(io_context& context);
    /// Unbound socket of a fixed family that rejects addresses of the other family.
    socket(io_context& context, address_family family);
    /// Creates a socket and binds it to `local`.
    socket(io_context& context, endpoint local, bind_options options = {});
    /// Closes the socket.
    ~socket();
    /// Moves the handle; only while idle. Move assignment closes the destination's previous socket first.
    socket(socket&& other) noexcept;
    socket& operator=(socket&& other) noexcept;
    socket(const socket&) = delete;
    socket& operator=(const socket&) = delete;

    /// Binds an idle socket (std::logic_error while an operation is outstanding). Synchronous; may run before run().
    void bind(endpoint local, bind_options options = {});
    /// The bound address and port. Throws std::system_error.
    endpoint local_endpoint() const;
    /// Sets or clears SO_BROADCAST. IPv4 only: IPv6 throws std::system_error(operation_not_supported).
    void set_broadcast(bool enabled = true);
    /// Sends one datagram and returns its size; `bytes` stay borrowed until completion. More than 65,507 bytes,
    /// or a partial send, throws std::system_error(message_size).
    task<std::size_t> send_to(std::span<const std::byte> bytes, endpoint destination);
    /// Receives one datagram into `buffer`, which must not be empty (std::invalid_argument).
    task<datagram> receive_from(std::span<std::byte> buffer);

    /// Owned datagrams for small applications. receive() allocates up to 64 KiB;
    /// use receive_from(span) to reuse storage or control larger datagrams.
    /// send() owns `bytes` and otherwise behaves like send_to().
    task<std::size_t> send(std::string bytes, endpoint destination)
    {
        co_return co_await send_to(as_bytes(bytes), std::move(destination));
    }

    /// Receives one datagram into an owned packet of at most `capacity` bytes. Throws std::invalid_argument for 0
    /// and std::length_error above 65536, before any I/O.
    task<packet> receive(std::size_t capacity = 65536)
    {
        if (capacity == 0)
        {
            throw std::invalid_argument("UDP receive capacity must be positive");
        }
        if (capacity > 65536)
        {
            throw std::length_error("UDP receive capacity exceeds 64 KiB");
        }
        packet result;
        result.data.resize(capacity);
        auto received = co_await receive_from(as_writable_bytes(result.data));
        result.data.resize(received.size);
        result.sender = std::move(received.sender);
        result.truncated = received.truncated;
        co_return result;
    }

    /// Idempotent. Cancels pending waits and closes the handle; keep the object alive until they unwind.
    void close() noexcept;

    /// Whether the socket owns a handle.
    bool is_open() const noexcept
    {
        return fd_ != invalid_socket;
    }

    /// The borrowed native socket; do not close it or change its blocking mode.
    native_socket_type native_handle() const noexcept
    {
        return fd_;
    }

    /// The owning context.
    io_context& context() const noexcept
    {
        return *context_;
    }

private:
    void select_family(address_family family);
    io_context* context_;
    native_socket_type fd_ = invalid_socket;
    bool sending_ = false, receiving_ = false;
    address_family family_ = address_family::ipv4;
    bool automatic_family_ = true, family_selected_ = false, ipv6_only_ = true;
};

/// An unbound socket whose family is chosen on first use.
inline socket open(io_context& io)
{
    return socket(io);
}

/// As above, on pace::this_context() at the call; throws std::logic_error when no context is running.
inline socket open()
{
    return open(this_context());
}

/// A socket bound to `local`; port 0 picks a free port.
inline socket bind(io_context& io, endpoint local, bind_options options = {})
{
    return {io, std::move(local), options};
}

/// As above, on pace::this_context() at the call; throws std::logic_error when no context is running.
inline socket bind(endpoint local, bind_options options = {})
{
    return bind(this_context(), std::move(local), options);
}

/// One datagram request, then the first reply from the same numeric endpoint.
/// Unrelated senders are ignored; this is filtering, not authentication/reliability. `capacity` must be
/// 1..65536 and the peer port nonzero. Adds no deadline: without one it waits for a reply forever.
task<packet> async_exchange(io_context&, endpoint peer, std::string request, std::size_t capacity = 65536);

/// As above, on the context that runs the task.
inline task<packet> async_exchange(endpoint peer, std::string request, std::size_t capacity = 65536)
{
    co_return co_await async_exchange(this_context(), std::move(peer), std::move(request), capacity);
}

/// Blocking async_exchange on a fresh context, bounded by `options` (30 s by default); expiry throws
/// std::system_error(timed_out). Throws std::logic_error when called inside a PACE context.
inline packet exchange(endpoint peer, std::string request, blocking_options options = {}, std::size_t capacity = 65536)
{
    return blocking_call(
        [&](io_context& io) { return async_exchange(io, std::move(peer), std::move(request), capacity); },
        std::move(options)
    );
}

/// Blocking: sends one datagram from a fresh socket on a fresh context and returns the bytes the local socket
/// accepted. Does not prove delivery. `options` behave as for exchange().
std::size_t send(endpoint peer, std::string message, blocking_options options = {});
} // namespace pace::udp
