#pragma once
#include <pace/io_context.hpp>
#include <stdexcept>

namespace pace {
// Resolve the context on this thread. Call inside a running PACE task/callback;
// no context is created implicitly and no work moves to another thread.
inline io_context& this_context() {
    auto* io = io_context::current();
    if (!io) throw std::logic_error("operation requires a running PACE context");
    return *io;
}
template<class Rep, class Period>
auto sleep_for(std::chrono::duration<Rep, Period> duration) {
    return this_context().sleep_for(duration);
}
inline auto yield() { return this_context().yield(); }
} // namespace pace
