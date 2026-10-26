# 2. Write an asynchronous program

[中文](02-tasks-timers.zh-CN.md) · [Previous: build](01-build.md) · [Next: TCP](03-tcp.md)

By the end of this chapter you will write a function that suspends for a timer,
returns a value to `main`, and propagates failures through ordinary C++ exceptions.
Use the `my_application` target from chapter 1, with `add_deps("pace_runtime")`.

## Start with a value, then introduce a wait

A coroutine returning `pace::task<std::string>` eventually produces a string.
Inside it, use `co_return` instead of `return`. Creating its task does not run the
body: tasks are lazy. The entry runner will start it on the calling thread.

Put this complete program in `main.cpp`:

```cpp
#include <pace/run.hpp>
#include <chrono>
#include <iostream>
#include <string>

pace::task<std::string> greet(pace::io_context& io) {
    co_await io.sleep_for(std::chrono::milliseconds(10));
    co_return "Hello, PACE";
}

int main() {
    try {
        std::cout << pace::run(greet) << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

Run `xmake -y my_application` and `xmake run my_application`. Expect `Hello, PACE`
and a normal exit. While `greet` awaits the timer, the reactor can run other ready
work. It does not dedicate a sleeping thread to the coroutine. The actual delay
can exceed 10 ms; a timer is not a real-time scheduling guarantee.

`pace::run(greet)` creates a fresh context, invokes `greet(io)` on that context,
and returns its result. When the entry finishes, it requests stop and drains
background cancellation. You do not add `io.stop()` to this one-shot program.
For an entry returning `task<void>`, `run` returns `void`.

## Pass owned inputs

To accept a name, change the signature to
`greet(pace::io_context& io, std::string name)` and return `"Hello, " + name`.
The string now belongs to the coroutine frame across its wait. Supply the extra
argument with an ordinary factory lambda:

```cpp
const auto message = pace::run([](pace::io_context& io) {
    return greet(io, "reader");
});
```

This lambda returns a task; it is not itself a coroutine. `run` also safely owns
a capturing coroutine lambda for the entire run:

```cpp
const auto answer = pace::run([value = 42](pace::io_context& io) -> pace::task<int> {
    co_await io.yield();
    co_return value;
});
```

That lifetime guarantee belongs to `run`. Do not generalize it to
`io.spawn([capture]() -> task<void> { ... }())`: an immediately invoked temporary
coroutine lambda can die while its coroutine still refers to the closure.
Reference captures remain references, even when the closure is owned.

## Await another task

Inside a coroutine, call a child directly:

```cpp
const auto message = co_await greet(io, "reader");
```

The parent resumes after the child's result is ready. If you first save the task,
consume it with `co_await std::move(pending)`; a task is move-only and single-use.
A child's exception is thrown at the `co_await` expression. Catch it there if the
parent can recover. Otherwise it reaches `run` and the catch in `main`.

`co_await io.yield()` and a zero-duration timer give other ready work a turn.
Neither moves CPU work to another thread. A blocking file read or long computation
still blocks this reactor; chapter 5 explains explicit worker placement.

## Understand what completion owns

`run` returns **owned results**, such as strings or numbers. Do not return a socket
bound to its temporary context, or a view into entry-local/captured storage.

An entry can use `io.spawn` for background work, but completing the entry cancels
that work; it does not wait for its natural completion. Background tasks must own
their data, or borrow objects outside `run` that remain alive until it returns.
Entry-local objects are destroyed as the entry returns, before background tasks
necessarily observe cancellation. If your application needs several independent
roots and explicit draining, use the manual model below and chapter 6.

## Take explicit control when needed

```cpp
pace::io_context io;
io.spawn(service(io)); // service returns task<void>
io.run();             // Someone must explicitly request io.stop().
```

Unlike `pace::run(factory)`, `io.run()` stays alive even when its last task finishes.
This is useful for a service receiving work from other threads. Do not switch
between the two forms without deciding who owns shutdown. Neither supports nesting
another reactor on the same running thread. Entry cancellation is reported by
`pace::run` as `std::system_error(operation_canceled)` rather than a missing value.

Try changing the entry to throw after its timer. Confirm that `main` reports the
failure and exits instead of hanging. Then run `xmake run tutorial_tasks` for the
[extended exercise](../../examples/tutorial_tasks.cpp), which also checks child
exceptions and task ownership.

[Entry-runner contract](../reference/run.md) · [Task contract](../reference/task.md)
