# 4. Tasks and timers

[中文](04-tasks-and-timers.zh.md) · [Learning path](README.md) · [Previous: HTTP server](03-http-server.md)

By the end of this chapter you will write a function that suspends for a timer, returns a value to `main`, and
reports failures through ordinary C++ exceptions. Add a target for a new file `tasks.cpp` to the `hello` project:

```lua
target("tasks")
    set_kind("binary")
    add_files("tasks.cpp")
    add_deps("pace_runtime")
```

## Start with a value, then introduce a wait

A coroutine returning `pace::task<std::string>` eventually produces a string. Inside it, use `co_return` instead of
`return`. Calling the coroutine creates its task but does not run the body: tasks are lazy. The entry runner starts
it on the calling thread.

Put this complete program in `tasks.cpp`:

```cpp
#include <pace/runtime.hpp>
#include <chrono>
#include <iostream>
#include <string>

pace::task<std::string> greet(pace::io_context& io)
{
    co_await io.sleep_for(std::chrono::milliseconds(10));
    co_return "Hello, PACE";
}

int main()
{
    try
    {
        std::cout << pace::run(greet) << '\n';
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

Run `xmake -y tasks` and `xmake run tasks`. Expect `Hello, PACE` and a normal exit. While `greet` awaits the timer,
the event loop can run other ready work; no thread sleeps on behalf of the coroutine. The actual delay can be longer
than 10 ms: a timer is not a real-time guarantee.

`pace::run(greet)` creates a fresh `io_context`, calls `greet(io)` on it and returns the task's result. When the
entry finishes, `run` stops the context and waits for cancelled background work to unwind, so this one-shot program
never calls `io.stop()` itself. For an entry returning `task<void>`, `run` returns `void`.

## Pass owned inputs

To accept a name, change the signature to `greet(pace::io_context& io, std::string name)` and return
`"Hello, " + name`. The string now belongs to the coroutine frame across its wait. Supply the extra argument with an
ordinary factory lambda:

```cpp
const auto message = pace::run([](pace::io_context& io) { return greet(io, "reader"); });
```

This lambda returns a task; it is not itself a coroutine. `run` also owns a capturing coroutine lambda safely for
the whole run:

```cpp
const auto answer = pace::run(
    [value = 42](pace::io_context& io) -> pace::task<int>
    {
        co_await io.yield();
        co_return value;
    }
);
```

That lifetime guarantee belongs to `run`. Do not generalize it to `io.spawn([capture]() -> task<void> { ... }())`:
the temporary lambda of an immediately invoked coroutine lambda can be destroyed while its coroutine still refers to
the captures. Reference captures stay references, even when `run` owns the closure.

## Await another task

Inside a coroutine, call a child directly:

```cpp
const auto message = co_await greet(io, "reader");
```

The parent resumes after the child's result is ready. If you first store the task, consume it with
`co_await std::move(pending)`: a task is move-only and can be awaited once. A child's exception is thrown at the
`co_await` expression. Catch it there if the parent can recover; otherwise it reaches `run` and the `catch` in `main`.

`co_await io.yield()` and a zero-length timer give other ready work a turn. Neither moves CPU work to another thread.
A blocking file read or a long computation still blocks this event loop; [chapter 8](08-workers-and-threads.md)
explains where such work goes.

## Understand what completion owns

`run` returns **owned results**, such as strings or numbers. Do not return a socket bound to its temporary context,
or a view into storage that belongs to the entry or its captures.

An entry can start background work with `io.spawn`, but completing the entry cancels that work; `run` does not wait
for it to finish naturally. Background tasks must own their data, or borrow objects declared outside `run` that live
until it returns. Locals of the entry are destroyed as the entry returns, before background tasks necessarily see
the cancellation. If your program needs several independent root tasks and explicit draining, use the manual model
below and [chapter 9](09-graceful-shutdown.md).

## Take explicit control when needed

```cpp
pace::io_context io;
io.spawn(service(io)); // service returns task<void>
io.run();              // Returns only after someone calls io.stop().
```

Unlike `pace::run(factory)`, `io.run()` keeps running after its last task finishes. That suits a service that
receives work from other threads. Decide who owns shutdown before choosing one form or the other. Neither form can
run a second event loop nested on a thread that is already running one. When the entry of `pace::run` is cancelled,
`run` reports it as `std::system_error` with `std::errc::operation_canceled` rather than returning a value.

Try making the entry throw after its timer, and check that `main` reports the failure and exits instead of hanging.
Then run the companion, `xmake run tutorial_tasks` ([source](../../examples/tutorial_tasks.cpp)): its `lesson`
coroutine also awaits a stored task, checks that it still runs on the same context, yields, and catches a child's
exception.

[`pace::run`](../reference/runtime/run.md) and [`task`](../reference/runtime/task.md) give the full contracts.

Next: [5. TCP](05-tcp.md)
