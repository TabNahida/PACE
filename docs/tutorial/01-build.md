# 1. Build and run

This chapter builds **your own program**, not a prewritten example. You need xmake
2.9.8+ and a C++20 compiler. These guides track this source checkout; the entry
runner and router were added after v0.2.0.

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

## Select protocol modules

The core does not need third-party runtime libraries. The default aggregate
also enables HTTP/1 and WebSocket. TLS/HTTPS adds OpenSSL; HTTP/2 adds nghttp2.
Enable either option independently, or both:

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
| `--http1=y/n` | y | HTTP/1 application module |
| `--websocket=y/n` | y | WebSocket application module |
| `--tls=y/n` | n | TLS and HTTPS with OpenSSL |
| `--dns=y/n` | n | Asynchronous DNS with c-ares |
| `--quic=y/n` | n | QUIC with OpenSSL 3.5+; also enabled by HTTP/3 |
| `--http2=y/n` | n | HTTP/2 with nghttp2; TLS is a separate option |
| `--http3=y/n` | n | HTTP/3 with nghttp3 and QUIC; independent of HTTP/1/2 |
| `--benchmarks=y/n` | n | TCP comparison against standalone Asio and libuv |
| `--components=y/n` | n | PACE operation microbenchmarks |
| `--scaling=y/n` | n | Worker-count and execution-mode scaling benchmark |
| `--profiling=y/n` | n | Intrusive stage timing diagnostics |
| `--sanitizers=y/n` | n | Address/undefined-behavior instrumentation on supported non-Windows toolchains |
| `--modules=y/n` | n | Optional C++20 named module (`import pace`) |
| `--cxxstandard=...` | cxx20 | C++ language selection |

Normal PACE use has no Asio or libuv dependency. Enabling comparison benchmarks
asks xmake to resolve those optional packages and can require network access.
Follow the [benchmark instructions](../../benchmarks/README.md) for reproducible
measurements; keep profiling disabled for performance comparisons.

The build description separates library configuration from auxiliary programs:

| File | Responsibility |
|---|---|
| [xmake.lua](../../xmake.lua) | Language, diagnostics, protocol options and the four library layers |
| [xmake/tests.lua](../../xmake/tests.lua) | `tests` option and correctness test targets |
| [xmake/examples.lua](../../xmake/examples.lua) | `examples` option, servers and tutorial targets |
| [xmake/benchmarks.lua](../../xmake/benchmarks.lua) | `benchmarks`, `components`, `scaling` options and their targets/packages |

All existing command-line flags and target names remain unchanged. Simple
options use `option("name", {default = ..., showmenu = ..., description = ...})`,
which xmake has supported [since 2.2.6](https://xmake.io/guide/project-configuration/syntax-description.html#syntax-simplification);
PACE's minimum version remains 2.9.8. Sanitizer and profiling switches stay with
the library because they change the library's compilation.

For all protocols, configure:

```sh
xmake f -m release --tls=y --dns=y --quic=y --http2=y --http3=y -y
xmake -y
xmake test -v
```

For core sockets only, explicitly disable the default application modules:

```sh
xmake f -c -m release --http1=n --websocket=n --tls=n --dns=n --quic=n --http2=n --http3=n -y
xmake -y
xmake test -v
```

HTTP/3 automatically enables QUIC. QUIC does not need `--tls=y`: it embeds TLS
1.3 through OpenSSL's QUIC API. All protocol tests use local peers; DNS tests run
a local nameserver and the default DNS tutorial resolves numeric loopback.
The QUIC/HTTP3 backend currently requires POSIX;
enabling it on Windows/MinGW produces an explicit configuration error. The
existing TCP, UDP, TLS, HTTP/1, HTTP/2 and WebSocket backends remain available.

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

Use `add_deps("pace_runtime")` for tasks/timers/execution, `pace_net` for TCP/UDP,
or individual modules `pace_tls`, `pace_dns`, `pace_quic`, `pace_http1`,
`pace_http2`, `pace_http3`, `pace_websocket`. The protocol must be enabled when
configuring. `pace_extensions` and `pace_application` aggregate enabled modules
in their respective layers. Selecting `pace_net` never links OpenSSL, c-ares or
HTTP code even when another target in the same project enables those features.

For a standalone vendored project, include PACE's build description to preserve
its dependency graph without assuming a published package registry entry:

```lua
set_project("my_pace_application")
set_languages("cxx20")
add_rules("mode.debug", "mode.release")

includes("third_party/PACE")

target("my_application")
    set_kind("binary")
    add_files("main.cpp")
    add_deps("pace")
```

Configure `--tests=n --examples=n` when only building a vendored library. Feature
definitions and static-library link order come from the repository's
[xmake.lua](../../xmake.lua). Do not define a feature in only the application.
Include that root file through `includes("third_party/PACE")`; it loads its own
Lua modules. Auxiliary source files and test/tutorial working directories are
anchored to the PACE checkout, so including it from another project does not
redirect them to the parent project's `tests` or `examples` directory.
The aggregate `pace` target no longer creates a monolithic `libpace.a`; manual
linkers must include each selected module and its transitive dependencies.
Keep library and application compilation consistent, including profiling. The
library is C++20 ABI code, not a stable C ABI; rebuild it for your toolchain.

For independent local configurations, set `XMAKE_CONFIGDIR` for every command.
The output option `-o` alone changes where binaries are written; it does not
create an independent configuration. For example, on a POSIX shell:

```sh
export XMAKE_CONFIGDIR="$PWD/build/core-config"
xmake f -c -m release -o build/core --http1=n --websocket=n --tests=y --examples=y -y
xmake -y
xmake test -v
```

Use a different configuration directory for another feature set. When returning
to the repository's default configuration, unset `XMAKE_CONFIGDIR`.

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
is in GCC 16. Remote module/ABI tests pass with GCC 16.0.1's 20260315 snapshot
(experimental toolchain PPA build) and Clang 21.1.8, using C++20 and both QUIC
backends. See the [Actions run](https://github.com/TabNahida/PACE/actions/runs/43988664266).
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

Choose `--quic_backend=openssl` (default) or `--quic_backend=ngtcp2` together with `--quic=y` or `--http3=y`. Both support C++20 and use OpenSSL 3.5.7 for TLS; the alternative backend additionally requires ngtcp2 1.26.0.
