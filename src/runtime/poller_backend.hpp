#pragma once
#include <pace/detail/poller.hpp>
namespace pace::detail {
struct poller_backend {
    virtual ~poller_backend() = default;
    virtual void update(native_socket_type, bool, bool, std::uint64_t) = 0;
    virtual void remove(native_socket_type) noexcept = 0;
    virtual std::size_t wait(std::span<poll_event>, int) = 0;
    virtual void wake() noexcept = 0;
    virtual bool persistent() const noexcept = 0;
    virtual io_backend backend() const noexcept = 0;
};
std::unique_ptr<poller_backend> make_native_poller(bool persistent);
std::unique_ptr<poller_backend> make_uring_poller();
std::unique_ptr<poller_backend> make_iocp_poller();
}
