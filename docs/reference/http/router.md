# HTTP routing

[简体中文](router.zh.md) · [HTTP](README.md)

## Overview

`make_handler` turns an ordinary callable into an `http::handler`, and `router` dispatches requests to such
handlers by exact method and path. Both work with the HTTP/1, HTTP/2 and HTTP/3 servers.

| Header | Target | Build option |
|:--|:--|:--|
| `<pace/http.hpp>` (content in `<pace/http/router.hpp>`) | `pace_http` (header-only) | none; always available |

`<pace/http1.hpp>` includes it too. The router is a small exact-match dispatcher: there are no path parameters,
wildcards, regular expressions, URL decoding, middleware or automatic `OPTIONS` replies.

## Synopsis

```cpp
#include <pace/http.hpp>
#include <pace/http1.hpp>

pace::http::router make_routes()
{
    pace::http::router routes;
    routes.get("/health", [] { return pace::http::text("ok\n"); });
    routes.post(
        "/echo",
        [](pace::http::request request) -> pace::task<pace::http::response>
        { co_return pace::http::response{.body = std::move(request.body)}; }
    );
    routes.add("DELETE", "/items", [](pace::http::request) { return pace::http::response{.status = 204}; });
    return routes;
}

pace::task<void> serve(pace::tcp::listener& listener)
{
    co_await pace::http::serve(listener, make_routes());
}
```

## Types

### `router`

```cpp
class router
{
public:
    template <class Function>
    router& add(std::string method, std::string path, Function&& function);
    template <class Function>
    router& get(std::string path, Function&& function);
    template <class Function>
    router& post(std::string path, Function&& function);
    template <class Function>
    router& mount(std::string prefix, Function&& function);

    task<response> operator()(request incoming) const;
};
```

A `router` is itself a handler: it converts to `http::handler`, so it can be passed to any `serve` function or
wrapped by another handler. A default-constructed router answers every request with 404.

| Member | Effect |
|:--|:--|
| `add(method, path, function)` | Registers `function` for an exact method and path. Returns `*this` |
| `get(path, function)` | `add("GET", path, function)` |
| `post(path, function)` | `add("POST", path, function)` |
| `mount(prefix, function)` | Registers `function` for every method and for every path under `prefix` |
| `operator()(request)` | Dispatches one request and returns the selected handler's task |

`function` is adapted with [`make_handler`](#make_handler), so it has the same requirements.

**Registration rules.** `method` must be a non-empty HTTP token; `path` must start with `/` and contain no space,
control character, `?` or `#`. Registering the same method and path twice throws. A mount prefix must start
with `/` and contain no space, control character, `?`, `#` or `\`; trailing slashes are removed, so `/static/` and
`/static` are the same prefix, and mounting the same prefix twice throws.

**Matching.** The request target's query (from the first `?`) is ignored for matching, but the handler receives
the target unchanged. Methods and paths compare case-sensitively and byte for byte; percent escapes are not
decoded, so `/%68ello` does not match `/hello`. For a request:

1. If a route has the request's exact path and method, it runs.
2. A `HEAD` request with no `HEAD` route on that path runs the path's `GET` route. The handler sees method
   `HEAD`; the protocol writes the response headers and drops the body.
3. If no route has that exact path, the mount with the longest matching prefix runs. A prefix matches whole path
   segments: `/api` matches `/api` and `/api/x`, not `/apix`. Prefix `/` matches every path. The mounted handler
   receives the full, unstripped target.
4. If no route and no mount matches, the reply is 404 with body `Not Found\n`.
5. If routes exist for the path but none for this method, the reply is 405 with body `Method Not Allowed\n` and
   an `Allow` field listing the path's methods in registration order, with `HEAD` added after `GET` when there
   is no explicit `HEAD` route.

An exact path therefore always wins over a mount, even when its method does not match.

**Copies.** Copying a router is cheap: copies share one route table. The first registration on a copy gives that
copy its own table, so routes added to a copy never change the original. The callables themselves stay shared
between the copies. Servers copy their handler for every connection; sharing the table keeps that copy free of
allocation.

## Functions

### `make_handler`

```cpp
template <class Function>
handler make_handler(Function&& function);
```

Adapts `function` to `http::handler`. `function` is called either with the `request` or with no arguments (the
request form is used when both are possible), and must return exactly `response` or `task<response>`; anything
else fails to compile with a `static_assert`. The adapter takes ownership of the callable, including one with
move-only captures, and keeps it alive through every invocation that has started, even if the adapter or router
is destroyed while that invocation is suspended. Captured references are not extended; what they refer to must
outlive the server.

The call is lazy: `function` runs when the returned task is first resumed, and an exception it throws is stored
in the task and rethrown at `co_await`. A function that returns `response` runs synchronously on the server's
event loop and must not block.

`<pace/http/router.hpp>` also declares `handler_access`, which holds the implementation of `make_handler` and
`router`. It is not exported by the `pace.http` module and is not part of the API.

## Errors

| Situation | Result |
|:--|:--|
| Invalid method, path or prefix, duplicate route or duplicate mount | `add`/`get`/`post`/`mount` throw `std::invalid_argument`; the router is unchanged |
| Callable with the wrong return type | Compile error |
| Exception from the selected handler | Propagates out of the returned task. A server converts it to its error reply (500 on HTTP/1) |
| No match | 404 or 405 response, not an exception |

## Threads and concurrency

Register every route before the router is handed to a server. Registration while the same router (or a copy that
still shares its table) is serving is not supported. Dispatch itself only reads the table, so copies of one
router may dispatch concurrently on different worker threads. The handlers are shared between those threads;
any state they hold or capture must be synchronized by the application.

## Lifetime and ownership

The router owns its routes and the adapted callables, shared between copies. A server that receives a router by
value holds its own copy, so the original may be destroyed or changed afterwards without affecting it.

## Limits and defaults

| Item | Value |
|:--|:--|
| Path lookup | One binary search over the sorted paths, then a scan of that path's methods |
| Mount lookup | Linear scan of the mounts, only when no exact path exists |
| Default response for an empty router | 404 `Not Found\n` |
| Default methods of `get` / `post` | `GET` / `POST` |

## See also

- [HTTP messages](README.md) and [URL clients](clients.md)
- [HTTP/1 application](../http1/app.md): `http::app` registers routes on an internal router
- [Gateway services](../gateway/README.md): `static_files` and `reverse_proxy` are mounted with `mount`
- Tutorial: [HTTP server](../../tutorial/03-http-server.md)
