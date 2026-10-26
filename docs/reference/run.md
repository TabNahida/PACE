# `pace::run`

Header: `<pace/run.hpp>` (also exported by `<pace/pace.hpp>`). Link `pace_runtime`.

```cpp
template<class Factory>
auto run(Factory&& factory); // factory(io_context&) returns task<T>; result is T
```

The factory is copied from an lvalue or moved from an rvalue into owned storage.
Move-only closures are supported. It is invoked once on a fresh context running
on the caller's thread; no worker thread is created. The factory stays alive
until the context has drained, so temporary capturing coroutine lambdas are safe
**when passed to this API**, without immediately invoking them yourself.

Entry completion requests stop. Pending background I/O/timers are canceled and
ready callbacks/tasks are drained before returning. This is not a join-all or a
server connection-drain API. Entry-local data can be destroyed before background
cancellation; spawned tasks must own data or borrow longer-lived objects. A task
that blocks or ignores cancellation can prevent completion.

`task<T>` returns its moved result, including move-only values; `task<void>` returns
void. Return owned values, not context-bound sockets or views into destroyed data.
Factory/entry exceptions propagate. Entry cancellation propagates as
`std::system_error(operation_canceled)`, even though manual `io_context::run`
suppresses expected root cancellation while stopping. Unhandled background errors
also propagate; do not rely on an ordering between multiple failures.

Calling inside an already running PACE context throws `std::logic_error` before
invoking the factory. Await a child task there instead. Empty entry tasks are
rejected when awaited. Existing `io_context::run`, `spawn`, `post` and runtime
worker APIs retain their original behavior.

[Guided introduction](../tutorial/02-tasks-timers.md) · [中文教程](../tutorial/02-tasks-timers.zh-CN.md)
