#pragma once
#include <chrono>
#include <functional>
#include <memory>
#include <system_error>

namespace pace
{
/// An absolute point on steady_clock. A default-constructed deadline is "none" and never expires.
class deadline
{
public:
    using clock = std::chrono::steady_clock;
    /// No deadline.
    deadline() noexcept = default;

    /// Expires at `when`.
    explicit deadline(clock::time_point when) noexcept
        : when_(when)
    {
    }

    /// Now + `delay`, computed when called. A delay <= 0 has already expired; a delay too large for the clock
    /// gives no deadline.
    template <class Rep, class Period>
    static deadline after(std::chrono::duration<Rep, Period> delay) noexcept
    {
        const auto now = clock::now();
        using ticks = std::chrono::duration<long double, clock::period>;
        const auto count = ticks(delay).count();
        const auto absolute = static_cast<long double>(now.time_since_epoch().count()) + count;
        if (!(count > 0))
        {
            return deadline(now);
        }
        if (absolute >= static_cast<long double>(clock::time_point::max().time_since_epoch().count()))
        {
            return {};
        }
        return deadline(clock::time_point(clock::duration(static_cast<clock::rep>(absolute))));
    }

    /// The time point; clock::time_point::max() for no deadline.
    clock::time_point time() const noexcept
    {
        return when_;
    }

    /// Whether the deadline exists and has passed.
    bool expired() const noexcept
    {
        return when_ != clock::time_point::max() && clock::now() >= when_;
    }

    /// The earlier of two deadlines.
    friend deadline earlier(deadline a, deadline b) noexcept
    {
        return a.when_ < b.when_ ? a : b;
    }

private:
    clock::time_point when_ = clock::time_point::max();
};

struct cancellation_state;
class cancellation_registration;

/// A cheap, copyable observer of one cancellation_source. A default-constructed token is never cancelled.
class cancellation_token
{
public:
    /// An empty token.
    cancellation_token() noexcept = default;

    /// Whether the token belongs to a source.
    bool stop_possible() const noexcept
    {
        return static_cast<bool>(state_);
    }

    /// The source's reason (operation_canceled or timed_out), else timed_out once its deadline has passed, else
    /// an empty code. A passed deadline is seen here but wakes no pending wait by itself.
    std::error_code error() const noexcept;

    /// Whether error() is non-empty.
    bool stop_requested() const noexcept
    {
        return static_cast<bool>(error());
    }

    /// Throws std::system_error(error()) if stop_requested().
    void throw_if_cancelled() const
    {
        if (state_)
        {
            if (auto e = error())
            {
                throw std::system_error(e);
            }
        }
    }

    /// The source's deadline; none for an empty token.
    pace::deadline expires() const noexcept;

private:
    friend class cancellation_source;
    friend class cancellation_registration;

    explicit cancellation_token(std::shared_ptr<cancellation_state> state)
        : state_(std::move(state))
    {
    }

    std::shared_ptr<cancellation_state> state_;
};

/// Low-level subscription to a token, used by cancellation_source::link and io_context::on_cancel.
/// The callback runs once on the thread that requests cancellation (inside the constructor if the token is already
/// cancelled) and must not throw. Prefer io_context::on_cancel, which runs callbacks on the context.
// No libc++ experimental stop-token dependency is needed on older Apple/Android toolchains.
class cancellation_registration
{
public:
    /// Registers `callback` with `token`; registers nothing for an empty token.
    cancellation_registration(const cancellation_token&, std::function<void()>);
    /// Unregisters; waits for a callback running on another thread. A callback may destroy its own registration.
    ~cancellation_registration();
    cancellation_registration(const cancellation_registration&) = delete;
    cancellation_registration& operator=(const cancellation_registration&) = delete;

private:
    struct implementation;
    std::shared_ptr<implementation> impl_;
    friend struct cancellation_state;
};

/// The owner of a cancellation state; copies share it. Requests may come from any thread; the first reason wins.
class cancellation_source
{
public:
    /// A new state whose tokens expire at `expires`.
    explicit cancellation_source(pace::deadline expires = {});

    /// A token observing this source.
    cancellation_token token() const noexcept
    {
        return cancellation_token(state_);
    }

    /// Cancels with operation_canceled. Returns true if this was the first request. Thread-safe.
    bool request_cancellation() const noexcept;
    /// Cancels with timed_out, as a deadline boundary does. Returns true if this was the first request.
    bool request_timeout() const noexcept;
    /// Cancels this source when `parent` is cancelled (at once if it already is), one way only. Does not copy the
    /// parent's deadline. Not synchronized: link before the source is shared. Throws std::invalid_argument when
    /// `parent` is this source's own token; an empty `parent` is ignored.
    void link(cancellation_token parent);

private:
    std::shared_ptr<cancellation_state> state_;
};

/// What an operation boundary adds to the token and deadline it inherits.
struct operation_options
{
    cancellation_token cancellation{}; ///< Also cancels the operation; none by default.
    pace::deadline deadline{};         ///< Shortens the inherited deadline; none by default.
};
} // namespace pace
