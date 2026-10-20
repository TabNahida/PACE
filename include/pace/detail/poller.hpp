#pragma once

#include <pace/platform.hpp>
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
// Delivery disarms the entire descriptor, including its other direction.
// UINT64_MAX is reserved for the internal wakeup notification.
class poller {
public:
    poller();
    ~poller();
    poller(const poller&) = delete;
    poller& operator=(const poller&) = delete;

    void update(native_socket_type fd, bool readable, bool writable, std::uint64_t token);
    void remove(native_socket_type fd) noexcept;
    std::size_t wait(std::span<poll_event> events, int timeout_ms);
    void wake() noexcept;

private:
    struct implementation;
    std::unique_ptr<implementation> impl_;
};

} // namespace pace::detail
