#pragma once
#include <pace/http_types.hpp>
#include <pace/operation.hpp>
#include <cstdint>
#include <deque>
#include <memory>
#include <utility>

namespace pace::detail {
// One reactor owns this queue. Handler tasks never access a protocol session or
// transport; connection exit cancels handlers and drains them before returning.
struct http_dispatch : std::enable_shared_from_this<http_dispatch> {
    struct result { std::int64_t id; http::response response; std::exception_ptr error; };
    io_context& io;
    http::handler handle;
    std::size_t active = 0;
    std::deque<result> completed;
    std::exception_ptr failure;
    std::function<void()> wake;
    bool open = true;
    cancellation_source source;
    std::coroutine_handle<> drain_waiter{};
    std::coroutine_handle<> waiter{};

    http_dispatch(io_context& context, http::handler handler, cancellation_token parent)
        : io(context), handle(std::move(handler)), source(parent.expires()) { source.link(parent); }
    void close() noexcept { open = false; wake = {}; completed.clear(); source.request_cancellation(); }
    void child_finished() {
        if (--active == 0 && drain_waiter) scheduler_access::schedule(io, std::exchange(drain_waiter, {}));
    }
    struct drain_awaiter {
        std::shared_ptr<http_dispatch> state;
        bool await_ready() const noexcept { return state->active == 0; }
        void await_suspend(std::coroutine_handle<> h) { state->drain_waiter = h; }
        void await_resume() const noexcept {}
    };
    static task<void> drain_impl(std::shared_ptr<http_dispatch> state) { co_await drain_awaiter{state}; }
    task<void> drain() { return task_access::bind(drain_impl(shared_from_this()), {}); }
    // A transport EOF removes network readability as a useful wake source.
    // Keep one context-affine, RAII-unlinked waiter for handler completion.
    struct completion_awaiter {
        std::shared_ptr<http_dispatch> state;
        std::coroutine_handle<> continuation{};
        ~completion_awaiter() {
            if (state->waiter == continuation) state->waiter = {};
        }
        bool await_ready() const noexcept {
            return !state->completed.empty() || state->failure || !state->open || state->io.stopped();
        }
        io_context::cancellation_handle registration{};
        std::error_code error{};
        template<class P> void await_suspend(std::coroutine_handle<P> handle) {
            if constexpr (requires { handle.promise().cancellation; }) {
                registration = state->io.on_cancel(handle.promise().cancellation, [this](std::error_code reason) {
                    error = reason;
                    if (auto h = std::exchange(state->waiter, {})) scheduler_access::schedule(state->io, h);
                });
            }
            if (state->waiter) throw std::logic_error("another handler-completion waiter is pending");
            continuation = handle; state->waiter = handle;
        }
        void await_resume() {
            registration.cancel();
            if (error) throw std::system_error(error);
            if (state->io.stopped() || !state->open)
                throw std::system_error(std::make_error_code(std::errc::operation_canceled));
        }
    };
    completion_awaiter wait_completion() { return {shared_from_this(), {}}; }
    void notify() {
        if (waiter) {
            const auto continuation = std::exchange(waiter, {});
            // Queue, never inline-resume, the protocol owner. yield's scheduling
            // path also accepts cancellation continuations after context.stop().
            io.yield().await_suspend(continuation);
        }
        if (wake) wake();
    }
    void start(std::int64_t id, http::request request) {
        ++active;
        try { io.spawn(invoke(shared_from_this(), id, std::move(request))); }
        catch (...) { --active; throw; }
    }
    static task<void> invoke(std::shared_ptr<http_dispatch> self, std::int64_t id, http::request request) {
        // A connection may close while this task is still queued to start.
        if (!self->open) { self->child_finished(); co_return; }
        result finished{id, {}, {}};
        try {
            if (request.method == "CONNECT")
                finished.response = {.status = 405, .body = "CONNECT is not supported\n"};
            else finished.response = co_await task_access::bind(self->handle(std::move(request)), self->source.token());
        } catch (const std::system_error& error) {
            if (error.code() == std::errc::operation_canceled) finished.error = std::current_exception();
            else finished.response = {.status = 500, .body = "Internal Server Error\n"};
        } catch (...) { finished.response = {.status = 500, .body = "Internal Server Error\n"}; }
        self->child_finished();
        if (!self->open) co_return;
        try { self->completed.push_back(std::move(finished)); }
        catch (...) { self->failure = std::current_exception(); }
        // Completion only schedules the owner; it must not reenter nghttp from
        // inside an application coroutine or one of its I/O callbacks.
        self->notify();
    }
};
struct close_http_dispatch {
    std::shared_ptr<http_dispatch> state;
    ~close_http_dispatch() { state->close(); }
};
} // namespace pace::detail
