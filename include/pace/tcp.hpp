#pragma once

#include <pace/io_context.hpp>
#include <pace/platform.hpp>
#include <pace/task.hpp>

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <span>
#include <string>
#include <string_view>

namespace pace::tcp {

// Numeric IPv4 addresses only. Name resolution should run outside the I/O loop.
struct endpoint {
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
class stream {
public:
    explicit stream(io_context& context);
    ~stream();
    stream(stream&& other) noexcept;
    stream& operator=(stream&& other) noexcept;
    stream(const stream&) = delete;
    stream& operator=(const stream&) = delete;

    task<void> connect(endpoint endpoint);
    task<std::size_t> read_some(std::span<char> buffer);
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

private:
    friend class listener;
    stream(io_context& context, native_socket_type fd) noexcept;
    io_context* context_;
    native_socket_type fd_ = invalid_socket;
};

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
};

} // namespace pace::tcp
