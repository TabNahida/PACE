#include <pace/io_context.hpp>
#include <pace/detail/poller.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#if defined(_MSC_VER)
#define PACE_BENCH_NOINLINE __declspec(noinline)
#else
#define PACE_BENCH_NOINLINE __attribute__((noinline))
#endif

namespace {
using bench_clock = std::chrono::steady_clock;

struct options {
    std::string component = "yield";
    std::uint64_t iterations = 100000;
    std::uint64_t warmup = 5000;
    std::uint64_t trial = 1;
    std::uint64_t depth = 4;
    std::uint64_t batch = 256;
};

constexpr std::array<std::string_view, 8> component_names{
    "lazy_task", "await_chain", "empty_poll", "yield", "post", "cross_thread_post",
    "spawn_completion", "zero_timer"};

struct measurement {
    std::uint64_t completed{};
    std::uint64_t checksum{};
    bench_clock::time_point begin{}, end{};
    std::uint64_t measured_ns{}; // Nonzero for summed, separately timed enqueue batches.
};

std::uint64_t expected_sum(std::uint64_t count) { return count * (count - 1) / 2; }

PACE_BENCH_NOINLINE pace::task<std::uint64_t> lazy_value(std::uint64_t value,
                                                       std::uint64_t& entered) {
    ++entered;
    co_return value;
}

// The volatile function pointer prevents eliding the unstarted factory call.
// Its indirect-call overhead is deliberately included, with no subtraction.
using task_factory = pace::task<std::uint64_t> (*)(std::uint64_t, std::uint64_t&);
task_factory volatile lazy_factory = &lazy_value;

measurement measure_lazy(std::uint64_t iterations) {
    measurement result;
    std::uint64_t entered = 0;
    result.begin = bench_clock::now();
    for (std::uint64_t i = 0; i < iterations; ++i) {
        auto unstarted = lazy_factory(i, entered);
    }
    result.end = bench_clock::now();
    if (entered != 0) throw std::runtime_error("lazy task body unexpectedly executed");
    result.completed = iterations;
    return result;
}

measurement measure_empty_poll(std::uint64_t iterations) {
    pace::detail::poller poller;
    std::array<pace::detail::poll_event, 256> events{};
    measurement result;
    result.begin = bench_clock::now();
    for (std::uint64_t i = 0; i < iterations; ++i)
        result.checksum += poller.wait(events, 0);
    result.end = bench_clock::now();
    result.completed = iterations;
    if (result.checksum) throw std::runtime_error("empty poll returned a user event");
    return result;
}

PACE_BENCH_NOINLINE pace::task<std::uint64_t> await_chain(std::uint64_t depth,
    std::uint64_t value, std::uint64_t& leaves) {
    if (depth == 1) {
        ++leaves;
        co_return value;
    }
    co_return co_await await_chain(depth - 1, value, leaves);
}

pace::task<void> await_driver(measurement& result, std::uint64_t iterations,
                             std::uint64_t depth) {
    for (std::uint64_t i = 0; i < iterations; ++i)
        result.checksum += co_await await_chain(depth, i, result.completed);
}

measurement measure_await(std::uint64_t iterations, std::uint64_t depth) {
    measurement result;
    auto driver = await_driver(result, iterations, depth);
    auto handle = driver.release();
    result.begin = bench_clock::now();
    handle.resume();
    result.end = bench_clock::now();
    const bool finished = handle.done();
    const auto error = handle.promise().exception;
    handle.destroy(); // Exclude the one outer driver's frame destruction.
    if (!finished) throw std::runtime_error("synchronous await chain suspended unexpectedly");
    if (error) std::rethrow_exception(error);
    return result;
}

template<bool Timer>
pace::task<void> yield_driver(pace::io_context& context, measurement& result,
                              std::uint64_t iterations) {
    result.begin = bench_clock::now();
    for (std::uint64_t i = 0; i < iterations; ++i) {
        if constexpr (Timer) co_await context.sleep_for(std::chrono::nanoseconds::zero());
        else co_await context.yield();
        ++result.completed;
        result.checksum += i;
    }
    result.end = bench_clock::now();
    context.stop();
}

struct post_chain {
    pace::io_context& context;
    measurement& result;
    std::uint64_t iterations;

    void next() {
        result.checksum += result.completed++;
        if (result.completed == iterations) {
            result.end = bench_clock::now();
            context.stop();
        } else {
            // One pointer capture keeps the callback small; std::function's
            // implementation and the loop dispatch remain part of the cost.
            context.post([this] { next(); });
        }
    }
    void start() {
        result.begin = bench_clock::now();
        context.post([this] { next(); });
    }
};

struct spawn_batches {
    pace::io_context& context;
    measurement& result;
    std::uint64_t iterations;
    std::uint64_t batch;
    std::uint64_t submitted{};
    std::uint64_t pending{};

    static pace::task<void> child(spawn_batches* state, std::uint64_t value) {
        state->result.checksum += value;
        ++state->result.completed;
        if (--state->pending == 0) {
            // Posted work is dispatched on the next loop pass, after this
            // pass's completed coroutine frames have been destroyed.
            state->context.post([state] { state->next(); });
        }
        co_return;
    }
    void next() {
        if (submitted == iterations) {
            result.end = bench_clock::now();
            context.stop();
            return;
        }
        pending = std::min(batch, iterations - submitted);
        const auto end = submitted + pending;
        while (submitted < end) {
            const auto value = submitted++;
            context.spawn(child(this, value));
        }
    }
    void start() {
        result.begin = bench_clock::now();
        next();
    }
};

measurement measure_cross_thread_post(std::uint64_t iterations, std::uint64_t batch) {
    pace::io_context context;
    measurement result;
    struct shared_state {
        std::mutex mutex;
        std::condition_variable changed;
        bool ready = false;
        bool drained = false;
        std::exception_ptr error;
        std::atomic<std::uint64_t> target{0};
        measurement* result;

        void complete() {
            result->checksum += result->completed++;
            if (result->completed == target.load(std::memory_order_relaxed)) {
                { std::lock_guard lock(mutex); drained = true; }
                changed.notify_one();
            }
        }
    } state;
    state.result = &result;
    context.post([&state] {
        { std::lock_guard lock(state.mutex); state.ready = true; }
        state.changed.notify_one();
    });
    std::thread consumer([&] {
        try { context.run(); }
        catch (...) {
            { std::lock_guard lock(state.mutex); state.error = std::current_exception(); }
            state.changed.notify_one();
        }
    });
    struct join_guard {
        pace::io_context& context;
        std::thread& consumer;
        ~join_guard() { context.stop(); if (consumer.joinable()) consumer.join(); }
    } cleanup{context, consumer};
    {
        std::unique_lock lock(state.mutex);
        state.changed.wait(lock, [&] { return state.ready || state.error; });
        if (state.error) std::rethrow_exception(state.error);
    }
    std::uint64_t submitted = 0;
    while (submitted < iterations) {
        const auto end = submitted + std::min(batch, iterations - submitted);
        { std::lock_guard lock(state.mutex); state.drained = false; }
        state.target.store(end, std::memory_order_relaxed);
        const auto before = bench_clock::now();
        for (; submitted < end; ++submitted) context.post([&state] { state.complete(); });
        const auto after = bench_clock::now();
        result.measured_ns += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count());
        // Completion synchronization and final drain are outside enqueue timing.
        std::unique_lock lock(state.mutex);
        state.changed.wait(lock, [&] { return state.drained || state.error; });
        if (state.error) std::rethrow_exception(state.error);
    }
    context.stop();
    consumer.join();
    if (state.error) std::rethrow_exception(state.error);
    return result;
}

measurement measure(const options& config, std::uint64_t iterations) {
    if (config.component == "lazy_task") return measure_lazy(iterations);
    if (config.component == "await_chain") return measure_await(iterations, config.depth);
    if (config.component == "empty_poll") return measure_empty_poll(iterations);
    if (config.component == "cross_thread_post") return measure_cross_thread_post(iterations, config.batch);
    // Reactor creation, the bootstrap post/spawn, stop and final destruction
    // are outside the timed interval. run() executes on this calling thread.
    pace::io_context context;
    measurement result;
    post_chain posts{context, result, iterations};
    spawn_batches spawns{context, result, iterations, config.batch};
    if (config.component == "post") context.post([&posts] { posts.start(); });
    else if (config.component == "spawn_completion") context.post([&spawns] { spawns.start(); });
    else if (config.component == "zero_timer") context.spawn(yield_driver<true>(context, result, iterations));
    else context.spawn(yield_driver<false>(context, result, iterations));
    context.run();
    return result;
}

std::uint64_t validate(const measurement& result, const options& config,
                       std::uint64_t iterations) {
    const auto expected = config.component == "lazy_task" || config.component == "empty_poll"
        ? 0 : expected_sum(iterations);
    if (result.completed != iterations || result.checksum != expected)
        throw std::runtime_error("component operation count/checksum mismatch");
    const auto duration = result.measured_ns ? static_cast<std::int64_t>(result.measured_ns)
        : std::chrono::duration_cast<std::chrono::nanoseconds>(result.end - result.begin).count();
    if (duration <= 0) throw std::runtime_error("measurement duration must be positive");
    return static_cast<std::uint64_t>(duration);
}

void list_components() {
    std::cout << "{\"components\":[";
    bool first = true;
    for (auto name : component_names) {
        if (!first) std::cout << ',';
        std::cout << '"' << name << '"';
        first = false;
    }
    std::cout << "],\"compiler\":\"";
#if defined(__clang__)
    std::cout << "Clang " << __clang_major__ << '.' << __clang_minor__ << '.' << __clang_patchlevel__;
#elif defined(_MSC_VER)
    std::cout << "MSVC " << _MSC_VER;
#elif defined(__GNUC__)
    std::cout << "GCC " << __GNUC__ << '.' << __GNUC_MINOR__ << '.' << __GNUC_PATCHLEVEL__;
#else
    std::cout << "unknown";
#endif
    std::cout << "\",\"cpp_standard\":";
#ifdef _MSVC_LANG
    std::cout << _MSVC_LANG;
#else
    std::cout << __cplusplus;
#endif
    std::cout << ",\"ndebug\":";
#ifdef NDEBUG
    std::cout << "true";
#else
    std::cout << "false";
#endif
    std::cout << ",\"profiling_enabled\":";
#ifdef PACE_ENABLE_PROFILING
    std::cout << "true";
#else
    std::cout << "false";
#endif
    std::cout << "}\n";
}

std::uint64_t parse_integer(std::string_view input) {
    if (input.empty() || input.find_first_not_of("0123456789") != std::string_view::npos)
        throw std::invalid_argument("expected non-negative integer");
    return std::stoull(std::string(input));
}
} // namespace

int main(int argc, char** argv) {
    try {
        options config;
        for (int i = 1; i < argc; ++i) {
            const std::string_view key = argv[i];
            if (key == "--list") { list_components(); return 0; }
            if (key == "--help") {
                std::cout << "pace_components --component lazy_task|await_chain|empty_poll|yield|post|cross_thread_post|spawn_completion|zero_timer "
                    "[--iterations 100000] [--warmup 5000] [--depth 4] [--batch 256] [--trial 1]\n";
                return 0;
            }
            if (++i == argc) throw std::invalid_argument("missing option value");
            const std::string_view value = argv[i];
            if (key == "--component") config.component = value;
            else if (key == "--iterations") config.iterations = parse_integer(value);
            else if (key == "--warmup") config.warmup = parse_integer(value);
            else if (key == "--depth") config.depth = parse_integer(value);
            else if (key == "--batch") config.batch = parse_integer(value);
            else if (key == "--trial") config.trial = parse_integer(value);
            else throw std::invalid_argument("unknown option: " + std::string(key));
        }
        bool known = false;
        for (auto name : component_names) if (name == config.component) known = true;
        if (!known || config.iterations == 0 || config.iterations > 100000000 || config.warmup > 10000000 ||
            config.depth == 0 || config.depth > 64 || config.batch == 0 || config.batch > 65536 || config.trial == 0)
            throw std::invalid_argument("unknown component or out-of-range configuration");
#ifdef PACE_ENABLE_PROFILING
        throw std::invalid_argument("component timing requires an uninstrumented build (--profiling=n)");
#endif
        if (config.warmup) validate(measure(config, config.warmup), config, config.warmup);
        const auto result = measure(config, config.iterations);
        const auto elapsed = validate(result, config, config.iterations);
        std::cout << std::fixed << std::setprecision(9)
            << "{\"benchmark\":\"components\",\"component\":\"" << config.component
            << "\",\"trial\":" << config.trial << ",\"iterations\":" << config.iterations
            << ",\"warmup\":" << config.warmup << ",\"chain_depth\":" << config.depth
            << ",\"batch_size\":" << config.batch << ",\"threads\":" << (config.component == "cross_thread_post" ? 2 : 1)
            << ",\"completed_operations\":" << result.completed
            << ",\"elapsed_ns\":" << elapsed << ",\"ns_per_operation\":" << static_cast<double>(elapsed) / config.iterations
            << ",\"checksum\":" << result.checksum << ",\"validated\":true}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "component benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
