#pragma once
#include <pace/runtime/io_context.hpp>

#include <stdexcept>

namespace pace
{
/// The context running on this thread. Throws std::logic_error if none is running; never creates one.
inline io_context& this_context()
{
    auto* io = io_context::current();
    if (!io)
    {
        throw std::logic_error("operation requires a running PACE context");
    }
    return *io;
}

/// this_context().sleep_for(duration). Throws std::logic_error if no context is running on this thread.
template <class Rep, class Period>
auto sleep_for(std::chrono::duration<Rep, Period> duration)
{
    return this_context().sleep_for(duration);
}

/// this_context().yield(). Throws std::logic_error if no context is running on this thread.
inline auto yield()
{
    return this_context().yield();
}
} // namespace pace
