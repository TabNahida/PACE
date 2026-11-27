# 14. Static files and a reverse proxy

[中文](14-gateway.zh.md) · [Learning path](README.md) · [Previous: QUIC and HTTP/3](13-quic-and-http3.md)

`http::app` can also serve a directory and forward a path prefix to other servers. These gateway services come with
`pace_http1` (HTTP/1 is enabled by default) and are declared in `<pace/gateway.hpp>`, which `<pace/http1.hpp>`
includes.

## Mount a directory

Create `public/index.html` containing `<h1>Hello from PACE</h1>`. Save this as `gateway.cpp`, with a `gateway`
target that depends on `pace_http1`:

```cpp
#include <pace/http1.hpp>

int main()
{
    pace::http::app app;
    app.mount("/", "./public");
    app.run(8080);
}
```

Run it from the directory that contains `public` (`xmake run` starts programs in their build directory, so use
`xmake run -w . gateway` from the project directory), then open `http://127.0.0.1:8080/`. `mount` checks the
directory when it is called: if `./public` does not exist there, it throws `std::system_error` and this program,
which has no `catch`, terminates. `/index.html` returns the
same file. Mounting at `/assets` instead maps `/assets/index.html` to `public/index.html`. Files outside the
directory are never served, and no directory listings are generated.

## Add a reverse proxy

Run a second PACE program as the API server on port 9000 (`api.cpp`, another target with `pace_http1`):

```cpp
#include <pace/http1.hpp>

int main()
{
    pace::http::app origin;
    origin.get("/hello", [] { return pace::http::text("hello from the API\n"); });
    origin.run(9000);
}
```

In `gateway.cpp`, before `app.run(8080)`, add:

```cpp
app.proxy("/api", "http://127.0.0.1:9000");
```

`http://127.0.0.1:8080/api/hello` now returns `hello from the API`. By default the prefix is stripped, so the
upstream receives `/hello`. The more specific `/api` mount wins over `/`, and exact routes win over mounts. To spread
requests over two upstreams that serve the same API, pass both URLs:

```cpp
app.proxy("/api", {"http://127.0.0.1:9000", "http://127.0.0.1:9001"});
```

Requests go to the upstreams in turn (round robin) and reuse idle upstream connections. A failed exchange is not
replayed on another upstream.

## Turn on bounded caching

Caching is off by default. Include `<chrono>` and replace the static mount with:

```cpp
using namespace std::chrono_literals;
pace::http::static_options files;
files.cache.max_bytes = 16 * 1024 * 1024;
files.cache.ttl = 10s;
files.cache_control = "public, max-age=10";
app.mount("/", "./public", files);
```

PACE now keeps a file's bytes for up to ten seconds, so an edit to the file can stay invisible until then.
`cache_control` sets the `Cache-Control` header for downstream caches; it does not enable PACE's own cache. The cache
budget and the file-size limit are separate settings.

To configure the proxy instead:

```cpp
pace::http::proxy_options proxy;
proxy.balance = pace::http::load_balance::least_connections;
proxy.timeout = 2s;
proxy.cache.max_bytes = 32 * 1024 * 1024;
proxy.cache.ttl = 10s;
app.proxy("/api", {"http://127.0.0.1:9000", "http://127.0.0.1:9001"}, proxy);
```

Only public GET responses that are eligible get stored. To make `/hello` cacheable, have its handler return a
response with the header `Cache-Control: public, max-age=10`. Responses that carry cookies or `Vary`, and requests
that carry credentials or cookies, bypass this shared cache.

## Use workers

On Linux, macOS and FreeBSD, replace `app.run(8080)` with:

```cpp
app.run_workers(4, {"0.0.0.0", 8080});
```

Each worker has its own event loop, listener and upstream connection pool; the workers share the port through
`SO_REUSEPORT`. The call blocks until shutdown, and the calling thread is one of the four workers. On Windows this
function supports a single worker. For shutdown under your application's control, use `app.serve(listener)` on a
runtime you run yourself, as in [chapter 9](09-graceful-shutdown.md).

Change the mount prefix, then send a HEAD request and a byte-range request with curl
(`curl -r 0-3 http://127.0.0.1:8080/`). Compare `files.cache.max_bytes = 0` with caching enabled. The
[gateway reference](../reference/gateway/README.md) explains validators, invalidation, concurrency, limits and how
this differs from a full nginx deployment. [I/O backends](../reference/runtime/io-backends.md) and the
[gateway measurements](../performance/http-gateway.md) cover the next tuning steps.

Next: [Guides](../guides/README.md), for recipes beyond the tutorial.
