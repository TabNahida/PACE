# 1. Build and run

This chapter builds **your own program**, not a prewritten example. You need xmake
2.9.8+ and a C++20 compiler. These guides require PACE 0.4.0 or newer.
Use the v0.4.0 source checkout for the setup below.

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

## C++ named module (`import pace`)

C++20 and newer can opt into the named module facade. It is off by default;
ordinary header-based targets remain available. This is a C++ language module,
separate from the independently linked protocol targets above. No `import std`
is required. See the [Clang module guide](https://clang.llvm.org/docs/StandardCPlusPlusModules.html#abi-non-breaking-styles)
for the ABI-preserving export-using approach used here.

Validated locally with **Clang 18.1.3, libstdc++ 13 and xmake 3.1.1**. Install a
matching `clang-scan-deps` (Ubuntu: `clang-tools-18`) and put its directory on
`PATH`; xmake's fallback scanner in 3.1.1 drops the language-standard flag and
cannot scan the coroutine headers. Clang 21 also passes the remote all-protocol
module test with both QUIC backends. GCC 13.3 fails to expose exported using
declarations; GCC 15.2 crashes in `expand_CO_FRAME` while importing inline
coroutines. The module therefore diagnoses GCC versions below 16 explicitly;
header builds remain supported. GCC's [coroutine/module serialization fix](https://gcc.gnu.org/pipermail/gcc-patches/2026-January/705464.html)
is in GCC 16. GCC 16.2 and Clang 21.1.8 pass the C++20 module and mixed-header ABI
tests with both QUIC backends. See the [Actions run](https://github.com/TabNahida/PACE/actions/runs/44238306831).
MSVC and Apple Clang module builds are not yet validated. They remain covered by the existing
header-mode platform matrix where applicable.

```sh
# Ubuntu LLVM packages put matching binaries in this directory.
export PATH="/usr/lib/llvm-18/bin:$PATH"
xmake f -c -m release --toolchain=clang --modules=y --cxxstandard=cxx20 --tests=y --examples=n -y
xmake -y pace_module_tests
xmake run pace_module_tests
```

For your application, replace `add_deps("pace")` with
`add_deps("pace_module")`, configure `--modules=y`, and write:

```cpp
#include <coroutine>
#include <iostream>
import pace;

pace::task<int> answer() {
    co_await pace::yield();
    co_return 42;
}
int main() {
    std::cout << pace::run(answer) << '\n';
}
```

Include the standard headers your source uses **before** `import pace`; no
PACE header is needed in the importing translation unit. The facade exports
runtime, TCP/UDP and shared HTTP message/router APIs, plus only enabled optional
protocols. `--http1=n --websocket=n` produces a core-only module with no optional
protocol library dependencies. Use `--cxxstandard=cxx23` or `cxxlatest` with the
same option when needed; the module does not impose a C++23 minimum.

The implementation remains in the existing static libraries. Header consumers
and import consumers in different translation units share the same types and
link together; the module smoke test exercises that ABI boundary. Do not
rebuild implementation `.cpp` files as module implementation units. Module
imports do not export macros; xmake still propagates feature definitions and
link dependencies through `pace_module`. Rebuild BMIs after changing compiler,
standard or protocol options, and do not distribute cached BMIs across different
toolchains. xmake may warn that `std`/`std.compat` modules are missing; this
facade uses standard-library headers and does not need those modules.

[Module CI](../../.github/workflows/modules.yml) covers Clang C++20 core/default
builds and C++23 with all optional protocols. This adds an import interface;
no compile-time or runtime speedup is claimed for modules themselves.

Choose `--quic_backend=ngtcp2` (default) or `--quic_backend=openssl` together with `--quic=y` or `--http3=y`. Both support C++20 and use OpenSSL 3.5.7 for TLS; the default backend also requires ngtcp2 1.26.0.
