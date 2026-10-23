#pragma once
#include <pace/http_types.hpp>
#include <pace/io_context.hpp>
#include <cstdint>
#include <deque>
#include <memory>

namespace pace::detail {
// One reactor owns this queue. Handler tasks never access a protocol session or
// transport; a connection may discard them safely without destroying awaiters
// that the application's handler has registered with its own I/O operations.
struct http_dispatch : std::enable_shared_from_this<http_dispatch> {
    struct result { std::int64_t id; http::response response; std::exception_ptr error; };
    io_context& io;
    http::handler handle;
    std::size_t active = 0;
    std::deque<result> completed;
    std::exception_ptr failure;
    std::function<void()> wake;
    bool open = true;

    http_dispatch(io_context& context, http::handler handler) : io(context), handle(std::move(handler)) {}
    void close() noexcept { open = false; wake = {}; completed.clear(); }
    void start(std::int64_t id, http::request request) {
        ++active;
        try { io.spawn(invoke(shared_from_this(), id, std::move(request))); }
        catch (...) { --active; throw; }
    }
    static task<void> invoke(std::shared_ptr<http_dispatch> self, std::int64_t id, http::request request) {
        // A connection may close while this task is still queued to start.
        if (!self->open) { --self->active; co_return; }
        result finished{id, {}, {}};
        try {
            if (request.method == "CONNECT")
                finished.response = {.status = 405, .body = "CONNECT is not supported\n"};
            else finished.response = co_await self->handle(std::move(request));
        } catch (const std::system_error& error) {
            if (error.code() == std::errc::operation_canceled) finished.error = std::current_exception();
            else finished.response = {.status = 500, .body = "Internal Server Error\n"};
        } catch (...) { finished.response = {.status = 500, .body = "Internal Server Error\n"}; }
        --self->active;
        if (!self->open) co_return;
        try { self->completed.push_back(std::move(finished)); }
        catch (...) { self->failure = std::current_exception(); }
        // Completion only schedules the owner; it must not reenter nghttp from
        // inside an application coroutine or one of its I/O callbacks.
        if (self->wake) self->wake();
    }
};
struct close_http_dispatch {
    std::shared_ptr<http_dispatch> state;
    ~close_http_dispatch() { state->close(); }
};
} // namespace pace::detail
