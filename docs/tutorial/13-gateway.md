# 13. Serve files and proxy an API

[中文](13-gateway.zh-CN.md) · [Learning path](README.md)

This chapter uses the gateway APIs on the development branch after 0.4.0.
Build from this checkout with HTTP/1 enabled (the default).

## Mount a directory

Create `public/index.html` containing `<h1>Hello from PACE</h1>`. Write `main.cpp`:

```cpp
#include <pace/http.hpp>

int main() {
    pace::http::app app;
    app.mount("/", "./public");
    app.run(8080);
}
```

Use the xmake project from [chapter 1](01-build.md), linking `pace_http1`.
Run it from the directory containing `public`, then visit `http://127.0.0.1:8080/`.
`/index.html` returns the same file. Mounting at `/assets` instead maps
`/assets/index.html` to `public/index.html`. Files outside that directory are not
served; directory listings are not generated.

## Add a reverse proxy

Run a second PACE application on port 9000:

```cpp
#include <pace/http.hpp>
int main() {
    pace::http::app origin;
    origin.get("/hello", [] { return pace::http::text("hello from the API\n"); });
    origin.run(9000);
}
```

In the first application, before `app.run(8080)`, add:

```cpp
app.proxy("/api", "http://127.0.0.1:9000");
```

`http://127.0.0.1:8080/api/hello` now returns `hello from the API`.
The default strips `/api`, so the upstream receives `/hello`.
The more specific `/api` mount wins over `/`; exact routes win over mounts.
To balance across two running upstreams, pass both URLs:

```cpp
app.proxy("/api", {"http://127.0.0.1:9000", "http://127.0.0.1:9001"});
```

Requests use round-robin selection and reusable upstream connections. Both
upstreams must serve the same API. A failed exchange is not replayed.

## Turn on bounded caching

Caching is disabled by default. Replace the static mount with:

```cpp
using namespace std::chrono_literals;
pace::http::static_options files;
files.cache.max_bytes = 16 * 1024 * 1024;
files.cache.ttl = 10s;
files.cache_control = "public, max-age=10";
app.mount("/", "./public", files);
```

PACE can now reuse a file's bytes for ten seconds. Editing it may remain invisible
until that TTL expires. `cache_control` controls downstream caches separately;
it does not enable PACE's cache. Cache budgets and file-size limits are independent.

To configure the proxy instead:

```cpp
pace::http::proxy_options proxy;
proxy.balance = pace::http::load_balance::least_connections;
proxy.timeout = 2s;
proxy.cache.max_bytes = 32 * 1024 * 1024;
proxy.cache.ttl = 10s;
app.proxy("/api", {"http://127.0.0.1:9000", "http://127.0.0.1:9001"}, proxy);
```

Only eligible public GET responses are stored. To make `/hello` cacheable, have
its origin handler return a response with `Cache-Control: public, max-age=10`.
Responses carrying cookies or `Vary`, and requests carrying authentication or
cookies, bypass this shared cache.

## Use workers

On Linux/macOS/FreeBSD, replace `app.run(8080)` with:

```cpp
app.run_workers(4, {"0.0.0.0", 8080});
```

Each worker owns a reactor, listener and upstream connection pool. This helper
blocks until shutdown and uses `SO_REUSEPORT`; Windows currently supports one
worker through this helper. For application-controlled shutdown, use
`app.serve(listener)` in your existing runtime as in [chapter 6](06-shutdown.md).

Try changing the mount prefix, then send a HEAD request and a byte-range request
with curl. Compare `files.cache.max_bytes = 0` with caching enabled. The
[reference](../reference/http-gateway.md) explains validators, invalidation,
concurrency, limits and the differences from a full nginx deployment.
[Backend selection](../reference/io-backends.md) and [local nginx measurements](../gateway-performance.md)
cover the next tuning steps.
