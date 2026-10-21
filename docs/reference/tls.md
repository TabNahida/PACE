# TLS with OpenSSL

[简体中文](tls.zh-CN.md) · [WebSocket](websocket.md)

Enable `--tls=y` and include `<pace/tls.hpp>`. The build exports `PACE_HAS_TLS`
and links OpenSSL `ssl` and `crypto`; without that option this header does not
declare TLS types. OpenSSL 1.1.1 or newer is required; use a supported OpenSSL 3.x
release for deployments. TLS 1.2 is the minimum protocol version. Obsolete SSL,
TLS 1.0/1.1, compression and renegotiation are disabled.

```sh
xmake f -m release --tls=y --examples=y --tests=y -y
xmake -y
xmake test -v
xmake run tutorial_tls
```

The [complete example](../../examples/tutorial_tls.cpp) verifies a local TLS
connection and exits. Its checked-in certificate and private key are public test
fixtures, never deployment credentials. The test CA is loaded only into that
client configuration, without modifying the operating system trust store.

## Configuration

```cpp
namespace pace::tls {
enum class role { client, server };
class context {
public:
    explicit context(role mode = role::client);
    ~context();
    context(context&&) noexcept;
    context& operator=(context&&) noexcept;
    context(const context&) = delete;
    context& operator=(const context&) = delete;
    void load_verify_file(const std::string& pem_file);
    void use_certificate_chain(const std::string& certificate_chain_file,
                               const std::string& private_key_file);
    void set_alpn_protocols(const std::vector<std::string>& protocols);
};
}
```

`tls::context` is TLS configuration, separate from an `io_context` reactor.
Configure it before creating streams. Do not modify configuration while any
streams use it or access it concurrently. Streams share ownership of the
configuration, so the public configuration object may be moved or destroyed
after stream creation. A moved-from configuration cannot configure new streams.

Client contexts always enable certificate-chain verification and load OpenSSL's
default trust paths. `load_verify_file` adds trusted PEM certificates, for example
a private CA. OpenSSL's trust installation is platform-dependent; this does not
automatically import the Windows certificate store or macOS Keychain. Configure
the appropriate CA file when the OpenSSL installation lacks usable default roots.
Missing or invalid files throw; they never silently disable verification.

Server contexts require a certificate chain and matching unencrypted PEM private
key. The chain file starts with the leaf followed by its intermediates. Loading
is synchronous and checks key correspondence; encrypted keys fail without a
terminal password prompt. A server does not require client certificates in this
initial API. There is no mutual-TLS policy or verification-disable switch.

ALPN identifiers are nonempty strings of 1–255 bytes with a total encoded length
of at most 65535 bytes. The server selects the first protocol in its configured
preference list that the client offers. An offered list without overlap fails the
handshake. An empty configured list disables ALPN; a peer that does not send ALPN
can complete without a selected protocol. Applications requiring `h2` must check
`selected_protocol() == "h2"` before exchanging HTTP/2 frames. ALPN negotiation
alone does not implement the selected application protocol.

## Streams and handshakes

```cpp
stream(io_context& io, const tls::context& configuration);
stream(tcp::stream transport, const tls::context& configuration);
~stream();
stream(stream&&) noexcept;
stream& operator=(stream&&) noexcept;

task<void> connect(tcp::endpoint endpoint, std::string peer_name);
task<void> handshake(std::string peer_name = {});
io_context& context() const noexcept;
bool is_open() const noexcept;
std::string selected_protocol() const;
void set_no_delay(bool enabled = true);
void close() noexcept;
```

The stream owns its TCP transport. Construct with an `io_context` for a new
socket, or move an accepted/connected TCP stream into the second constructor.
Do not use the moved transport for plaintext I/O. `context()` returns the reactor,
and `selected_protocol()` returns an owned copy of the negotiated ALPN identifier
or an empty string. `is_open()` checks the owned transport, not handshake success.

For clients, `connect(endpoint, peer_name)` performs TCP connection and TLS
handshake. TCP endpoints currently use numeric IPv4; the name is the expected
certificate identity, independently of that address. For example:

```cpp
pace::tls::context configuration; // Verified clients by default.
pace::tls::stream connection(io, configuration);
co_await connection.connect({"192.0.2.1", 443}, "api.example.com");
```

The example address above is reserved for documentation. Use your service's
resolved address. DNS names configure SNI and certificate hostname checking;
numeric IP names check IP subject alternative names and do not send DNS SNI.
An empty name or embedded NUL is rejected. There is no anonymous/insecure client
mode. On an already-connected client stream call `handshake(peer_name)`.

On the server, wrap the accepted stream and call `handshake()` without a name:

```cpp
pace::tls::stream connection(co_await listener.accept(), server_configuration);
co_await connection.handshake();
```

Both handshake paths suspend on nonblocking socket readiness. Handshake failure
or cancellation closes the transport. There is no individual handshake deadline;
an application may request reactor stop or arrange lifetime-safe stream closure.

## Records, ownership and concurrency

```cpp
task<std::size_t> read_some(std::span<char> buffer);
task<std::size_t> read_some(std::span<std::byte> buffer);
task<void> write_all(std::string_view bytes);
task<void> write_all(std::span<const std::byte> bytes);
```

Await operations on the stream's owning reactor after a successful handshake.
**Exactly one asynchronous TLS operation may be outstanding at a time**, including
read versus write, handshake and shutdown. Overlap throws `logic_error` without
canceling the existing operation. This initial interface is suitable for
sequential request/response flows; independent reader/writer coroutines must not
share one TLS stream. OpenSSL's WANT_READ/WANT_WRITE transitions can require either
direction for an operation, so TCP's independent read/write contract does not
apply here.

Keep the stream object at a stable address, with its reactor alive, until each
operation completes. Move only while idle. Buffers/views borrow application
memory throughout suspension. `write_all` handles partial encrypted writes and
finishes when OpenSSL has accepted every plaintext byte; it does not promise
peer processing. `read_some` returns decrypted bytes, possibly fewer than requested.
With a nonempty buffer, zero means a verified TLS `close_notify`. Bare TCP EOF
without that alert is a protocol error, preventing silent acceptance of truncated
TLS traffic. An empty buffer returns zero without reading.

TLS failures use `std::system_error` with `std::errc::protocol_error` and OpenSSL
diagnostic text. Reactor cancellation uses `operation_canceled`; TCP connect
errors retain TCP error codes. Invalid arguments and operation-state violations
use `invalid_argument`/`logic_error`. A failed active operation closes the socket,
except that a rejected overlapping operation leaves the existing operation alone.
Catch expected per-connection failures so an unhandled root does not stop peers.

## Closing

```cpp
task<void> shutdown(std::chrono::milliseconds timeout = std::chrono::milliseconds(100));
task<void> close_gracefully(std::chrono::milliseconds timeout = std::chrono::milliseconds(100));
```

`shutdown` sends `close_notify`, waits cooperatively for its peer's notification,
then closes the transport. `close_gracefully` is its alias. A 1 ms cancellable
timer bounds readiness retries; reaching the timeout closes normally, while a
TLS error or cancellation closes and propagates. Zero/negative timeout attempts
shutdown once and then closes if the exchange is incomplete. The bound cannot
preempt blocking application code or guarantee peer receipt. No other TLS
operation may be active during shutdown.

`close()` immediately cancels transport waits and closes the native socket; it
does not send `close_notify` or wait for suspended members. Retain the C++ stream
until those members resume and finish. Destruction also closes immediately.
As with TCP, close on the owning reactor or while it is not running. After moving,
only destruction, assignment, `close()` and `is_open()` are supported on the source.

Linux, macOS, Windows and FreeBSD use the same nonblocking OpenSSL state machine.
A custom socket BIO preserves full-width Winsock handles and uses SIGPIPE-safe
sends on supported POSIX targets. TLS is optional and adds no OpenSSL dependency
to builds that leave it disabled.
