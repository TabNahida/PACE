# 3. Build an HTTP server

[中文](03-http-server.zh.md) · [Learning path](README.md) · [Previous: HTTP requests](02-http-client.md)

This chapter grows [chapter 1](01-install-and-first-program.md)'s server: how routing answers requests, how to
build a response, and how a handler waits without blocking the server. Keep working in `main.cpp` of the `hello`
project. After each change, stop the server, then build and run it again with `xmake -y hello` and
`xmake run hello`.

## Read the server from the inside out

```cpp
pace::http::app app;
app.get("/health", [] { return pace::http::text("ok\n"); });
app.post("/echo", [](pace::http::request request) { return pace::http::text(std::move(request.body)); });
app.run(8080);
```

`http::app` owns the routes. `app.run(8080)` creates an event loop on this thread, binds `127.0.0.1:8080` and serves
connections until the process stops. To listen on every interface, pass an endpoint: `app.run({"0.0.0.0", 8080})`.

A handler may take the `http::request` or nothing at all, and returns an `http::response`. `http::text`,
`http::html` and `http::json` make a response with the matching content type from a string you already have;
`json` does not serialize anything for you.

Try these requests against the running server:

- `curl -i http://127.0.0.1:8080/missing`: no route matches, so the router answers 404.
- `curl -I http://127.0.0.1:8080/health`: HEAD uses the GET handler, and the response is sent without its body.
- `curl -i -X POST http://127.0.0.1:8080/health`: the path exists but not for POST, so the answer is 405 with
  `Allow: GET, HEAD`.
- `curl -i 'http://127.0.0.1:8080/health?probe=1'`: the query is not part of the match; the handler still sees it
  in `request.target`.

Paths match exactly and case-sensitively, without percent-decoding or wildcards.

## Build the response yourself

`http::text` is a shortcut. To keep the next routes short, add `namespace http = pace::http;` after the includes.
Then replace the echo route with one that fills in the `http::response` fields directly:

```cpp
app.post(
    "/echo",
    [](http::request request)
    { return http::response{.body = std::move(request.body), .content_type = "application/octet-stream"}; }
);
```

Run `curl -i --data-binary hello http://127.0.0.1:8080/echo`; expect `hello` with the new content type. Moving
`request.body` hands the buffered string to the response without a copy. `response` also has `status`, `headers`
and `close`. Do not set `Content-Length` or `Connection` yourself: the server writes the framing headers. For other
methods, use `app.route("PUT", "/echo", handler)`.

## Add a real suspension point

Include `<chrono>` and add a route whose handler waits:

```cpp
app.get(
    "/slow",
    []() -> pace::task<http::response>
    {
        co_await pace::sleep_for(std::chrono::milliseconds(5));
        co_return http::text("done\n");
    }
);
```

The handler now returns `task<response>` and finishes with `co_return`; the app accepts both forms. While it waits,
the event loop serves other connections. `pace::sleep_for` uses the event loop the handler runs on. Coroutine syntax
alone does not move work off the loop: a blocking call, such as a synchronous database query, still blocks every
connection on it. [Chapter 8](08-workers-and-threads.md) shows where such work goes.

## Await another request in a handler

A handler that calls another service uses the async client, so the loop keeps serving while it waits:

```cpp
app.get(
    "/proxy",
    []() -> pace::task<http::response>
    {
        auto reply = co_await http::async_get("http://127.0.0.1:8080/health");
        co_return http::text("upstream said: " + reply.body, reply.status);
    }
);
```

`curl http://127.0.0.1:8080/proxy` prints `upstream said: ok`: the handler requested `/health` from this same server
and built its own response from the reply. Build a new response rather than returning the client's `reply` as it is:
`reply.headers` holds the headers as received, including `Content-Length` and `Content-Type`, which the server
writes itself and rejects in a handler's response. The blocking `http::get` from [chapter 2](02-http-client.md) would throw here, because it refuses to
run inside a running PACE context. The async calls have no default deadline; [chapter 7](07-structured-concurrency.md)
adds one.

## Check it with the companion

From the PACE checkout, `xmake run tutorial_http` serves `/health`, `/echo` and `/slow` with explicit request limits
on a free port, requests `/health?probe=1` and `/slow` with `async_get`, prints `HTTP /health and /slow verified`
and exits. `xmake run tutorial_http serve` keeps the same server on port 8080 until Ctrl-C, and stops it cleanly
through the signal handling that [chapter 9](09-graceful-shutdown.md) explains.

## Add HTTPS

Serving HTTPS needs a certificate and the `--tls=y` build. [Chapter 10](10-tls-and-dns.md) sets up TLS with local
test certificates; [HTTP recipes](../guides/http-recipes.md) then shows the HTTPS server and also covers connection
reuse, deadlines, body limits, streamed responses and the client close policy, for when your application needs them.
[HTTP/1](../reference/http1/README.md) and [routing](../reference/http/router.md) list the full contracts.

Next: [4. Tasks and timers](04-tasks-and-timers.md)
