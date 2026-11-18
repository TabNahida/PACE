#pragma once

#include <pace/io_context.hpp>
#include <pace/ip.hpp>
#include <pace/platform.hpp>
#include <pace/task.hpp>
#include <pace/this_context.hpp>
#include <pace/stream_io.hpp>
#include <pace/blocking.hpp>

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <span>
#include <string>
#include <string_view>

namespace pace::tcp {

// Numeric, unbracketed IPv4/IPv6. Resolve hostnames through pace::dns.
// Use scope_id for an IPv6 interface index; textual %zone suffixes are rejected.
struct endpoint {
    std::string address = "127.0.0.1";
    std::uint16_t port = 0;
    address_family family = address_family::unspecified;
    std::uint32_t scope_id = 0;
};

struct listen_options {
    bool reuse_port = false;
    int backlog = 1024;
    bool ipv6_only = true; // Set false on an IPv6 endpoint for explicit dual-stack.
};

// Move-only, context-affine TCP socket. The context must outlive the socket.
// One read and one write may be outstanding concurrently. Buffers must remain
// valid until their operation completes. close() must run on the owning loop
// (or before/after run()); it cancels outstanding operations before closing.
// Move a socket only when it has no outstanding operations.
class stream {
public:
    explicit stream(io_context& context);
    stream(io_context& context, address_family family);
    ~stream();
    stream(stream&& other) noexcept;
    stream& operator=(stream&& other) noexcept;
    stream(const stream&) = delete;
    stream& operator=(const stream&) = delete;

    task<void> connect(endpoint endpoint);
    task<std::size_t> read_some(std::span<char> buffer);
    // Fill the buffer or throw on premature EOF. A failed read may have filled
    // a prefix; no bytes beyond buffer.size() are consumed.
    task<void> read_exact(std::span<char> buffer);
    task<void> read_exact(std::span<std::byte> buffer) {
        return read_exact(std::span<char>(reinterpret_cast<char*>(buffer.data()), buffer.size()));
    }
    // Owned convenience result with a pre-allocation bound (1 MiB by default).
    task<std::string> read_exact(std::size_t size, std::size_t maximum = 1024 * 1024);
    task<void> write_all(std::string_view bytes);
    task<std::size_t> read_some(std::span<std::byte> buffer) {
        return read_some(std::span<char>(reinterpret_cast<char*>(buffer.data()), buffer.size()));
    }
    task<void> write_all(std::span<const std::byte> bytes) {
        return write_all(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    }
    void set_no_delay(bool enabled = true);
    // Half-close the send side, then discard peer input until EOF or a bound.
    // This lets queued output reach peers before unread input can cause a reset.
    // Must have no other operations outstanding; cancellation also closes it.
    task<void> close_gracefully(std::chrono::milliseconds timeout = std::chrono::milliseconds(100),
                               std::size_t discard_limit = 64 * 1024);
    void close() noexcept;
    [[nodiscard]] native_socket_type native_handle() const noexcept { return fd_; }
    [[nodiscard]] io_context& context() const noexcept { return *context_; }
    [[nodiscard]] bool is_open() const noexcept { return fd_ != invalid_socket; }
    [[nodiscard]] endpoint local_endpoint() const;
    [[nodiscard]] endpoint remote_endpoint() const;

private:
    friend class listener;
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
};

// Numeric endpoint; the returned stream belongs to the supplied/current loop.
task<stream> connect(io_context& context, endpoint destination);
task<stream> connect(endpoint destination);

class listener {
public:
    listener(io_context& context, endpoint endpoint,
                 listen_options options = {});
    ~listener();
    listener(listener&& other) noexcept;
    listener& operator=(listener&& other) noexcept;
    listener(const listener&) = delete;
    listener& operator=(const listener&) = delete;

    task<stream> accept();
    [[nodiscard]] endpoint local_endpoint() const;
    [[nodiscard]] io_context& context() const noexcept { return *context_; }
    [[nodiscard]] native_socket_type native_handle() const noexcept { return fd_; }
    void close() noexcept;

private:
    io_context* context_;
    native_socket_type fd_ = invalid_socket;
    address_family family_ = address_family::ipv4;
};

// Bind immediately. Current-context form is for a running task/callback.
inline listener listen(io_context& io, endpoint local, listen_options options = {}) {
    return listener(io, std::move(local), options);
}
inline listener listen(endpoint local, listen_options options = {}) {
    return listen(this_context(), std::move(local), options);
}
// One connection, one owned request, and an exactly framed reply. No pooling.
inline task<std::string> async_exchange(io_context& io, endpoint peer, std::string request,
        std::size_t reply_size, std::size_t maximum = 1024 * 1024) {
    if (reply_size > maximum) throw std::length_error("TCP reply exceeds maximum");
    auto socket = co_await connect(io, std::move(peer));
    co_await pace::write(socket, std::move(request));
    auto reply = co_await pace::read_exact(socket, reply_size, maximum);
    co_await socket.close_gracefully();
    co_return reply;
}
inline task<std::string> async_exchange(endpoint peer, std::string request,
        std::size_t reply_size, std::size_t maximum = 1024 * 1024) {
    co_return co_await async_exchange(this_context(), std::move(peer), std::move(request), reply_size, maximum);
}
inline std::string exchange(endpoint peer, std::string request, std::size_t reply_size,
        blocking_options options = {}, std::size_t maximum = 1024 * 1024) {
    return detail::blocking_call([&](io_context& io) {
        return async_exchange(io, std::move(peer), std::move(request), reply_size, maximum);
    }, options);
}
} // namespace pace::tcp
