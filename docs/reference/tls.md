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
enum class trust_source { system, application };
enum class sni_policy { use_default, reject };
class context {
public:
    explicit context(role mode = role::client, trust_source trust = trust_source::system);
    ~context();
    context(context&&) noexcept;
    context& operator=(context&&) noexcept;
    context(const context&) = delete;
    context& operator=(const context&) = delete;
    void load_verify_file(const std::string& pem_file);
    void load_verify_pem(std::string_view certificates);
    void use_certificate_chain(const std::string& certificate_chain_file,
                               const std::string& private_key_file);
    void set_alpn_protocols(const std::vector<std::string>& protocols);
    void add_server_name(std::string name, const context& configuration);
    void set_server_name_policy(sni_policy missing, sni_policy unrecognized);
};
}
```

`tls::context` is TLS configuration, separate from an `io_context` reactor.
Configuration freezes when the first stream is constructed from it, or when it
is registered as an SNI target. Later mutation is rejected with `logic_error`,
even after those streams have closed. Configure certificates, trust and ALPN
before that boundary; use a new context to change them. Setters and freezing are
serialized internally. Once configured, a context can create independent streams
on multiple reactors; moving or destroying the public object must not race with
access to that object. A moved-from configuration cannot configure new streams.

Streams share ownership of the configuration and its entire SNI target table.
Public context objects may be moved or destroyed after stream construction;
certificates, trust roots and callback state remain alive until the last owning
stream/context is destroyed.

Clients always verify the certificate chain and expected DNS/IP identity.
The default `trust_source::system` loads OpenSSL's default trust paths during
construction; a loading error throws. `trust_source::application` instead starts
with an empty trust store and **never calls default-path loading**. This permits
explicit trust configuration even when a deployment's OpenSSL defaults are
unavailable. Without a suitable root, the handshake fails; there is no insecure
fallback.

`load_verify_file` adds roots from an explicit PEM file. `load_verify_pem` adds one
or more in-memory `CERTIFICATE` PEM blocks, separated by whitespace. It copies
the certificates into the context-owned store; the input can be destroyed as soon
as the call returns. Duplicate certificates are accepted. Empty, malformed or
mixed-content bundles throw; the entire bundle is parsed before any roots are
added. No temporary files, borrowed `X509_STORE`, verification callbacks or mutable
native handles are exposed.

```cpp
pace::tls::context client(pace::tls::role::client,
                          pace::tls::trust_source::application);
client.load_verify_pem(application_ca_bundle); // Owned/copied roots, still verified.
pace::tls::stream connection(io, client);
co_await connection.connect({"192.0.2.1", 443}, "api.example.com");
```

PACE does not automatically import Windows native roots or the macOS Keychain.
Applications can export their chosen platform certificates to in-memory PEM and
pass that bundle, or supply an explicit CA file. Wrong hostnames, untrusted roots
and expired certificates remain handshake errors with either trust source.

Server contexts require a certificate chain and matching unencrypted PEM private
key. The chain file starts with the leaf followed by its intermediates. Loading
is synchronous and checks key correspondence before installing the new pair, so
a missing/mismatched key does not replace an earlier valid certificate. Encrypted keys fail without a
terminal password prompt. A server does not require client certificates in this
initial API. There is no mutual-TLS policy or verification-disable switch.

ALPN identifiers are nonempty strings of 1–255 bytes with a total encoded length
of at most 65535 bytes. The server selects the first protocol in its configured
preference list that the client offers. An offered list without overlap fails the
handshake. An empty configured list disables ALPN; a peer that does not send ALPN
can complete without a selected protocol. Applications requiring `h2` must check
`selected_protocol() == "h2"` before exchanging HTTP/2 frames. ALPN negotiation
alone does not implement the selected application protocol.

## Independent server certificates with SNI

```cpp
pace::tls::context domain_a(pace::tls::role::server);
domain_a.use_certificate_chain("a-fullchain.pem", "a-key.pem");
domain_a.set_alpn_protocols({"http/1.1"});
pace::tls::context domain_b(pace::tls::role::server);
domain_b.use_certificate_chain("b-fullchain.pem", "b-key.pem");
domain_b.set_alpn_protocols({"http/1.1"});

pace::tls::context server(pace::tls::role::server);
server.use_certificate_chain("default-fullchain.pem", "default-key.pem");
server.set_alpn_protocols({"http/1.1"});
server.add_server_name("a.example", domain_a); // Freezes and retains domain_a.
server.add_server_name("b.example", domain_b);
server.set_server_name_policy(pace::tls::sni_policy::use_default,
                              pace::tls::sni_policy::reject);
// Missing SNI uses the default; unknown SNI fails this handshake.
```

Names match exactly, ignoring ASCII case. Supply ASCII DNS labels (IDNs as
A-labels); wildcards, IP literals, trailing dots, empty/invalid labels, duplicate
names, client contexts and self-references are rejected. Targets must not contain
another SNI routing table; their certificate and ALPN configuration is selected
directly. A target can be shared by multiple names or server contexts.

Missing and unrecognized SNI have independent policies; both initially
`use_default`. A numeric client peer name sends no SNI and follows the missing
policy. Malformed DNS SNI is rejected. With `use_default`, clients still check
the fallback certificate against their requested identity. A selected target
that cannot complete a handshake fails only that connection and **never falls
back** to the default. Catch its exception before continuing to serve peers.

Selection runs synchronously inside the owning reactor's handshake. It only
looks up the frozen table and changes that connection's `SSL` context; it never
replaces a certificate on a shared default context or loads files during a
handshake. The selected context supplies its certificate chain and ALPN policy.
The same behavior applies to implicit TLS and TCP streams upgraded with STARTTLS.

SNI routing contexts disable session caches and tickets, including TLS 1.3
tickets, so session resumption cannot bypass per-connection selection. This is
necessary because OpenSSL's servername callback runs after session-resumption
processing. Its callback runs before ALPN selection. See the official
[OpenSSL SNI callback contract](https://docs.openssl.org/3.0/man3/SSL_CTX_set_tlsext_servername_callback/).

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
handshake. TCP endpoints use numeric IPv4 or IPv6; the name is the expected
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

For STARTTLS, finish the protocol's plaintext negotiation on the TCP stream,
then move it into `tls::stream` and call `handshake(peer_name)` on the client or
`handshake()` on the server. Read only the plaintext response bytes: encrypted
bytes already consumed into an application buffer cannot be fed back through
this socket-based TLS interface.

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
then closes the transport. `close_gracefully` is its alias. A cancellable reactor
timer bounds nonblocking readiness waits; reaching the timeout closes normally, while a
TLS error or cancellation closes and propagates. Zero/negative timeout attempts
shutdown once and then closes if the exchange is incomplete. The bound cannot
preempt blocking application code or guarantee peer receipt. No other TLS
operation may be active during shutdown. Its scoped timer is removed immediately
when shutdown finishes; no polling task or delayed callback outlives the operation.

`close()` immediately cancels transport waits and closes the native socket; it
does not send `close_notify` or wait for suspended members. Retain the C++ stream
until those members resume and finish. Destruction also closes immediately.
As with TCP, close on the owning reactor or while it is not running. After moving,
only destruction, assignment, `close()` and `is_open()` are supported on the source.

Linux, macOS, Windows and FreeBSD use the same nonblocking OpenSSL state machine.
A custom socket BIO preserves full-width Winsock handles and uses SIGPIPE-safe
sends on supported POSIX targets. TLS is optional and adds no OpenSSL dependency
to builds that leave it disabled.
