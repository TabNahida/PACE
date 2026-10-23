#pragma once

#include <pace/io_context.hpp>
#include <pace/ip.hpp>
#include <pace/platform.hpp>
#include <pace/task.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace pace::udp {

struct endpoint {
    std::string address = "127.0.0.1";
    std::uint16_t port = 0;
    address_family family = address_family::unspecified;
    std::uint32_t scope_id = 0;
};

struct bind_options {
    bool ipv6_only = true; // Set false on an IPv6 endpoint for explicit dual-stack.
};

struct datagram {
    std::size_t size = 0; // Bytes copied; a zero-length datagram is not EOF.
    endpoint sender;
    bool truncated = false; // The remainder of this datagram was discarded.
};

// Numeric IPv4/IPv6, move-only, context-affine. One send and one receive may be
// outstanding. Keep the object and borrowed buffers alive until completion.
// Move only while idle. Close on the owning loop (or outside run()), then allow
// canceled operations to unwind before destroying the object. Context outlives it.
class socket {
public:
    explicit socket(io_context& context);
    socket(io_context& context, address_family family);
    socket(io_context& context, endpoint local, bind_options options = {});
    ~socket();
    socket(socket&& other) noexcept;
    socket& operator=(socket&& other) noexcept;
    socket(const socket&) = delete;
    socket& operator=(const socket&) = delete;

    void bind(endpoint local, bind_options options = {});
    endpoint local_endpoint() const;
    void set_broadcast(bool enabled = true);
    task<std::size_t> send_to(std::string_view bytes, endpoint destination);
    task<datagram> receive_from(std::span<char> buffer);
    task<std::size_t> send_to(std::span<const std::byte> bytes, endpoint destination) {
        return send_to(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
                       std::move(destination));
    }
    task<datagram> receive_from(std::span<std::byte> buffer) {
        return receive_from(std::span<char>(reinterpret_cast<char*>(buffer.data()), buffer.size()));
    }
    void close() noexcept;
    bool is_open() const noexcept { return fd_ != invalid_socket; }
    native_socket_type native_handle() const noexcept { return fd_; }
    io_context& context() const noexcept { return *context_; }
private:
    void select_family(address_family family);
    io_context* context_;
    native_socket_type fd_ = invalid_socket;
    bool sending_ = false, receiving_ = false;
    address_family family_ = address_family::ipv4;
    bool automatic_family_ = true, family_selected_ = false, ipv6_only_ = true;
};

} // namespace pace::udp
