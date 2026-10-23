# Functional CI and architecture coverage

PACE's CI separates native execution, emulated execution and compilation against
a foreign SDK. A green compile-only job is not evidence that networking ran on
that device. The matrices below describe the configured checks; consult the
[main workflow results](https://github.com/TabNahida/PACE/actions/workflows/ci.yml)
and [portability results](https://github.com/TabNahida/PACE/actions/workflows/portability.yml)
for the exact revision and jobs that passed.

## Native execution

Every main matrix entry builds the runtime, TCP/UDP, enabled protocols, tests and
tutorials, then runs `xmake test -v` and independent Python WebSocket peers.
The executable's ELF, PE or Mach-O header is checked against the requested CPU
and word size before execution. In particular, the ARM Windows job cannot pass
by accidentally building an x64 executable and running it through Windows
emulation.

| OS / runner | Target | Compiler / standard | Protocols |
|---|---|---|---|
| Ubuntu 24.04 | x86_64 | GCC, C++20 and C++23 | All |
| Ubuntu 24.04 | x86_64 | Clang, C++20 | All |
| Ubuntu 24.04 ARM | aarch64 | GCC, C++20 | All |
| Ubuntu 24.04 | i386, 32-bit process on x86_64 Linux | GCC multilib, C++20 | All |
| macOS 15 Intel | x86_64 | Apple Clang, C++20 | All |
| macOS 15 | arm64 | Apple Clang, C++20 | All |
| Windows 2025 | x64 and x86 | MSVC, C++20 | All except QUIC/HTTP3 |
| Windows 11 ARM | arm64 | MSVC, C++20 | All except QUIC/HTTP3 |
| FreeBSD 14.3 VM | x86_64 | Clang, C++20 | All |
| Windows 2025 / MSYS2 UCRT | x86_64 | MinGW GCC, C++20 | Core, HTTP1, WebSocket |

“All” includes HTTP1, WebSocket, TLS, DNS, QUIC, HTTP2 and HTTP3. Windows QUIC and
HTTP3 remain unsupported; Windows jobs check the explicit configuration error.
MinGW is an additional compiler/backend check without optional third-party
protocol dependencies, rather than a second full dependency matrix.
Windows ARM64 uses a scoped [OpenSSL build correction](diagnostics/windows-arm64-openssl.md)
validated by independent memory-BIO handshakes and the ordinary protocol tests.

Linux retains all-protocol ASan/UBSan/leak checks, a dependency-free core build,
and an HTTP3-only build that does not enable HTTP1, HTTP2 or TCP TLS.
The x86_64 GCC, Intel macOS, x64 Windows and FreeBSD entries additionally check
benchmark checksums, result schemas and instrumented-runtime counters. These
are small correctness workloads: timing data is discarded and CI publishes no
performance rankings. Performance measurements are collected on local Linux.

HTTP/2 backpressure scenarios each have a 30-second watchdog. The deliberately
small socket buffers can interact with FreeBSD's TCP segmentation and delayed
ACKs, so a single deadline shared by the entire growing suite could expire
while later scenarios were still progressing. The per-scenario bound retains
the same payloads, 4 KiB buffer settings and correctness assertions. FreeBSD
enables opt-in stage/socket diagnostics for failures; these are correctness
diagnostics, not performance measurements.

## Emulated Linux execution

Ubuntu cross compilers build release executables with C++20. Explicit QEMU user
emulators then run all five enabled test suites and eight tutorials, including
loopback TCP/UDP, HTTP and WebSocket. There is no implicit `binfmt_misc` setup.
The runner verifies every executable's ELF machine type, word size and byte
order before running it.

| Target | Compiler prefix | Emulator | Word size / byte order |
|---|---|---|---|
| ARMv7 hard float | `arm-linux-gnueabihf-` | `qemu-arm` | 32-bit / little endian |
| RISC-V64 | `riscv64-linux-gnu-` | `qemu-riscv64` | 64-bit / little endian |
| PowerPC64 LE | `powerpc64le-linux-gnu-` | `qemu-ppc64le` | 64-bit / little endian |
| IBM Z / s390x | `s390x-linux-gnu-` | `qemu-s390x` | 64-bit / big endian |

These jobs cover the runtime, TCP/UDP, HTTP1 and WebSocket. They do not compile
TLS, DNS, QUIC, HTTP2 or HTTP3. QEMU translates CPU instructions and Linux system
calls onto the host kernel; this exercises target ABI/endianness, but does not
validate a physical target's kernel, hardware memory model or performance.

The local equivalent, after installing `g++-arm-linux-gnueabihf` and `qemu-user`,
is:

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

## Mobile SDK compilation only

| SDK / build host | Targets | Scope |
|---|---|---|
| Android NDK 27.3.13750724, API 30 / Ubuntu 24.04 | armeabi-v7a, arm64-v8a, x86_64 | Static libraries only |
| iOS SDK, minimum iOS 15 / macOS 15 | arm64 device and arm64 simulator | Static libraries only |

Both SDK checks compile the runtime, TCP/UDP, HTTP1 and WebSocket without optional
dependencies. Archive members are inspected to verify the expected ELF/Mach-O
architecture. They do not link or sign an application, launch a simulator, or
run network tests. Device lifecycle, entitlements, sandbox restrictions and
socket behavior therefore remain unverified; these targets are not yet claimed
as supported runtime platforms. WebAssembly and 32-bit Apple targets are omitted
because the current socket backends do not support them.

## Runtime and maintenance cost

There are 23 independent jobs across the two workflows. Jobs cancel superseded
runs, use read-only repository permissions and have bounded timeouts. Protocol
dependency caches are separated by OS, target architecture and compiler.
The extra emulated/mobile jobs avoid third-party protocol builds; expensive
benchmark-schema/profiling checks run once per primary desktop OS and FreeBSD.
No architecture is silently allowed to fail. Portability artifacts record the
target, execution mode, inspected binary metadata and passed functional checks;
compile-only and metadata-only reports explicitly state that they ran no tests.

Runner labels and SDK selections were checked against the following official
sources when this matrix was added:

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
