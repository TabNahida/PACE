# 6. Orderly shutdown

[中文](06-shutdown.zh-CN.md) · [Previous: execution](05-execution.md) · [Next: UDP](07-udp.md)

Before making a long-lived server, build a small cancellation experiment. Replace
`main.cpp` with this complete program, link `pace_runtime`, then build and run it:

```cpp
#include <pace/io_context.hpp>
#include <chrono>
#include <iostream>

pace::task<void> waiting(pace::io_context& io, bool& canceled) {
    try { co_await io.sleep_for(std::chrono::hours(1)); }
    catch (const std::system_error& error) {
        if (error.code() != std::errc::operation_canceled) throw;
        canceled = true;
    }
}

int main() {
    try {
        pace::io_context io;
        bool canceled = false;
        auto stop = io.call_after(std::chrono::milliseconds(10), [&io] { io.stop(); });
        io.spawn(waiting(io, canceled));
        io.run();
        if (!canceled) throw std::runtime_error("cancellation was not observed");
        std::cout << "Canceled and drained\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

Expect `Canceled and drained`, without waiting an hour. The timer callback
requests stop; the pending coroutine then resumes with cancellation. Only after
that coroutine has unwound does `io.run()` return. Keep the timer handle in a
variable: destroying it would disarm the stop callback. Keep `canceled` outside
the task so it remains alive during cancellation. The entry runner from chapter 2
requests this same stop automatically when its entry completes; manual services
choose the moment themselves.

Complete source: [`tutorial_shutdown.cpp`](../../examples/tutorial_shutdown.cpp).
Run `xmake run tutorial_shutdown`. It starts a long timer, requests stop after a
short delay, verifies that cancellation was observed, and checks that a local RAII
cleanup ran before `run()` returned.


For one operation or a related set of tasks, prefer [local cancellation and
structured groups](12-structured-concurrency.md). `io.stop()` is whole-context
shutdown. Closing a raw listener only cancels accept; the HTTP `serve` helpers
add a structured scope and cancel/join their accepted connections before returning.

## Cancellation is an ordinary error path

```cpp
try {
    co_await io.sleep_for(std::chrono::hours(1));
} catch (const std::system_error& error) {
    if (error.code() != std::errc::operation_canceled) throw;
    // Release synchronous application resources and finish this task.
}
```

`stop()` permanently requests shutdown and wakes the loop. Pending readiness and
timer waits resume with cancellation; ready tasks and callbacks are drained.
RAII objects in a coroutine frame are destroyed as it unwinds. A stopped context
rejects new `spawn`/`post` operations, and further waits observe cancellation.
Perform any asynchronous flushing that you need **before** stopping the context.

An uncaught cancellation from a root is suppressed when the context is already
stopping. A different root failure still propagates from `run()` after draining.
Code that never suspends or returns, or blocks in a synchronous call, can prevent
shutdown from finishing. Stop does not forcibly interrupt a thread.

## Stop accepting before stopping the reactor

For a service that should finish current responses, apply these steps in order:

1. On the owning context, mark the application as draining and close its listener.
2. Let accepted connections finish; where appropriate, make handlers return `response.close = true`.
3. Wait for a tracked active-connection count to reach zero, up to an application grace deadline.
4. Call `io.stop()` or `runtime.stop()` to cancel anything that remains.
5. Wait for run to return/join external run threads, destroy sockets/listeners, then destroy contexts.

Closing a listener only cancels its pending accept; accepted streams continue.
The accept task must catch this expected cancellation and return, as the TCP
tutorial does. Otherwise an uncaught root cancellation **before** context stop
would be treated as an error and stop the loop prematurely. `http::serve` already
handles listener cancellation this way.

The following application helper assumes `active_connections` is maintained only
on this same context, with a decrement in each connection task's RAII cleanup:

```cpp
pace::task<void> drain_connections(pace::io_context& io,
                                    pace::tcp::listener& listener,
                                    const std::size_t& active_connections,
                                    std::chrono::milliseconds grace) {
    listener.close();
    const auto deadline = std::chrono::steady_clock::now() + grace;
    while (active_connections != 0 && std::chrono::steady_clock::now() < deadline)
        co_await io.sleep_for(std::chrono::milliseconds(1));
    io.stop();
}
```

This is an application integration pattern, not a library connection-count API.
Its listener, count and context must outlive the helper and connection tasks;
use a normal bounded grace duration. With `http::serve`, there is no public active
count/join handle. If you need one, write your own accept loop that wraps
`http::serve_connection` in a named counting coroutine. Coordinate each worker's
drain through its own context; a shared count across threads must be synchronized.

## Socket close and object lifetime

`socket.close()` cancels its registered waits and releases its native handle on
the owning loop. It does not wait for suspended member operations to finish.
Keep the C++ socket object alive until those operations resume and unwind.
Destroying or moving the object first would leave its coroutine methods with an
invalid `this` pointer. Do not destroy a pending task to simulate cancellation.

`co_await socket.close_gracefully()` is a different operation: with no concurrent
socket operations, it half-closes sends and drains peer input for up to 100 ms or
64 KiB by default, then closes. It always closes even on error/cancellation. Use
it after the final application write and before stopping the context. It bounds
the drain, not peer acknowledgment, and reaching a bound is normal completion.

## Signals and external owners

The server tutorials' signal handlers only set a lock-free atomic flag. A normal
coroutine checks the flag and calls `stop()`. Do not call arbitrary reactor or
socket APIs directly from a signal handler. On another normal application thread,
`stop()` itself is a supported thread-safe operation.

`runtime.run()` and `runtime.run_on_current_thread()` join their worker threads
before returning. If you launch either from an application-owned thread, you
still must join that outer thread before destroying the runtime. Standalone
`io_context` owners likewise must stop and join their run threads explicitly.
Join any application blocking workers too, so they cannot submit results into a
destroyed context.

Reference: [`io_context`](../reference/io-context.md), [`runtime`](../reference/runtime.md),
[`tcp::stream::close_gracefully`](../reference/tcp.md#bounded-graceful-close).
