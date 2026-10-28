# 4. Build an HTTP service, one route at a time

[中文](04-http.zh-CN.md) · [Previous: TCP](03-tcp.md) · [Next: execution](05-execution.md)

Our goal is a health endpoint, an echo endpoint, then a handler that awaits I/O.
Use chapter 1's application, change its dependency to `pace_http1`, configure
`xmake f --http1=y -y`, and replace `main.cpp` with this complete server:

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

Build `xmake -y my_application`, run `xmake run my_application`, then use another
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

## 4. Call the server from C++

Save this as `client.cpp`; add a binary target named `client` with
`add_files("client.cpp")` and `add_deps("pace_http1")`. Keep the server running in
the first terminal, then build/run `client` in the second:

```cpp
#include <pace/http.hpp>
#include <pace/run.hpp>
#include <iostream>

int main() {
    try {
        const auto reply = pace::run(pace::http::get("http://127.0.0.1:8080/health"));
        std::cout << reply.status << " " << reply.body;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

Expect `200 ok`. The URL helper connects, builds Host, reads the response and
closes the connection. Inspect status codes; transport or framing failures throw.
For POST, use `http::post(url, body, content_type)`. Numeric loopback needs no DNS;
hostnames require `--dns=y`, and `https://` requires `--tls=y` and valid trust roots.

The repository's `xmake run tutorial_http` exercises health and asynchronous routes.
The [companion](../../examples/tutorial_http.cpp) adds bounded signal shutdown.

## Add HTTPS

First follow [DNS/TLS](08-dns-tls.md) to verify a local peer. Then use the
[HTTPS recipe](../reference/http-recipes.md#add-https) for certificate configuration.
Connection reuse, deadlines, body admission, response producers and close-policy
settings live in [HTTP recipes](../reference/http-recipes.md), so they can be
introduced when your application needs them.

Next: [execution and worker selection](05-execution.md).
