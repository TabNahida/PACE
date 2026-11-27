# Gateway services

[简体中文](README.zh.md) · [Reference](../README.md)

## Overview

Handlers that turn a PACE HTTP/1 server into a small embedded gateway: `static_files` serves a directory,
`reverse_proxy` forwards to fixed upstream servers with load balancing, and `response_cache` is the bounded,
sharded cache both use.

| Header | Target | Build option |
|:--|:--|:--|
| `<pace/gateway.hpp>` (content in `<pace/gateway/services.hpp>`) | `pace_http1` | `--http1=y` (default) |

The gateway has no target of its own yet: it is compiled into `pace_http1` and enabled together with HTTP/1,
because `http::app` uses it and the proxy uses the HTTP/1 client. `<pace/http1.hpp>` also declares it. The named
module is `pace.gateway`; the names are in namespace `pace::http`. An HTTPS upstream needs `--tls=y`, an upstream
with a host name `--dns=y`.

Both services are ordinary `http::handler` values. Mount them with [`app::mount` and `app::proxy`](../http1/app.md)
or with [`router::mount`](../http/router.md). Everything is buffered: there is no configuration language, process
supervision, compression, disk cache, certificate management, active health check or WebSocket/`CONNECT`
tunnelling.

## Synopsis

```cpp
#include <pace/gateway.hpp>
#include <pace/http1.hpp>

#include <iostream>

int main()
{
    pace::http::static_options files;
    files.cache.max_bytes = 64 * 1024 * 1024; // enable the local file cache
    pace::http::static_files assets("/assets", "./public", files);

    pace::http::proxy_options proxy;
    proxy.balance = pace::http::load_balance::least_connections;
    pace::http::reverse_proxy api("/api", {"http://127.0.0.1:9001", "http://127.0.0.1:9002"}, proxy);

    pace::http::router routes;
    routes.mount("/assets", assets).mount("/api", api);
    routes.get("/health", [] { return pace::http::text("ok\n"); });

    pace::run([&](pace::io_context& io) { return pace::http::serve(io, {"127.0.0.1", 8080}, routes); });
    std::cout << assets.cache_stats().hits << " cache hits\n";
}
```

## Types

### `static_files`

```cpp
class static_files
{
public:
    static_files(std::string prefix, std::filesystem::path directory, static_options options = {});
    task<response> operator()(request incoming) const;
    void clear_cache() const;
    cache_statistics cache_stats() const;
};
```

Serves regular files below `directory` for request paths below `prefix`. The constructor opens `directory`, which
must exist. Copies share one service, including its cache.

- `operator()` answers `GET` and `HEAD`; other methods get 405 with `Allow: GET, HEAD`. A path outside `prefix`
  gets 404. The part after the prefix is percent-decoded once and mapped to a file.
- A directory is served through its `index` file, without a redirect and without a listing. Link to directories with
  a trailing `/` so that relative links in the page resolve.
- Responses carry `Content-Type` from the file extension, `ETag`, `Last-Modified`, `Accept-Ranges: bytes`,
  `X-Content-Type-Options: nosniff` and, when set, `Cache-Control`. The ETag is a hash (XXH64 of the content) plus
  the size, so it changes with any edit, including one that keeps size and timestamp. It is not a cryptographic
  checksum.
- `If-None-Match` (weak or strong tags, or `*`) and, without it, `If-Modified-Since` (equal to `Last-Modified`,
  exact text) produce 304.
- `GET` honours one byte range, `bytes=a-b`, `bytes=a-` or `bytes=-n`, with 206 and `Content-Range`; an
  unsatisfiable range gets 416. Several ranges, other units and an `If-Range` that does not match the ETag or
  `Last-Modified` exactly are ignored, and the full file is sent. `HEAD` ignores `Range`.
- `clear_cache()` empties the local cache; `cache_stats()` reads its counters.

**Path safety.** Rejected with 400: invalid percent escapes, `.` and `..` segments, control bytes, backslashes,
Windows alternate data streams (`:`), segments ending in a dot or space, and hidden segments (starting with `.`)
unless `allow_dotfiles` is set. Symbolic links are never followed, neither as the file nor as an intermediate
directory; on Windows reparse points are rejected too and the opened file's final path is checked against the
root. Missing files and anything other than a regular file get 404. The root directory itself is trusted: publish
only a directory whose whole content may be served.

**Reads.** Files are read synchronously on the event loop and returned as a buffered body, up to
`max_file_bytes` (larger files get 413). There is no `sendfile`, asynchronous disk I/O or streaming. Even a cache
hit copies the body into the response. Keep served files small; a large or cold file set belongs on a dedicated
file server.

**Local cache.** When `options.cache.max_bytes` is non-zero, 200 responses are cached by decoded path (the query
is ignored), with their validators, until the TTL ends or they are evicted. Files are not checked again on a hit,
so a changed or deleted file can be served from the cache until then; call `clear_cache()` after deploying. A
request with `Cache-Control` or `Pragma` bypasses the cache.

### `static_options`

```cpp
struct static_options
{
    std::string index = "index.html";
    std::size_t max_file_bytes = std::size_t{8} * 1024 * 1024;
    bool allow_dotfiles = false;
    std::string cache_control = "no-cache";
    cache_options cache{};
};
```

| Member | Meaning |
|:--|:--|
| `index` | File served for a directory: one file name, no separators, `%`, `.` or `..`. Empty means a directory gets 404 |
| `max_file_bytes` | Largest file served; larger files get 413 |
| `allow_dotfiles` | Serve path segments that start with `.` |
| `cache_control` | `Cache-Control` value sent to clients; empty sends none. Independent of the local cache |
| `cache` | The local cache; disabled while `cache.max_bytes` is 0 |

### `reverse_proxy`

```cpp
class reverse_proxy
{
public:
    reverse_proxy(std::string prefix, std::vector<std::string> upstreams, proxy_options options = {});
    task<response> operator()(request incoming) const;
    void clear_cache() const;
    cache_statistics cache_stats() const;
};
```

Forwards requests under `prefix` to one of `upstreams` over HTTP/1.1 and returns the upstream's response. Copies
share one service, including its cache.

**Upstreams** are absolute `http://` or `https://` base URLs without query or fragment, parsed like
[client URLs](../http/clients.md#url-rules). HTTPS upstreams are verified against the system roots with ALPN
`http/1.1`. Upstreams speak HTTP/1.1 only; there is no HTTP/2 or HTTP/3 upstream and no protocol translation.

**Forwarding.** With `strip_prefix`, the prefix is removed from the target (`/api/a?x=1` mounted at `/api` becomes
`/a?x=1`), then the upstream's base path is prepended. Method, body and query are kept. `Host` becomes the
upstream's authority. Hop-by-hop fields, fields named by `Connection`, framing and trailer fields, and incoming
`Forwarded` and `X-Forwarded-*` fields are removed; no client address is added, so add your own identity header if
the upstream needs one. `CONNECT` and requests with `Upgrade` get 501. The upstream response is buffered up to
`max_response_bytes`, its trailers are dropped and HTTP/1 framing is regenerated. A `HEAD` response keeps the
upstream's `Content-Length` through `response::representation_length`.

**Balancing.** Each worker context keeps its own connection pools and counters for each upstream; nothing is shared
between threads except the cache.

- `round_robin` takes the next upstream that is neither cooling down nor at `max_connections`.
- `least_connections` takes the eligible upstream with the fewest active exchanges, rotating among ties.
- A transport failure, timeout or malformed response puts the upstream in a cooldown of `fail_timeout` and drops
  its idle connections. HTTP error statuses do not count as failures.
- Idle keep-alive connections (at most `max_idle_connections` per upstream) are reused; one idle longer than
  `idle_timeout` is discarded when it is next picked.

Nothing is queued and nothing is retried: when no upstream is eligible the reply is 503 at once, and a request
that fails is not replayed, not even a `GET` that met a stale keep-alive connection.

**Proxy cache.** When `options.cache.max_bytes` is non-zero, the proxy caches a response only if all of these hold:

- the request is a `GET` with no body and a target of at most 4096 bytes, without `Authorization`, `Cookie`,
  `Range`, any `If-*` field, `Cache-Control` or `Pragma`;
- the response is a 200 whose `Cache-Control` contains `public` or `s-maxage`, without `private`, `no-cache`,
  `no-store` or an unrecognized directive, and without `Set-Cookie`, `Vary`, `Content-Range` or `Pragma`.

The key is the downstream `Host` plus the full target with query. The lifetime is the smaller of the configured
TTL and the upstream's freshness, reduced by its `Date` and `Age`; cache hits carry an updated `Age`. This is a
deliberately small subset of HTTP caching, not a full RFC 9111 cache.

### `proxy_options`, `load_balance`

```cpp
enum class load_balance
{
    round_robin,
    least_connections
};

struct proxy_options
{
    load_balance balance = load_balance::round_robin;
    bool strip_prefix = true;
    std::size_t max_connections = 64;
    std::size_t max_idle_connections = 16;
    std::size_t max_response_bytes = std::size_t{8} * 1024 * 1024;
    std::chrono::milliseconds timeout{30000};
    std::chrono::seconds idle_timeout{30};
    std::chrono::milliseconds fail_timeout{1000};
    cache_options cache{};
};
```

| Member | Meaning |
|:--|:--|
| `balance` | Upstream selection, per worker context |
| `strip_prefix` | Remove the mount prefix before forwarding |
| `max_connections` | Concurrent exchanges per upstream per worker context; beyond it the upstream is skipped. Must be at least 1 |
| `max_idle_connections` | Idle keep-alive connections kept per upstream per worker context; at most `max_connections` |
| `max_response_bytes` | Largest upstream body |
| `timeout` | One deadline for DNS, connect, TLS and the exchange; positive. Expiry gives 504 |
| `idle_timeout` | Age after which an idle connection is discarded instead of reused |
| `fail_timeout` | Cooldown of an upstream after a failure |
| `cache` | The shared proxy cache; disabled while `cache.max_bytes` is 0 |

### `response_cache`

```cpp
class response_cache
{
public:
    explicit response_cache(cache_options options = {});
    ~response_cache();
    response_cache(const response_cache&) = delete;
    response_cache& operator=(const response_cache&) = delete;
    std::shared_ptr<const response> get(std::string_view key, std::chrono::seconds* resident_age = nullptr);
    std::optional<response> get_copy(std::string_view key, std::chrono::seconds* resident_age = nullptr);
    void put(std::string key, response value, std::chrono::seconds ttl);
    void clear();
    [[nodiscard]] cache_statistics statistics() const;
};
```

A thread-safe cache of immutable responses by string key, split into independently locked shards and evicting with
the CLOCK algorithm. It stores whatever it is given; the HTTP rules above belong to the services that use it.

| Member | Effect |
|:--|:--|
| `get(key, resident_age)` | The stored snapshot, or null when missing or expired. Writes the time since insertion to `*resident_age` when given |
| `get_copy(key, resident_age)` | An owned copy of the stored response, or empty. On a running context it remembers the last snapshot it returned for this cache, so repeated hits on the same key skip the shard lock and the shared reference count. Outside a context it copies the result of `get` |
| `put(key, value, ttl)` | Stores `value` for `min(ttl, options.ttl)`. Ignored when the cache is disabled, `ttl` is not positive, the body is larger than `max_object_bytes` or the entry is larger than its shard's budget. Replaces an entry with the same key; evicts as needed |
| `clear()` | Removes every entry. Statistics are kept. Responses already returned stay valid |
| `statistics()` | Current counters |

A `clear` or a replaced entry is seen by the next lookup; a miss that races with `clear` may store its result
again afterwards, and concurrent misses on one key are not coalesced. `get_copy` keeps one snapshot per cache per
context, which can outlive its eviction until the next lookup on that context or the context's destruction; create
caches and cached services once, not per request.

### `cache_options`

```cpp
struct cache_options
{
    std::size_t max_bytes = 0;
    std::size_t max_entries = 1024;
    std::size_t max_object_bytes = std::size_t{1024} * 1024;
    std::chrono::seconds ttl{60};
    std::size_t shards = 16;
};
```

| Member | Meaning |
|:--|:--|
| `max_bytes` | Budget for stored entries, counting body, key and header fields. 0 disables the cache |
| `max_entries` | Number of entries; at least 1 |
| `max_object_bytes` | Largest body stored |
| `ttl` | Longest lifetime of an entry; not negative |
| `shards` | Number of shards, 1 to 256 (at most `max_entries` are used). Budgets and entries are divided equally among them, so uneven keys can leave part of the total unused |

`max_bytes` is not a limit on process memory: slot storage, allocator overhead and snapshots still held by
responses in flight or by `get_copy` come on top.

### `cache_statistics`

```cpp
struct cache_statistics
{
    std::uint64_t hits = 0, misses = 0, evictions = 0;
    std::size_t entries = 0, bytes = 0;
};
```

Hits (including `get_copy` hits from every context, live or destroyed), misses, evictions, current entries and
accounted bytes. Counters are summed shard by shard, so a sample taken during traffic is not one atomic snapshot.

## Errors

| Situation | Result |
|:--|:--|
| Prefix not starting with `/`, or containing `?`, `#`, `\`, `%`, spaces or control characters | Constructor throws `std::invalid_argument` |
| Invalid `index` or `cache_control` | `static_files` constructor throws `std::invalid_argument` |
| Static directory cannot be opened | `static_files` constructor throws `std::system_error` |
| No upstream, `max_connections` 0, `max_idle_connections > max_connections`, `timeout` not positive, negative `idle_timeout` or `fail_timeout` | `reverse_proxy` constructor throws `std::invalid_argument` |
| Upstream URL invalid, with query or fragment, HTTPS without TLS support, host name without DNS support | `reverse_proxy` constructor throws `std::invalid_argument` |
| `cache_options` with `shards` 0 or above 256, `max_entries` 0 or negative `ttl` | Constructor throws `std::invalid_argument` |
| Static request: bad method / unsafe path / missing file / too large | 405 / 400 / 404 / 413 response |
| Proxy: no eligible upstream | 503 response |
| Proxy: connection, TLS or transport failure | 502 response |
| Proxy: malformed upstream response | 502 response |
| Proxy: `timeout` reached | 504 response |
| Proxy: `CONNECT` or `Upgrade` | 501 response |
| Cancellation of the downstream request | Propagates as `std::system_error` |

The deadline can interrupt only operations that wait; it cannot stop synchronous work.

## Threads and concurrency

Services and caches may be shared by any number of worker contexts. The caches are internally synchronized;
`clear_cache`, `cache_stats` and every `response_cache` member may be called from any thread. Proxy connection
pools, active counts and the round-robin position live in each context and are never shared, so the limits and
the balancing apply per worker. Static file reads run synchronously on the context that serves the request.

## Lifetime and ownership

`static_files` and `reverse_proxy` are copyable handles to shared state. The task returned by `operator()` owns a
reference to the service and the request, so the service object may be destroyed while requests are in flight.
Each context that used a proxy, or called `get_copy` on a cache, keeps that state until the context is destroyed:
configure a fixed set of services rather than creating one per request. Snapshots returned by `get` stay valid
while held, independently of the cache.

## Limits and defaults

| Item | Default |
|:--|:--|
| `static_options::index` | `index.html` |
| `static_options::max_file_bytes` | 8 MiB |
| `static_options::allow_dotfiles` | `false` |
| `static_options::cache_control` | `no-cache` |
| `proxy_options::balance` | `round_robin` |
| `proxy_options::strip_prefix` | `true` |
| `proxy_options::max_connections` | 64 per upstream per context |
| `proxy_options::max_idle_connections` | 16 per upstream per context |
| `proxy_options::max_response_bytes` | 8 MiB |
| `proxy_options::timeout` | 30 s |
| `proxy_options::idle_timeout` | 30 s |
| `proxy_options::fail_timeout` | 1 s |
| `cache_options::max_bytes` | 0 (cache disabled) |
| `cache_options::max_entries` | 1024 |
| `cache_options::max_object_bytes` | 1 MiB |
| `cache_options::ttl` | 60 s |
| `cache_options::shards` | 16 |
| Cacheable proxy target | 4096 bytes |

The static ETag hash is adapted from [xxHash 0.8.3](https://github.com/Cyan4973/xxHash/tree/v0.8.3)
(BSD-2-Clause); its copyright and license are kept in `src/gateway/content_hash.hpp`. It adds no dependency.

## See also

- [HTTP/1 application](../http1/app.md), [routing](../http/router.md), [HTTP/1 server](../http1/server.md)
- [Gateway performance](../../performance/http-gateway.md)
- Tutorial: [gateway](../../tutorial/14-gateway.md), [workers and threads](../../tutorial/08-workers-and-threads.md)
