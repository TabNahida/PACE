# `pace::run` and the current context

[简体中文](run.zh.md) · [Runtime](README.md)

## Overview

`pace::run` runs one task to completion on a fresh `io_context` on the calling thread and returns its result; the
helpers next to it find the context a task is running on and turn an async call into a blocking one.

| Header | Target | Build option |
|:--|:--|:--|
| `<pace/runtime/run.hpp>` (also `<pace/runtime.hpp>`) | `pace_runtime` | always built |
| `<pace/runtime/this_context.hpp>` (also `<pace/runtime.hpp>`) | `pace_runtime` | always built |
| `<pace/runtime/blocking.hpp>` (also `<pace/runtime.hpp>`) | `pace_runtime` | always built |

`run` creates no threads and keeps no global loop: each call has its own context, which ends when the call
returns.

## Synopsis

```cpp
#include <pace/runtime.hpp>

#include <chrono>
#include <iostream>
#include <string>

using namespace std::chrono_literals;

int main()
{
    std::string name = "PACE";
    // Pass the lambda itself: run owns it, and its captures, until the context has drained.
    auto text = pace::run(
        [name]() -> pace::task<std::string>
        {
            co_await pace::sleep_for(1ms);
            co_return "Hello, " + name;
        }
    );
    std::cout << text << '\n';
}
```

## Types

### `blocking_options`

```cpp
struct blocking_options
{
    std::chrono::milliseconds timeout{30000}; ///< Total budget from the start of the call; must be positive.
    cancellation_token cancellation{};        ///< Cancels the call; may be cancelled from another thread.
    pace::deadline deadline{};                ///< Absolute deadline; none by default.
};
```

The options of every blocking ("direct") call, such as `http::get`, `tcp::exchange` and `udp::exchange`.

| Member | Meaning |
|:--|:--|
| `timeout` | A total time budget, measured from the start of the call. Must be positive. |
| `cancellation` | A token that cancels the call; it may be cancelled from another thread. |
| `deadline` | An absolute deadline. The call's budget ends at the earlier of `deadline` and now + `timeout`. |

The budget covers the whole call, for example name resolution, connecting, handshakes, the exchange and cleanup.
The async forms (`async_get`, `tcp::async_exchange`, …) take no `blocking_options` and add no deadline: they
inherit the caller's token and deadline, and [`with_options`](structured-concurrency.md) sets a local one.

### Entry support

```cpp
struct entry_access
{
    template <class Factory>
    static auto invoke(Factory& factory, io_context& io);
    template <class Task>
    using result = /* T for task<T> */;
    template <class T>
    struct state;                  // value (absent for void), failure, completed
};

template <class T, class Factory>
task<void> run_entry(io_context& io, Factory& factory, entry_access::state<T>& outcome);
```

Implementation support shared by `run`, operation boundaries and task groups. `entry_access::invoke` calls
`factory(io)` if that is well-formed and `factory()` otherwise, and `entry_access::result` names the result type of a
factory's task. `run_entry` is the root task `pace::run` spawns: it awaits the factory's task, records the value or
exception in `outcome`, and stops `io` when it finishes; it is a namespace-scope function because GCC 16 cannot read
class-scope coroutines back from a named-module interface. Not part of the API; applications use `pace::run`.

## Functions

### `run`

```cpp
template <class Factory>
requires(std::is_invocable_v<std::decay_t<Factory>&, io_context&> || std::is_invocable_v<std::decay_t<Factory>&>)
auto run(Factory&& factory);

template <class T>
auto run(task<T> entry);
```

Creates an `io_context`, runs the entry on it on the calling thread, and returns the entry's result: `T` for a
`task<T>` (moved out, so move-only results work), nothing for `task<void>`.

- **Factory form.** `factory` is a callable taking `io_context&` or nothing and returning a `task<T>`; if both
  calls are valid, the `io_context&` form is used. It is copied (lvalue) or moved (rvalue) into storage owned by
  `run`, invoked once, and kept alive until the context has drained. That makes a temporary capturing coroutine
  lambda safe here, as long as you pass the lambda and do not call it yourself.
- **Task form.** `entry` is an already-created task. `run` owns the task, but whatever the task borrowed stays
  borrowed: a task made by calling a temporary capturing lambda already points at a destroyed closure.

When the entry finishes, successfully or not, the context is stopped: pending waits and timers of background tasks
(roots spawned with `this_context().spawn(…)`) are cancelled, and ready work drains before `run` returns. This is not
a join of background work and not a graceful connection drain.

### `this_context`

```cpp
inline io_context& this_context();
```

The context running on the calling thread (`io_context::current()`). Throws `std::logic_error` when no PACE
context is running there; it never creates one. The convenience overloads of other modules that take no
`io_context&` (`tcp::connect(endpoint)`, `dns::resolve(name)`, …) use it, either when their task starts or, for
synchronous calls such as `tcp::listen`, when they are called.

### `sleep_for` and `yield`

```cpp
template <class Rep, class Period>
auto sleep_for(std::chrono::duration<Rep, Period> duration);

inline auto yield();
```

`this_context().sleep_for(duration)` and `this_context().yield()`: they resolve the context when called, throw
`std::logic_error` if none is running, and return the same awaiters as the [`io_context`](io-context.md)
members, without allocating. Await the result at once, on the same context.

### `blocking_call`

```cpp
template <class Factory>
auto blocking_call(Factory&& factory, blocking_options options);
```

Runs `factory`'s task with `pace::run` inside an operation boundary whose cancellation is `options.cancellation`
and whose deadline is the earlier of `options.deadline` and now + `options.timeout`, and returns its result. It is
how the blocking calls of the net and HTTP modules are built, and works for any factory `pace::run` accepts:

```cpp
auto value = pace::blocking_call(
    []() -> pace::task<int>
    {
        co_await pace::sleep_for(10ms);
        co_return 7;
    },
    {.timeout = 1s}
);
```

## Errors

| Error | When |
|:--|:--|
| `std::logic_error` | `run` or `blocking_call` called on a thread that is already running a PACE context (checked before the factory is invoked: await the task instead); `this_context`, `sleep_for` or `yield` called where no context is running |
| The entry's exception | The entry task threw, including a `std::system_error` for its own cancellation |
| `std::system_error(operation_canceled)` | The entry did not finish, for example because the context was stopped under it |
| A background error | A root spawned on the context failed; see the reactor error policy of [`io_context`](io-context.md#errors) |
| `std::invalid_argument` | `blocking_call` with `timeout <= 0` |
| `std::system_error(timed_out)` | `blocking_call`'s budget ran out, including when the work finished but only after the deadline |
| `std::system_error(operation_canceled)` | `blocking_call`'s `options.cancellation` was cancelled |

`run` reports one failure. When the entry and a background task both fail, which one is rethrown is not specified.
Unlike `io_context::run`, which treats `operation_canceled` during shutdown as expected, `run` reports the entry's
cancellation to its caller.

A blocking call's budget is cooperative: CPU work, synchronous setup such as loading trust stores, and blocking
foreign code are not interrupted and can make the call take longer than the budget. Nothing is retried, and bytes
already sent are not taken back. No background work outlives the call.

## Threads and concurrency

`run` and `blocking_call` block the calling thread and run everything on it; no worker thread is created. They
refuse to run on a thread that is already running a PACE context, but they do not detect other event loops: called
from a GUI or another library's loop thread, they block that loop. Separate calls on separate threads are
independent.

`this_context`, `sleep_for` and `yield` read the calling thread's current context; an awaiter they return belongs to
that context and must be awaited there.

## Lifetime and ownership

- `run` owns the factory until shutdown has finished, including after an error.
- The context ends when `run` returns. Return owned values (`std::string`, structs of data), not sockets, streams or
  views that refer to that context or to the factory's captures.
- Background roots must own their data: the entry's locals can be destroyed before those roots observe the
  cancellation.
- `blocking_options::cancellation` is copied; its source may be cancelled from any thread while the call runs.

## Limits and defaults

| Item | Value |
|:--|:--|
| `blocking_options::timeout` | 30 s (30000 ms); must be positive |
| `blocking_options::deadline` | none |
| `blocking_options::cancellation` | none (empty token) |
| Threads created by `run` | 0 |
| Nested `run` | rejected |

## See also

- [`io_context`](io-context.md): what `run` creates and stops
- [`task`](task.md): ownership of task arguments
- [Structured concurrency](structured-concurrency.md): `with_options`, deadlines and cancellation
- [HTTP clients](../http/clients.md), [TCP](../net/tcp.md), [UDP](../net/udp.md): the blocking calls that take
  `blocking_options`
- [Tutorial: tasks and timers](../../tutorial/04-tasks-and-timers.md)
