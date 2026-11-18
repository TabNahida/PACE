#pragma once
#include <pace/operation.hpp>

namespace pace {
struct blocking_options {
    std::chrono::milliseconds timeout{30000}; // Compatibility shorthand for a total deadline.
    cancellation_token cancellation{};
    pace::deadline deadline{};
};
namespace detail {
template<class Factory>
auto blocking_call(Factory&& factory, blocking_options options) {
    if (io_context::current()) throw std::logic_error("blocking client called inside a PACE context; use the async API");
    if (options.timeout.count() <= 0) throw std::invalid_argument("blocking timeout must be positive");
    return pace::run(with_options(std::forward<Factory>(factory), {
        .cancellation = std::move(options.cancellation),
        .deadline = earlier(options.deadline, pace::deadline::after(options.timeout))}));
}
}
}
