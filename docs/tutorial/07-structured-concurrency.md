# 7. Structured concurrency

[中文](07-structured-concurrency.zh.md) · [Learning path](README.md) · [Previous: UDP](06-udp.md)

A direct `http::get(url)` is still the shortest way to get one response. When several asynchronous operations must
start together and finish together, put them in a task group. This chapter links `pace_runtime`, and `pace_http1`
from its second section on.

## Start two tasks and collect their results

Save this complete program as `structured.cpp`, with a `structured` target that depends on `pace_runtime`. It is the
companion program [`tutorial_structured.cpp`](../../examples/tutorial_structured.cpp):

```cpp
#include <pace/runtime.hpp>

#include <chrono>
#include <iostream>
#include <string>
using namespace std::chrono_literals;

pace::task<std::string> main_task()
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
            auto a = co_await first;
            auto b = co_await second;
            co_return a + ", " + b + "!";
        },
        {.deadline = pace::deadline::after(2s)}
    );
}

int main()
{
    try
    {
        std::cout << pace::run(main_task) << '\n';
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

Build and run `structured`, or run `xmake run tutorial_structured` in the PACE checkout. Expect
`Hello, structured PACE!` and a normal exit. The second task finishes first, but each handle gives the result of its
own task. `group.spawn` starts a child on this context, and `co_await first` takes that child's result once. A child
whose handle you discard still belongs to the group, which waits for it.

`with_task_group` returns only when its body and all its children have finished. If a child fails, its siblings are
cancelled and joined before the exception reaches the caller. If the body throws, the children are cancelled and
joined too. When the body returns normally, the group waits for the remaining children; it does not cancel them.

The two-second deadline covers the whole group, including nested calls. It is an absolute point on the monotonic
clock, so a nested operation cannot restart or extend it. When it expires, the group cancels its work and, after the
cleanup, throws `std::system_error` with `std::errc::timed_out`.

## Replace timers with network requests

Include `<pace/http1.hpp>` and change the target's dependency to `pace_http1`. With the `hello` server from
[chapter 1](01-install-and-first-program.md) running, replace the group body with:

```cpp
[](pace::task_group& group) -> pace::task<std::string>
{
    auto first = group.spawn(pace::http::async_get("http://127.0.0.1:8080/health"));
    auto second = group.spawn(pace::http::async_get("http://127.0.0.1:8080/health"));
    auto a = co_await first;
    auto b = co_await second;
    co_return a.body + b.body;
}
```

Expect `ok` twice. Both requests use their own connections and inherit the group's cancellation and deadline. The
same code works with `http2::async_get` or `http3::async_get` when those modules are enabled and the server speaks
that protocol; see [URL clients](../reference/http/clients.md).

## Cancel one operation without stopping the loop

Inside a task, one operation can have its own budget:

```cpp
try
{
    auto reply = co_await pace::with_deadline(
        pace::deadline::after(100ms),
        pace::http::async_get("http://127.0.0.1:8080/health")
    );
    std::cout << reply.body;
}
catch (const std::system_error& error)
{
    if (error.code() != std::errc::timed_out)
    {
        throw;
    }
    std::cout << "request timed out\n";
}
```

Other tasks on the same loop keep running. Handle an error you can recover from inside the child if its siblings
should continue: an error that escapes a child fails its whole group. To cancel from outside, create a
`pace::cancellation_source`, pass `source.token()` to `with_cancellation` (or in `operation_options`), and call
`source.request_cancellation()` from the controlling code. The cancelled operation throws `std::system_error` with
`std::errc::operation_canceled`. Use `io.stop()` only to shut down the whole loop.

## Give shared data a safe owner

Child factories own the values they capture until they complete. A child may borrow data that the caller keeps
alive for the whole `with_task_group` call, but not a local of the body: the body's locals are destroyed when it
returns or throws, possibly while children still run. Give such data to the group with `group.own`:

```cpp
co_await pace::with_task_group(
    [](pace::task_group& group) -> pace::task<void>
    {
        auto& text = group.own(std::string("owned until every child finishes"));
        (void)group.spawn(
            [&text]() -> pace::task<void>
            {
                co_await pace::sleep_for(5ms);
                std::cout << text << '\n';
            }
        );
        co_return; // text stays alive while the child waits
    }
);
```

`group.own` keeps the object until every child has been joined, also when an exception unwinds the group. Copying
or moving data into a child's factory works as well. Pass a capturing coroutine lambda itself to `spawn`; do not call
a temporary lambda and pass the task it returns, which would borrow the destroyed lambda. Do not return sockets or
views that borrow storage the group owns.

Shielding cleanup from cancellation, which error wins when several fail, and the threading rules are in the
[structured concurrency reference](../reference/runtime/structured-concurrency.md).

Next: [8. Workers and threads](08-workers-and-threads.md)
