# 1. Build and run

This chapter builds **your own program**, not a prewritten example. You need xmake
2.9.8+ and a C++20 compiler. These guides track this source checkout. The direct client APIs are unreleased;
use the checkout rather than the 0.3.1 package.

## Create a small application

Make a working directory with this layout. Place this PACE checkout under
`third_party/PACE`:

```text
hello/
  xmake.lua
  main.cpp
  third_party/PACE/
```

Write the application's `xmake.lua`:

```lua
set_project("hello")
set_languages("cxx20")
add_rules("mode.debug", "mode.release")
includes("third_party/PACE")

target("my_application")
    set_kind("binary")
    add_files("main.cpp")
    add_deps("pace_http1")
```

The dependency supplies headers and platform libraries. Do not copy PACE's
`.cpp` files into your target or construct a manual static-library link order.
Start `main.cpp` with this complete program:

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

From **hello**, configure and run:

```sh
xmake f -m release --tests=n --examples=n --http1=y --websocket=n -y
xmake -y my_application
xmake run my_application
```

Keep the [quick start's server](00-quick-start.md) running in another terminal.
Expect `200 ok` and client exit. `get` directly returns a response; no coroutine
setup is needed. Continue with [HTTP](04-http.md), [TCP](03-tcp.md) or [UDP](07-udp.md).
[Chapter 2](02-tasks-timers.md) introduces tasks when you need async composition.

If a header cannot be found, check the `includes` path and dependency. If a
protocol target is missing, enable its option before building. A connection error
usually means the local server is not running or the URL has the wrong port.

## Build the repository's companion exercises

From the PACE checkout:

```sh
xmake f -m release --examples=y --tests=y -y
xmake -y tutorial_tasks
xmake run tutorial_tasks
```

CI uses xmake 3.0.9 with GCC, Clang or MSVC on Linux, macOS, Windows and 64-bit
FreeBSD. On Windows use a shell with the Visual Studio C++ environment available.
On FreeBSD install **xmake-io** from upstream: the FreeBSD package named `xmake`
is a different tool. The [CI workflow](../../.github/workflows/ci.yml) has a
pinned source-install example.

Build all enabled targets and run correctness tests:

```sh
xmake -y
xmake test -v
```

Tests include the enabled protocol suites and bounded tutorial smoke checks.
The [22 October local Linux validation](../../tests/results/linux-protocols-2026-10-22/README.md)
records full, core-only and HTTP3-only builds and sanitizer coverage.

## Select the module you need

| Application | Enable | Target dependency |
|---|---|---|
| Tasks / timers | No protocol option | `pace_runtime` |
| TCP / UDP | No protocol option | `pace_net` |
| HTTP/1 | `--http1=y` | `pace_http1` |
| WebSocket | `--websocket=y` | `pace_websocket` |
| DNS | `--dns=y` | `pace_dns` |
| TLS | `--tls=y` | `pace_tls` |
| HTTP/2 | `--http2=y` (TLS separately) | `pace_http2` |
| QUIC / HTTP/3 | `--quic=y` / `--http3=y` | `pace_quic` / `pace_http3` |

Change your application's `add_deps` and reconfigure before building. HTTP/3 enables
QUIC, which embeds its own TLS and currently requires POSIX. Enabling optional
modules can download dependencies. Do not use the public test certificates in deployment.

[Full build configuration](../reference/build-configuration.md) covers all flags,
C++ standard selection, sanitizers, profiling, vendoring and isolated builds.
Next: [tasks and timers](02-tasks-timers.md).
