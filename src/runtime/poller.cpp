#include "poller_backend.hpp"
#include <cstdlib>
#include <string_view>
#include <system_error>

namespace pace::detail {
struct poller::implementation { std::unique_ptr<poller_backend> backend; };
poller::poller(bool persistent, io_backend selected) : impl_(std::make_unique<implementation>()) {
    if (selected == io_backend::automatic) {
        if (const char* value = std::getenv("PACE_IO_BACKEND")) {
            const std::string_view name(value);
            bool found = false;
            for (auto candidate : {io_backend::automatic, io_backend::epoll, io_backend::kqueue,
                                   io_backend::wsapoll, io_backend::io_uring, io_backend::iocp})
                if (name == backend_name(candidate)) { selected = candidate; found = true; break; }
            if (!found) throw std::invalid_argument("invalid PACE_IO_BACKEND");
        }
    }
    if (selected == io_backend::io_uring) { impl_->backend = make_uring_poller(); return; }
    if (selected == io_backend::iocp) { impl_->backend = make_iocp_poller(); return; }
#if defined(_WIN32)
    if (selected == io_backend::automatic) {
        try { impl_->backend = make_iocp_poller(); return; }
        catch (const std::system_error& e) { if (e.code() != std::errc::not_supported) throw; }
    }
    const auto native = io_backend::wsapoll;
    persistent = false;
#elif defined(__linux__)
    const auto native = io_backend::epoll;
#else
    const auto native = io_backend::kqueue;
    persistent = false;
#endif
    if (selected != io_backend::automatic && selected != native)
        throw std::system_error(std::make_error_code(std::errc::not_supported), "requested I/O backend");
    impl_->backend = make_native_poller(persistent);
}
poller::~poller() = default;
void poller::update(native_socket_type fd, bool read, bool write, std::uint64_t token) { impl_->backend->update(fd, read, write, token); }
void poller::remove(native_socket_type fd) noexcept { impl_->backend->remove(fd); }
void poller::forget(native_socket_type fd) noexcept { impl_->backend->forget(fd); }
std::size_t poller::wait(std::span<poll_event> output, int timeout) { return impl_->backend->wait(output, timeout); }
void poller::wake() noexcept { impl_->backend->wake(); }
bool poller::persistent() const noexcept { return impl_->backend->persistent(); }
io_backend poller::backend() const noexcept { return impl_->backend->backend(); }
}
