// Cancellation callbacks registered on an io_context, and cancelling pending waits.
#include "runtime/io_context_state.hpp"

namespace pace
{
void io_context::implementation::cancel_all(bool running_on_owner)
{
    notify_shutdown();
    for (auto& state : descriptors)
    {
        if (state.fd == invalid_socket)
        {
            continue;
        }
        poller.remove(state.fd);
        finish(state, state.reader, canceled());
        finish(state, state.writer, canceled());
    }
    descriptors.clear();
    free_descriptors.clear();
    descriptor_count = 0;
    tokens.clear();
    dirty_descriptors.clear();
#ifdef _WIN32
    while (!handle_waits.empty())
    {
        cancel_handle_wait(*handle_waits.begin()->second, canceled());
    }
#endif
    while (!timers.empty())
    {
        auto* timer = timers.top();
        timers.pop();
        timer->error = canceled();
        ready.push_back(timer->continuation);
    }
    if (callbacks)
    {
        callbacks->clear();
    }
    cancellation_started = true;
    if (cancellations && running_on_owner)
    {
        auto failure = cancellations->dispatch(true);
        if (!first_error)
        {
            first_error = failure;
        }
    }
}

io_context::cancellation_handle::~cancellation_handle()
{
    cancel();
}

io_context::cancellation_handle::cancellation_handle(cancellation_handle&& other) noexcept
    : records_(std::move(other.records_))
    , id_(std::exchange(other.id_, 0))
{
}

io_context::cancellation_handle& io_context::cancellation_handle::operator=(cancellation_handle&& other) noexcept
{
    if (this != &other)
    {
        cancel();
        records_ = std::move(other.records_);
        id_ = std::exchange(other.id_, 0);
    }
    return *this;
}

void io_context::cancellation_handle::cancel() noexcept
{
    if (auto id = std::exchange(id_, 0))
    {
        if (auto records = records_.lock())
        {
            records->cancel(id);
        }
    }
    records_.reset();
}

io_context::cancellation_handle io_context::on_cancel(
    cancellation_token token,
    std::function<void(std::error_code)> callback
)
{
    if (!token.stop_possible())
    {
        return {};
    }
    check_affinity(this);
    if (stopped())
    {
        throw std::system_error(canceled());
    }
    if (!callback)
    {
        throw std::invalid_argument("empty cancellation callback");
    }
    token.throw_if_cancelled();
    if (!impl_->cancellations)
    {
        impl_->cancellations = std::make_shared<cancellation_records>(impl_->poller);
    }
    auto& records = *impl_->cancellations;
    if (records.next_id == UINT64_MAX)
    {
        throw std::overflow_error("cancellation identity exhausted");
    }
    cancellation_handle result;
    result.id_ = ++records.next_id;
    result.records_ = impl_->cancellations;
    auto& entry = records.entries.try_emplace(result.id_).first->second;
    entry.id = result.id_;
    entry.token = std::move(token);
    entry.callback = std::move(callback);
    entry.wake.emplace(entry.token, [&records, &entry] { records.enqueue(entry); });
    return result;
}

void io_context::cancel_wait(io_awaiter& operation, std::error_code error)
{
    const auto token = impl_->tokens.find(operation.fd);
    if (token == impl_->tokens.end())
    {
        return;
    }
    auto& state = *impl_->find_descriptor(token->second);
    if (state.reader != &operation && state.writer != &operation)
    {
        return;
    }
    impl_->finish(state, &operation, error);
    impl_->update(state, token->second);
}

void io_context::cancel_wait(timer_awaiter& operation, std::error_code error)
{
    if (!impl_->timers.erase(&operation))
    {
        return;
    }
    operation.error = error;
    impl_->ready.push_back(operation.continuation);
}
} // namespace pace
