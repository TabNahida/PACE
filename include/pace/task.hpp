#pragma once

#include <coroutine>
#include <exception>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace pace {

namespace detail {
struct promise_base {
    std::coroutine_handle<> continuation = std::noop_coroutine();
    std::exception_ptr exception;
    std::coroutine_handle<> completed_next{};
    void* completion_context = nullptr;
    void (*on_completion)(void*, std::coroutine_handle<>) noexcept = nullptr;

    std::suspend_always initial_suspend() const noexcept { return {}; }
    void unhandled_exception() noexcept { exception = std::current_exception(); }
    struct final_awaiter {
        bool await_ready() const noexcept { return false; }
        template<class P>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) const noexcept {
            auto& p = h.promise();
            if (p.on_completion) p.on_completion(p.completion_context, h);
            return p.continuation;
        }
        void await_resume() const noexcept {}
    };
    final_awaiter final_suspend() const noexcept { return {}; }
};
}

// Lazy, move-only, single-consumer coroutine. An rvalue co_await transfers ownership.
template<class T = void>
class [[nodiscard]] task {
public:
    struct promise_type : detail::promise_base {
        std::optional<T> value;
        task get_return_object() noexcept { return task{handle_type::from_promise(*this)}; }
        template<class U> void return_value(U&& result) { value.emplace(std::forward<U>(result)); }
    };
    using handle_type = std::coroutine_handle<promise_type>;
    task() noexcept = default;
    task(task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    task& operator=(task&& other) noexcept {
        if (this != &other) { if (handle_) handle_.destroy(); handle_ = std::exchange(other.handle_, {}); }
        return *this;
    }
    task(const task&) = delete;
    ~task() { if (handle_) handle_.destroy(); }
    handle_type release() noexcept { return std::exchange(handle_, {}); }

    struct awaiter {
        handle_type handle;
        awaiter(handle_type h) noexcept : handle(h) {}
        awaiter(awaiter&& other) noexcept : handle(std::exchange(other.handle, {})) {}
        ~awaiter() { if (handle) handle.destroy(); }
        bool await_ready() const noexcept { return !handle || handle.done(); }
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> parent) noexcept {
            handle.promise().continuation = parent;
            return handle;
        }
        T await_resume() {
            if (!handle) throw std::logic_error("cannot await an empty task");
            if (handle.promise().exception) std::rethrow_exception(handle.promise().exception);
            return std::move(*handle.promise().value);
        }
    };
    awaiter operator co_await() && noexcept { return awaiter{release()}; }
private:
    explicit task(handle_type handle) noexcept : handle_(handle) {}
    handle_type handle_{};
};

template<>
class [[nodiscard]] task<void> {
public:
    struct promise_type : detail::promise_base {
        task get_return_object() noexcept { return task{handle_type::from_promise(*this)}; }
        void return_void() const noexcept {}
    };
    using handle_type = std::coroutine_handle<promise_type>;
    task() noexcept = default;
    task(task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    task& operator=(task&& other) noexcept {
        if (this != &other) { if (handle_) handle_.destroy(); handle_ = std::exchange(other.handle_, {}); }
        return *this;
    }
    task(const task&) = delete;
    ~task() { if (handle_) handle_.destroy(); }
    handle_type release() noexcept { return std::exchange(handle_, {}); }
    struct awaiter {
        handle_type handle;
        awaiter(handle_type h) noexcept : handle(h) {}
        awaiter(awaiter&& other) noexcept : handle(std::exchange(other.handle, {})) {}
        ~awaiter() { if (handle) handle.destroy(); }
        bool await_ready() const noexcept { return !handle || handle.done(); }
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> parent) noexcept {
            handle.promise().continuation = parent;
            return handle;
        }
        void await_resume() {
            if (!handle) throw std::logic_error("cannot await an empty task");
            if (handle.promise().exception) std::rethrow_exception(handle.promise().exception);
        }
    };
    awaiter operator co_await() && noexcept { return awaiter{release()}; }
private:
    explicit task(handle_type handle) noexcept : handle_(handle) {}
    handle_type handle_{};
};

} // namespace pace
