#pragma once
#include <pace/runtime/run.hpp>

namespace pace
{
/// The T of the task<T> that Factory returns.
template <class Factory>
using operation_result =
    entry_access::result<decltype(entry_access::invoke(std::declval<Factory&>(), std::declval<io_context&>()))>;

/// Implementation support: the coroutine behind with_options. Not part of the API.
template <class Factory>
task<operation_result<Factory>> scoped_operation(Factory factory, operation_options options)
{
    auto parent = co_await this_task::cancellation();
    cancellation_source source(earlier(earlier(parent.expires(), options.cancellation.expires()), options.deadline));
    source.link(parent);
    source.link(options.cancellation);
    source.token().throw_if_cancelled();
    auto& io = this_context();
    io_context::timer_handle timer;
    if (source.token().expires().time() != deadline::clock::time_point::max())
    {
        timer = io.call_at(source.token().expires().time(), [source] { source.request_timeout(); });
    }
    if constexpr (std::is_void_v<operation_result<Factory>>)
    {
        co_await entry_access::invoke_bound(factory, io, source.token());
        source.token().throw_if_cancelled();
    }
    else
    {
        auto result = co_await entry_access::invoke_bound(factory, io, source.token());
        source.token().throw_if_cancelled();
        co_return result;
    }
}

/// An operation boundary: runs the factory's task under a new token linked to the inherited one and to
/// `options.cancellation`, with the earliest of the inherited deadline and the options' deadlines; a nested boundary
/// never extends the budget. Throws std::system_error(timed_out) when the deadline passes, even after the work
/// returned, and the token's error when cancelled. The factory is owned until completion; call on a running context.
template <class Factory>
auto with_options(Factory factory, operation_options options)
{
    return scoped_operation(std::move(factory), std::move(options));
}

/// with_options for an already-created task; what the task borrowed stays borrowed.
template <class T>
auto with_options(task<T> work, operation_options options)
{
    return with_options([work = std::move(work)]() mutable { return std::move(work); }, std::move(options));
}

/// with_options(work, {.deadline = when}).
template <class Work>
auto with_deadline(pace::deadline when, Work work)
{
    return with_options(std::move(work), {.deadline = when});
}

/// with_options(work, {.cancellation = token}).
template <class Work>
// NOLINTNEXTLINE(performance-unnecessary-value-param): token is moved; tidy misses the designated-initializer move
auto with_cancellation(cancellation_token token, Work work)
{
    return with_options(std::move(work), {.cancellation = std::move(token)});
}

/// Runs `work` without the inherited token or deadline, for cleanup during cancellation. io_context::stop() still
/// applies. Combine with with_deadline() to keep the cleanup bounded.
template <class Work>
auto shield(Work work)
{
    return task_access::bind(with_options(std::move(work), {}), {});
}
} // namespace pace
