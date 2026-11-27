# Continuous integration and architecture coverage

[简体中文](ci.zh.md) · [Contributing](README.md)

PACE's CI separates native execution, emulated execution and compilation against a foreign SDK. A green
compile-only job is not evidence that networking ran on that device. The tables below describe the configured
checks; the [workflow results](https://github.com/TabNahida/PACE/actions) show the exact revision and jobs that
passed. CI checks correctness only: it discards benchmark timings and publishes no performance rankings.
Published performance measurements come from local Linux.

## Workflows

The workflows live in `.github/workflows/`. Every workflow except the measurement ones runs on pushes to `main`
and on pull requests, and can be started by hand.

| Workflow | File | Checks |
|---|---|---|
| Build and functional validation | `ci.yml` | Native builds and tests on Linux, macOS, Windows and FreeBSD; sanitizers; each module on its own |
| Architecture portability | `portability.yml` | QEMU execution on four Linux architectures, Android and iOS compilation, MinGW |
| QUIC backends and modern compilers | `quic-backends.yml` | Both QUIC backends with GCC 15, GCC 16 and Clang 21; named modules; backend interoperability |
| C++ named modules | `modules.yml` | `import pace` with Clang 18 in three configurations |
| Code style | `style.yml` | clang-format, generated module interfaces |
| Static analysis | `tidy.yml` | clang-tidy on the lines a change touches |
| Documentation | `docs.yml` | Translations, structure, links and README figures checked; site built with `mkdocs build --strict` and published to GitHub Pages from `main` once Pages is enabled |
| Fuzz | `fuzz.yml` | libFuzzer on the HTTP/1 and WebSocket parsers; seed corpus replay on macOS and Windows |
| Interop | `interop.yml` | curl, `openssl s_client` and h2spec against PACE servers |
| Benchmark baseline | `baseline.yml` | Manual: every benchmark suite for one revision, raw results as artifacts |
| Network and runtime performance | `network-performance.yml`, `runtime-performance.yml` | Manual or on measurement branches: paired comparisons on hosted runners |

## Native execution

Every entry of the main matrix in `ci.yml` builds the runtime, TCP/UDP, every protocol the platform supports,
the tests and the tutorials, then runs `xmake test -v` and independent Python WebSocket peers. The executable's
ELF, PE or Mach-O header is checked against the requested CPU and word size before execution. In particular,
the ARM Windows job cannot pass by accidentally building an x64 executable and running it through Windows
emulation.

| OS / runner | Target | Compiler / standard | Protocols | I/O backend |
|---|---|---|---|---|
| Ubuntu 24.04 | x86_64 | GCC, C++20 and C++23 | All | epoll |
| Ubuntu 24.04 | x86_64 | GCC, C++20, `--uring=y` | All | io_uring |
| Ubuntu 24.04 | x86_64 | Clang, C++20 | All | epoll |
| Ubuntu 24.04 ARM | aarch64 | GCC, C++20 | All | epoll |
| Ubuntu 24.04 | i386, 32-bit process on x86_64 Linux | GCC multilib, C++20 | All | epoll |
| macOS 15 Intel | x86_64 | Apple Clang, C++20 | All | kqueue |
| macOS 15 | arm64 | Apple Clang, C++20 | All | kqueue |
| Windows 2025 | x64 | MSVC, C++20 | All except QUIC/HTTP3 | IOCP |
| Windows 2025 | x86 | MSVC, C++20 | All except QUIC/HTTP3 | WSAPoll |
| Windows 11 ARM | arm64 | MSVC, C++20 | All except QUIC/HTTP3 | IOCP |
| FreeBSD 14.3 VM | x86_64 | Clang, C++20 | All | kqueue |

“All” means HTTP1, WebSocket, TLS, DNS, QUIC, HTTP2 and HTTP3. Windows does not support QUIC and HTTP3; the
Windows jobs check that configuring them fails with an explicit error. Windows ARM64 uses a scoped
[OpenSSL build correction](windows-arm64-openssl.md), and that job also runs the independent OpenSSL diagnostic
and keeps its symbols as an artifact.

Three more Linux jobs in `ci.yml` cover what the matrix does not:

- **Sanitizers**: every protocol in a debug build with ASan, UBSan and leak detection; any report is fatal.
- **Each module on its own**: the core alone, then TLS, DNS, QUIC, HTTP1, HTTP2, HTTP3 and WebSocket each
  enabled on their own, with `--header_check=y`, which compiles every public header in its own translation
  unit. A module that silently relies on another module, or a header that misses an include, fails here.
- **FreeBSD**: the FreeBSD row above runs in a VM, with the same tests, Python peers and benchmark checks.

The x86_64 GCC, Intel macOS, x64 Windows and FreeBSD entries additionally check benchmark checksums, result
schemas and instrumented-runtime counters (a second build with `--profiling=y`). These are small correctness
workloads: timing data is discarded.

HTTP/2 backpressure scenarios each have a 30-second watchdog. The deliberately small socket buffers can interact
with FreeBSD's TCP segmentation and delayed ACKs, so a single deadline shared by the entire growing suite could
expire while later scenarios were still progressing. The per-scenario bound keeps the same payloads, 4 KiB
buffer settings and correctness assertions. FreeBSD enables opt-in HTTP/2 tracing for failures; these are
correctness diagnostics, not performance measurements.

## Emulated Linux execution

In `portability.yml`, Ubuntu cross compilers build release executables with C++20. Explicit QEMU user emulators
then run every enabled test suite and tutorial, including loopback TCP/UDP, HTTP and WebSocket. There is no
implicit `binfmt_misc` setup. The runner verifies every executable's ELF machine type, word size and byte order
before running it.

| Target | Compiler prefix | Emulator | Word size / byte order |
|---|---|---|---|
| ARMv7 hard float | `arm-linux-gnueabihf-` | `qemu-arm` | 32-bit / little endian |
| RISC-V64 | `riscv64-linux-gnu-` | `qemu-riscv64` | 64-bit / little endian |
| PowerPC64 LE | `powerpc64le-linux-gnu-` | `qemu-ppc64le` | 64-bit / little endian |
| IBM Z / s390x | `s390x-linux-gnu-` | `qemu-s390x` | 64-bit / big endian |

These jobs cover the runtime, TCP/UDP, HTTP1 and WebSocket. They do not compile TLS, DNS, QUIC, HTTP2 or HTTP3.
QEMU translates CPU instructions and Linux system calls onto the host kernel; this exercises the target ABI and
byte order, but does not validate a physical target's kernel, hardware memory model or performance.

The local equivalent, after installing `g++-arm-linux-gnueabihf` and `qemu-user`, is:

```sh
export XMAKE_CONFIGDIR=/tmp/pace-armv7-ci-config
xmake f -c -p linux -a armv7 --toolchain=cross \
  --cross=arm-linux-gnueabihf- --bin=/usr/bin -m release \
  -o /tmp/pace-armv7-ci-build --cxxstandard=cxx20 \
  --http1=y --websocket=y --tls=n --dns=n --quic=n --http2=n --http3=n \
  --benchmarks=n --components=n --scaling=n --profiling=n --sanitizers=n \
  --examples=y --tests=y -y
xmake -y
python3 scripts/ci_portability.py --build-dir /tmp/pace-armv7-ci-build \
  --target armv7 --mode qemu-user --emulator qemu-arm \
  --sysroot /usr/arm-linux-gnueabihf --output /tmp/pace-armv7-ci.json
```

`portability.yml` also builds with MinGW GCC (MSYS2 UCRT64) on Windows 2025 and runs the core, HTTP1 and
WebSocket suites, the tutorials and the Python WebSocket peers natively. It is an additional compiler check
without optional third-party protocol dependencies, not a second full dependency matrix.

## Mobile SDK compilation only

| SDK / build host | Targets | Scope |
|---|---|---|
| Android NDK 27.3.13750724, API 30 / Ubuntu 24.04 | armeabi-v7a, arm64-v8a, x86_64 | Static libraries only |
| iOS SDK, minimum iOS 15 / macOS 15 | arm64 device and arm64 simulator | Static libraries only |

Both SDK checks compile the runtime, TCP/UDP, HTTP1 and WebSocket without optional dependencies. Archive members
are inspected to verify the expected ELF/Mach-O architecture. They do not link or sign an application, launch a
simulator, or run network tests. Device lifecycle, entitlements, sandbox restrictions and socket behavior
therefore remain unverified; these targets are not claimed as supported runtime platforms. WebAssembly and
32-bit Apple targets are omitted because the socket backends do not support them.

## QUIC backends and modern compilers

`quic-backends.yml` builds every protocol with each QUIC backend (`--quic_backend=openssl` and `ngtcp2`) using
GCC 15, GCC 16.2 and Clang 21, and runs all tests and tutorials. With GCC 16 and Clang 21 it also builds the
named module with every protocol and runs `pace_module_tests`, which mixes header and module consumers across
translation units; GCC 15 is skipped there because it hits an internal compiler error on imported inline
coroutines. A final job exchanges payloads between a client and a server built with different backends, in
both directions.

## Named modules

`modules.yml` builds `import pace` with Clang 18 and its module scanner in three configurations: the core alone
with C++20, the default modules (HTTP1 and WebSocket) with C++20, and every protocol with C++23. It runs the
enabled tests, including the cross-translation-unit ABI test.

## Style, static analysis and fuzzing

- `style.yml` installs the pinned clang-format 22.1.8 and runs `python scripts/format.py --check`, then
  `python scripts/generate_modules.py --check` to make sure the named module interfaces match
  `modules/exports.json`.
- `tidy.yml` configures every protocol on Linux, exports compile commands and runs the pinned clang-tidy 22.1.8
  on the lines of `include/` and `src/` that the change touches; any warning fails the job. `.clang-tidy` lists
  the checks and the reasons for the ones that are off.
- `fuzz.yml` builds the `http1_server`, `http1_client` and `websocket_server` targets with libFuzzer, ASan and
  UBSan (Clang 21, `--fuzz=libfuzzer`) and fuzzes each for 60 seconds on pushes and pull requests and for
  30 minutes nightly. The working corpus is cached between runs and crash reproducers are uploaded as artifacts.
  The seed corpus in `tests/fuzz/corpus` is replayed as an ordinary test (`--fuzz=replay`) on macOS 15 and
  Windows 2025.

## Interoperability with external peers

`interop.yml` builds `pace_interop_server` with every protocol and runs `tests/interop/run_interop.py` against
it: curl over HTTP/1.1, HTTPS, h2c and h2, `openssl s_client`, and h2spec 2.6.0. h2spec is informational until
its known failures are recorded in `tests/interop/h2spec-known-failures.txt`. `tests/interop/peers.json` pins
the peers and lists the ones later phases add.

## Performance workflows

`baseline.yml` is started by hand. It builds every benchmark suite for one tag, branch or commit on Linux x86_64,
Linux arm64 and macOS arm64, measures them with `scripts/run_baseline.py` and uploads the raw results. Hosted
runners are shared and noisy: compare these artifacts only with other runs of the same workflow, and use a quiet
local Linux machine for published numbers. `network-performance.yml` and `runtime-performance.yml` run paired
comparisons on each I/O backend, including FreeBSD, when started by hand or pushed to a measurement branch; they
too only produce artifacts and job summaries.

## Runtime and maintenance cost

Jobs cancel superseded runs, use read-only repository permissions and have bounded timeouts. Protocol dependency
caches are separated by OS, target architecture and compiler. The emulated and mobile jobs avoid third-party
protocol builds; benchmark-schema and profiling checks run once per primary desktop OS and FreeBSD. No job is
allowed to fail silently. Portability artifacts record the target, execution mode, inspected binary metadata and
passed functional checks; compile-only and metadata-only reports state explicitly that they ran no tests.

Runner labels and SDK selections were checked against the following official sources when the matrix was added:

- [xmake-repo workflows](https://github.com/xmake-io/xmake-repo/tree/dev/.github/workflows)
  provide the reference for native ARM, separate Apple architectures, cross
  compilers and mobile SDK jobs. PACE uses a smaller dependency matrix tailored
  to its epoll, kqueue and WSAPoll backends.
- [GitHub hosted runner reference](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)
  lists the explicit Ubuntu 24.04 ARM, Windows 11 ARM and macOS 15 Intel labels.
- [Ubuntu 24.04 image contents](https://github.com/actions/runner-images/blob/main/images/ubuntu/Ubuntu2404-Readme.md)
  lists the Android SDK; the workflow installs the pinned NDK if the image does
  not already contain it.
- [QEMU user-mode documentation](https://www.qemu.org/docs/master/user/main.html)
  describes syscall translation and the limits of emulated execution.

## Explicit I/O backend coverage

One Linux native job builds `--uring=y` and runs the full suite with `PACE_IO_BACKEND=io_uring`. Windows x64 and
ARM64 explicitly select IOCP; x86 selects WSAPoll because the independent SDK probe rejects notification
registrations under WoW64. Backend tests verify the selected implementation, queue pressure, cancellation and
socket reuse. Gateway tests cover path containment, cache policy, pooling, balancing and deadlines. Remote
runners provide correctness results only. See [I/O backends](../reference/runtime/io-backends.md).
