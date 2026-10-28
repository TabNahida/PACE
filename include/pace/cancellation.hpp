#pragma once
#include <chrono>
#include <memory>
#include <functional>
#include <system_error>

namespace pace {
class deadline {
public:
    using clock = std::chrono::steady_clock;
    deadline() noexcept = default;
    explicit deadline(clock::time_point when) noexcept : when_(when) {}
    template<class Rep, class Period>
    static deadline after(std::chrono::duration<Rep, Period> delay) noexcept {
        const auto now = clock::now();
        using ticks = std::chrono::duration<long double, clock::period>;
        const auto count = ticks(delay).count();
        const auto absolute = static_cast<long double>(now.time_since_epoch().count()) + count;
        if (!(count > 0)) return deadline(now);
        if (absolute >= static_cast<long double>(clock::time_point::max().time_since_epoch().count())) return {};
        return deadline(clock::time_point(clock::duration(static_cast<clock::rep>(absolute))));
    }
    clock::time_point time() const noexcept { return when_; }
    bool expired() const noexcept { return when_ != clock::time_point::max() && clock::now() >= when_; }
    friend deadline earlier(deadline a, deadline b) noexcept { return a.when_ < b.when_ ? a : b; }
private:
    clock::time_point when_ = clock::time_point::max();
};
namespace detail { struct cancellation_state; class cancellation_registration; }
class cancellation_token {
public:
    cancellation_token() noexcept = default;
    bool stop_possible() const noexcept { return bool(state_); }
    std::error_code error() const noexcept;
    bool stop_requested() const noexcept { return bool(error()); }
    void throw_if_cancelled() const { if (state_) if (auto e = error()) throw std::system_error(e); }
    pace::deadline expires() const noexcept;
private:
    friend class cancellation_source;
    friend class detail::cancellation_registration;
    explicit cancellation_token(std::shared_ptr<detail::cancellation_state> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::cancellation_state> state_;
};
namespace detail {
// Internal subscription. Destruction synchronizes with an in-flight notification
// on another thread; self-unregistration is supported. No libc++ experimental
// stop-token dependency is needed on older Apple/Android toolchains.
class cancellation_registration {
public:
    cancellation_registration(cancellation_token, std::function<void()>);
    ~cancellation_registration();
    cancellation_registration(const cancellation_registration&) = delete;
private:
    struct implementation;
    std::shared_ptr<implementation> impl_;
    friend struct cancellation_state;
};
}
// Copyable source; requests may come from any thread. First reason wins.
class cancellation_source {
public:
    explicit cancellation_source(pace::deadline expires = {});
    cancellation_token token() const noexcept { return cancellation_token(state_); }
    bool request_cancellation() const noexcept;
    // Used by a deadline boundary; produces errc::timed_out.
    bool request_timeout() const noexcept;
    // Link before starting work. Cancellation propagates from parent to child only.
    void link(cancellation_token parent);
private:
    std::shared_ptr<detail::cancellation_state> state_;
};
struct operation_options {
    cancellation_token cancellation{};
    pace::deadline deadline{};
};
} // namespace pace
