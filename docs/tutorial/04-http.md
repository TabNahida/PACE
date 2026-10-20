# 4. HTTP services

Complete source: [`tutorial_http.cpp`](../../examples/tutorial_http.cpp).
The default mode performs a bounded loopback GET and checks status, declared
length, body and EOF:

```sh
xmake run tutorial_http
```

It prints `HTTP /health verified`. For a server you can query with curl:

```sh
xmake run tutorial_http serve
curl --http1.1 http://127.0.0.1:8080/health
curl --http1.1 -H "Expect:" --data-binary "hello" http://127.0.0.1:8080/echo
curl --http1.1 -I http://127.0.0.1:8080/health
```

The server returns `ok`, echoes the POST body, and correctly omits the HEAD body.
Ctrl-C requests cooperative shutdown. The default smoke client's hand-written
request is intentionally narrow; PACE does not currently provide a general HTTP
client, URL parser or TLS.

## Write a handler

```cpp
pace::task<pace::http::response> route(pace::http::request request) {
    if ((request.method == "GET" || request.method == "HEAD") && request.target == "/health")
        co_return pace::http::response{.body = "ok\n"};
    if (request.method == "POST" && request.target == "/echo")
        co_return pace::http::response{.body = std::move(request.body),
                                      .content_type = "application/octet-stream"};
    co_return pace::http::response{.status = 404, .body = "Not Found\n"};
}
```

The request is passed by value and owns its body/headers, so it remains usable
across handler suspension. `request.header("content-type")` returns a borrowed
view into that owned request; copy it if it must outlive the request. Targets are
raw strings, so `/health?full=1` does not equal `/health` in this simple router.

Responses also own their data. PACE generates Content-Length and Connection,
and uses `content_type` for Content-Type. Set custom fields through `headers`
only when they are not those managed fields or Transfer-Encoding. Set
`response.close = true` to end persistence after a response. Invalid response
metadata or a normal handler exception becomes a closing 500.

## Start serving with explicit bounds

```cpp
const pace::http::limits bounds{
    .max_header_bytes = 8192,
    .max_body_bytes = 65536,
    .max_requests_per_connection = 100
};
io.spawn(pace::http::serve(listener, route, bounds));
```

`serve` borrows the listener and spawns an independent task per accepted connection.
Keep the listener and context alive through those tasks. The handler is copied;
shared application state still needs synchronization when copies run on different
worker contexts. Connections process their own requests sequentially, and the
100th response in this example ends persistence.

The limits bound parsing bytes and request count. They do not impose connection
deadlines, a maximum number of connected clients or a global memory budget. A
client may remain connected while sending slowly. Add an application-level
connection policy when building a service that needs those controls.

## Supported wire behavior

HTTP/1.0 and HTTP/1.1 with Content-Length framing are supported. PACE rejects
ambiguous framing such as duplicate Content-Length or Transfer-Encoding combined
with Content-Length. Chunked transfer, any Expect request, CONNECT and upgrades
are currently unsupported. The `-H "Expect:"` curl option keeps the POST within
this supported scope. Header/body overflows produce 431/413 when a response can
be sent.

Closing the listener on its owning context ends accepts but leaves accepted connections running.
Stopping its context cancels both. PACE's final HTTP response uses bounded
graceful socket close; that improves handling of unread rejected-request bytes
but is not a guarantee against every peer reset.

Reference: [HTTP messages, parsing, errors and limits](../reference/http.md).
Next: [execution and worker selection](05-execution.md).
