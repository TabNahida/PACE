#include <pace/detail/poller.hpp>
#include <pace/detail/profiling.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <stdexcept>

#if defined(__linux__)
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include <fcntl.h>
#include <sys/event.h>
#include <unistd.h>
#include <unordered_map>
#elif defined(_WIN32)
#include <unordered_map>
#include <vector>
#else
#error "PACE supports Linux, macOS, Windows, and FreeBSD"
#endif

namespace pace::detail {
namespace {
constexpr auto wake_token = std::numeric_limits<std::uint64_t>::max();
constexpr std::size_t max_batch = 256;

[[noreturn]] void fail(const char* operation) {
#ifdef _WIN32
    throw std::system_error(::WSAGetLastError(), std::system_category(), operation);
#else
    throw std::system_error(errno, std::generic_category(), operation);
#endif
}
} // namespace

#if defined(__linux__)

struct poller::implementation {
    int queue = -1;
    int notification = -1;
    bool persistent = false;

    implementation() {
        queue = ::epoll_create1(EPOLL_CLOEXEC);
        if (queue < 0) fail("epoll_create1");
        notification = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (notification < 0) {
            const int error = errno;
            ::close(queue);
            throw std::system_error(error, std::generic_category(), "eventfd");
        }
        epoll_event event{};
        event.events = EPOLLIN;
        event.data.u64 = wake_token;
        if (::epoll_ctl(queue, EPOLL_CTL_ADD, notification, &event) < 0) {
            const int error = errno;
            ::close(notification);
            ::close(queue);
            throw std::system_error(error, std::generic_category(), "epoll_ctl wakeup");
        }
    }
    ~implementation() { ::close(notification); ::close(queue); }
};

void poller::update(native_socket_type fd, bool readable, bool writable, std::uint64_t token) {
    profiling::scope timing(profiling::stage::poller_update);
    if (token == wake_token) throw std::invalid_argument("poller token UINT64_MAX is reserved");
    if (!readable && !writable) { remove(fd); return; }
    epoll_event event{};
    event.events = (impl_->persistent ? 0U : EPOLLONESHOT) | EPOLLRDHUP;
    if (readable) event.events |= EPOLLIN;
    if (writable) event.events |= EPOLLOUT;
    event.data.u64 = token;
    if (::epoll_ctl(impl_->queue, EPOLL_CTL_MOD, fd, &event) == 0) return;
    if (errno != ENOENT) fail("epoll_ctl modify");
    if (::epoll_ctl(impl_->queue, EPOLL_CTL_ADD, fd, &event) < 0) fail("epoll_ctl add");
}

void poller::remove(native_socket_type fd) noexcept {
    ::epoll_ctl(impl_->queue, EPOLL_CTL_DEL, fd, nullptr);
}

std::size_t poller::wait(std::span<poll_event> output, int timeout_ms) {
    if (output.empty()) return 0;
    // epoll_wait initializes every returned entry; only [0, count) is read.
    std::array<epoll_event, max_batch> events;
    const int count = ::epoll_wait(impl_->queue, events.data(),
        static_cast<int>(std::min(output.size(), events.size())), timeout_ms);
    if (count < 0) {
        if (errno == EINTR) return 0;
        fail("epoll_wait");
    }
    std::size_t size = 0;
    for (int i = 0; i < count; ++i) {
        const auto& event = events[static_cast<std::size_t>(i)];
        if (event.data.u64 == wake_token) {
            std::uint64_t value;
            while (::read(impl_->notification, &value, sizeof(value)) < 0 && errno == EINTR) {}
            continue;
        }
        output[size++] = {event.data.u64,
            bool(event.events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)),
            bool(event.events & EPOLLOUT),
            bool(event.events & (EPOLLERR | EPOLLHUP))};
    }
    return size;
}

void poller::wake() noexcept {
    const std::uint64_t value = 1;
    while (::write(impl_->notification, &value, sizeof(value)) < 0 && errno == EINTR) {}
}

#elif defined(__APPLE__) || defined(__FreeBSD__)

struct poller::implementation {
    int queue = -1;
    std::unordered_map<native_socket_type, unsigned> interests;

    implementation() {
        queue = ::kqueue();
        if (queue < 0) fail("kqueue");
        if (::fcntl(queue, F_SETFD, FD_CLOEXEC) < 0) {
            const int error = errno;
            ::close(queue);
            throw std::system_error(error, std::generic_category(), "fcntl kqueue");
        }
        struct kevent event{};
        EV_SET(&event, 0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
        if (::kevent(queue, &event, 1, nullptr, 0, nullptr) < 0) {
            const int error = errno;
            ::close(queue);
            throw std::system_error(error, std::generic_category(), "kevent wakeup");
        }
    }
    ~implementation() { ::close(queue); }

    void erase(native_socket_type fd, short filter) noexcept {
        struct kevent change{};
        EV_SET(&change, static_cast<std::uintptr_t>(fd), filter, EV_DELETE, 0, 0, nullptr);
        while (::kevent(queue, &change, 1, nullptr, 0, nullptr) < 0 && errno == EINTR) {}
    }
};

void poller::update(native_socket_type fd, bool readable, bool writable, std::uint64_t token) {
    profiling::scope timing(profiling::stage::poller_update);
    static_assert(sizeof(std::uintptr_t) >= sizeof(token), "PACE kqueue requires a 64-bit target");
    if (token == wake_token) throw std::invalid_argument("poller token UINT64_MAX is reserved");
    if (!readable && !writable) { remove(fd); return; }
    auto [entry, inserted] = impl_->interests.try_emplace(fd, 0);
    (void)inserted;
    const auto previous = entry->second;
    std::array<struct kevent, 2> changes{};
    int count = 0;
    auto* data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(token));
    if (readable) {
        EV_SET(&changes[static_cast<std::size_t>(count)], static_cast<std::uintptr_t>(fd),
            EVFILT_READ, EV_ADD | EV_ENABLE | EV_ONESHOT, 0, 0, data);
        ++count;
    } else if (previous & 1U) impl_->erase(fd, EVFILT_READ);
    if (writable) {
        EV_SET(&changes[static_cast<std::size_t>(count)], static_cast<std::uintptr_t>(fd),
            EVFILT_WRITE, EV_ADD | EV_ENABLE | EV_ONESHOT, 0, 0, data);
        ++count;
    } else if (previous & 2U) impl_->erase(fd, EVFILT_WRITE);
    entry->second = (readable ? 1U : 0U) | (writable ? 2U : 0U);
    int result;
    do { result = ::kevent(impl_->queue, changes.data(), count, nullptr, 0, nullptr); }
    while (result < 0 && errno == EINTR);
    if (result < 0) {
        const int error = errno;
        remove(fd);
        throw std::system_error(error, std::generic_category(), "kevent update");
    }
}

void poller::remove(native_socket_type fd) noexcept {
    const auto entry = impl_->interests.find(fd);
    if (entry == impl_->interests.end()) return;
    if (entry->second & 1U) impl_->erase(fd, EVFILT_READ);
    if (entry->second & 2U) impl_->erase(fd, EVFILT_WRITE);
    impl_->interests.erase(entry);
}

std::size_t poller::wait(std::span<poll_event> output, int timeout_ms) {
    if (output.empty()) return 0;
    // kevent initializes every returned entry; unused capacity is never read.
    std::array<struct kevent, max_batch> events;
    timespec timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_nsec = (timeout_ms % 1000) * 1000000L;
    const int count = ::kevent(impl_->queue, nullptr, 0, events.data(),
        static_cast<int>(std::min(output.size(), events.size())), timeout_ms < 0 ? nullptr : &timeout);
    if (count < 0) {
        if (errno == EINTR) return 0;
        fail("kevent wait");
    }
    std::array<native_socket_type, max_batch> descriptors;
    std::size_t size = 0;
    for (int i = 0; i < count; ++i) {
        const auto& event = events[static_cast<std::size_t>(i)];
        if (event.filter == EVFILT_USER) continue;
        const auto fd = static_cast<native_socket_type>(event.ident);
        std::size_t index = 0;
        while (index < size && descriptors[index] != fd) ++index;
        if (index == size) {
            descriptors[size] = fd;
            output[size++] = {static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(event.udata)),
                false, false, false};
        }
        auto& result = output[index];
        result.readable |= event.filter == EVFILT_READ;
        result.writable |= event.filter == EVFILT_WRITE;
        result.error |= bool(event.flags & EV_ERROR) || ((event.flags & EV_EOF) && event.fflags != 0);
    }
    // EV_ONESHOT removed delivered filters already. Disarm only their counterpart.
    for (std::size_t i = 0; i < size; ++i) {
        const auto entry = impl_->interests.find(descriptors[i]);
        if (entry == impl_->interests.end()) continue;
        if ((entry->second & 1U) && !output[i].readable) impl_->erase(descriptors[i], EVFILT_READ);
        if ((entry->second & 2U) && !output[i].writable) impl_->erase(descriptors[i], EVFILT_WRITE);
        entry->second = 0;
    }
    return size;
}

void poller::wake() noexcept {
    struct kevent event{};
    EV_SET(&event, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    while (::kevent(impl_->queue, &event, 1, nullptr, 0, nullptr) < 0 && errno == EINTR) {}
}

#elif defined(_WIN32)

struct poller::implementation {
    SOCKET notification = INVALID_SOCKET;
    SOCKET notifier = INVALID_SOCKET;
    std::vector<WSAPOLLFD> descriptors;
    std::vector<std::uint64_t> tokens;
    std::unordered_map<native_socket_type, std::size_t> indices;

    implementation() {
        initialize_sockets();
        try {
            notification = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (notification == INVALID_SOCKET) fail("socket wakeup receiver");
            notifier = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (notifier == INVALID_SOCKET) fail("socket wakeup sender");
            sockaddr_in endpoint{};
            endpoint.sin_family = AF_INET;
            endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (::bind(notification, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) == SOCKET_ERROR)
                fail("bind wakeup");
            int length = sizeof(endpoint);
            if (::getsockname(notification, reinterpret_cast<sockaddr*>(&endpoint), &length) == SOCKET_ERROR)
                fail("getsockname wakeup");
            if (::connect(notifier, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) == SOCKET_ERROR)
                fail("connect wakeup");
            u_long nonblocking = 1;
            if (::ioctlsocket(notification, FIONBIO, &nonblocking) == SOCKET_ERROR ||
                ::ioctlsocket(notifier, FIONBIO, &nonblocking) == SOCKET_ERROR) fail("ioctlsocket wakeup");
            descriptors.push_back({notification, POLLRDNORM, 0});
            tokens.push_back(wake_token);
        } catch (...) {
            if (notification != INVALID_SOCKET) ::closesocket(notification);
            if (notifier != INVALID_SOCKET) ::closesocket(notifier);
            throw;
        }
    }
    ~implementation() { ::closesocket(notifier); ::closesocket(notification); }

    void erase(std::size_t index) noexcept {
        indices.erase(static_cast<native_socket_type>(descriptors[index].fd));
        if (index + 1 != descriptors.size()) {
            descriptors[index] = descriptors.back();
            tokens[index] = tokens.back();
            indices.find(static_cast<native_socket_type>(descriptors[index].fd))->second = index;
        }
        descriptors.pop_back();
        tokens.pop_back();
    }
};

void poller::update(native_socket_type fd, bool readable, bool writable, std::uint64_t token) {
    profiling::scope timing(profiling::stage::poller_update);
    if (token == wake_token) throw std::invalid_argument("poller token UINT64_MAX is reserved");
    if (!readable && !writable) { remove(fd); return; }
    short events = 0;
    if (readable) events |= POLLRDNORM;
    if (writable) events |= POLLWRNORM;
    if (const auto entry = impl_->indices.find(fd); entry != impl_->indices.end()) {
        impl_->descriptors[entry->second].events = events;
        impl_->tokens[entry->second] = token;
        return;
    }
    const auto index = impl_->descriptors.size();
    impl_->descriptors.push_back({static_cast<SOCKET>(fd), events, 0});
    try {
        impl_->tokens.push_back(token);
        try { impl_->indices.emplace(fd, index); }
        catch (...) { impl_->tokens.pop_back(); throw; }
    } catch (...) { impl_->descriptors.pop_back(); throw; }
}

void poller::remove(native_socket_type fd) noexcept {
    const auto entry = impl_->indices.find(fd);
    if (entry != impl_->indices.end()) impl_->erase(entry->second);
}

std::size_t poller::wait(std::span<poll_event> output, int timeout_ms) {
    if (output.empty()) return 0;
    const int count = ::WSAPoll(impl_->descriptors.data(), static_cast<ULONG>(impl_->descriptors.size()), timeout_ms);
    if (count == SOCKET_ERROR) {
        if (::WSAGetLastError() == WSAEINTR) return 0;
        fail("WSAPoll");
    }
    if (impl_->descriptors[0].revents) {
        std::array<char, 256> bytes{};
        // Bound draining so a producer flood cannot starve coroutine execution.
        for (std::size_t i = 0; i < max_batch; ++i)
            if (::recv(impl_->notification, bytes.data(), static_cast<int>(bytes.size()), 0) <= 0) break;
    }
    std::size_t size = 0;
    std::size_t index = 1;
    while (index < impl_->descriptors.size() && size < output.size()) {
        const auto events = impl_->descriptors[index].revents;
        if (!events) { ++index; continue; }
        output[size++] = {impl_->tokens[index],
            bool(events & (POLLRDNORM | POLLRDBAND | POLLHUP)),
            bool(events & POLLWRNORM), bool(events & (POLLERR | POLLHUP | POLLNVAL))};
        impl_->erase(index);
    }
    return size;
}

void poller::wake() noexcept {
    const char byte = 1;
    ::send(impl_->notifier, &byte, 1, 0);
}

#endif

poller::poller(bool persistent) : impl_(std::make_unique<implementation>()) {
#if defined(__linux__)
    impl_->persistent = persistent;
#else
    if (persistent) throw std::invalid_argument("persistent polling is unavailable on this platform");
#endif
}
poller::~poller() = default;

} // namespace pace::detail
