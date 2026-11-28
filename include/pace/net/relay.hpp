#pragma once

#include <pace/net/tcp.hpp>
#include <pace/runtime/task_group.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>

namespace pace::tls
{
class stream;
} // namespace pace::tls

namespace pace
{
/// Zero timeouts disable the respective deadline. Linger starts when the first direction sees EOF.
struct relay_options
{
    std::chrono::milliseconds idle_timeout{};
    std::chrono::milliseconds linger{};
    /// Request Linux splice for two TCP streams with the default observer. Copy is the portable fallback.
    bool splice = false;
};

enum class relay_direction
{
    a_to_b,
    b_to_a
};

/// A relay policy can suspend before a write to enforce a per-direction or shared bandwidth budget.
template <class Observer>
concept relay_observer = requires(Observer& observer, relay_direction direction, std::size_t bytes) {
    { observer.before_write(direction, bytes) } -> std::same_as<task<void>>;
};

struct relay_result
{
    std::uint64_t a_to_b = 0;
    std::uint64_t b_to_a = 0;
};

/// Implementation support for the pooled transfer buffers and inactivity/half-close timers.
struct relay_access
{
    using buffer = std::array<std::byte, 16 * 1024>;

    struct pool
    {
        std::array<std::unique_ptr<buffer>, 16> idle;
        std::size_t count = 0;
    };

    struct lease
    {
        pool& owner;
        std::unique_ptr<buffer> data;

        explicit lease(pool& cache)
            : owner(cache)
            , data(cache.count ? std::move(cache.idle[--cache.count]) : std::make_unique_for_overwrite<buffer>())
        {
        }

        ~lease()
        {
            if (owner.count < owner.idle.size())
            {
                owner.idle[owner.count++] = std::move(data);
            }
        }

        lease(const lease&) = delete;
        lease& operator=(const lease&) = delete;
    };

    struct state
    {
        io_context& io;
        relay_options options;
        cancellation_source timeout;
        std::chrono::steady_clock::time_point idle_deadline{};
        io_context::timer_handle idle_timer;
        io_context::timer_handle linger_timer;
        bool half_closed = false;

        void progress()
        {
            if (options.idle_timeout.count())
            {
                idle_deadline = io.sleep_for(options.idle_timeout).deadline;
            }
        }

        void arm_idle()
        {
            idle_timer = io.call_at(
                idle_deadline,
                [this]
                {
                    if (std::chrono::steady_clock::now() >= idle_deadline)
                    {
                        timeout.request_timeout();
                    }
                    else
                    {
                        arm_idle();
                    }
                }
            );
        }

        void eof()
        {
            if (!half_closed && options.linger.count())
            {
                linger_timer = io.call_after(options.linger, [this] { timeout.request_timeout(); });
            }
            half_closed = true;
        }
    };

    struct no_observer
    {
    };

    template <byte_stream Stream>
    static decltype(auto) endpoint(Stream& stream)
    {
        if constexpr (std::same_as<Stream, tls::stream>)
        {
            return typename Stream::relay_view(stream);
        }
        else
        {
            return (stream);
        }
    }

    static pool& buffers(io_context& io)
    {
        static const auto key = std::make_shared<char>();
        return *static_cast<pool*>(io.local_state(key, [] { return std::make_shared<pool>(); }).get());
    }
};

#if defined(__linux__)
/// Implementation support. Returns false to finish via the copy loop.
task<bool> relay_splice(tcp::stream&, tcp::stream&, std::uint64_t&, relay_access::state&);
#endif

template <byte_stream From, byte_stream To, class Observer>
task<void> relay_direction_copy(
    From& from,
    To& to,
    relay_direction direction,
    std::uint64_t& transferred,
    relay_access::state& state,
    Observer& observer
)
{
#if defined(__linux__)
    if constexpr (
        std::same_as<From, tcp::stream> && std::same_as<To, tcp::stream>
        && std::same_as<Observer, relay_access::no_observer>
    )
    {
        if (state.options.splice && co_await relay_splice(from, to, transferred, state))
        {
            co_return;
        }
    }
#endif
    relay_access::lease buffer(relay_access::buffers(state.io));
    std::size_t quantum = 0;
    for (;;)
    {
        const auto count = co_await from.read_some(*buffer.data);
        if (!count)
        {
            state.eof();
            co_await to.shutdown_write();
            co_return;
        }
        if (count > buffer.data->size())
        {
            throw std::length_error("relay read exceeded its buffer");
        }
        state.progress();
        if constexpr (!std::same_as<Observer, relay_access::no_observer>)
        {
            co_await observer.before_write(direction, count);
        }
        co_await to.write_all(std::span(*buffer.data).first(count));
        transferred += count;
        state.progress();
        quantum += count;
        if (quantum >= 256 * 1024)
        {
            quantum = 0;
            co_await state.io.yield();
        }
    }
}

/// Runs two pumps on the streams' common context. TLS streams acquire an exclusive duplex adapter.
/// Other streams must permit a simultaneous read and write.
/// EOF forwards FIN and waits for the reverse direction. Error, cancellation and timeout close both streams.
/// The optional observer is owned for the transfer; its before_write(direction, size) returns task<void>.
template <byte_stream A, byte_stream B, class Observer = relay_access::no_observer>
requires(std::same_as<Observer, relay_access::no_observer> || relay_observer<Observer>)
task<relay_result> relay(A& a, B& b, relay_options options = {}, Observer observer = {})
{
    auto& io = this_context();
    if (&a.context() != &io || &b.context() != &io)
    {
        throw std::logic_error("relay streams must belong to the running io_context");
    }
    if constexpr (std::same_as<A, B>)
    {
        if (std::addressof(a) == std::addressof(b))
        {
            throw std::invalid_argument("relay requires two distinct streams");
        }
    }
    if (options.idle_timeout.count() < 0 || options.linger.count() < 0)
    {
        throw std::invalid_argument("relay timeouts must be nonnegative");
    }
    // An adapted endpoint holds its exclusive protocol session until both pumps have joined.
    decltype(auto) left = relay_access::endpoint(a);
    decltype(auto) right = relay_access::endpoint(b);
    relay_access::state state{io, options, cancellation_source{}, {}, {}, {}, false};
    relay_result result{};
    io_context::cancellation_handle close;
    try
    {
        if (options.idle_timeout.count())
        {
            state.progress();
            state.arm_idle();
        }
        co_await with_task_group(
            [&](task_group& group) -> task<void>
            {
                // Stream close also releases custom streams whose waits do not subscribe to the group's token.
                close = io.on_cancel(
                    group.token(),
                    [&](std::error_code)
                    {
                        a.close();
                        b.close();
                    }
                );
                (void)group.spawn(
                    relay_direction_copy(left, right, relay_direction::a_to_b, result.a_to_b, state, observer)
                );
                (void)group.spawn(
                    relay_direction_copy(right, left, relay_direction::b_to_a, result.b_to_a, state, observer)
                );
                co_await group.join();
            },
            {.cancellation = state.timeout.token()}
        );
    }
    catch (...)
    {
        a.close();
        b.close();
        if (auto reason = state.timeout.token().error())
        {
            throw std::system_error(reason);
        }
        throw;
    }
    co_return result;
}
} // namespace pace
