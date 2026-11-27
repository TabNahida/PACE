# API design: short calls, explicit ownership

[简体中文](api-design.zh.md) · [Explanation](README.md)

PACE offers two levels of API over the same machinery. Short calls cover the common cases in one line: fetch a
URL, serve a few routes, run one task to completion. The explicit API underneath (`io_context`, `task`,
sockets, listeners, protocol clients and servers) stays public and fully usable, and the short calls are written
on top of it rather than beside it. Neither level adds a framework dependency, an implicit worker pool, a hidden
global context or a macro DSL. This page explains the choices behind the short calls and what each one costs.

## Direct calls and their coroutine forms

Every client operation has a blocking form and a coroutine form with the same name and an `async_` prefix:

| Blocking, on a plain thread | Coroutine, inside a context | Module |
|---|---|---|
| `http::get`, `http::post`, `http::fetch` | `http::async_get`, `http::async_post`, `http::async_fetch` | HTTP/1 |
| `http2::get`, `post`, `fetch`; the same in `http3` | `http2::async_get`, `async_post`, `async_fetch`; the same in `http3` | HTTP/2, HTTP/3 |
| `tcp::exchange`, `udp::exchange` | `tcp::async_exchange`, `udp::async_exchange` | net |

The blocking form creates a context on the calling thread, runs the coroutine form to completion and returns an
owned result, so a program that only needs a response does not see the reactor at all. It takes
`blocking_options` (`<pace/runtime/blocking.hpp>`) with a cancellation token and a total deadline of 30 seconds
by default, because a blocking call without a deadline can hang a thread that has no other way out. It throws
`std::logic_error` when called on a thread that is already running a PACE context: blocking there would stall
every coroutine on that reactor, so the coroutine form is the only correct choice. The coroutine forms take
no implicit deadline; they inherit cancellation and deadlines from the operation boundary they run in.

The URL calls own their arguments, parse the URL lazily and choose DNS and TLS according to the build: a hostname
needs `--dns=y` and `https` needs `--tls=y`, numeric `http` URLs work with the default build. They keep the
protocol limits of the explicit API and add no retries, redirects, cookies or decompression, so their behavior is
exactly that of one exchange. See the [tutorial](../tutorial/02-http-client.md) and the
[client reference](../reference/http/clients.md).

## One-shot entry

`pace::run(factory)` (`<pace/runtime/run.hpp>`) creates a context on the calling thread, runs the task the factory
returns, stops the context when that task finishes and returns its value:

```cpp
const auto response = pace::run([](pace::io_context& io) {
    return pace::http::async_get(io, pace::tcp::endpoint{"127.0.0.1", 8080}, "/health");
});
```

Without it, every program repeats the same scaffolding: construct a context, spawn a task that stores its result
somewhere and stops the context, run the context, then rethrow any stored exception. `run` does exactly that and
nothing more. The context is still passed explicitly, so the code inside the factory is the same code that would
run on a runtime worker. The factory is owned until shutdown finishes, which makes a capturing coroutine lambda
safe here, where invoking it directly would leave its captures dangling. The cost is one wrapper coroutine for the
entry, not another thread. Entry completion cancels background roots; it is not a structured join of everything
spawned. Use [task groups](../reference/runtime/structured-concurrency.md) to join concurrent work, and explicit
contexts and listeners when application lifetimes require them.

## Ordinary handlers and asynchronous handlers

A handler that needs no wait should be an ordinary function. `http::make_handler` and `http::router`
(`<pace/http/router.hpp>`) adapt both `response(request)` and `task<response>(request)` callables, with or without
the `request` parameter, to the one handler type shared by HTTP/1, HTTP/2 and HTTP/3. Function pointers and
trivially copyable empty callables are stored by value, so the common stateless lambda costs nothing beyond the
`std::function`. Stateful callables are held through shared storage: pending invocations survive destruction of
the router, including move-only captures, and copies of a handler observe the same state. That state is shared
across worker contexts, so the application synchronizes it. Router copies share their read-only route table and
detach it on registration, so a server can copy its handler for each accepted connection without copying routes.

The router deliberately matches exact, case-sensitive method and path pairs, answers 404 and 405 (with `Allow`)
itself and falls back from HEAD to GET; `mount()` adds segment prefixes with longest-prefix selection. Routes are
kept sorted by path, so a lookup is one binary search plus a scan of the methods on that path. There are no path
parameters, middleware or URL normalization: applications that need them keep their own handler, which works with
all three HTTP servers because they share the message and handler types.

## Fewer listener declarations when control is unnecessary

`http::serve(io, endpoint, handler)` owns its HTTP/1 listener. Binding happens when the task starts, so a bind
error propagates through the coroutine like any other failure. `http::app` (`<pace/http1.hpp>`) goes one step
further: it collects routes, mounts and proxies, and `run(port)` serves them on the calling thread, binding
loopback when only a port is given. It snapshots its routes when serving starts and installs no signal handlers
and no worker threads; `run_workers` is the explicit opt-in to one listener per worker.

Keep the `serve(listener, …)` overload for immediate bind results, port-zero lookup, TLS, admission and streaming
options, or stopping acceptance independently. A shorter entry does not provide signal handling or graceful
connection draining; [graceful shutdown](../tutorial/09-graceful-shutdown.md) shows how to build both from the
explicit API. The tutorials teach both forms step by step, including their lifetime limits:
[learning path](../tutorial/README.md).
