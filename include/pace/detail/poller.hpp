#pragma once

#include <pace/platform.hpp>
#include <pace/io_backend.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace pace::detail {

struct poll_event {
    std::uint64_t token;
    bool readable;
    bool writable;
    bool error;
};

// All operations except wake() belong to the owning reactor thread.
// In the default one-shot mode, delivery disarms the entire descriptor,
// including its other direction. Persistent mode retains explicit interests.
// UINT64_MAX is reserved for the internal wakeup notification.
class poller {
public:
#if defined(__linux__)
    static constexpr bool supports_persistent = true;
#else
    static constexpr bool supports_persistent = false;
#endif
    // Persistent level-triggered subscriptions are an internal Linux reactor
    // mode. The default retains the one-shot contract for standalone callers.
    explicit poller(bool persistent = false, io_backend selected = io_backend::automatic);
    bool persistent() const noexcept;
    io_backend backend() const noexcept;
    ~poller();
    poller(const poller&) = delete;
    poller& operator=(const poller&) = delete;

    void update(native_socket_type fd, bool readable, bool writable, std::uint64_t token);
    void remove(native_socket_type fd) noexcept;
    void forget(native_socket_type fd) noexcept; // Owner is about to close the native socket.
    std::size_t wait(std::span<poll_event> events, int timeout_ms);
    void wake() noexcept;

private:
    struct implementation;
    std::unique_ptr<implementation> impl_;
};

} // namespace pace::detail
