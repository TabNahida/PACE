#pragma once

#include <pace/io_context.hpp>
#include <pace/this_context.hpp>
#include <functional>
#include <optional>
#include <type_traits>

namespace pace {
namespace detail {
template<class Factory>
auto invoke_entry(Factory& factory, io_context& io) {
    if constexpr (std::is_invocable_v<Factory&, io_context&>) return std::invoke(factory, io);
    else return std::invoke(factory);
}
template<class T> struct entry_result;
template<class T> struct entry_result<task<T>> { using type = T; };
template<class T> struct entry_state {
    std::optional<T> value;
    std::exception_ptr failure;
    bool completed = false;
};
template<> struct entry_state<void> {
    std::exception_ptr failure;
    bool completed = false;
};

template<class T, class Factory>
task<void> run_entry(io_context& io, Factory& factory, entry_state<T>& state) {
    struct stop_on_exit {
        io_context& io;
        ~stop_on_exit() { io.stop(); }
    } stop{io};
    try {
        if constexpr (std::is_void_v<T>) co_await invoke_entry(factory, io);
        else state.value.emplace(co_await invoke_entry(factory, io));
        state.completed = true;
    } catch (...) {
        // io_context suppresses shutdown cancellation; run must still report
        // cancellation of its entry task to the synchronous caller.
        state.failure = std::current_exception();
        throw;
    }
}
} // namespace detail

// Run one entry task on a fresh context on this thread. Own the factory until
// shutdown finishes, including a temporary capturing coroutine lambda. Entry
// completion stops the context and drains cancellation of background roots.
// Return owned data, not sockets/views that borrow this context or the factory.
// Background tasks must own their data: entry-local objects can be destroyed
// before those tasks observe cancellation. Nested runs are rejected.
template<class Factory>
requires (std::is_invocable_v<std::decay_t<Factory>&, io_context&> ||
          std::is_invocable_v<std::decay_t<Factory>&>)
auto run(Factory&& factory) {
    using function = std::decay_t<Factory>;
    using result = typename detail::entry_result<decltype(detail::invoke_entry(
        std::declval<function&>(), std::declval<io_context&>()))>::type;
    if (io_context::current()) throw std::logic_error("nested pace::run is not supported");
    function owned(std::forward<Factory>(factory));
    detail::entry_state<result> state;
    io_context io; // Destroy roots before their borrowed factory/state on error.
    io.spawn(detail::run_entry<result>(io, owned, state));
    io.run();
    if (state.failure) std::rethrow_exception(state.failure);
    if (!state.completed) throw std::system_error(std::make_error_code(std::errc::operation_canceled));
    if constexpr (!std::is_void_v<result>) return result(std::move(*state.value));
}
// The task owns its arguments, but references/coroutine-lambda captures remain
// borrowed. Pass a capturing coroutine lambda itself to run(), not lambda().
template<class T>
auto run(task<T> entry) {
    return run([entry = std::move(entry)]() mutable { return std::move(entry); });
}
} // namespace pace
