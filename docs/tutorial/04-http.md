# 4. Get an HTTP response, then build a service

[中文](04-http.zh-CN.md) · [Learning path](README.md)

Use chapter 1's current source setup, link `pace_http1` and configure `--http1=y`.
The direct calls and `async_` names in this chapter are not in the 0.3.1 package.

## Start with a direct GET

Save this as `client.cpp`; add a binary target named `client` with
`add_files("client.cpp")` and `add_deps("pace_http1")`:

```cpp
#include <pace/http.hpp>
#include <iostream>

int main() {
    try {
        const auto reply = pace::http::get("http://127.0.0.1:8080/health");
        std::cout << reply.status << " " << reply.body;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

Once the server below is running, build/run `client`; expect `200 ok`. `get` blocks
this thread and returns the response, with a default 30-second total timeout.
No `run` or `co_await` is required. Status codes remain in the result; network or
protocol failures throw. To send a body to the echo route added below, replace the
request line with:

```cpp
auto reply = pace::http::post("http://127.0.0.1:8080/echo", "hello", "text/plain");
```

For another method, use `fetch`, for example HEAD:

```cpp
pace::http::request request;
request.method = "HEAD";
auto reply = pace::http::fetch("http://127.0.0.1:8080/health", std::move(request));
```

Numeric HTTP needs only HTTP/1; hostnames need `--dns=y`, HTTPS needs `--tls=y`.
See [direct clients](../reference/direct-clients.md) for total deadlines, custom CA
trust and migration from older names.

## Supply a local service

Save the complete server below as `server.cpp`; create a `server` target linked to
`pace_http1`, just as for the client.

```cpp
#include <pace/http.hpp>
#include <pace/run.hpp>
#include <iostream>

namespace http = pace::http;

int main() {
    try {
        http::app app;
        app.get("/health", [] { return http::text("ok\n"); });
        app.run({"127.0.0.1", 8080});
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

Build `xmake -y server client`, run `xmake run server`, then use another
terminal: `curl -i http://127.0.0.1:8080/health`. Expect status 200 and `ok`.
This server runs until stopped; Ctrl-C uses default process termination here.
The companion `tutorial_http serve` adds cooperative signal shutdown (chapter 6).

## 1. Read the small server from the inside out

`http::app` owns the routes and listener. `app.run` creates and runs the event loop.
A handler may ignore the request and return `http::text`, `http::html` or
`http::json` directly; use a request parameter when you need the body or headers.
No coroutine is needed until your handler actually waits.

Try `/missing`: the router generates 404. Try `curl -I .../health`: HEAD falls back
to GET, and the serializer suppresses the body. Try POST to `/health`: expect 405
with `Allow: GET, HEAD`. `/health?probe=1` still matches `/health`; the handler sees
the original target including the query. Matching is exact and case-sensitive,
without percent decoding or wildcard rules.

## 2. Add an echo route

Insert this registration before `app.run(...)`:

```cpp
app.post("/echo", [](http::request request) {
    return http::response{.body = std::move(request.body),
                          .content_type = "application/octet-stream"};
});
```

Rebuild/restart, then run
`curl -H 'Expect:' --data-binary 'hello' http://127.0.0.1:8080/echo`.
Expect `hello`. Moving `request.body` transfers the buffered string into the
response. Do not generate Content-Length or Connection headers yourself: the
serializer does that. Use `app.route("PUT", "/echo", handler)` for other methods.

## 3. Add a real suspension point

Add `<chrono>` and another route:

```cpp
app.get("/slow", []() -> pace::task<http::response> {
    co_await pace::sleep_for(std::chrono::milliseconds(5));
    co_return http::text("done\n");
});
```

Now the return type is `task<response>` and the return statement is `co_return`.
The app accepts both forms. The reactor can run other ready work while
this handler waits. A synchronous database call would still block it; coroutine
syntax alone does not offload work. `pace::sleep_for` uses the current context.

## 4. Await another network request in a handler

A handler uses the async client so other work can proceed during its wait:

```cpp
app.get("/proxy", []() -> pace::task<http::response> {
    co_return co_await http::async_get("http://127.0.0.1:8080/health");
});
```

`async_get`, `async_post` and `async_fetch` return tasks on the existing context.
They do not add the direct client's default 30-second deadline. Use the reference
for connection reuse and application-managed deadlines. Blocking clients reject
calls from inside the running context.

The repository's `xmake run tutorial_http` checks routes and async clients; its
companion also demonstrates bounded signal shutdown.

## Add HTTPS

First follow [DNS/TLS](08-dns-tls.md) to verify a local peer. Then use the
[HTTPS recipe](../reference/http-recipes.md#add-https) for certificate configuration.
Connection reuse, deadlines, body admission, response producers and close-policy
settings live in [HTTP recipes](../reference/http-recipes.md), so they can be
introduced when your application needs them.

Next: [execution and worker selection](05-execution.md).
