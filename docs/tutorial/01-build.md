# 1. Build and run

This chapter builds **your own program**, not a prewritten example. You need xmake
2.9.8+ and a C++20 compiler. These guides track this source checkout; the component conveniences used below
require PACE v0.3.1 or newer.

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
    add_deps("pace_runtime")
```

The dependency supplies headers and platform libraries. Do not copy PACE's
`.cpp` files into your target or construct a manual static-library link order.
Start `main.cpp` with this complete program:

```cpp
#include <pace/run.hpp>
#include <iostream>

pace::task<void> hello(pace::io_context&) {
    std::cout << "Hello, PACE\n";
    co_return;
}

int main() {
    try { pace::run(hello); }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

From **hello**, configure and run:

```sh
xmake f -m release --tests=n --examples=n --http1=n --websocket=n -y
xmake -y my_application
xmake run my_application
```

Expect `Hello, PACE` followed by process exit. `pace_runtime` needs no optional
protocol libraries. Chapter 2 adds a timer and a returned value. Later chapters
change `add_deps` to the networking/protocol module being used and reconfigure
its feature option. Commands named `tutorial_*` are companion exercises run from
the **PACE repository root**, with `--examples=y`; they are not targets in hello.

If a header cannot be found, check the `includes` path and dependency. If a
protocol target is missing, enable its option before building. If the program
prints but never exits, check whether you used manual `io.run()` without a stop
path; the next chapter distinguishes both execution forms.

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
