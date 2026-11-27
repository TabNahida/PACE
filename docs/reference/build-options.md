# Build options

[简体中文](build-options.zh.md) · [Reference](README.md)

## Overview

PACE builds with [xmake](https://xmake.io) 2.9.8 or newer (CI uses 3.0.9 and 3.1.1). Options are set with
`xmake f` and kept until the next `xmake f`; `-c` clears the previous configuration first.

```sh
xmake f -c -m release --tls=y --http2=y -y
xmake -y
xmake test -v
```

The build is described in these files:

| File | Contents |
|:--|:--|
| [`xmake.lua`](../../xmake.lua) | Project, language standard, the Windows QUIC check, sanitizer and fuzz flags, the includes below |
| [`xmake/options.lua`](../../xmake/options.lua) | The library options |
| [`xmake/packages.lua`](../../xmake/packages.lua) | Pinned third-party packages |
| [`xmake/targets.lua`](../../xmake/targets.lua) | One library target per module, and the aggregate `pace` |
| [`xmake/modules.lua`](../../xmake/modules.lua) | The C++ named module target `pace_module` |
| [`xmake/checks.lua`](../../xmake/checks.lua) | The header self-containment check |
| [`xmake/tests.lua`](../../xmake/tests.lua), [`examples.lua`](../../xmake/examples.lua), [`benchmarks.lua`](../../xmake/benchmarks.lua), [`fuzz.lua`](../../xmake/fuzz.lua) | Tests, examples, benchmarks and fuzzers, each with its own options |

Build modes are `debug`, `release` and `releasedbg` (`-m`). The runtime and networking modules need no
third-party library; each optional module adds only its own packages.

## Library options

| Option | Default | Effect |
|:--|:--|:--|
| `--http1=y/n` | `y` | HTTP/1.0 and HTTP/1.1 module, including `http::app` and the gateway services (`pace_http1`) |
| `--websocket=y/n` | `y` | WebSocket module (`pace_websocket`) |
| `--tls=y/n` | `n` | TLS and HTTPS with OpenSSL (`pace_tls`); also adds the TLS forms of HTTP/1, HTTP/2 and WebSocket |
| `--dns=y/n` | `n` | Asynchronous DNS with c-ares (`pace_dns`); the URL clients then resolve host names |
| `--quic=y/n` | `n` | QUIC (`pace_quic`); POSIX only |
| `--quic_backend=ngtcp2/openssl` | `ngtcp2` | QUIC implementation: ngtcp2 with OpenSSL's TLS, or OpenSSL's native QUIC |
| `--http2=y/n` | `n` | HTTP/2 with nghttp2 (`pace_http2`); HTTPS needs `--tls=y` as well |
| `--http3=y/n` | `n` | HTTP/3 with nghttp3 (`pace_http3`); turns QUIC on |
| `--cxxstandard=cxx20/cxx23/cxxlatest` | `cxx20` | C++ language standard |
| `--modules=y/n` | `n` | Build the C++ named modules (`pace_module`) |
| `--sanitizers=y/n` | `n` | AddressSanitizer and UndefinedBehaviorSanitizer |
| `--profiling=y/n` | `n` | Intrusive per-thread stage timings, for diagnostics only |
| `--uring=y/n` | `n` | Linux io_uring readiness backend (liburing) |
| `--fuzz=none/libfuzzer/replay` | `none` | Fuzz targets |
| `--header_check=y/n` | `n` | Compile every public header on its own |

Each enabled module defines `PACE_HAS_<MODULE>` (`PACE_HAS_HTTP1`, `PACE_HAS_WEBSOCKET`, `PACE_HAS_TLS`,
`PACE_HAS_DNS`, `PACE_HAS_QUIC`, `PACE_HAS_HTTP2`, `PACE_HAS_HTTP3`) for every target that depends on it; the
ngtcp2 backend also defines `PACE_QUIC_NGTCP2`. Do not define these macros yourself: they must come from the
build of the library.

Configuring `--quic=y` or `--http3=y` on Windows or MinGW stops with the error "QUIC/HTTP3 currently require a
POSIX platform", before any package is fetched. See [platform support](platform-support.md).

To build only the runtime and networking, turn off the modules that are on by default:

```sh
xmake f -c -m release --http1=n --websocket=n -y
```

To build everything:

```sh
xmake f -c -m release --tls=y --dns=y --quic=y --http2=y --http3=y -y
```

### Packages

xmake fetches these pinned versions the first time an option needs them, which can need network access:

| Package | Version | Needed by |
|:--|:--|:--|
| OpenSSL | 3.5.7 | `--tls`, `--quic`, `--http3` |
| ngtcp2 | 1.26.0 | `--quic` / `--http3` with `--quic_backend=ngtcp2` |
| nghttp2 | 1.70.0 | `--http2` |
| nghttp3 | 1.18.0 | `--http3` |
| c-ares | 1.34.8 | `--dns` |
| liburing | 2.12 | `--uring` on Linux |
| doctest | 2.5.3 | `--tests` (test executables only) |
| Asio, libuv | 1.34.2, 1.52.1 | `--benchmarks` (comparison benchmarks only) |

Windows ARM64 and FreeBSD use repository-local package definitions for OpenSSL and ngtcp2 respectively
(`xmake/openssl_arm64.lua`, `xmake/ngtcp2_bsd.lua`); see
[Windows ARM64 OpenSSL](../contributing/windows-arm64-openssl.md).

## Targets

| Target | Kind | Contents | Depends on | Packages |
|:--|:--|:--|:--|:--|
| `pace_runtime` | static | Tasks, `io_context`, timers, cancellation, structured concurrency, workers | — | liburing with `--uring` |
| `pace_net` | static | TCP, UDP, addresses, stream helpers | `pace_runtime` | — |
| `pace_http` | header-only | HTTP messages and router | `pace_runtime` | — |
| `pace_tls` | static | TLS streams | `pace_net` | OpenSSL |
| `pace_dns` | static | DNS resolver | `pace_net` | c-ares |
| `pace_quic` | static | QUIC (one backend) | `pace_net` | OpenSSL, ngtcp2 for that backend |
| `pace_http1` | static | HTTP/1, `http::app`, gateway | `pace_net`, `pace_http`, and `pace_tls`, `pace_dns` when enabled | — |
| `pace_http2` | static | HTTP/2 | `pace_net`, `pace_http`, and `pace_tls`, `pace_dns` when enabled | nghttp2 |
| `pace_http3` | static | HTTP/3 | `pace_net`, `pace_http`, `pace_quic`, and `pace_dns` when enabled | nghttp3 |
| `pace_websocket` | static | WebSocket | `pace_net`, and `pace_tls` when enabled | — |
| `pace` | phony | Every enabled module | all of the above that are enabled | — |
| `pace_module` | static | C++ named modules (with `--modules=y`) | `pace` | — |

A module target compiles only `src/<module>/`, installs only its own headers (`include/pace/<module>/` and the
umbrella `<pace/<module>.hpp>`) and links only the modules and packages it needs. `xmake build pace_tls` builds the
runtime, networking and TLS and nothing else. Selecting `pace_net` never links OpenSSL, c-ares or HTTP code, even
when another target in the same project enables them. There is no single `libpace.a`: a manual link line must name
each module it uses and their dependencies.

Dependencies are public: a target that depends on a module inherits its include paths, defines, packages and
system libraries (`pthread` on POSIX; `ws2_32` and `bcrypt` on Windows).

## Depending on one module

Inside this repository, or in a project that includes it, depend on the narrowest target you use and include
its umbrella header:

```lua
target("my_application")
    set_kind("binary")
    add_files("main.cpp")
    add_deps("pace_http2")       -- or "pace" for every enabled module
```

```cpp
#include <pace/http2.hpp>        // or <pace/pace.hpp> for every enabled module
```

The module must be enabled when configuring (`--http2=y` here). To build PACE inside another xmake project, keep
its checkout in your tree and include its root file, which loads its own build files and keeps its paths
anchored to the checkout:

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

Configure `--tests=n --examples=n` when you need only the libraries. The library is C++ ABI code, not a stable C
ABI: build it with the same toolchain and options (including profiling) as the application.

### Independent configurations

xmake keeps one configuration per project directory. `-o <dir>` only changes where outputs go. For several
configurations side by side, give each its own `XMAKE_CONFIGDIR` and use it for every command:

```sh
export XMAKE_CONFIGDIR="$PWD/build/core-config"
xmake f -c -m release -o build/core --http1=n --websocket=n -y
xmake -y
xmake test -v
```

Unset `XMAKE_CONFIGDIR` to return to the default configuration.

## C++ named modules

`--modules=y` builds `pace_module`, which provides one named module per library module: `pace.runtime`,
`pace.net`, `pace.http`, `pace.http1`, `pace.gateway`, `pace.tls`, `pace.dns`, `pace.quic`, `pace.http2`,
`pace.http3`, `pace.websocket`, and `pace` for all of them. Each module exports only its own names, so a program
that uses TCP imports both:

```cpp
import pace.runtime;
import pace.net;
```

`import pace;` is the equivalent of `#include <pace/pace.hpp>`. A module whose library is disabled still exists
and exports nothing. The module interfaces in `modules/` are generated from `modules/exports.json` by
`scripts/generate_modules.py`. They wrap the ordinary headers and libraries, so module and header consumers share
one ABI and can be mixed in one program.

Depend on `pace_module` instead of `pace`. Named modules need Clang (CI uses Clang 18 and 21) or GCC 16 or newer;
GCC 15 and older stop with an error, because of a compiler defect with coroutines in modules. Headers remain the
primary interface; the default build never runs a module scanner.

## Header self-containment check

`--header_check=y` adds `pace_header_check`, which compiles every header under `include/pace/` in its own
translation unit, so a header that relies on an include it does not make fails here rather than in a consumer.
Headers of disabled modules compile to nothing. Build it with every module enabled:

```sh
xmake f -c -m release -o build/hc --tls=y --dns=y --quic=y --http2=y --http3=y --header_check=y -y
xmake -y
```

CI also builds each module on its own with the check enabled, which catches a module that silently relies on
another one.

## Sanitizers

`--sanitizers=y` compiles and links every target, the libraries included, with
`-fsanitize=address,undefined -fno-omit-frame-pointer`. It has no effect on the `windows` (MSVC) platform. Use a debug build and make
failures fatal:

```sh
xmake f -c -m debug -o build/asan --sanitizers=y -y
xmake -y
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 xmake test -v
```

## Profiling

`--profiling=y` defines `PACE_ENABLE_PROFILING` for `pace_runtime` and everything that depends on it, and records
per-thread stage timings. It adds overhead: use a separate build directory, and keep it off for performance
comparisons. Applications must be built with the same setting as the library.

## io_uring

`--uring=y` on Linux links liburing into `pace_runtime` and defines `PACE_HAS_IO_URING`. On other platforms the
option does nothing. Enabling it does not change the default backend, which stays epoll: select io_uring with
`PACE_IO_BACKEND=io_uring` or an explicit `io_context` constructor argument. See
[I/O backends](runtime/io-backends.md).

## Fuzzing

`--fuzz=libfuzzer` (Clang with libFuzzer) builds fuzz targets for the HTTP/1 server and client parsers and the
WebSocket server, and instruments the library code with ASan and UBSan as well. `--fuzz=replay` builds the same
targets with a small driver that replays input files, so any toolchain can run the corpus as a regression test.
In both modes `xmake test` replays the seed corpus in `tests/fuzz/corpus/`.

```sh
xmake f -c -m debug -o build/fuzz --toolchain=clang --fuzz=libfuzzer -y
xmake -y
xmake run pace_fuzz_http1_server tests/fuzz/corpus/http1_server
```

## Tests, examples and benchmarks

| Option | Default | Builds |
|:--|:--|:--|
| `--tests=y/n` | `y` | doctest executables (`pace_*_tests`) for the enabled modules, and `pace_interop_server` for external interop peers |
| `--examples=y/n` | `y` | `echo_server`, `http_server` and the `tutorial_*` programs of the enabled modules; with `--tests=y` the tutorials also run as tests |
| `--benchmarks=y/n` | `n` | TCP comparison against standalone Asio and libuv (`pace_bench`) |
| `--components=y/n` | `n` | PACE component microbenchmarks |
| `--scaling=y/n` | `n` | Worker-count and execution-mode scaling benchmarks |
| `--protocol_benchmarks=y/n` | `n` | HTTP, WebSocket and protocol probes for the enabled modules |

`xmake test -v` runs every test executable and tutorial that was built. Benchmark methodology is in
[performance](../performance/README.md).

## See also

- [Platform support](platform-support.md)
- [I/O backends](runtime/io-backends.md)
- [Install and first program](../tutorial/01-install-and-first-program.md)
- [CI coverage](../contributing/ci.md)
