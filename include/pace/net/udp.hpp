#pragma once

#include <pace/net/buffer.hpp>
#include <pace/net/client_options.hpp>
#include <pace/net/ip.hpp>
#include <pace/runtime/blocking.hpp>
#include <pace/runtime/io_context.hpp>
#include <pace/runtime/platform.hpp>
#include <pace/runtime/task.hpp>
#include <pace/runtime/this_context.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
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
    /// Enable destination address and ECN reception before binding.
    bool packet_info = false;
    /// Enable Linux transparent bind and original-destination control messages.
    bool transparent = false;
    /// Enable Linux GRO or Windows URO. Read via the packet_info overload to obtain segment_size.
    bool coalesce = false;
};

/// Per-datagram control information. A missing address, zero interface or absent ECN lets the OS choose on send.
struct packet_info
{
    std::string local_address;
    unsigned interface_index = 0;
    std::optional<unsigned> ecn{};
    /// GSO/USO segment size on send, GRO/URO segment size on receive (zero for an ordinary datagram).
    std::uint16_t segment_size = 0;
    /// Original destination supplied by Linux transparent reception, including its port.
    std::optional<endpoint> original_destination{};
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

/// One borrowed payload and its destination for send_batch().
struct outgoing_datagram
{
    std::span<const std::byte> bytes;
    endpoint destination;
};

/// One caller-owned receive buffer and its result for receive_batch().
struct incoming_datagram
{
    std::span<std::byte> buffer;
    datagram result{};
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
    /// Takes ownership of an idle IPv4/IPv6 UDP socket on success. On failure the caller owns fd.
    static socket adopt(io_context& context, native_socket_type fd);
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
    task<result<std::size_t>> send_to(as_result_t, std::span<const std::byte> bytes, endpoint destination);
    /// Receives one datagram into `buffer`, which must not be empty (std::invalid_argument).
    task<datagram> receive_from(std::span<std::byte> buffer);
    task<result<datagram>> receive_from(as_result_t, std::span<std::byte> buffer);

    /// Send with a source address, interface and/or ECN codepoint (0..3).
    task<std::size_t> send_to(std::span<const std::byte> bytes, endpoint destination, packet_info info);
    task<result<std::size_t>> send_to(
        as_result_t,
        std::span<const std::byte> bytes,
        endpoint destination,
        packet_info info
    );
    /// Receives the available ancillary information; enable bind_options::packet_info before packets arrive.
    task<datagram> receive_from(std::span<std::byte> buffer, packet_info& info);
    task<result<datagram>> receive_from(as_result_t, std::span<std::byte> buffer, packet_info& info);

    /// Sends a prefix of up to 32 messages of one address family. Returns after the first successful batch.
    task<std::size_t> send_batch(std::span<const outgoing_datagram> messages);
    /// Waits for one datagram and drains up to 32 into the supplied slots. Only the returned prefix is valid.
    task<std::size_t> receive_batch(std::span<incoming_datagram> messages);

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
    socket(io_context& context, native_socket_type fd, address_family family, bool ipv6_only) noexcept;
    template <bool Report>
    task<result_access::value<Report, std::size_t>> send_impl(
        std::span<const std::byte> bytes,
        endpoint destination,
        const packet_info* info
    );
    template <bool Report>
    task<result_access::value<Report, datagram>> receive_impl(std::span<std::byte> buffer, packet_info* info);
    void select_family(address_family family);
    io_context* context_;
    native_socket_type fd_ = invalid_socket;
    bool sending_ = false, receiving_ = false;
    bool coalescing_ = false;
    address_family family_ = address_family::ipv4;
    bool automatic_family_ = true, family_selected_ = false, ipv6_only_ = true;
};

/// Opens immediately on options.context, or on the running context if omitted.
inline socket open(const client_options& options = {})
{
    return socket(options.context ? *options.context : this_context());
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

/// Sends one datagram and accepts the first reply from the same numeric endpoint.
/// max_reply_size is the receive capacity (1..65536); oversized replies report truncation.
task<packet> async_exchange(endpoint peer, std::string request, client_options options = {});
packet exchange(endpoint peer, std::string request, client_options options = {});
/// Blocking send on a fresh socket. Reports the number of bytes accepted locally.
std::size_t send(endpoint peer, std::string message, client_options options = {});
} // namespace pace::udp
