#pragma once
#include <pace/runtime/operation.hpp>

namespace pace
{
/// Options of a blocking call. Its budget ends at the earlier of `deadline` and now + `timeout`.
struct blocking_options
{
    std::chrono::milliseconds timeout{30000}; ///< Total budget from the start of the call; must be positive.
    cancellation_token cancellation{};        ///< Cancels the call; may be cancelled from another thread.
    pace::deadline deadline{};                ///< Absolute deadline; none by default.
};

/// Runs `factory`'s task with pace::run inside an operation boundary built from `options` and returns its result.
/// Throws std::logic_error on a thread already running a PACE context, std::invalid_argument for timeout <= 0,
/// std::system_error(timed_out) when the budget runs out and std::system_error(operation_canceled) on cancellation.
template <class Factory>
// NOLINTNEXTLINE(performance-unnecessary-value-param): sink; tidy misses the move of options.cancellation
auto blocking_call(Factory&& factory, blocking_options options)
{
    if (io_context::current())
    {
        throw std::logic_error("blocking client called inside a PACE context; use the async API");
    }
    if (options.timeout.count() <= 0)
    {
        throw std::invalid_argument("blocking timeout must be positive");
    }
    return pace::run(with_options(
        std::forward<Factory>(factory),
        {.cancellation = std::move(options.cancellation),
         .deadline = earlier(options.deadline, pace::deadline::after(options.timeout))}
    ));
}
} // namespace pace
