# HTTP gateway services

[Guided tutorial](../tutorial/13-gateway.md) · [中文](http-gateway.zh-CN.md)

Include `<pace/http.hpp>` and link `pace_http1`. These APIs are new after 0.4.0.
The implementation is an embedded, buffered HTTP gateway, with no configuration
language, process supervisor, compression, disk cache, automatic TLS certificate
management, active health checks or WebSocket/CONNECT tunneling.

## Routes and lifetime

```cpp
app& app::mount(std::string prefix, std::filesystem::path directory, static_options = {});
app& app::proxy(std::string prefix, std::string upstream, proxy_options = {});
app& app::proxy(std::string prefix, std::vector<std::string> upstreams, proxy_options = {});
app& app::proxy(std::string prefix, std::initializer_list<std::string> upstreams, proxy_options = {});
void app::run_workers(std::size_t workers, tcp::endpoint endpoint, limits = {}) const;
```

Prefixes are absolute URL paths and match segment boundaries (`/api` does not match
`/apix`). Longest prefix wins; an exact route, including its method restrictions,
wins over mounts. Query strings do not affect prefix selection. Like other routes,
`handler()`/`serve()` capture a route snapshot; later configuration changes do not
change existing handlers. Configure routing before starting workers.

`run_workers` creates a fixed runtime and one listener per context. Multi-worker
listening requires `SO_REUSEPORT` (Linux, macOS, FreeBSD); Windows supports one worker
through this helper. There is no background process or signal handler. Use
`app.serve(listener)` with your own runtime for programmatic graceful shutdown.

`static_files` and `reverse_proxy` are copyable handler values with shared internal
ownership. Calling either returns a task that owns the service and request. Their
cache methods are thread-safe. Proxy connection pools and balancing counters are
reactor-local, never shared sockets. A context retains each used proxy's state
until context destruction; configure a bounded set of services instead of creating
a fresh proxy per request. All I/O obeys the owning context's execution contract.

To retain cache controls, register a named service through the router:

```cpp
pace::http::static_files assets("/assets", "./public", options);
pace::http::router routes;
routes.mount("/assets", assets);
// Pass routes to http::serve(listener, routes).
auto stats = assets.cache_stats();
assets.clear_cache();
```

The same pattern applies to `reverse_proxy`. Clearing entries does not revoke
responses already in flight. Concurrent misses can refill after a clear; it is
not a transaction barrier. There is no single-flight miss coalescing.

## Static files

| `static_options` field | Default | Contract |
|---|---:|---|
| `index` | `index.html` | One filename, no traversal or separators |
| `max_file_bytes` | 8 MiB | Larger files return 413 |
| `allow_dotfiles` | false | Hidden path components rejected by default |
| `cache_control` | `no-cache` | Downstream HTTP policy, independent of local caching |
| `cache` | disabled | Optional snapshot cache described below |

GET and HEAD support MIME inference, `ETag`, `Last-Modified`, `If-None-Match`,
exact canonical `If-Modified-Since`, and one byte range (including suffix ranges).
Weak ETags work for conditional GET; `If-Range` requires an exact validator.
Multiple ranges are ignored; unsatisfiable valid ranges return 416.
HEAD ignores Range and suppresses response bytes while retaining the complete representation length. Other
methods return 405 with `Allow: GET, HEAD`. No directory listing is generated.
Directory indexes also resolve without a trailing slash; there is no redirect.
Use slash-terminated directory links for relative browser asset URLs.

Paths are decoded once. Traversal components, invalid percent escapes, control
bytes, backslashes, Windows alternate-stream syntax, and trailing dot/space
components are rejected. Symlinks and intermediate symlink directories are not
followed. Windows also rejects reparse points and checks the opened handle's final
path against the configured root. The configured root itself is trusted; expose
only a directory whose contents you intend to publish. Regular files only.

Reads are **synchronous and buffered on the reactor**, bounded by `max_file_bytes`.
There is no sendfile, asynchronous disk executor or large-file streaming here.
Cache hits still copy the body for the owned HTTP response. Keep hot assets small;
large/cold-file deployments should retain a dedicated file server.

Static caching keys decoded file paths, excluding query parameters. It keeps file
bytes and validators until TTL/eviction/clear, without stat-on-hit. Replacing or
deleting a file does not invalidate its cached representation immediately.
Request `Cache-Control` or `Pragma` bypasses the local cache.

## Reverse proxy

Upstreams are fixed `http://`/`https://` base URLs without query/fragment.
Hostnames require `--dns=y`; HTTPS requires `--tls=y` and verified system trust.
Numeric HTTP upstreams need no optional modules. Upstream transport is HTTP/1.1.
The service does not negotiate HTTP/2 or HTTP/3 upstreams.

| `proxy_options` field | Default | Contract |
|---|---:|---|
| `balance` | `round_robin` | Or `least_connections`; counters are per worker |
| `strip_prefix` | true | `/api/a?x=1` at `/api` becomes `/a?x=1` |
| `max_connections` | 64 | Concurrent exchanges **per upstream per reactor** |
| `max_idle_connections` | 16 | Idle keep-alive connections per upstream per reactor |
| `max_response_bytes` | 8 MiB | Upstream buffered-body bound |
| `timeout` | 30 s | One cooperative DNS/connect/TLS/exchange deadline |
| `idle_timeout` | 30 s | Lazy expiration when a pooled connection is borrowed |
| `fail_timeout` | 1 s | Passive cooldown after failed exchange |
| `cache` | disabled | Shared eligible-response cache |

The base URL path is prepended after optional prefix removal. Method, body and
query survive forwarding. `Host` becomes the upstream authority. Hop-by-hop headers,
headers nominated by `Connection`, framing/trailer headers and incoming
`Forwarded`/`X-Forwarded-*` are removed. PACE regenerates HTTP/1 framing and does not
automatically invent a client IP. Configure application identity explicitly if
needed. Upgrade and CONNECT return 501. Response bodies are buffered and trailers
are not forwarded. Normal HTTP admission/body limits apply on the downstream side.

Round-robin skips cooling-down/saturated origins. Least-connections chooses the
fewest active exchanges among eligible origins, rotating ties. HTTP error status
codes do not count as transport failures. Saturation/all unavailable returns 503;
transport or malformed-response failure returns 502; exchange timeout returns
504. No queued admission, automatic retries or stale-on-error responses. A stale
keep-alive connection can fail one exchange; even GET is not automatically replayed.
Local cancellation propagates; deadlines can only interrupt cooperative waits.

HEAD from an upstream preserves Content-Length through
`response::representation_length`, used by the HTTP/1 server only for HEAD.
These gateway helpers target HTTP/1; there is no implicit protocol translation.

### Proxy cache eligibility

Only bodyless GET requests with a bounded target (4096 bytes) are candidates.
Authentication, cookies, ranges, conditional headers, `Cache-Control` or `Pragma`
bypass caching. Keys include downstream Host and the full target/query. Only 200
responses explicitly marked `public` or `s-maxage` are stored. `Set-Cookie`, `Vary`,
private/no-cache/no-store policies, content ranges and unrecognized cache directives
prevent storage. The effective TTL is bounded by configured TTL and upstream
freshness; upstream Date/Age reduce freshness, and cache hits update Age.
This deliberately conservative subset does not implement all RFC cache behavior.

## Shared cache and locality

| `cache_options` field | Default | Meaning |
|---|---:|---|
| `max_bytes` | 0 | Accounted stored-entry budget; zero disables cache |
| `max_entries` | 1024 | Entry count limit |
| `max_object_bytes` | 1 MiB | Body size eligibility bound |
| `ttl` | 60 s | Maximum local TTL |
| `shards` | 16 | Independent locks and budgets |

Budgets are partitioned equally between shards; skewed keys can underutilize the
aggregate budget. Entry accounting includes bodies, keys and header metadata.
Slot/hash-table allocations are separately bounded by `max_entries`; allocator
bookkeeping and snapshots held by senders mean **this is not a process RSS cap**.

`response_cache` provides `get(key, optional_resident_age)`, `put(key, response, ttl)`,
`clear()` and `statistics()`. Values are immutable shared snapshots. This generic
cache does not enforce HTTP policy; the static/proxy services apply their own rules.
Statistics expose hits, misses, evictions, entries and accounted bytes. Clearing
removes entries, not accumulated counters.

CLOCK replacement uses contiguous slots and a reference bit rather than mutating
a linked LRU list on every hit. Sharding limits lock contention, heterogeneous
lookup avoids temporary keys in `get`, and response copies happen after releasing
the lock. Per-reactor pools avoid a global connection mutex. Reactor descriptors
also use dense generation-checked slots, eliminating a second event-dispatch hash
lookup and rejecting late events after slot reuse. None uses CPU-model-specific
cache sizes or affinity. See [measurements and limitations](../gateway-performance.md).

## Reactor-local cache hits

Static mounts and proxies use `response_cache::get_copy()` internally. On a running
reactor it remembers **one** immutable snapshot per cache/context. Repeated hits
check the shard's invalidation generation and the original steady-clock expiry,
then return an independent response copy. They avoid the shard mutex, hash lookup,
CLOCK writes and snapshot reference-count increments. On a key change, expiry,
replacement, eviction or clear, the next lookup takes the shared path again.
Outside a reactor, `get_copy()` copies a regular `get()` snapshot. The context
retains the cache internals until context destruction; construct one long-lived
cache/service per mount rather than constructing a new cached service per request.

Generation and mutex/statistics storage are separated by 128-byte alignment;
reader hit counters are separately aligned and written only by their owning
reactor. `statistics()` includes live and retired reader hits (a concurrent sample
is not a globally atomic snapshot). The cache remains bounded by its configured
entry budget, but each reactor can retain one evicted snapshot, with body size at
most `max_object_bytes`, plus headers/key/reader metadata. This memory survives
until the next lookup on that cache or context destruction, just like a snapshot
returned by `get()`. Clear prevents subsequent stale hits; already selected
responses may finish. Mutation racing with a miss can repopulate a cleared cache.

The local snapshot favors consecutive hot-key hits; alternating keys still use
shared shards. Cached HTTP bodies are still copied into owned responses and wire
buffers. This is not a zero-copy or universally lock-free cache.

Static ETags now use seed-zero XXH64 over file contents, including same-size edits
with unchanged timestamps. ETag strings can change after this upgrade, causing a
normal fresh response instead of 304. The compact internal implementation is
adapted from [xxHash 0.8.3](https://github.com/Cyan4973/xxHash/tree/v0.8.3), BSD-2-Clause;
its copyright/license are retained in `include/pace/detail/content_hash.hpp`.
It adds no runtime package dependency and is not a cryptographic integrity check.
