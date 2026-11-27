# HTTP/1 application

[简体中文](app.zh.md) · [HTTP/1](README.md)

## Overview

`http::app` collects routes, static directories and reverse proxies, then serves them over HTTP/1: on the calling
thread with `run`, on several worker threads with `run_workers`, or as a task on a context you already run with
`serve`.

| Header | Target | Build option |
|:--|:--|:--|
| `<pace/http1.hpp>` | `pace_http1` | `--http1=y` (default) |

An `app` is a thin layer over [`router`](../http/router.md), [`static_files`, `reverse_proxy`](../gateway/README.md)
and the buffered [`serve`](server.md). It installs no signal handler, starts no background thread on its own and
has no middleware, path parameters or URL decoding.

## Synopsis

```cpp
#include <pace/http1.hpp>

int main()
{
    pace::http::app app;
    app.get("/", [] { return pace::http::text("hello\n"); })
        .post("/echo", [](pace::http::request request) { return pace::http::text(std::move(request.body)); })
        .mount("/assets", "./public")
        .proxy("/api", "http://127.0.0.1:9000");
    app.run(8080); // http://127.0.0.1:8080, until the process is stopped
}
```

## Types

### `app`

```cpp
class app
{
public:
    template <class Function>
    app& route(std::string method, std::string path, Function&& function);
    template <class Function>
    app& get(std::string path, Function&& function);
    template <class Function>
    app& post(std::string path, Function&& function);

    app& mount(std::string prefix, std::filesystem::path directory, static_options options = {});
    app& proxy(std::string prefix, std::vector<std::string> upstreams, proxy_options options = {});
    app& proxy(std::string prefix, std::initializer_list<std::string> upstreams, proxy_options options = {});
    app& proxy(std::string prefix, std::string upstream, proxy_options options = {});

    void run_workers(std::size_t workers, tcp::endpoint endpoint, limits bounds = {}) const;
    task<void> serve(io_context& io, tcp::endpoint endpoint, limits bounds = {}) const;
    task<void> serve(tcp::listener& listener, limits bounds = {}) const;
    void run(tcp::endpoint endpoint, limits bounds = {}) const;
    void run(std::uint16_t port = 8080, limits bounds = {}) const;

    [[nodiscard]] router handler() const;
};
```

**Configuration.** Every configuration member returns `*this`, so calls chain.

| Member | Effect |
|:--|:--|
| `route(method, path, function)` | `router::add`: an exact route. `function` takes a `request` or nothing and returns `response` or `task<response>` |
| `get(path, function)`, `post(path, function)` | `route("GET", …)`, `route("POST", …)` |
| `mount(prefix, directory, options)` | Serves files under `directory` for paths under `prefix`, with a [`static_files`](../gateway/README.md#static_files) mounted on the router |
| `proxy(prefix, upstreams, options)` | Forwards requests under `prefix` to one or more upstream base URLs, with a [`reverse_proxy`](../gateway/README.md#reverse_proxy) mounted on the router |

Exact routes win over mounts and proxies, and the longest matching prefix wins among those; the full
[matching rules](../http/router.md#types) are on the router page. To read cache statistics or clear the cache of a
static or proxy service, construct the service yourself and mount it on a `router` instead.

**Serving.**

| Member | Effect |
|:--|:--|
| `serve(io, endpoint, bounds)` | A task that binds `endpoint` when it starts, then serves; see `serve(io_context&, tcp::endpoint, …)` on the [server page](server.md#serve) |
| `serve(listener, bounds)` | A task that serves on a listener you own; it borrows the listener |
| `run(endpoint, bounds)` | Creates a context on the calling thread, serves `endpoint` on it and returns only when that context stops or serving fails |
| `run(port, bounds)` | `run({"127.0.0.1", port}, bounds)`: loopback only. Pass an endpoint such as `{"0.0.0.0", 8080}` to listen on other interfaces |
| `run_workers(workers, endpoint, bounds)` | Creates a `runtime` of `workers` contexts, binds one listener per context to `endpoint` and runs them: context 0 on the calling thread, the others on `workers - 1` new threads. Returns when the runtime stops |
| `handler()` | A copy of the routes as a `router`, for HTTP/2, HTTP/3, TLS, `server_options` or your own adapters |

`serve` and `handler` take a snapshot of the routes when they are called: routes added afterwards do not reach a
server that has already started, and the task does not refer to the `app`. `run` and `run_workers` serve the
routes as they are when called.

With more than one worker, `run_workers` binds every listener with `SO_REUSEPORT` (`SO_REUSEPORT_LB` on FreeBSD)
so the kernel spreads connections across them; this works on Linux, macOS and FreeBSD. On Windows only one worker
is possible. With port 0, the first listener picks a free port and the others bind the same one.

## Errors

| Situation | Exception |
|:--|:--|
| Invalid method, path or prefix; duplicate route or mount | `std::invalid_argument` from the configuration call |
| Invalid static or proxy options, or upstream URL | `std::invalid_argument` from `mount` or `proxy` (see [gateway errors](../gateway/README.md#errors)) |
| `mount` directory cannot be opened | `std::system_error` from `mount` |
| Bind or listen failure | `std::system_error` from `run`, `run_workers` or the `serve(io, endpoint, …)` task |
| `run` or `run_workers` called on a thread that already runs a PACE context | `std::logic_error` |
| `run_workers(0, …)` | `std::invalid_argument` |
| `run_workers` with several workers where `SO_REUSEPORT` does not exist | `std::system_error` with `std::errc::operation_not_supported` |

Errors of individual requests and connections are handled by the server and never leave `run`; see the
[server page](server.md#errors).

## Threads and concurrency

Configure the app on one thread before serving; configuration is not synchronized. `run` blocks the calling thread
and serves on it alone. `run_workers` serves on `workers` threads at once: every handler, static service and proxy
then runs concurrently on several threads, so state shared by handlers must be synchronized. The gateway services
already are (see [gateway threads](../gateway/README.md#threads-and-concurrency)).

To stop `run` or `run_workers` from code, for example on a signal, use `serve` with your own `io_context` or
`runtime` and stop that ([graceful shutdown](../../tutorial/09-graceful-shutdown.md)).

## Lifetime and ownership

The app owns its router. `serve`, `run` and `handler` hold copies of it and do not borrow the app, so the app may be
destroyed while a `serve` task is still running. `serve(listener, …)` borrows the listener, which must outlive
the task. Captures of handlers live as long as the routes that hold them.

## Limits and defaults

| Item | Default |
|:--|:--|
| `run` port | 8080 |
| `run(port)` address | `127.0.0.1` |
| `limits` | See the [HTTP/1 page](README.md#limits-and-defaults) |
| Listener backlog | 1024 |
| Worker listeners | One per context, `SO_REUSEPORT` when `workers > 1` |
| `static_options`, `proxy_options` | See [gateway defaults](../gateway/README.md#limits-and-defaults) |

## See also

- [Routing](../http/router.md), [HTTP/1 server](server.md), [gateway services](../gateway/README.md)
- [`pace::run`](../runtime/run.md), [runtime and workers](../runtime/runtime.md)
- Tutorial: [HTTP server](../../tutorial/03-http-server.md), [workers and threads](../../tutorial/08-workers-and-threads.md),
  [gateway](../../tutorial/14-gateway.md)
