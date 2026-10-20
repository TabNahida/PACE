# 2. Tasks and timers

Complete source: [`tutorial_tasks.cpp`](../../examples/tutorial_tasks.cpp).
Run `xmake run tutorial_tasks` from the repository root.

The smallest execution unit is a `pace::io_context`. Submit a root coroutine and
run the context on your calling thread:

```cpp
pace::io_context io;
io.spawn(lesson(io));
io.run();
```

The context lives longer than `lesson`; the named coroutine borrows it safely.
`spawn` takes ownership of its `task<void>` but does not execute it inline.
`run()` begins execution and remains alive until `io.stop()` is requested.
Completing the last task alone does not stop an idle context.

## Own data across suspension

The example's child task takes a string **by value**:

```cpp
pace::task<std::string> delayed_message(pace::io_context& io, std::string name) {
    co_await io.sleep_for(std::chrono::milliseconds(5));
    co_return "Hello, " + name;
}
```

Its string belongs to the coroutine frame throughout the wait. A reference,
`std::string_view` or pointer would only borrow storage. Changing a parameter to a
view does not make a lazy operation keep its source alive. The context is a
reference because the enclosing `main` deliberately owns it through `run()`.

```cpp
auto pending = delayed_message(io, "PACE"); // Body has not started yet.
const auto message = co_await std::move(pending);
```

The task is move-only and consumed once. A named task is an lvalue, so move it
into `co_await`; a direct call result such as
`co_await delayed_message(io, "PACE")` is already an rvalue. Do not await a consumed
task again. Use named coroutine functions with owned arguments rather than
immediately invoked temporary capturing coroutine lambdas: destroying the lambda
closure can invalidate captures used after suspension.

## Timer and yield behavior

```cpp
co_await io.sleep_for(std::chrono::milliseconds(5));
co_await io.yield();
co_await io.sleep_for(std::chrono::milliseconds(0));
```

All three suspend on the same context. The 5 ms delay uses a monotonic clock;
actual resumption can be later. Both `yield` and a zero/expired timer give the
scheduler another turn. None move a task to another worker or run it in parallel.
Long synchronous loops and blocking calls still hold up this worker.

`pace::io_context::current()` identifies the currently executing context. Outside
its `run()` it returns `nullptr`; in this example it equals `&io`. Operations on
another context's sockets or awaiters are not an implicit handoff.

## Exceptions and termination

An awaited child exception reaches the parent's ordinary try/catch. The example
checks that its intentionally failing child is caught and then calls `io.stop()`.
If an exception instead escapes a root task, the context requests stop, drains
its queued work and rethrows from `run()`. Catch that final failure in `main`.

Stop also cancels pending I/O and timers. Expected cancellation is a
`std::system_error` whose code compares with `std::errc::operation_canceled`.
Read [orderly shutdown](06-shutdown.md) before adding multiple long-lived roots.

Reference: [`task`](../reference/task.md), [`io_context`](../reference/io-context.md).
Next: [TCP echo](03-tcp.md).
