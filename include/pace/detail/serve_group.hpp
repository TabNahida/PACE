#pragma once
#include <pace/task_group.hpp>
namespace pace::detail {
// All accepted connections belong to this serve invocation. Listener shutdown
// cancels them; the invocation returns only after their handlers have unwound.
template<class Function> task<void> serve_group(Function accept_loop) {
    try {
        co_await with_task_group([accept_loop = std::move(accept_loop)](task_group& group) mutable -> task<void> {
            co_await accept_loop(group);
            group.cancel();
        });
    } catch (const std::system_error& error) {
        if (error.code() != std::errc::operation_canceled) throw;
    }
}
}
