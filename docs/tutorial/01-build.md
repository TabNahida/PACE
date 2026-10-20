# 1. Build and run

PACE requires a compiler with C++20 coroutines and xmake 2.9.8 or newer. CI uses
xmake 3.0.9. Linux, macOS, Windows and 64-bit FreeBSD have native readiness
backends. Use a recent GCC, Clang or MSVC toolchain on the selected platform.

From the repository root:

```sh
xmake f -m release --cxxstandard=cxx20 --examples=y --tests=y -y
xmake -y
xmake test -v
xmake run tutorial_tasks
```

The tasks example prints `Hello, PACE` and returns. The test command also runs the
runtime/network tests and registered example smoke checks. `xmake run TARGET`
locates the binary for the active platform; hardcoded `build/linux/...` paths are
unnecessary. On Windows use a shell with the compiler environment available.

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
        add_syslinks("ws2_32", {public = true})
        add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX", "_WIN32_WINNT=0x0A00", {public = true})
    else
        add_syslinks("pthread", {public = true})
    end

target("my_application")
    set_kind("binary")
    add_files("main.cpp")
    add_deps("pace")
```

Keep library and application compilation consistent, particularly the profiling
definition. The library is C++20 ABI code, not a stable C ABI. Rebuild it for the
selected compiler/toolchain configuration.

Next: [tasks and timers](02-tasks-timers.md).
