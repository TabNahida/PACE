#pragma once

#include <pace/platform.hpp>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <utility>

// Diagnostic wall-clock accounting. All state belongs to the calling thread.
// Scopes must be closed before a coroutine suspends; nested synchronous scopes
// subtract child time so exclusive totals do not double count.
namespace pace::detail::profiling {
enum class stage {
    reactor_run, inbox, post_dispatch, coroutine_execution, root_reaping,
    timers, poll_nonblocking, poll_blocking, event_dispatch, io_arm,
    poller_update, recv, send, accept, count
};
inline constexpr std::array names{
    "reactor_run", "inbox", "post_dispatch", "coroutine_execution", "root_reaping",
    "timers", "poll_nonblocking", "poll_blocking", "event_dispatch", "io_arm",
    "poller_update", "recv", "send", "accept"
};
struct counter { std::uint64_t calls = 0, inclusive_ns = 0, exclusive_ns = 0; };
using counters = std::array<counter, static_cast<std::size_t>(stage::count)>;
#ifdef PACE_ENABLE_PROFILING
inline constexpr bool enabled = true;
class scope;
inline thread_local counters values{};
inline thread_local scope* active = nullptr;
struct preserve_error {
    int saved_errno = errno;
#ifdef _WIN32
    int socket_error = ::WSAGetLastError();
#endif
    ~preserve_error() {
#ifdef _WIN32
        ::WSASetLastError(socket_error);
#endif
        errno = saved_errno;
    }
};
class scope {
    using clock = std::chrono::steady_clock;
    stage stage_;
    scope* parent_;
    clock::time_point start_;
    std::uint64_t children_ = 0;
    bool open_ = true;
public:
    explicit scope(stage value) noexcept : stage_(value), parent_(active) {
        preserve_error saved;
        start_ = clock::now();
        active = this;
    }
    scope(const scope&) = delete;
    scope& operator=(const scope&) = delete;
    ~scope() { close(); }
    void close() noexcept {
        if (!open_) return;
        preserve_error saved;
        const auto elapsed = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start_).count());
        auto& value = values[static_cast<std::size_t>(stage_)];
        ++value.calls;
        value.inclusive_ns += elapsed;
        value.exclusive_ns += elapsed - children_;
        if (parent_) parent_->children_ += elapsed;
        active = parent_;
        open_ = false;
    }
};
inline void reset() noexcept { values = {}; }
inline counters snapshot() noexcept { return values; }
#else
inline constexpr bool enabled = false;
class scope {
public:
    explicit scope(stage) noexcept {}
    void close() noexcept {}
};
inline void reset() noexcept {}
inline counters snapshot() noexcept { return {}; }
#endif
template<class F> decltype(auto) measure(stage value, F&& operation) {
    scope timing(value);
    return std::forward<F>(operation)();
}
} // namespace pace::detail::profiling
