# 0. Start with a small service

[中文](00-quick-start.zh-CN.md) · [Learning path](README.md) · [Next: build details](01-build.md)

This chapter uses **PACE 0.3.0**, xmake 2.9.8+ and a C++20 compiler. Work in an
empty application directory. First run a service, then add a body, a wait and a client.

## 1. Create the project

Save this as `xmake.lua`:

```lua
add_rules("mode.debug", "mode.release")
add_repositories("tab-repo https://github.com/TabNahida/repo-xmake.git")
add_requires("pace 0.3.0")

target("hello")
    set_kind("binary")
    set_languages("c++20")
    add_files("main.cpp")
    add_packages("pace")
```

The package manager installs the library. HTTP/1 and WebSocket are enabled by
default; this first program does not need TLS or DNS dependencies. Chapter 1 also
covers including the source checkout in your build.

## 2. Write a health check

Save this as `main.cpp`:

```cpp
#include <pace/http.hpp>

int main() {
    pace::http::app app;
    app.get("/health", [] { return pace::http::text("ok\n"); });
    app.run(8080);
}
```

Run `xmake f -m release -y`, `xmake -y`, then `xmake run hello`. In another
terminal, `curl -i http://127.0.0.1:8080/health` should return 200 and `ok`.
A missing route returns 404, POST to `/health` returns 405, and HEAD omits the body.

`app.run(8080)` runs on the calling thread and binds loopback only. To listen on
another address, use `app.run({"0.0.0.0", 8080})`. This server keeps running;
Ctrl-C uses the system's default termination behavior. There is no implicit
signal handler or worker thread. Bind errors throw; catch `std::exception` in
`main` when you need to report them.

## 3. Add a response and a request body

Insert before `app.run`:

```cpp
app.get("/status", [] { return pace::http::json(R"({"ready":true})"); });
app.post("/echo", [](pace::http::request request) {
    return pace::http::text(std::move(request.body));
});
```

Rebuild and restart. `curl -H 'Expect:' --data-binary hello http://127.0.0.1:8080/echo`
should return `hello`. `text`, `html` and `json` own their string and choose the
Content-Type; their optional second argument is the status. `json` accepts
already-serialized JSON; it neither serializes C++ objects nor validates JSON.
Handlers can omit the request argument when unused. For another method, use
`app.route("PUT", "/path", handler)`.

## 4. Introduce a coroutine when you need to wait

Include `<chrono>` and register this route before `app.run`:

```cpp
app.get("/slow", []() -> pace::task<pace::http::response> {
    co_await pace::sleep_for(std::chrono::milliseconds(10));
    co_return pace::http::text("done\n");
});
```

`sleep_for` uses the context currently executing the handler, so no context
parameter is needed here. Other ready work can run during the wait. Blocking
computation or synchronous database calls still block this thread. Calling
`sleep_for` outside a running PACE context throws `logic_error`.

## 5. Write a client

Save `client.cpp` below. Copy the hello target in xmake.lua, name it `client`,
and change its source file to `client.cpp`:

```cpp
#include <pace/http.hpp>
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

Keep the server running, then run `xmake -y client` and `xmake run client`.
Expect `200 ok`. `get` creates a lazy task; `run` drives it and returns its response.
Group several operations in a no-argument coroutine:

```cpp
pace::run([]() -> pace::task<void> {
    auto reply = co_await pace::http::post("http://127.0.0.1:8080/echo", "hello", "text/plain");
    std::cout << reply.body << '\n';
});
```

For hostnames or HTTPS, change the requirement to
`add_requires("pace 0.3.0", {configs = {dns = true, tls = true}})` and reconfigure.
HTTPS uses system trust roots and verifies the URL hostname. Fragments are omitted
from the wire. The URL generates Host; callers cannot override it. Redirects,
request retries and decompression are not automatic. See the
[convenience API reference](../reference/convenience.md) for limits and custom CA trust.

## 6. Move down to TCP

Inside a coroutine, connect and receive a fixed-size message with:

```cpp
auto socket = co_await pace::tcp::connect({"127.0.0.1", 9090});
co_await socket.write_all("ping");
auto reply = co_await socket.read_exact(4);
```

This needs a server that echoes four bytes; chapter 3 builds one from scratch.
`read_exact` handles short reads, throws on early EOF, and bounds its string
allocation to 1 MiB by default. TCP remains a byte stream; one `read_some` is not
a message boundary.

For endpoint inspection, caller-owned threads, graceful draining or streamed HTTP
responses, continue with the explicit APIs. `app.serve(io, endpoint)` runs on an
existing context and snapshots the routes: later registrations do not modify that
running server. Continue with [tasks and timers](02-tasks-timers.md).
