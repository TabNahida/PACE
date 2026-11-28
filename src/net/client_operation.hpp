#pragma once

#include <pace/net/client_options.hpp>
#include <pace/net/stream_io.hpp>
#include <pace/runtime/blocking.hpp>

namespace pace::net
{
template <class T>
task<T> client_operation(const client_options& options, task<T> work)
{
    auto& io = this_context();
    if (options.context && options.context != &io)
    {
        throw std::logic_error("client operation must run on its selected io_context");
    }
    operation_options budget{.cancellation = options.cancellation, .deadline = options.deadline};
    if (options.timeout)
    {
        if (options.timeout->count() <= 0)
        {
            throw std::invalid_argument("client timeout must be positive");
        }
        budget.deadline = earlier(budget.deadline, deadline::after(*options.timeout));
    }
    if (budget.cancellation.stop_possible() || budget.deadline.time() != deadline::clock::time_point::max())
    {
        return with_options(std::move(work), std::move(budget));
    }
    return work;
}

// blocking_call owns the operation boundary. Its async body inherits that same token and deadline.
inline blocking_options take_blocking_budget(client_options& options)
{
    if (options.context)
    {
        throw std::invalid_argument("blocking clients use a fresh io_context");
    }
    blocking_options budget;
    budget.timeout = options.timeout.value_or(budget.timeout);
    budget.deadline = std::exchange(options.deadline, {});
    budget.cancellation = std::move(options.cancellation);
    options.timeout.reset();
    return budget;
}

template <class Stream>
task<std::string> exchange_bytes(
    task<Stream> connection,
    std::string request,
    std::size_t reply_size,
    std::size_t maximum
)
{
    if (reply_size > maximum)
    {
        throw std::length_error("stream reply exceeds maximum");
    }
    auto stream = co_await std::move(connection);
    co_await pace::write(stream, std::move(request));
    auto reply = co_await pace::read_exact(stream, reply_size, maximum);
    co_await stream.close_gracefully();
    co_return reply;
}
} // namespace pace::net
