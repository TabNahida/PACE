#include "poller_backend.hpp"
#include <system_error>
#if defined(__linux__) && defined(PACE_HAS_IO_URING)
#include <liburing.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <poll.h>
#include <unordered_map>
#include <limits>
#include <array>

namespace pace::detail {
namespace {
constexpr auto wake_id = std::numeric_limits<std::uint64_t>::max();
void checked(int result, const char* op) {
    if (result < 0) throw std::system_error(-result, std::generic_category(), op);
}
class uring_poller final : public poller_backend {
    io_uring ring_{};
    int notification_ = -1;
    std::uint64_t next_ = 0;
    struct subscription { int fd; std::uint64_t token; };
    std::unordered_map<std::uint64_t, subscription> pending_;
    std::unordered_map<int, std::uint64_t> current_;
    void submit() {
        int result;
        do { result = io_uring_submit(&ring_); } while (result == -EINTR);
        checked(result, "io_uring_submit");
    }
    io_uring_sqe* entry() {
        auto* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) { submit(); sqe = io_uring_get_sqe(&ring_); }
        if (!sqe) throw std::system_error(std::make_error_code(std::errc::no_buffer_space), "io_uring SQ full");
        return sqe;
    }
    void arm_wake() {
        auto* sqe = entry(); io_uring_prep_poll_add(sqe, notification_, POLLIN);
        io_uring_sqe_set_data64(sqe, wake_id);
    }
public:
    uring_poller() {
        // A submission batch is not a connection limit. Flush full batches in
        // entry(); keep per-worker locked memory small even with many reactors.
        io_uring_params params{};
        checked(io_uring_queue_init_params(256, &ring_, &params), "io_uring_queue_init");
        try {
            if (!(params.features & IORING_FEAT_NODROP))
                throw std::system_error(std::make_error_code(std::errc::not_supported), "io_uring requires NODROP");
            notification_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            if (notification_ < 0) throw std::system_error(errno, std::generic_category(), "io_uring eventfd");
            arm_wake(); submit();
        } catch (...) { if (notification_ >= 0) ::close(notification_); io_uring_queue_exit(&ring_); throw; }
    }
    ~uring_poller() override { io_uring_queue_exit(&ring_); ::close(notification_); }
    bool persistent() const noexcept override { return false; }
    io_backend backend() const noexcept override { return io_backend::io_uring; }
    void update(native_socket_type fd, bool read, bool write, std::uint64_t token) override {
        if (token == wake_id) throw std::invalid_argument("reserved poll token");
        remove(fd);
        if (!read && !write) return;
        if (next_ >= wake_id - 1) throw std::overflow_error("io_uring subscription identity exhausted");
        const auto id = ++next_;
        pending_.emplace(id, subscription{fd, token});
        try { current_.emplace(fd, id); }
        catch (...) { pending_.erase(id); throw; }
        io_uring_sqe* sqe;
        try { sqe = entry(); } catch (...) { current_.erase(fd); pending_.erase(id); throw; }
        io_uring_prep_poll_add(sqe, fd, static_cast<unsigned>((read ? POLLIN : 0) | (write ? POLLOUT : 0) | POLLRDHUP));
        io_uring_sqe_set_data64(sqe, id);
    }
    void remove(native_socket_type fd) noexcept override {
        const auto it = current_.find(fd);
        if (it == current_.end()) return;
        const auto id = it->second; current_.erase(it);
        // CQEs carry identities, never pointers into a destroyed awaiter. Late
        // poll/cancel completions are discarded even after descriptor reuse.
        try {
            auto* sqe = entry(); io_uring_prep_poll_remove(sqe, id); io_uring_sqe_set_data64(sqe, 0);
        } catch (...) { /* A closed descriptor or queue destruction also ends the poll. */ }
    }
    std::size_t wait(std::span<poll_event> output, int timeout) override {
        if (output.empty()) return 0;
        submit();
        io_uring_cqe* first = nullptr;
        auto result = io_uring_peek_cqe(&ring_, &first);
        if (result == -EAGAIN && timeout != 0) {
            __kernel_timespec duration{};
            duration.tv_sec = timeout / 1000; duration.tv_nsec = (timeout % 1000) * 1000000LL;
            result = io_uring_wait_cqe_timeout(&ring_, &first, timeout < 0 ? nullptr : &duration);
        }
        if (result == -EINTR || result == -ETIME || result == -EAGAIN) return 0;
        checked(result, "io_uring_wait_cqe");
        std::size_t count = 0;
        for (std::size_t processed = 0; processed < 4096 && count < output.size(); ++processed) {
            io_uring_cqe* cqe = nullptr;
            if (io_uring_peek_cqe(&ring_, &cqe) < 0) break;
            const auto id = io_uring_cqe_get_data64(cqe); const int events = cqe->res;
            io_uring_cqe_seen(&ring_, cqe);
            if (id == wake_id) {
                std::uint64_t value;
                while (::read(notification_, &value, sizeof(value)) < 0 && errno == EINTR) {}
                arm_wake();
            } else if (id) {
                const auto pending = pending_.find(id);
                if (pending == pending_.end()) continue;
                const auto info = pending->second; pending_.erase(pending);
                const auto active = current_.find(info.fd);
                if (active == current_.end() || active->second != id) continue;
                current_.erase(active);
                output[count++] = {info.token, bool(events & (POLLIN | POLLRDHUP | POLLHUP)),
                    bool(events & POLLOUT), events < 0 || bool(events & (POLLERR | POLLHUP | POLLNVAL))};
            }
        }
        return count;
    }
    void wake() noexcept override {
        const std::uint64_t value = 1;
        while (::write(notification_, &value, sizeof(value)) < 0 && errno == EINTR) {}
    }
};
}
std::unique_ptr<poller_backend> make_uring_poller() { return std::make_unique<uring_poller>(); }
}
#else
namespace pace::detail {
std::unique_ptr<poller_backend> make_uring_poller() {
    throw std::system_error(std::make_error_code(std::errc::not_supported), "io_uring requires a Linux build with --uring=y");
}
}
#endif
