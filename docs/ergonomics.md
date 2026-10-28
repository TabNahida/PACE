# Shorter application code, explicit ownership

Current client entry points: direct `http::get/post/fetch` return responses; their
coroutine forms are `async_get/async_post/async_fetch`. TCP/UDP offer `exchange` and
`async_exchange`. See [direct clients and migration](reference/direct-clients.md).

The convenience layer removes repeated entry/shutdown and routing code while
retaining the reactor, task and transport APIs. It adds no framework dependency,
implicit worker pool, hidden global context or macro DSL.

## 0.3.0 application entry

`http::app` combines route registration and a caller-thread `run(port)`. URL GET,
POST and fetch own request data and select optional DNS/TLS explicitly at build
time. `pace::run(task)` and nullary factories, current-context waits, and TCP
connect/read_exact remove common setup and short-read loops. See the
[quick start](tutorial/00-quick-start.md) and [contracts](reference/convenience.md).
These helpers retain protocol limits. URL clients do not add retries or redirects;
only direct calls add a default total deadline. DNS is linked into HTTP/1 only when enabled.

## One-shot entry

Previously a task had to stop its context, and main had to construct, spawn and
run it. `pace::run(factory)` now creates that scope and returns `task<T>`'s value:

```cpp
const auto response = pace::run([](pace::io_context& io) {
    return pace::http::async_get(io, pace::tcp::endpoint{"127.0.0.1", 8080}, "/health");
});
```

The context is still passed explicitly. The factory is owned through shutdown,
including captured coroutine lambdas. This costs one wrapper coroutine for the
entry, not another thread. Existing manual run semantics do not change. Entry
completion cancels background roots; it is not structured join-all. Use explicit
context/listener ownership when application lifetimes require it.

## Ordinary handlers and asynchronous handlers

A response that needs no wait should be an ordinary function. `http::make_handler`
and `http::router` adapt both `response(request)` and `task<response>(request)`.
The adapter owns each callable via shared storage so pending invocations survive
router destruction, including move-only captures. This adds a shared allocation
per registration and one wrapper task per invocation; direct coroutine handlers
remain available for applications that do not want the convenience layer.
Router copies share their read-only table and detach it on registration, avoiding
whole-table copies when a server copies its handler for each accepted connection.

The router deliberately matches exact method/path pairs with 404/405 and HEAD
fallback. Its search is linear; there is no unmeasured claim that it accelerates
routing. Applications needing large tables, parameters, middleware or custom URL
normalization can keep their own handler. It works with all three HTTP adapters
because they share message and handler types.

## Fewer listener declarations when control is unnecessary

`http::serve(io, endpoint, handler)` owns the HTTP/1 listener. Binding happens
when its task starts, so errors propagate through the coroutine. Keep the
`serve(listener, ...)` overload for immediate bind results, port-zero lookup,
TLS, streaming/admission options or independently stopping acceptance. A shorter
entry does not claim to provide signal handling or graceful connection draining.

The tutorials teach both forms progressively, including their lifetime limits:
[English learning path](tutorial/README.md) · [中文学习路线](tutorial/README.zh-CN.md).
