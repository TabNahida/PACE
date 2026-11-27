# 8. Workers and threads

[中文](08-workers-and-threads.zh.md) · [Learning path](README.md) · [Previous: structured concurrency](07-structured-concurrency.md)

So far one thread ran one event loop. This chapter gives independent work to two event loops, then runs a loop on a
thread your application owns. Save this complete program as `workers.cpp`, with a `workers` target that depends on
`pace_runtime`:

```cpp
#include <pace/runtime.hpp>
#include <atomic>
#include <chrono>
#include <iostream>

pace::task<void> work(pace::runtime& pool, std::size_t index, std::atomic<unsigned>& remaining)
{
    auto& io = pool.context(index);
    co_await io.sleep_for(std::chrono::milliseconds(5));
    if (remaining.fetch_sub(1) == 1)
    {
        pool.stop();
    }
}

int main()
{
    try
    {
        pace::runtime pool(2);
        std::atomic<unsigned> remaining{2};
        pool.context(0).spawn(work(pool, 0, remaining));
        pool.context(1).spawn(work(pool, 1, remaining));
        pool.run_on_current_thread();
        std::cout << "Both workers finished\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

Build and run `workers`. Expect `Both workers finished`. The counter is atomic because the two tasks can touch it
from different OS threads. Each task is placed on its context before it starts, and `co_await` never moves it to
another one. The last task to finish stops both contexts, and `run_on_current_thread` joins the worker thread before
`remaining` and `pool` are destroyed. Change `run_on_current_thread()` to `run()` and you get the same result with
both contexts on background threads.

The companion [`tutorial_workers.cpp`](../../examples/tutorial_workers.cpp) runs the same layout in both modes and
checks which thread runs each context:

```sh
xmake run tutorial_workers
xmake run tutorial_workers background
```

Both modes use two contexts and check that each task stays on its assigned context. The default `caller` mode also
checks that context zero runs on `main`'s thread. The last task to finish stops the runtime, and the program prints
`Two contexts completed in caller mode` (or `background mode`).

## Choose the execution layout

```cpp
pace::runtime runtime(2);
// Spawn work on runtime.context(0) and runtime.context(1).
runtime.run_on_current_thread();
```

This creates one background worker thread and uses the calling thread for the other context. The argument `2` is
the **total number of contexts**, including the caller's. `runtime(1).run_on_current_thread()` creates no worker
thread. When the calling thread should only wait, use `runtime.run()`: it starts one thread per context and joins all
of them. Both calls block until the runtime is stopped and drained; neither leaves a service running in the
background.

Without a count, the constructor uses the reported hardware concurrency, at least one. You can pass any positive
count your resources support. Work goes where you put it with `context(index)`: nothing balances or steals tasks
between contexts. Mutable objects shared between contexts need ordinary C++ synchronization, like the atomic counter
above.

A runtime runs once, whichever mode you choose; calling either run method again is an error. Caller mode cannot run
inside another running PACE context. When a root task fails, the runtime stops the other contexts and rethrows the
failure after joining its workers.

## Run without a runtime

[Chapter 4](04-tasks-and-timers.md) showed the manual model: one context and `io.run()` on the calling thread. A
context can also run on a thread your application owns. In a copy of
[`tutorial_tasks.cpp`](../../examples/tutorial_tasks.cpp), add this coroutine after `lesson`, so that the context has a
way to stop:

```cpp
pace::task<void> controlled_lesson(pace::io_context& io)
{
    co_await lesson(io);
    io.stop();
}
```

Then replace the body of `main`'s `try` block with:

```cpp
pace::io_context io;
std::exception_ptr failure;
io.spawn(controlled_lesson(io));
std::thread run_thread(
    [&io, &failure]
    {
        try
        {
            io.run();
        }
        catch (...)
        {
            failure = std::current_exception();
        }
    }
);
run_thread.join(); // controlled_lesson() stops the context; join also makes failure safe to read.
if (failure)
{
    std::rethrow_exception(failure);
}
return 0;
```

Include `<thread>` and `<exception>`. The lambda is an ordinary thread function, not a coroutine, and both objects it
references outlive the join. A long-running service would call `io.stop()` from its control thread before joining.
Make sure the join also happens when an exception unwinds `main`: destroying a joinable `std::thread` terminates the
process. Destroying `io` does not join any thread for you.

You can create several independent contexts and run each on its own application thread. Never run one context on
several threads at once, and do not start a `run` inside another PACE context on the same thread.

## Submit from other threads

`io.spawn(std::move(task))` hands over an owned root task, and `io.post(function)` queues a plain callback. Both are
safe to call from any thread. A callback can change state owned by the loop, close a socket that is still alive, or
spawn another coroutine on that loop. Whatever it references must stay alive until it runs, and once the context
stops, the submission is rejected with `std::errc::operation_canceled`.

```cpp
io.post([&io] { io.stop(); });
```

This callback refers to a context whose lifetime its owner controls. For data, capture an owned `std::string` or a
`shared_ptr` by value. `post` takes a `std::function`, which in C++20 requires a copyable callable: a lambda that
owns a `unique_ptr` does not fit. A task passed to `spawn` can own move-only values in its coroutine frame.

## Keep blocking work off the event loop

A synchronous file-system call, a database driver or a long computation inside a coroutine still blocks the thread
that runs it. PACE has no awaitable executor for blocking work. Run such work on threads your application owns, then
`post` the owned result back to the context that needs it. Keep the context alive until those threads are joined, and
handle a rejected `post` during shutdown.

Changing `runtime(1)` to `runtime(8)` does not make a blocking call on context zero run in parallel: it only creates
seven more independent event loops. Neither `co_await io.yield()` nor awaiting a child task moves a computation to
another thread.

Choosing worker counts, listeners that share a port across workers, and measuring the effect are covered in
[protocol tuning](../guides/protocol-tuning.md). Reference: [`runtime`](../reference/runtime/runtime.md),
[`io_context`](../reference/runtime/io-context.md).

Next: [9. Orderly shutdown](09-graceful-shutdown.md)
