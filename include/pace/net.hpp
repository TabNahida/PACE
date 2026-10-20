#pragma once

#include <pace/io_context.hpp>
#include <pace/platform.hpp>
#include <pace/task.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace pace {

// Numeric IPv4 addresses only. Name resolution should run outside the I/O loop.
struct ipv4_endpoint {
    std::string address = "127.0.0.1";
    std::uint16_t port = 0;
};

struct listen_options {
    bool reuse_port = false;
    int backlog = 1024;
};

// Move-only, context-affine TCP socket. The context must outlive the socket.
// One read and one write may be outstanding concurrently. Buffers must remain
// valid until their operation completes. close() must run on the owning loop
// (or before/after run()); it cancels outstanding operations before closing.
// Move a socket only when it has no outstanding operations.
class tcp_stream {
public:
    explicit tcp_stream(io_context& context);
    ~tcp_stream();
    tcp_stream(tcp_stream&& other) noexcept;
    tcp_stream& operator=(tcp_stream&& other) noexcept;
    tcp_stream(const tcp_stream&) = delete;
    tcp_stream& operator=(const tcp_stream&) = delete;

    task<void> connect(ipv4_endpoint endpoint);
    task<std::size_t> read_some(std::span<char> buffer);
    task<void> write_all(std::string_view bytes);
    task<std::size_t> read_some(std::span<std::byte> buffer) {
        return read_some(std::span<char>(reinterpret_cast<char*>(buffer.data()), buffer.size()));
    }
    task<void> write_all(std::span<const std::byte> bytes) {
        return write_all(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    }
    void set_no_delay(bool enabled = true);
    void close() noexcept;
    [[nodiscard]] native_socket_type native_handle() const noexcept { return fd_; }
    [[nodiscard]] io_context& context() const noexcept { return *context_; }
    [[nodiscard]] bool is_open() const noexcept { return fd_ != invalid_socket; }

private:
    friend class tcp_listener;
    tcp_stream(io_context& context, native_socket_type fd) noexcept;
    io_context* context_;
    native_socket_type fd_ = invalid_socket;
};

class tcp_listener {
public:
    tcp_listener(io_context& context, ipv4_endpoint endpoint,
                 listen_options options = {});
    ~tcp_listener();
    tcp_listener(tcp_listener&& other) noexcept;
    tcp_listener& operator=(tcp_listener&& other) noexcept;
    tcp_listener(const tcp_listener&) = delete;
    tcp_listener& operator=(const tcp_listener&) = delete;

    task<tcp_stream> accept();
    [[nodiscard]] ipv4_endpoint local_endpoint() const;
    [[nodiscard]] io_context& context() const noexcept { return *context_; }
    [[nodiscard]] native_socket_type native_handle() const noexcept { return fd_; }
    void close() noexcept;

private:
    io_context* context_;
    native_socket_type fd_ = invalid_socket;
};

} // namespace pace
