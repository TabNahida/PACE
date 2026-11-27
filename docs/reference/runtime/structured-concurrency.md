# Structured concurrency, cancellation and deadlines

[简体中文](structured-concurrency.zh.md) · [Runtime](README.md)

## Overview

Task groups run child tasks concurrently inside a scope that joins them; cancellation tokens and deadlines stop
work cooperatively without stopping the context; operation boundaries attach a token and a deadline to any task.

| Header | Target | Build option |
|:--|:--|:--|
| `<pace/runtime/task_group.hpp>` (also `<pace/runtime.hpp>`) | `pace_runtime` | always built |
| `<pace/runtime/operation.hpp>` (also `<pace/runtime.hpp>`) | `pace_runtime` | always built |
| `<pace/runtime/cancellation.hpp>` (also `<pace/runtime.hpp>`) | `pace_runtime` | always built |

Every task carries a `cancellation_token`. Awaiting a task passes the caller's token down; an operation boundary
(`with_options`, `with_deadline`, `with_cancellation`, `with_task_group`) makes a new token linked to it, with the
earliest of the inherited and new deadlines. Cancelling a token wakes the waits registered under it with
`std::system_error` (`operation_canceled`, or `timed_out` for a deadline); the context keeps running.

## Synopsis

```cpp
#include <pace/runtime.hpp>

#include <chrono>
#include <iostream>
#include <string>

using namespace std::chrono_literals;

pace::task<std::string> fetch_both()
{
    co_return co_await pace::with_task_group(
        [](pace::task_group& group) -> pace::task<std::string>
        {
            auto first = group.spawn(
                []() -> pace::task<std::string>
                {
                    co_await pace::sleep_for(10ms);
                    co_return "Hello";
                }
            );
            auto second = group.spawn(
                []() -> pace::task<std::string>
                {
                    co_await pace::sleep_for(5ms);
                    co_return "structured PACE";
                }
            );
            auto a = co_await first; // rethrows the child's exception, if any
            auto b = co_await second;
            co_return a + ", " + b + "!";
        },
        {.deadline = pace::deadline::after(2s)} // one budget for the whole group
    );
}

int main()
{
    std::cout << pace::run(fetch_both) << '\n';
}
```

## Types

### `deadline`

```cpp
class deadline
{
public:
    using clock = std::chrono::steady_clock;
    deadline() noexcept = default;
    explicit deadline(clock::time_point when) noexcept;
    template <class Rep, class Period>
    static deadline after(std::chrono::duration<Rep, Period> delay) noexcept;
    clock::time_point time() const noexcept;
    bool expired() const noexcept;
    friend deadline earlier(deadline a, deadline b) noexcept;
};
```

An absolute point on `steady_clock`.

| Member | Behaviour |
|:--|:--|
| `deadline()` | No deadline: `time()` is `clock::time_point::max()` and it never expires. |
| `deadline(when)` | Expires at `when`. |
| `after(delay)` | Now + `delay`, computed when called. Zero and negative delays give a deadline that has already expired; a delay too large for the clock gives no deadline. |
| `time()` | The time point (`max()` for none). |
| `expired()` | Whether the deadline exists and has passed. |
| `earlier(a, b)` | The earlier of two deadlines; found by argument-dependent lookup. |

### `cancellation_token`

```cpp
class cancellation_token
{
public:
    cancellation_token() noexcept = default;
    bool stop_possible() const noexcept;
    std::error_code error() const noexcept;
    bool stop_requested() const noexcept;
    void throw_if_cancelled() const;
    pace::deadline expires() const noexcept;
};
```

A cheap, copyable view of one `cancellation_source`. A default-constructed token is empty: it can never be
cancelled.

| Member | Behaviour |
|:--|:--|
| `stop_possible()` | Whether the token belongs to a source. |
| `error()` | The source's reason (`operation_canceled` or `timed_out`), else `timed_out` if the source's deadline has passed, else an empty code. |
| `stop_requested()` | `error()` is non-empty. |
| `throw_if_cancelled()` | Throws `std::system_error(error())` if `stop_requested()`. |
| `expires()` | The source's deadline (none for an empty token). |

A passed deadline is seen whenever the token is checked, but nothing wakes a suspended wait at that moment: the
timer that does is armed by an operation boundary.

### `cancellation_source`

```cpp
class cancellation_source
{
public:
    explicit cancellation_source(pace::deadline expires = {});
    cancellation_token token() const noexcept;
    bool request_cancellation() const noexcept;
    bool request_timeout() const noexcept;
    void link(cancellation_token parent);
};
```

The owner of a cancellation state. Copies share the same state.

| Member | Behaviour |
|:--|:--|
| `cancellation_source(expires)` | A new state with deadline `expires`. |
| `token()` | A token observing this source. |
| `request_cancellation()` | Cancels with `operation_canceled`. Returns `true` if this call was the first request; later requests change nothing (the first reason wins). Thread-safe. |
| `request_timeout()` | The same with `timed_out`; used by deadline timers. |
| `link(parent)` | Cancels this source when `parent` is cancelled, with `timed_out` if the parent's reason is `timed_out` and `operation_canceled` otherwise; immediately if `parent` already is. One way only: cancelling this source does not affect `parent`. Does not copy the parent's deadline. An empty `parent` is ignored; linking a source to its own token throws `std::invalid_argument`. |

Requests may come from any thread. `link` is not synchronized: link before the source is shared or work starts.

### `cancellation_registration`

```cpp
class cancellation_registration
{
public:
    cancellation_registration(const cancellation_token&, std::function<void()>);
    ~cancellation_registration();
    cancellation_registration(const cancellation_registration&) = delete;
    cancellation_registration& operator=(const cancellation_registration&) = delete;
};
```

The low-level subscription that `link` and `io_context::on_cancel` are built on. The callback runs once, on the
thread that requests cancellation, or inside the constructor if the token is already cancelled; for an empty token
nothing is registered. It must not throw and should only record state and wake something. The destructor
unregisters the callback and, if it is running on another thread, waits for it to return; a callback may destroy
its own registration. Custom awaiters should use [`io_context::on_cancel`](io-context.md), which runs the callback
on the context instead.

### `operation_options`

```cpp
struct operation_options
{
    cancellation_token cancellation{};
    pace::deadline deadline{};
};
```

The token and deadline an operation boundary adds to the ones it inherits. Both default to none.

### `task_group`

```cpp
class task_group
{
public:
    task_group(const task_group&) = delete;
    task_group& operator=(const task_group&) = delete;
    ~task_group() = default;

    template <class Factory>
    auto spawn(Factory factory);           // join_handle<T>
    template <class T>
    auto spawn(task<T> work);              // join_handle<T>
    template <class T>
    T& own(T value);
    void cancel();
    cancellation_token token() const noexcept;
    task<void> join();
    template <class Factory>
    static auto run(Factory factory) -> task<entry_access::result<std::invoke_result_t<Factory&, task_group&>>>;
};
```

A scope of child tasks on one context. Only `task_group::run` and `with_task_group` create one, and they hand it
to the body by reference; it cannot be copied or moved, so it cannot outlive the scope that joins its children.

| Member | Behaviour |
|:--|:--|
| `spawn(factory)` | Takes ownership of `factory` (callable with `io_context&` or nothing, returning `task<T>`), starts the child on this context on a later turn, and returns a `join_handle<T>`. The child's token is the group's. |
| `spawn(work)` | The same for an already-created task. The group owns the task; what the task borrowed stays borrowed. |
| `own(value)` | Moves `value` into storage the group keeps until all children have finished, and returns a reference to it. Use it for data that children borrow. |
| `cancel()` | Cancels the group's token: the body and every child see `operation_canceled` at their next wait. The scope still joins them, then ends normally: the `operation_canceled` this causes is the requested outcome, not a failure. Use it to stop the remaining children once one has the answer. |
| `token()` | The group's token. |
| `join()` | Closes the group (no more `spawn` or `own`) as soon as it is called and waits until every child has finished. The wait observes the group's token, so a cancelled group makes `join()` throw instead of waiting; the group is closed either way. It does not rethrow children's exceptions. |
| `run(factory)` | The scope itself, a lazy task; see `with_task_group`. |

### `join_handle<T>`

```cpp
template <class T>
class [[nodiscard]] join_handle
{
public:
    join_handle(join_handle&&) noexcept = default;
    join_handle& operator=(join_handle&&) noexcept = default;
    join_handle(const join_handle&) = delete;
    join_handle& operator=(const join_handle&) = delete;
    ~join_handle() = default;
    struct awaiter;
    awaiter operator co_await() &;
    awaiter operator co_await() &&;
};
```

The result of one child. `co_await handle` waits for the child and returns its value or rethrows its exception.
A handle can be awaited once, on the group's context; awaiting moves the result out and empties the handle.
Dropping a handle neither detaches nor cancels the child: the group still joins it, and its failure still fails
the group. The nested `awaiter` (`await_ready`, `await_suspend`, `await_resume` over the child's shared state)
is opaque.

### Implementation support

```cpp
template <class Factory>
using operation_result = /* T of the task<T> that Factory returns */;

struct group_access
{
    struct state;                                  // the group's shared state
    template <class T> struct child;               // one child's shared state
    template <class Factory> struct owned_child;   // child state plus its owned factory
    template <class Factory>
    static task<void> run_child(std::shared_ptr<state> group, std::shared_ptr<owned_child<Factory>> owned);
    struct wait;                                   // waits until no child is running
    static task<void> drain(std::shared_ptr<state> group, bool boundary = false);
};

struct operation_access
{
    template <class Factory>
    static task<operation_result<Factory>> scoped(Factory factory, operation_options options);  // with_options
};
```

Declared in the public headers because the templates above use them, gathered in two structs so that they add two
names to `pace`: the shared state of a group and of each child, the root task that runs a child, the wait for a
group to drain, and the coroutine behind `with_options`. They are not part of the API. `operation_result<Factory>` is the result type a boundary or `spawn` returns for `Factory`.

## Functions

### `with_options`, `with_deadline`, `with_cancellation`

```cpp
template <class Factory>
auto with_options(Factory factory, operation_options options);
template <class T>
auto with_options(task<T> work, operation_options options);
template <class Work>
auto with_deadline(pace::deadline when, Work work);
template <class Work>
auto with_cancellation(cancellation_token token, Work work);
```

An operation boundary: a lazy `task<T>` that runs `work` (a factory callable with `io_context&` or nothing, or a
`task<T>`) under a new cancellation source and returns its result. `with_deadline` and `with_cancellation` are
`with_options` with one field set.

When the boundary starts it

1. makes a source whose deadline is the earliest of the inherited token's deadline, `options.cancellation`'s and
   `options.deadline`, so a nested boundary can shorten the budget but never extend it;
2. links it to the inherited token and to `options.cancellation`, so either cancels the work;
3. throws at once if the result is already cancelled (the work is never invoked);
4. arms a callback timer on the current context that requests `timed_out` at the deadline;
5. runs the work with the new token, and after it finishes throws if the source was cancelled or the deadline has
   passed, even if the work returned a value.

Cancellation flows only inward: an inner deadline expiring does not cancel the caller, which simply receives the
`timed_out` exception. The boundary owns `work` until it finishes; pass a capturing lambda itself rather than the
task from calling it. The `async_` calls of other modules add no total deadline of their own; wrap them in a
boundary to bound them. Protocol phase timeouts can still fail sooner, but nothing extends the boundary's budget.

### `shield`

```cpp
template <class Work>
auto shield(Work work);
```

Runs `work` with no inherited token and no inherited deadline, for cleanup that must run even though the caller is
being cancelled. `io_context::stop()` still applies. Bound it with a deadline of its own:

```cpp
co_await pace::shield(pace::with_deadline(pace::deadline::after(100ms), send_goodbye));
```

`shield` does not detach: the caller waits for the cleanup, and its own cancellation is reported at its next
cancellation point afterwards. Prefer synchronous RAII cleanup when it is enough.

### `with_task_group`

```cpp
template <class Factory>
auto with_task_group(Factory factory, operation_options options = {});
```

A lazy `task<T>` that creates a `task_group` on the current context, runs `factory(group)` (which returns
`task<T>`) as the body, joins every child, and returns the body's result. With non-empty `options` the scope runs
inside `with_options`. `task_group::run(factory)` is the same scope without options.

How the scope ends:

1. The group's token is linked to the inherited token and has its deadline.
2. The first exception that escapes the body or a child is recorded and cancels the group, so the siblings and the
   body see `operation_canceled`. Later exceptions are dropped; errors are not aggregated.
3. When the body returns or throws, the scope waits for every child, ignoring cancellation for that wait.
4. It then destroys the storage made by `own`.
5. It rethrows the recorded exception; otherwise, if the enclosing operation was cancelled (the inherited token or
   the deadline), it throws `std::system_error` with that reason; otherwise it returns the body's value.

The group's own `cancel()` is not an error. `operation_canceled` raised by the body or a child because of it is not
recorded, and the scope returns normally: a `void` body's scope returns, and a value-returning body's scope returns
the value the body produced. If the body ended without a value (it was itself cancelled), the scope throws
`operation_canceled`. Another exception from the body or a child is still recorded and rethrown. This makes
"first to finish wins" a group that the winning child cancels:

```cpp
auto winner = co_await pace::task_group::run(
    [&](pace::task_group& group) -> pace::task<std::string>
    {
        auto& answer = group.own(std::string{});
        for (auto& mirror : mirrors)
        {
            (void)group.spawn(
                [&group, &answer, &mirror]() -> pace::task<void>
                {
                    answer = (co_await pace::http::async_get(mirror)).body;
                    group.cancel();
                }
            );
        }
        try
        {
            co_await group.join();
        }
        catch (const std::system_error&)
        {
            // join reports the cancellation that ended the race
        }
        co_return answer;
    }
);
```

Because a child's exception propagates out of the scope, a failure in a nested group fails the child that ran it;
catch errors inside a child to contain them.

## Errors

| Error | When |
|:--|:--|
| `std::system_error(operation_canceled)` | A wait whose token was cancelled by a source, a linked parent, `task_group::cancel()` or a sibling's failure; `spawn` on a cancelled group |
| `std::system_error(timed_out)` | A boundary's deadline passed, during the work or before its result was returned |
| The first exception of the body or a child | Rethrown by `with_task_group` after all children have finished |
| `std::logic_error` | `spawn`, `own`, `cancel`, `join` or awaiting a `join_handle` off the group's context; `spawn` or `own` after `join` started; a second concurrent `join`; the body returning while a `join` (for example from a child) is still pending; awaiting an empty or already-awaited `join_handle` |
| `std::invalid_argument` | `cancellation_source::link` with the source's own token |

Cancellation is not rollback. A cancelled read does not cancel a write on the same descriptor, sockets stay open,
and bytes may already have been sent or received. Each protocol's page says what cancellation leaves behind for its
objects.

## Threads and concurrency

- `cancellation_source::request_cancellation` and `request_timeout` may be called from any thread. The callbacks
  PACE registers for a request only mark work and wake the owning reactor; waits are unregistered and resumed on their own
  context.
- Everything else here (boundaries, groups, handles) runs on one context. A group is cooperative concurrency on one
  reactor, not a thread pool: a child that blocks or loops holds every other child.
- Cancellation is cooperative. It cannot interrupt CPU work, OS scheduling or blocking foreign code, and a child
  that never reaches a cancellation point keeps its scope from finishing.
- When a wait completes and its token is cancelled by the time the task resumes, the cancellation wins over the
  result.
- Do not join a group from one of its own children: it would wait for itself. The scope rejects this when the body
  exits, after cancelling and draining the children.
- `io_context::spawn` remains an unstructured root: it inherits no token and nobody joins it. Use
  `task_group::spawn` for work that belongs to a scope.

## Lifetime and ownership

- Boundaries and groups own their factories until the work finishes; a child's factory and its captures are
  destroyed before the join completes.
- The caller's data outside the whole scope may be borrowed: the caller is suspended until the scope returns.
- The body's locals are destroyed when the body returns or throws, possibly before the children finish. Children
  must capture by value or borrow storage made by `group.own`.
- Return owned values from a scope, not views into its storage.
- Never destroy a running coroutine to cancel it: request cancellation and join it.

## Limits and defaults

| Item | Value |
|:--|:--|
| `operation_options` | no token, no deadline |
| `cancellation_source` deadline | none |
| `with_task_group` options | none |
| Children per group | no fixed limit |
| Awaits per `join_handle` | 1 |
| Errors kept per group | the first |

## See also

- [`task`](task.md): token inheritance on `co_await`
- [`io_context`](io-context.md): `on_cancel` and callback timers for custom awaiters
- [`pace::run`](run.md): `blocking_options` and `blocking_call`
- [Tutorial: structured concurrency](../../tutorial/07-structured-concurrency.md) ·
  [graceful shutdown](../../tutorial/09-graceful-shutdown.md)
- [Execution model](../../explanation/execution-model.md)
