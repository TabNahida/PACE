# `pace::task`

[简体中文](task.zh.md) · [Runtime](README.md)

## Overview

`task<T>` is the coroutine type of every PACE asynchronous call: a lazy, move-only result with exactly one
consumer.

| Header | Target | Build option |
|:--|:--|:--|
| `<pace/runtime/task.hpp>` (also `<pace/runtime.hpp>`) | `pace_runtime` | always built |

A task does nothing when it is created. Its body starts when it is awaited (`co_await std::move(work)`) or handed
to a context as a root (`io_context::spawn`, `pace::run`, `task_group::spawn`). It carries no executor of its own:
it runs on the context of whoever starts it.

## Synopsis

```cpp
#include <pace/runtime.hpp>

#include <chrono>
#include <string>

using namespace std::chrono_literals;

pace::task<std::string> greet(std::string name) // name is owned by the frame
{
    co_await pace::sleep_for(5ms);
    co_return "Hello, " + name;
}

pace::task<std::size_t> entry()
{
    auto pending = greet("PACE"); // lazy: nothing has run yet
    const auto text = co_await std::move(pending);
    co_return text.size();
}

int main()
{
    return pace::run(entry) == 11 ? 0 : 1;
}
```

## Types

### `task<T>` and `task<void>`

```cpp
template <class T = void>
class [[nodiscard]] task;
template <>
class [[nodiscard]] task<void>;
```

`T` is stored in a `std::optional<T>`, so it must be an object type that `std::optional` accepts; references are
not supported. Both specializations have the same members:

```cpp
using handle_type = std::coroutine_handle<promise_type>;
task() noexcept;
task(task&& other) noexcept;
task& operator=(task&& other) noexcept;
task(const task&) = delete;
task& operator=(const task&) = delete;
~task();
handle_type release() noexcept;
awaiter operator co_await() && noexcept;
```

| Member | Behaviour |
|:--|:--|
| `task()` | An empty task: no coroutine frame. Awaiting it throws `std::logic_error`. |
| move constructor | Takes the frame; the source becomes empty. |
| move assignment | Destroys the frame the destination owned, then takes the source's. |
| `~task()` | Destroys a frame it still owns, with its parameters and locals, without running an unstarted body. It does not wait for anything. |
| `release()` | Empties the task and returns its raw handle (possibly empty). The caller must then resume, complete and destroy the frame exactly once. It neither schedules nor safely detaches the task. |
| `operator co_await() &&` | Moves the frame into an owning `awaiter`. Only an rvalue can be awaited: write `co_await std::move(work)` for a named task. |

There is no lvalue await, `get()`, `result()`, `done()` or conversion to `bool`.

### `task<T>::awaiter`

```cpp
struct awaiter
{
    handle_type handle;
    awaiter(handle_type h) noexcept;
    awaiter(awaiter&& other) noexcept;
    awaiter(const awaiter&) = delete;
    awaiter& operator=(const awaiter&) = delete;
    awaiter& operator=(awaiter&&) = delete;
    ~awaiter();
    bool await_ready() const noexcept;
    template <class P>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<P> parent);
    T await_resume(); // void for task<void>
};
```

The awaiter owns the frame and destroys it when the `co_await` expression ends.

- `await_ready` is true for an empty or already finished frame.
- `await_suspend` gives the child the parent's cancellation token (unless the child was bound to its own token by
  an operation boundary), throws `std::system_error` if that token is already cancelled, so the child body never
  starts, and otherwise transfers control to the child (symmetric transfer).
- `await_resume` throws `std::logic_error` for an empty task, rethrows an exception that escaped the child body,
  throws `std::system_error` if the child's token was cancelled by the time it finished, and otherwise moves the
  result out.

The last rule means a cancellation observed when a child finishes wins over the value it returned.

### `promise_type`

```cpp
// task<T>::promise_type
struct promise_type : promise_base
{
    std::optional<T> value;
    task get_return_object() noexcept;
    template <class U>
    void return_value(U&& result);
};

// task<void>::promise_type
struct promise_type : promise_base
{
    std::size_t root_index = 0; // used only while io_context::spawn owns the frame
    task get_return_object() noexcept;
    void return_void() const noexcept;
};
```

The compiler uses these to build the coroutine. `return_value` constructs `value` from the operand of
`co_return`.

### `promise_base`

```cpp
struct promise_base
{
    cancellation_token cancellation;
    bool cancellation_bound = false;
    std::coroutine_handle<> continuation = std::noop_coroutine();
    std::exception_ptr exception;
    std::coroutine_handle<> completed_next{};
    void* completion_context = nullptr;
    void (*on_completion)(void*, std::coroutine_handle<>) noexcept = nullptr;
    std::suspend_always initial_suspend() const noexcept;
    void unhandled_exception() noexcept;
    struct final_awaiter;
    final_awaiter final_suspend() const noexcept;
};
```

The shared promise base. `initial_suspend` makes every task lazy; `unhandled_exception` stores the escaping
exception for `await_resume`; `final_suspend` resumes `continuation` (the awaiting parent, or nothing for a root).
`cancellation` is the token the frame observes; `this_task::cancellation()` reads it. The other fields are
bookkeeping for `io_context` roots. Application code does not touch them.

### `task_access`

```cpp
struct task_access
{
    template <class T>
    static task<T> bind(task<T> work, cancellation_token token);
};
```

Implementation support for operation boundaries and task groups: `bind` gives an unstarted task its own token,
which it then keeps instead of inheriting its parent's. Use [`with_cancellation`](structured-concurrency.md) or
[`shield`](structured-concurrency.md) instead.

## Functions

### `this_task::cancellation`

```cpp
namespace pace::this_task
{
cancellation_awaiter cancellation() noexcept;
}
```

`co_await pace::this_task::cancellation()` returns a copy of the current task's `cancellation_token` without
suspending and without throwing, even if the token is cancelled. Use it to check `stop_requested()` during cleanup
or to pass the token to a custom wait. In a coroutine whose promise has no PACE token the result is an empty token.
The awaiter type
`this_task::cancellation_awaiter` has a public `cancellation_token value` member and the usual
`await_ready`/`await_suspend`/`await_resume`; treat it as opaque.

## Errors

| Situation | Thrown by |
|:--|:--|
| Awaiting an empty or moved-from task | `std::logic_error` from `co_await` |
| The child body throws | The same exception, rethrown from `co_await` |
| The inherited token is cancelled before the child starts, or by the time it finishes | `std::system_error` with the token's code (`std::errc::operation_canceled`, or `std::errc::timed_out` for a deadline) |

An exception escaping a root task (one given to `io_context::spawn`) is not lost: the context records it and stops,
see [`io_context`](io-context.md#errors).

## Threads and concurrency

A task is not tied to a thread until it starts; from then on it runs on the context that resumed it. Awaiting a
task does not switch threads and does not promise a scheduler turn: a child that finishes without suspending
continues straight into its parent. Everything a task awaits must belong to the context it runs on.

A task has one consumer. Awaiting it consumes it; there is no way to await the same task twice or from two
coroutines.

## Lifetime and ownership

- Arguments passed by value (`std::string`, `tcp::stream`, …) are moved into the coroutine frame and live until the
  task finishes or is destroyed.
- References, pointers, `std::span` and `std::string_view` parameters are borrowed: the frame does not keep the
  object alive, so the referenced object must outlive the task.
- A lambda coroutine's captures live in the closure object, not in the frame. Calling a temporary capturing lambda
  (`[x] () -> task<void> { … }()`) leaves the task pointing at a destroyed closure. Pass the lambda itself to an API
  that owns it (`pace::run`, `with_options`, `task_group::spawn`), or use a named coroutine function with value
  parameters.
- Destroying a task that has not started is safe. Destroying a frame that is suspended inside a registered wait is
  not a way to cancel it: request cancellation and let it finish instead.

## Limits and defaults

| Item | Value |
|:--|:--|
| Default `T` | `void` |
| Consumers per task | 1 |
| Supported `T` | object types accepted by `std::optional<T>` |

## See also

- [`io_context`](io-context.md): running root tasks, timers and readiness waits
- [`pace::run`](run.md): run one task to completion from `main`
- [Structured concurrency](structured-concurrency.md): tokens, deadlines and task groups
- [Tutorial: tasks and timers](../../tutorial/04-tasks-and-timers.md)
- [Ownership](../../explanation/ownership.md)
