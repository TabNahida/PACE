# Headers, platform handles and compatibility

## Header selection

Use the narrow header for a component or include `<pace/pace.hpp>` for the full
public surface. The umbrella includes tasks, reactors, runtime and the protocol
APIs enabled in the build. It retains the old TCP aliases.
`<pace/tcp.hpp>` is the canonical header for new TCP code.

| Include | Purpose |
|---|---|
| `<pace/task.hpp>` | Lazy coroutine return types |
| `<pace/io_context.hpp>` | Reactor and awaiters |
| `<pace/runtime.hpp>` | Fixed-context worker runtime |
| `<pace/tcp.hpp>` | Numeric IPv4/IPv6 TCP |
| `<pace/udp.hpp>` | Numeric IPv4/IPv6 datagrams |
| `<pace/http.hpp>` | HTTP/1.0–1.1 client and server; HTTPS with TLS enabled |
| `<pace/tls.hpp>` | Verified TLS streams, with `--tls=y` |
| `<pace/http2.hpp>` | HTTP/2 batch client and server, with `--http2=y` |
| `<pace/dns.hpp>` | DNS resolver, with `--dns=y` |
| `<pace/quic.hpp>` | POSIX QUIC streams, with `--quic=y` |
| `<pace/http3.hpp>` | HTTP/3 over QUIC, with `--http3=y` |
| `<pace/websocket.hpp>` | WebSocket client and server; wss with TLS enabled |
| `<pace/platform.hpp>` | Portable native socket handle types |
| `<pace/net.hpp>` | Previous TCP naming aliases |
| `<pace/networking.hpp>` | Core runtime and sockets |
| `<pace/extensions.hpp>` | Core plus enabled Layer 2 extensions |
| `<pace/application.hpp>` | Shared HTTP messages plus enabled application protocols |
| `<pace/pace.hpp>` | All enabled modules through public includes |

## Native socket values

```cpp
// namespace pace
#ifdef _WIN32
using native_socket_type = std::uintptr_t;
#else
using native_socket_type = int;
#endif
inline constexpr native_socket_type invalid_socket =
    static_cast<native_socket_type>(-1);
```

The Windows handle retains pointer width; do not store it in an `int` or call POSIX
`close` on it. The invalid sentinel is available for borrowed-handle checks.
`native_handle()` never transfers ownership. Raw readiness waits require a valid
native resource whose owner coordinates cancellation and close with the context.
On Windows readiness is for Winsock sockets, not arbitrary file handles. PACE
does not expose a portable asynchronous file API through these aliases.

Windows socket initialization occurs automatically before PACE creates sockets or
poller wake resources; consumers do not need to call WSAStartup for ordinary PACE
use. The public platform header includes Winsock declarations and defines
`NOMINMAX`/`WIN32_LEAN_AND_MEAN` when absent. Include Winsock headers before legacy
Windows socket declarations in application-specific platform code.

`pace::detail::initialize_sockets()` and its older internal synonym
`initialize_winsock()` are visible inline implementation helpers. They are not
application APIs; PACE calls them itself. Other `detail` headers, including the
poller and profiling scopes, likewise have no public compatibility contract.

## Compatibility aliases

Including `<pace/net.hpp>` or the umbrella preserves these exact type aliases:

```cpp
namespace pace {
using tcp_stream = tcp::stream;
using tcp_listener = tcp::listener;
using ipv4_endpoint = tcp::endpoint;
using listen_options = tcp::listen_options;
}
```

They do not wrap, copy or alter behavior; ownership and errors are those of the
canonical TCP types. New documentation uses `pace::tcp::*`. There is no
`pace::net` namespace or generic protocol endpoint abstraction.

## Platform scope

Linux uses epoll, 64-bit macOS/FreeBSD use kqueue, and Windows uses WSAPoll.
The Windows backend is a readiness implementation rather than IOCP. OS support
does not imply equal scaling or measured performance. Same-port listener reuse
is platform-dependent; the worker runtime itself is available independently of
that listener strategy. See [architecture](../architecture.md) and the
[benchmark methodology](../../benchmarks/README.md) for implementation and evidence.


The main functional CI matrix is configured for these module groups:

| Module group | Linux | macOS | FreeBSD | Windows |
|---|:---:|:---:|:---:|:---:|
| Runtime, TCP, UDP, HTTP/1.1, WebSocket | ✓ | ✓ | ✓ | ✓ |
| TLS, DNS, HTTP/2 | ✓ | ✓ | ✓ | ✓ |
| QUIC, HTTP/3 | ✓ | ✓ | ✓ | Unsupported |

Native CPU coverage includes x86_64 and aarch64 on Linux, macOS and Windows,
plus 32-bit Linux i386 and Windows x86 processes. FreeBSD uses an x86_64 VM.
Linux also checks GCC/Clang, C++23, sanitizers and builds with all optional
protocols disabled or only HTTP/3 enabled. An additional MinGW x86_64 job runs
core, HTTP1 and WebSocket tests on Windows.

ARMv7, RISC-V64, PowerPC64 LE and s390x Linux run core, HTTP1 and WebSocket
functional tests through QEMU user-mode emulation. These use the host Linux
kernel and do not establish native-hardware support or performance. Android
(three ABIs) and iOS (arm64 device/simulator) have compile-only library checks;
their network runtime, application integration and device restrictions remain
unverified.

The [CI coverage guide](../ci.md) lists exact runners, dependencies and evidence
limits. [Main workflow results](https://github.com/TabNahida/PACE/actions/workflows/ci.yml)
and [portability results](https://github.com/TabNahida/PACE/actions/workflows/portability.yml)
record which revisions passed. CI benchmark executables run short correctness
checks; published performance comparisons use local Linux measurements.
