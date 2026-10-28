#pragma once
#include <pace/run.hpp>
#include <chrono>

namespace pace {
// A cooperative budget for a complete blocking call, including connection,
// protocol exchange and cleanup. Positive durations only; no helper threads.
struct blocking_options {
    std::chrono::milliseconds timeout{30000};
};
namespace detail {
template<class Factory>
auto blocking_call(Factory&& factory, blocking_options options) {
    using result = typename entry_result<decltype(invoke_entry(factory, std::declval<io_context&>()))>::type;
    if (io_context::current()) throw std::logic_error("blocking client called inside a PACE context; use the async API");
    if (options.timeout.count() <= 0) throw std::invalid_argument("blocking timeout must be positive");
    bool expired = false;
    const auto started = std::chrono::steady_clock::now();
    auto elapsed = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started) >= options.timeout;
    };
    auto entry = [&](io_context& io) -> task<result> {
        auto deadline = io.call_after(options.timeout, [&] { expired = true; io.stop(); });
        if constexpr (std::is_void_v<result>) co_await invoke_entry(factory, io);
        else co_return co_await invoke_entry(factory, io);
    };
    try {
        if constexpr (std::is_void_v<result>) {
            pace::run(entry);
            if (expired || elapsed()) throw std::system_error(std::make_error_code(std::errc::timed_out));
        } else {
            auto value = pace::run(entry);
            if (expired || elapsed()) throw std::system_error(std::make_error_code(std::errc::timed_out));
            return value;
        }
    } catch (...) {
        if (expired || elapsed()) throw std::system_error(std::make_error_code(std::errc::timed_out), "blocking client deadline");
        throw;
    }
}
} // namespace detail
} // namespace pace
