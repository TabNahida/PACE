# 5. Execution and worker selection

Complete source: [`tutorial_workers.cpp`](../../examples/tutorial_workers.cpp).
Run both modes to check which thread executes context zero:

```sh
xmake run tutorial_workers
xmake run tutorial_workers background
```

Both modes use two contexts and verify that each task stays on its assigned
context. The default `caller` mode also checks that context zero runs on `main`'s
thread. The last completed task requests stop for the runtime.

## Choose the execution layout

```cpp
pace::runtime runtime(2);
// Submit work to runtime.context(0) and runtime.context(1).
runtime.run_on_current_thread();
```

This creates one background worker and uses the caller for the other context.
The argument `2` is the **total number of contexts**, including the caller.
`runtime(1).run_on_current_thread()` creates no runtime worker threads. For a caller
that should only wait, use `runtime.run()`: it creates one background thread per
context and joins all of them. Both calls block until stop and draining finish;
neither starts a detached service.

Without an explicit count, construction uses reported hardware concurrency with
a minimum of one. You can choose any positive count supported by your resources;
zero is rejected. Work is assigned explicitly through `context(index)`, not
automatically balanced or stolen. Sharing a mutable object across contexts needs
normal C++ synchronization. The tutorial uses an atomic completion counter.

The runtime is single-use regardless of execution mode. Calling either run method
again is an error. Caller mode cannot be nested inside another running PACE
context. A root failure stops peer contexts and is rethrown after worker joins.

## Run without a runtime

The [first tutorial](02-tasks-timers.md) already uses the simplest manual model:
one context and `io.run()` on the caller. A context can instead run on a thread
owned by your application. This replacement for the first tutorial's main body
reuses its named `lesson` coroutine:

```cpp
pace::io_context io;
std::exception_ptr failure;
io.spawn(lesson(io));
std::thread run_thread([&io, &failure] {
    try { io.run(); }
    catch (...) { failure = std::current_exception(); }
});
run_thread.join(); // lesson() requests stop; join also synchronizes failure access.
if (failure) std::rethrow_exception(failure);
```

Include `<thread>` and `<exception>`. The lambda is a normal thread function,
not a coroutine, and both referenced objects outlive its join. A long-lived
service can request `io.stop()` from its control thread before joining. Arrange
the same join during exception unwinding; destroying a joinable `std::thread`
terminates the process. Destruction of `io` is not an automatic thread join.

You can create several independent contexts and run each on a different
application thread. Do not run one context simultaneously from multiple threads,
and do not nest one `run` inside another PACE context on the same thread.

## Submit from other threads

`io.spawn(std::move(task))` transfers an owned root task, while `io.post(function)` queues
a synchronous callback. Both are thread-safe. A callback can change loop-owned
state, close a still-live socket, or submit another named coroutine on that loop.
Its referenced objects must remain alive until the callback runs, and shutdown
can reject the submission with `operation_canceled`.

```cpp
io.post([&io] { io.stop(); });
```

This is a normal callback with a context whose lifetime is controlled by its
owner. For payloads, capture an owned `std::string` or a `shared_ptr` by value.
`std::function` in C++20 requires a copyable callable; a lambda that only owns a
`unique_ptr` is not directly suitable for `post`. A task submitted with `spawn`
can own move-only values in its coroutine frame.

## Keep blocking work off the reactor

A synchronous filesystem call, database driver or large CPU computation inside a
coroutine still blocks its executing thread. PACE does not currently include an
awaitable blocking-work executor. Use application-owned workers for such work,
then `post` an owned result back to the relevant context. Keep the context alive
until those workers are joined, and handle a rejected post during shutdown.

Changing `runtime(1)` to `runtime(8)` does not make a blocking operation on context
zero parallel within that context. It only creates other independent reactors.
Neither `co_await io.yield()` nor awaiting a child task offloads a computation.

## Tune with a declared workload

Choose worker counts by measuring the workload that matters, leaving CPU capacity
for clients, the kernel and any blocking workers. Keep payloads, connection counts,
compiler options and hardware placement consistent. The runtime provides neither
CPU affinity nor an adaptive pool. A count equal to logical CPUs is not evidence
of maximum throughput.

For inbound TCP/HTTP on platforms supporting reuse-port, the existing
[`echo_server`](../../examples/echo_server.cpp) and
[`http_server`](../../examples/http_server.cpp) examples create a listener per
context on the same port. The OS distributes newly accepted connections. Windows
does not currently support that strategy in PACE; those examples use one worker
there. Multiple contexts with separate ports or outbound connections are still
possible. Never move an active socket to another context as a balancing shortcut.

Reference: [`runtime`](../reference/runtime.md), [`io_context`](../reference/io-context.md).
Next: [orderly shutdown](06-shutdown.md).
