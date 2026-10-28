# 0. Get a response with one call

[中文](00-quick-start.zh-CN.md) · [Learning path](README.md) · [Build details](01-build.md)

Start with ordinary C++. No coroutine, event-loop setup or `run` is needed for a
single client request. These guides use the **current source checkout**: the direct
clients and `async_` HTTP names are unreleased and are not in the 0.3.1 package.

## 1. Write the client

Save this complete program as `client.cpp`:

```cpp
#include <pace/http.hpp>
#include <iostream>

int main() {
    try {
        auto reply = pace::http::get("http://127.0.0.1:8080/health");
        std::cout << reply.status << " " << reply.body;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`get` connects, sends GET, reads the response and closes the connection before
returning. It blocks the calling thread; `reply` owns its data. HTTP error statuses
such as 404 are responses, so check `reply.status`. Connection, protocol and timeout
errors throw. The default total timeout is 30 seconds.

The URL refers to a local service. Next we create that service and build both files.

## 2. Give it a small server

Save this as `server.cpp`:

```cpp
#include <pace/http.hpp>
#include <iostream>

int main() {
    try {
        pace::http::app app;
        app.get("/health", [] { return pace::http::text("ok\n"); });
        app.post("/echo", [](pace::http::request request) {
            return pace::http::text(std::move(request.body));
        });
        app.run(8080);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

`http::app` owns the routes. `text` creates a response from an owned string, and the
POST handler returns the body it receives. `app.run(8080)` listens on loopback and
keeps serving on this thread. Stop this introductory server with Ctrl-C.

## 3. Build and run

You need xmake 2.9.8+ and a C++20 compiler. Put this PACE checkout at
`third_party/PACE` in your application directory, beside `client.cpp`, `server.cpp`
and the following `xmake.lua`:

```lua
set_project("hello")
set_languages("cxx20")
add_rules("mode.debug", "mode.release")
includes("third_party/PACE")

for _, name in ipairs({"client", "server"}) do
    target(name)
        set_kind("binary")
        add_files(name .. ".cpp")
        add_deps("pace_http1")
end
```

From your application directory:

```sh
xmake f -m release --tests=n --examples=n --http1=y --websocket=n -y
xmake -y client server
xmake run server
```

In a second terminal, run `xmake run client`. Expect `200 ok` and client exit.
The server remains available for another call. [Chapter 1](01-build.md) explains
module selection and the repository's companion examples.

## 4. Send a body or choose a timeout

Replace the client's `get` line with:

```cpp
auto reply = pace::http::post("http://127.0.0.1:8080/echo", "hello", "text/plain");
```

Rebuild/run the client; expect `200 hello`. To bound a GET to two seconds, include
`<chrono>` and use:

```cpp
auto reply = pace::http::get("http://127.0.0.1:8080/health",
                             {.timeout = std::chrono::seconds(2)});
```

For hostnames configure `--dns=y`; for HTTPS add `--tls=y`. HTTPS verifies the URL
hostname using system trust roots. See [HTTP](04-http.md) for other methods and
custom trust. No redirect or request retry is automatic.

## 5. Use the same approach for TCP and UDP

With a four-byte TCP echo server from [chapter 3](03-tcp.md):

```cpp
auto reply = pace::tcp::exchange({"127.0.0.1", 9090}, "ping", 4);
```

With the UDP echo server from [chapter 7](07-udp.md):

```cpp
auto packet = pace::udp::exchange({"127.0.0.1", 9091}, "hello");
if (packet.truncated) throw std::runtime_error("UDP reply truncated");
```

Include `<pace/tcp.hpp>` or `<pace/udp.hpp>` and link `pace_net`. Both calls use the
same timeout options. TCP needs an agreed reply length; UDP returns one packet,
including its sender. Each chapter builds a working client and server from scratch.

## 6. Move to async when composing work

Inside a PACE coroutine, use `async_get`, `async_post` or `async_fetch`:

```cpp
pace::run([]() -> pace::task<void> {
    auto reply = co_await pace::http::async_get("http://127.0.0.1:8080/health");
    std::cout << reply.body;
});
```

This version yields the event-loop thread while waiting. It is useful when your
application already runs tasks, handles connections, or manages concurrent work.
Blocking clients reject calls from an active PACE context; use the async form in
handlers. The direct client's 30-second deadline is not added to async calls.

Continue with [HTTP](04-http.md), [TCP](03-tcp.md) or [UDP](07-udp.md) first; read
[tasks and timers](02-tasks-timers.md) when you need async composition. Exact
contracts, tuning and the 0.3.1 migration table live in
[direct clients](../reference/direct-clients.md).
