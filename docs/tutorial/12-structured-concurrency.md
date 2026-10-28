# 12. Start tasks together and finish them together

[中文](12-structured-concurrency.zh-CN.md) · [Learning path](README.md) · [Reference](../reference/structured-concurrency.md)

A direct `http::get(url)` is still the shortest way to get one response. Use a task
group when several asynchronous operations must share a lifetime. This chapter
requires PACE 0.4.0 or newer and links `pace_runtime`.

## 1. Start two tasks and collect their results

Save this complete program as `main.cpp` in chapter 1's project, and change its
`add_deps` to `pace_runtime`:

```cpp
#include <pace/task_group.hpp>
#include <chrono>
#include <iostream>
#include <string>
using namespace std::chrono_literals;

pace::task<std::string> main_task() {
    co_return co_await pace::with_task_group([](pace::task_group& group) -> pace::task<std::string> {
        auto first = group.spawn([]() -> pace::task<std::string> {
            co_await pace::sleep_for(10ms);
            co_return "Hello";
        });
        auto second = group.spawn([]() -> pace::task<std::string> {
            co_await pace::sleep_for(5ms);
            co_return "structured PACE";
        });
        auto a = co_await first;
        auto b = co_await second;
        co_return a + ", " + b + "!";
    }, {.deadline = pace::deadline::after(2s)});
}
int main() {
    try { std::cout << pace::run(main_task) << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
```

Build/run `my_application`. Expect `Hello, structured PACE!` and process exit.
The second task can finish first, but each handle gives the result of its own task.
`group.spawn` starts a child on this context. `co_await first` consumes that result
once. Even if you discard a handle, the group still waits for its child.

`with_task_group` returns only when its body and all children finish. If a child
fails, siblings are canceled and joined before the exception reaches your caller.
If the body throws, children are also canceled and joined. Returning from the body
normally waits for remaining children; it does not cancel them.

The two-second deadline covers the entire group, including nested calls. It is an
absolute monotonic deadline, so a nested operation cannot restart or extend it.
Timeout throws `system_error` with `errc::timed_out` after cleanup.

## 2. Replace timers with network requests

Include `<pace/http.hpp>` and link `pace_http1`. With the quick-start server running,
replace the group body with:

```cpp
[](pace::task_group& group) -> pace::task<std::string> {
    auto first = group.spawn(pace::http::async_get("http://127.0.0.1:8080/health"));
    auto second = group.spawn(pace::http::async_get("http://127.0.0.1:8080/health"));
    auto a = co_await first;
    auto b = co_await second;
    co_return a.body + b.body;
}
```

Both requests own separate connections and inherit the group's cancellation and
deadline. The same names work with `http2::async_get` or `http3::async_get` when
those modules and compatible servers are enabled. See [unified HTTP clients](../reference/http-clients.md).

## 3. Cancel one operation without stopping the loop

Inside a task, a boundary can have its own budget:

```cpp
try {
    auto reply = co_await pace::with_deadline(pace::deadline::after(100ms),
        pace::http::async_get("http://127.0.0.1:8080/health"));
    std::cout << reply.body;
} catch (const std::system_error& error) {
    if (error.code() != std::errc::timed_out) throw;
    std::cout << "request timed out\n";
}
```

Other tasks on the same loop keep running. Handle a recoverable error inside the
child if you want its siblings to continue; an error escaping a child fails its
group. For external cancellation create `pace::cancellation_source`, pass
`source.token()` through `with_cancellation` or `operation_options`, and call
`source.request_cancellation()` from the controlling thread. The resulting error is
`errc::operation_canceled`. Use `io.stop()` only when shutting down the whole loop.

## 4. Give shared data a safe owner

Child factories own their value captures until completion. A child may borrow data
from the caller outside the whole `with_task_group` call. It must not borrow a body
local that is destroyed when the body returns or throws. Use `group.own` instead:

```cpp
co_await pace::with_task_group([](pace::task_group& group) -> pace::task<void> {
    auto& text = group.own(std::string("owned until every child finishes"));
    (void)group.spawn([&text]() -> pace::task<void> {
        co_await pace::sleep_for(5ms);
        std::cout << text << '\n';
    });
    co_return; // text remains alive while the child waits
});
```

`group.own` keeps the object until all children are joined, including on exceptions.
Alternatively copy or move data into a child factory. Pass a capturing coroutine
lambda itself to `spawn`; do not immediately invoke a temporary lambda and pass its
borrowed task. Do not return sockets/views that borrow group-owned storage.

The repository companion is `xmake run tutorial_structured`. Cleanup shielding,
error precedence, custom awaiters and concurrency contracts belong in
[the reference](../reference/structured-concurrency.md).
