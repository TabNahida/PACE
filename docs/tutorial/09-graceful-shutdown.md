# 9. Orderly shutdown

[中文](09-graceful-shutdown.zh.md) · [Learning path](README.md) · [Previous: workers and threads](08-workers-and-threads.md)

Before writing a long-running server, try a small cancellation experiment. Save this complete program as
`shutdown.cpp`, with a `shutdown` target that depends on `pace_runtime`, then build and run it:

```cpp
#include <pace/runtime.hpp>
#include <chrono>
#include <iostream>

pace::task<void> waiting(pace::io_context& io, bool& canceled)
{
    try
    {
        co_await io.sleep_for(std::chrono::hours(1));
    }
    catch (const std::system_error& error)
    {
        if (error.code() != std::errc::operation_canceled)
        {
            throw;
        }
        canceled = true;
    }
}

int main()
{
    try
    {
        pace::io_context io;
        bool canceled = false;
        auto stop = io.call_after(std::chrono::milliseconds(10), [&io] { io.stop(); });
        io.spawn(waiting(io, canceled));
        io.run();
        if (!canceled)
        {
            throw std::runtime_error("cancellation was not observed");
        }
        std::cout << "Canceled and drained\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

Expect `Canceled and drained` without waiting an hour. The timer callback stops the context; the waiting coroutine
then resumes with a cancellation error, and only after it has unwound does `io.run()` return. Keep the timer handle
in a variable: destroying it disarms the callback. `canceled` lives outside the task so that it is still alive
during cancellation. The entry runner from [chapter 4](04-tasks-and-timers.md) performs this same stop when its entry
completes; a service you run by hand chooses the moment itself.

The companion [`tutorial_shutdown.cpp`](../../examples/tutorial_shutdown.cpp) does the same with a second task
that stops the context after 5 ms. `xmake run tutorial_shutdown` checks that the cancellation was observed and that
a local RAII guard in the coroutine ran before `run()` returned, then prints
`Cancellation observed; coroutine resources released`.

For one operation or a group of related tasks, prefer the local cancellation and task groups of
[chapter 7](07-structured-concurrency.md): `io.stop()` shuts down the whole context. Closing a listener only cancels
its pending accept; the HTTP `serve` functions run their connections in a structured scope, and cancel and join them
before they return.

## Cancellation is an ordinary error path

```cpp
try
{
    co_await io.sleep_for(std::chrono::hours(1));
}
catch (const std::system_error& error)
{
    if (error.code() != std::errc::operation_canceled)
    {
        throw;
    }
    // Release synchronous application resources and finish this task.
}
```

`stop()` requests shutdown for good and wakes the loop. Pending socket and timer waits resume with cancellation;
tasks and callbacks that are already ready still run. RAII objects in a coroutine frame are destroyed as it unwinds.
A stopped context rejects new `spawn` and `post` calls, and every further wait sees the cancellation. Do any
asynchronous flushing **before** you stop the context.

A root task that ends with an uncaught cancellation while the context is stopping is not an error. Any other failure
of a root task still propagates from `run()` once draining is complete. Code that never suspends or never returns,
or that blocks in a synchronous call, can keep shutdown from finishing: stopping does not interrupt a thread.

## Stop accepting before stopping the event loop

For a service that should finish the responses it has started, follow these steps in order:

1. On the owning context, mark the application as draining and close its listener.
2. Let accepted connections finish; where appropriate, make handlers return responses with `close = true`.
3. Wait until a tracked count of active connections reaches zero, up to a grace period your application chooses.
4. Call `io.stop()` or `runtime.stop()` to cancel whatever remains.
5. Wait for `run` to return and join any threads that run it, destroy sockets and listeners, then destroy the
   contexts.

Closing a listener cancels only its pending accept; accepted streams continue. The accept loop must catch that
expected cancellation and return, as the companion of [chapter 5](05-tcp.md) does. Otherwise a root task ending with
an uncaught cancellation **before** the context stops counts as a failure and stops the loop early. `http::serve`
already handles a closed listener this way.

The following helper assumes that `active_connections` is changed only on this same context, and that every
connection task decrements it from an RAII guard:

```cpp
pace::task<void> drain_connections(
    pace::io_context& io,
    pace::tcp::listener& listener,
    const std::size_t& active_connections,
    std::chrono::milliseconds grace
)
{
    listener.close();
    const auto deadline = std::chrono::steady_clock::now() + grace;
    while (active_connections != 0 && std::chrono::steady_clock::now() < deadline)
    {
        co_await io.sleep_for(std::chrono::milliseconds(1));
    }
    io.stop();
}
```

This is a pattern for your application, not a library API for counting connections. The listener, the count and
the context must outlive the helper and the connection tasks; pick a bounded grace period. `http::serve` exposes no
count of active connections or handle to join them. If you need one, write your own accept loop and run
`http::serve_connection` inside a counting coroutine. With several workers, drain each one through its own context;
a count shared across threads must be synchronized.

## Socket close and object lifetime

`socket.close()` cancels the socket's registered waits and releases its native handle on the owning loop. It does
not wait for the suspended operations to finish. Keep the C++ socket object alive until they resume and unwind:
destroying or moving it first leaves those member coroutines with a dangling `this`. Do not destroy a pending task to
simulate cancellation.

`co_await socket.close_gracefully()` is a different operation. With no other operation outstanding on the socket, it
shuts down the sending side, discards the peer's input for up to 100 ms or 64 KiB by default, then closes. It always
closes the socket, also after an error or cancellation. Use it after the last write and before stopping the context.
It bounds the drain, not the peer's acknowledgement, and reaching a bound is a normal completion.

## Signals and external owners

The signal handlers of the companion servers (`tutorial_http serve`, `tutorial_tcp serve`) only set a lock-free
atomic flag. An ordinary coroutine checks the flag and calls `stop()`. Do not call reactor or socket functions from a
signal handler. From another ordinary thread, `stop()` itself is safe to call.

`runtime.run()` and `runtime.run_on_current_thread()` join their worker threads before they return. If you call
either from a thread your application owns, you still have to join that thread before destroying the runtime. An
`io_context` you run yourself likewise has to be stopped and its run thread joined. Join your own blocking workers
too, so that they cannot submit results to a destroyed context.

Reference: [`io_context`](../reference/runtime/io-context.md), [`runtime`](../reference/runtime/runtime.md),
[TCP `close_gracefully`](../reference/net/tcp.md).

Next: [10. TLS and DNS](10-tls-and-dns.md)
