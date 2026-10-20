# `pace::task<T>`

Include `<pace/task.hpp>`. `task<T = void>` is a lazy, move-only coroutine result
with one consumer. Creating a task allocates its coroutine frame as needed and
copies/moves parameters into it; its body starts only when awaited or spawned.
`T` must be suitable for storage in `std::optional<T>`; references are not supported.
Use `task<void>` for operations without a result.

```cpp
template<class T = void> class task;
template<> class task<void>;
```

Both specializations expose these ownership operations:

```cpp
task() noexcept;                         // Empty; no coroutine state.
task(task&& other) noexcept;
task& operator=(task&& other) noexcept;
task(const task&) = delete;
~task();
handle_type release() noexcept;
awaiter operator co_await() && noexcept;
```

Copying is unavailable. Moving empties the source; move assignment first destroys
any state already owned by the destination. Destruction destroys a still-owned
frame, including its parameter and local objects, without executing an unstarted
body. It does not wait for external work. Destroying a coroutine that still has
an operation registered in a context is not a supported cancellation mechanism.

`co_await` accepts an rvalue and transfers frame ownership to an awaiter. To await
a named task use `co_await std::move(work)`. There is no lvalue await, `get()`,
`result()`, `done()` or implicit conversion to `bool` on `task`.

```cpp
pace::task<int> answer() { co_return 42; }

pace::task<void> use_answer(pace::io_context& io) {
    auto pending = answer();
    const int value = co_await std::move(pending);
    if (value != 42) throw std::runtime_error("wrong answer");
    io.stop();
}
```

Awaiting an empty or already-consumed task throws `std::logic_error`. An exception
thrown in the child body is stored and rethrown by `await_resume()`. Successful
`task<T>` completion moves its value out; `task<void>` returns nothing. The child
frame is then destroyed by the owning awaiter. A child that completes without
suspending may continue directly into its parent; awaiting a task is not a promise
of a scheduler turn or a change of thread.

`io_context::spawn` accepts only `task<void>`. Wrap a value-returning child in a
named `task<void>` that awaits and consumes its result. A task carries no executor
of its own: it inherits execution from the parent or root context. Operations
inside it must still belong to that context.

## Compiler-facing members

These members are public to implement the coroutine protocol. Application code
normally uses `co_return`, `co_await` and `spawn` instead of calling them directly.

```cpp
struct promise_type;  // Nested in each specialization.
using handle_type = std::coroutine_handle<promise_type>;

// task<T>::promise_type
std::optional<T> value;
task get_return_object() noexcept;
template<class U> void return_value(U&& result);

// task<void>::promise_type
task get_return_object() noexcept;
void return_void() const noexcept;
```

`return_value` constructs `value` from the forwarded operand of `co_return`.
The promise derives from an internal base that initially suspends, captures
unhandled exceptions, and transfers control to the parent at final suspension.
Its continuation and completion bookkeeping are implementation details, not an
application scheduling interface.

Both nested awaiters expose:

```cpp
struct awaiter {
    handle_type handle;
    awaiter(handle_type h) noexcept;
    awaiter(awaiter&& other) noexcept;
    ~awaiter();
    bool await_ready() const noexcept;
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> parent) noexcept;
    T await_resume();  // void for task<void>.
};
```

The awaiter is an owner: moving transfers its handle, and destruction destroys
that handle's frame. `await_ready` is true for an empty or completed handle.
`await_suspend` installs the parent continuation and transfers execution to the
child. `await_resume` checks for an empty state and stored exception before
returning the result. Do not construct multiple owning awaiters for one handle.

`release()` is a low-level ownership escape hatch. It empties the task and returns
the raw coroutine handle, possibly empty. The caller must then arrange safe
resumption, completion and exactly one destruction. It neither schedules the task
nor detaches it safely. Do not destroy a released frame while its waits are
registered or while another thread may resume it.

## Lifetimes

Passing `std::string` or a `tcp::stream` by value to a named coroutine can transfer
ownership into its frame. Passing `std::string_view`, `std::span`, `T&` or a pointer
does not extend the referenced object's lifetime. A temporary capturing coroutine
lambda can leave its coroutine referring to a destroyed closure. Prefer the
named-function pattern in [tasks and timers](../tutorial/02-tasks-timers.md).
