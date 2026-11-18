#pragma once
#include <pace/run.hpp>

namespace pace {
namespace detail {
template<class Factory>
using operation_result = typename entry_result<decltype(invoke_entry(std::declval<Factory&>(), std::declval<io_context&>()))>::type;
template<class Factory>
task<operation_result<Factory>> scoped_operation(Factory factory, operation_options options) {
    auto parent = co_await this_task::cancellation();
    cancellation_source source(earlier(earlier(parent.expires(), options.cancellation.expires()), options.deadline));
    source.link(parent);
    source.link(options.cancellation);
    source.token().throw_if_cancelled();
    auto& io = this_context();
    io_context::timer_handle timer;
    if (source.token().expires().time() != deadline::clock::time_point::max())
        timer = io.call_at(source.token().expires().time(), [source] { source.request_timeout(); });
    if constexpr (std::is_void_v<operation_result<Factory>>) {
        co_await task_access::bind(invoke_entry(factory, io), source.token());
        source.token().throw_if_cancelled();
    } else {
        auto result = co_await task_access::bind(invoke_entry(factory, io), source.token());
        source.token().throw_if_cancelled();
        co_return result;
    }
}
}
// Factory is owned until completion. Prefer a factory over an immediately invoked
// capturing coroutine lambda. Nested boundaries inherit the earliest deadline.
template<class Factory>
auto with_options(Factory factory, operation_options options) {
    return detail::scoped_operation(std::move(factory), std::move(options));
}
template<class T>
auto with_options(task<T> work, operation_options options) {
    return with_options([work = std::move(work)]() mutable { return std::move(work); }, std::move(options));
}
template<class Work> auto with_deadline(pace::deadline when, Work work) {
    return with_options(std::move(work), {.deadline = when});
}
template<class Work> auto with_cancellation(cancellation_token token, Work work) {
    return with_options(std::move(work), {.cancellation = std::move(token)});
}
// Explicitly mask inherited local cancellation for cleanup. Context stop still
// applies. Combine with with_deadline() to keep cleanup bounded.
template<class Work> auto shield(Work work) {
    return detail::task_access::bind(with_options(std::move(work), {}), {});
}
} // namespace pace
