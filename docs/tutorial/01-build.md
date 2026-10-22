# 1. Build and run

PACE requires xmake 2.9.8+ and a compiler with C++20 coroutines. From the
repository root, build and run the smallest tutorial first:

```sh
xmake f -m release -y
xmake -y tutorial_tasks
xmake run tutorial_tasks
```

This prints `Hello, PACE` and returns. Read its [complete source](../../examples/tutorial_tasks.cpp),
then select a [protocol example](README.md#2-pick-a-protocol) or add an application
target below. `xmake run TARGET` finds the active platform's binary.

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

## Enable TLS and HTTP/2

The default core does not need third-party runtime libraries. TLS/HTTPS adds
OpenSSL; HTTP/2 adds nghttp2. Enable either option independently, or both:

```sh
xmake f -m release --tls=y --http2=y -y
xmake -y
xmake run tutorial_tls
xmake run tutorial_http2
```

xmake resolves OpenSSL 3.5.7 and nghttp2 1.70.0; first-time installation may need
network access. HTTP/2 without TLS uses prior-knowledge cleartext TCP. HTTPS and
HTTP/2 over TLS require certificate configuration; see [TLS](../reference/tls.md),
[HTTP](04-http.md) and [HTTP/2](../reference/http2.md). Tutorial certificates in
`tests/certs` are public test fixtures and must never be deployment credentials.

## Select a language version or build purpose

```sh
xmake f -m release --cxxstandard=cxx23 -y
xmake -y

# Independent diagnostic build directory; timing scopes add overhead.
xmake f -m release -o build/profile --profiling=y --tests=y -y
xmake -y
xmake test -v
```

Supported standard selections are `cxx20`, `cxx23` and `cxxlatest`. Newer standards
still require compiler support. Reconfigure before switching mode or options;
xmake retains configuration between commands.

| Option | Default | Purpose |
|---|---|---|
| `--tests=y/n` | y | Runtime and network tests |
| `--examples=y/n` | y | Runnable examples and tutorials |
| `--tls=y/n` | n | TLS and HTTPS with OpenSSL |
| `--http2=y/n` | n | HTTP/2 with nghttp2; TLS is a separate option |
| `--benchmarks=y/n` | n | TCP comparison against standalone Asio and libuv |
| `--components=y/n` | n | PACE operation microbenchmarks |
| `--scaling=y/n` | n | Worker-count and execution-mode scaling benchmark |
| `--profiling=y/n` | n | Intrusive stage timing diagnostics |
| `--sanitizers=y/n` | n | Address/undefined-behavior instrumentation on supported non-Windows toolchains |
| `--cxxstandard=...` | cxx20 | C++ language selection |

Normal PACE use has no Asio or libuv dependency. Enabling comparison benchmarks
asks xmake to resolve those optional packages and can require network access.
Follow the [benchmark instructions](../../benchmarks/README.md) for reproducible
measurements; keep profiling disabled for performance comparisons.

## Add application code

Inside this repository, a target can depend on the existing `pace` target:

```lua
target("my_application")
    set_kind("binary")
    add_files("app/main.cpp")
    add_deps("pace")
```

The dependency exports include paths, platform definitions and required system
libraries. Include `<pace/pace.hpp>` for the full surface, or a narrow public
header such as `<pace/tcp.hpp>`.

For a standalone vendored project, a minimal xmake target can compile the library
sources directly without assuming a published package registry entry:

```lua
set_project("my_pace_application")
set_languages("cxx20")
add_rules("mode.debug", "mode.release")

target("pace")
    set_kind("static")
    add_files("third_party/PACE/src/*.cpp")
    add_includedirs("third_party/PACE/include", {public = true})
    if is_plat("windows", "mingw") then
        add_syslinks("ws2_32", "bcrypt", {public = true})
        add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX", "_WIN32_WINNT=0x0A00", {public = true})
    else
        add_syslinks("pthread", {public = true})
    end

target("my_application")
    set_kind("binary")
    add_files("main.cpp")
    add_deps("pace")
```

This vendored target builds the default core. For optional protocols, mirror the
packages and public `PACE_HAS_TLS` / `PACE_HAS_HTTP2` definitions in the repository's
[xmake.lua](../../xmake.lua). Do not define a feature in only the application.
Keep library and application compilation consistent, including profiling. The
library is C++20 ABI code, not a stable C ABI; rebuild it for your toolchain.

Next: [tasks and timers](02-tasks-timers.md).
