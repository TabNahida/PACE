# 2. Make HTTP requests

[中文](02-http-client.zh.md) · [Learning path](README.md) · [Previous: first program](01-install-and-first-program.md)

One HTTP request needs no coroutine, event loop or `run`: the direct clients block the calling thread and return
the response. Keep [chapter 1](01-install-and-first-program.md)'s `hello` server running in one terminal while you
work through this chapter.

## Write the client

Save this as `client.cpp` in the `hello` project:

```cpp
#include <pace/http1.hpp>
#include <iostream>

int main()
{
    try
    {
        auto reply = pace::http::get("http://127.0.0.1:8080/health");
        std::cout << reply.status << " " << reply.body;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

Add a target for it to `xmake.lua`:

```lua
target("client")
    set_kind("binary")
    add_files("client.cpp")
    add_deps("pace_http1")
```

Build it with `xmake -y client` and run `xmake run client`. Expect `200 ok`.

`get` connects, sends the request, reads the whole response and closes the connection before it returns. It blocks
this thread, and `reply` owns its status, headers and body. An HTTP error status such as 404 is a response, not an
exception: check `reply.status`. Connection and timeout errors throw `std::system_error`, and a malformed response
throws `http::protocol_error`. The client does not follow redirects or retry a request.

## Send a body

Replace the `get` line with:

```cpp
auto reply = pace::http::post("http://127.0.0.1:8080/echo", "hello", "text/plain");
```

Build and run the client again; expect `200 hello`. The third argument is the `Content-Type`; without it the body is
sent as `application/octet-stream`.

## Choose a timeout

A direct call has a total budget of 30 seconds by default, covering connection, request and response. To give a GET
two seconds, include `<chrono>` and pass the options after the URL:

```cpp
auto reply = pace::http::get("http://127.0.0.1:8080/health", {.timeout = std::chrono::seconds(2)});
```

When the budget runs out the call throws `std::system_error` with `std::errc::timed_out`. Stop the server and run the
client to see how a refused connection is reported instead.

## Use another method

`fetch` sends a request you build yourself. For example, a HEAD request:

```cpp
pace::http::request request;
request.method = "HEAD";
auto reply = pace::http::fetch("http://127.0.0.1:8080/health", std::move(request));
```

Expect `200 ` with an empty body. The URL supplies the target and the `Host` header; a `Host` header of your own is
rejected. Set `request.headers` and `request.body` for anything else the request needs.

## Reach hostnames and HTTPS

The URLs so far use a numeric address, which works in the default build. A hostname URL needs DNS and an
`https://` URL needs TLS. Enable both and configure again; xmake downloads c-ares and OpenSSL the first time:

```sh
xmake f -m release --tests=n --examples=n --dns=y --tls=y -y
```

Then the same call reaches a public HTTPS server:

```cpp
auto reply = pace::http::get("https://example.com/");
```

HTTPS verifies the server's certificate with OpenSSL's default trust store and checks it against the host in the
URL. If that store is empty, verification fails with `unable to get local issuer certificate`; this happens with the
OpenSSL that xmake builds on macOS. Point the `SSL_CERT_FILE` environment variable at a CA bundle such as
`/etc/ssl/cert.pem`, or load the bundle yourself. To trust your own CA, pass a `tls::context` before the URL:
`pace::http::get(trust, url)`.
[Chapter 10](10-tls-and-dns.md) builds such a context and a local TLS peer.

## Send TCP and UDP in one call

TCP and UDP have the same kind of direct call. With the TCP echo server from [chapter 5](05-tcp.md):

```cpp
auto reply = pace::tcp::exchange({"127.0.0.1", 9090}, "ping", 4);
```

With the UDP echo server from [chapter 6](06-udp.md):

```cpp
auto packet = pace::udp::exchange({"127.0.0.1", 9091}, "hello");
if (packet.truncated)
{
    throw std::runtime_error("UDP reply truncated");
}
```

Include `<pace/net.hpp>` and link `pace_net`. Both take the same timeout options as the HTTP calls. TCP needs the
length of the reply, because TCP does not keep message boundaries; UDP returns one datagram and its sender.

## Move to async when composing work

Inside a PACE coroutine, use `async_get`, `async_post` or `async_fetch`:

```cpp
pace::run(
    []() -> pace::task<void>
    {
        auto reply = co_await pace::http::async_get("http://127.0.0.1:8080/health");
        std::cout << reply.body;
    }
);
```

`pace::run` creates an event loop on this thread and runs the coroutine to completion. While the request waits, the
loop can run other work. Use the async form when your program already runs tasks, handles connections or makes
several requests at once. The blocking calls throw `std::logic_error` when called inside a running PACE context, so a
handler or coroutine always uses the async form. The async calls do not add the 30-second budget; set a deadline
yourself where you need one ([chapter 7](07-structured-concurrency.md)).

The companion program `xmake run tutorial_http_client` serves a coroutine handler on a local listener and checks its
chunked response with `async_get`. [Chapter 4](04-tasks-and-timers.md) explains tasks and `pace::run`.
[HTTP/1](../reference/http1/README.md) and [URL clients](../reference/http/clients.md) list every overload and
default.

Next: [3. Build an HTTP server](03-http-server.md)
