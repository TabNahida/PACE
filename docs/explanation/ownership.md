# Ownership and lifetimes

[简体中文](ownership.zh.md) · [Explanation](README.md)

PACE never keeps an object alive behind your back. There is no hidden reference counting on sockets, buffers or
handlers: whoever holds a value owns it, and borrowing is visible in the type. This page explains the rules that
follow from that and the mistakes they prevent. The rules themselves are summarized in the
[runtime reference](../reference/runtime/README.md); the mechanics are in [Architecture](architecture.md#tasks-and-object-ownership).

## Why explicit ownership

Implicit sharing (`shared_ptr` around every connection, callbacks that capture it) is convenient and costs an
atomic increment and decrement on every hand-off, a heap allocation per object and, worse, lifetimes nobody can
read from the code. A connection that "lives as long as someone still references it" is a connection whose
shutdown order is accidental. PACE uses coroutine frames instead: the frame of the task that serves a connection
owns the socket, the buffers and the parsed request, and they are destroyed, in a known order, when that task ends.

## Tasks are lazy and consumed once

Calling a coroutine function creates its frame but runs nothing. The task starts when it is awaited or spawned,
and either consumes it. A `task` is move-only, and an lvalue task cannot be awaited:

```cpp
auto pending = stream.read_some(buffer);
co_await std::move(pending);   // explicit: this task is used up here
```

Laziness is what makes the rest predictable: nothing runs before its owner decides, so there is never a task
racing ahead of the code that is supposed to own its result.

## What a coroutine frame owns

Parameters passed **by value** are copied or moved into the frame and live as long as the task. Parameters passed
by reference, pointer, `std::span` or `std::string_view` are not: the frame holds the reference, and the referenced
object must outlive the task.

```cpp
pace::task<void> send(pace::tcp::stream& stream, std::string_view text);

std::string greeting = make_greeting();
co_await send(stream, greeting);          // fine: greeting outlives the awaited task

auto task = send(stream, make_greeting()); // wrong: the temporary string dies here,
co_await std::move(task);                  // before the lazy task first runs
```

Because tasks are lazy, "the argument is alive during the call" is not enough: it has to be alive until the task
finishes. Owning parameters remove the question. PACE's own APIs offer both: `stream.write_all(text)` borrows and
is meant to be awaited at once, while `pace::write(stream, std::string)` owns its bytes and can be stored or spawned.

## Coroutine lambdas

A lambda's captures live in the closure object, not in the coroutine frame. If the closure is a temporary, the
coroutine reads destroyed captures after its first suspension:

```cpp
io.spawn([&config]() -> pace::task<void> { co_await serve(config); }());  // wrong: the closure dies at the ;
```

Prefer a named coroutine function with value parameters. Where a lambda is convenient, give it to something that
keeps it: `pace::run(lambda)` owns the callable until shutdown finishes, which is why it takes the lambda rather
than `lambda()`.

## Sockets and listeners

- A socket or listener must stay at a stable address while one of its operations is suspended: do not move or
  destroy it until every operation on it has completed.
- Close it on its own context. `close()` cancels its pending operations, which resume with
  `std::errc::operation_canceled`; destroy the object after they have unwound.
- The `io_context` outlives every socket, listener and timer created on it.
- The teardown order of an application is therefore: stop the context, join the thread that ran it, destroy
  sockets and listeners, destroy the context. `pace::run` and `runtime::run` do the first two for you.

## Results own their data

Calls that return data return it by value: the direct clients return a complete `response`, `udp::exchange`
returns the datagram, `pace::run` returns the entry's value. Nothing returned refers into a context that has
already stopped or a buffer that will be reused. Views (`std::span`, `std::string_view`) appear only as
parameters, where the caller controls the lifetime, never as results that outlive the call.

## Shared state between workers

HTTP handlers and other callbacks are stored by value in the tasks that serve with them. State those handlers reach
through references or pointers, such as a cache or a counter, is the
application's, and so is its synchronization when several workers use it: PACE does not lock anything on your
behalf. Prefer state per worker,
which needs no lock, and share only what must be shared.

## See also

- [Execution model](execution-model.md)
- [Runtime reference](../reference/runtime/README.md), [`task`](../reference/runtime/task.md)
- Tutorial: [Tasks and timers](../tutorial/04-tasks-and-timers.md)
