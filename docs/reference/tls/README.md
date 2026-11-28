# TLS (`pace/tls.hpp`)

[简体中文](README.zh.md) · [Reference](../README.md)

## Overview

The TLS module runs TLS over a TCP stream or any other [`pace::byte_stream`](../net/README.md#byte_stream) with
OpenSSL, for verified clients, servers with per-name certificates, STARTTLS and ClientHello routing.

| Header | Target | Build option |
|:--|:--|:--|
| `<pace/tls.hpp>` | `pace_tls` | `--tls=y` |

`<pace/tls.hpp>` includes `<pace/tls/stream.hpp>`, which also includes `<pace/net/tcp.hpp>`. The build option
defines `PACE_HAS_TLS` and links OpenSSL `ssl` and `crypto`; without it the header declares nothing and no other
module depends on OpenSSL for TLS. With `--modules=y` the names are exported by `import pace.tls;`.

OpenSSL 1.1.1 or newer is required; deploy with a supported OpenSSL 3.x release. TLS 1.2 is the minimum protocol
version; SSL, TLS 1.0 and 1.1, compression and renegotiation are disabled. Linux, macOS, Windows and FreeBSD use the
same nonblocking OpenSSL state machine.

The checked-in example `examples/tutorial_tls.cpp` verifies a loopback TLS connection with test certificates from
`tests/certs`. Those certificates and keys are public test fixtures, never deployment credentials; the test CA is
loaded only into the example's client configuration.

```sh
xmake f -m release --tls=y --examples=y --tests=y -y
xmake -y
xmake run tutorial_tls
```

## Synopsis

```cpp
#include <pace/net.hpp>
#include <pace/runtime.hpp>
#include <pace/tls.hpp>

#include <iostream>

// Server: wrap each accepted TCP stream and complete the handshake.
pace::task<void> serve_one(pace::tcp::listener& listener, const pace::tls::context& server)
{
    auto connection = co_await pace::tls::accept(co_await listener.accept(), server);
    co_await pace::write(connection, "hello\n");
    co_await connection.shutdown();
}

// Client: verify the certificate for "api.example.com" against the system trust store.
pace::task<void> client(pace::tcp::endpoint address)
{
    pace::tls::context configuration;
    configuration.set_alpn_protocols({"http/1.1"});
    auto connection = co_await pace::tls::connect(configuration, address, "api.example.com");
    std::cout << "ALPN: " << connection.selected_protocol() << '\n';
    std::cout << co_await pace::read_exact(connection, 6);
    co_await connection.shutdown();
}
```

## Types

### Enumerations and aliases

```cpp
enum class role
{
    client,
    server
};
enum class trust_source
{
    system,
    application
};
enum class sni_policy
{
    use_default,
    reject
};
enum class pin_type
{
    leaf_certificate,
    subject_public_key_info
};
using sha256_digest = std::array<std::byte, 32>;
using verification_callback = std::function<bool(std::span<const std::string>)>;
using client_hello_callback = std::function<client_hello_action(const client_hello&)>;
```

| Name | Meaning |
|:--|:--|
| `role` | Whether a `context` configures clients or servers |
| `trust_source` | `system` loads OpenSSL's default trust paths; `application` starts with an empty trust store |
| `sni_policy` | For a server with SNI routing: `use_default` serves the default certificate, `reject` fails the handshake |
| `pin_type` | What a pin hashes: the leaf certificate's DER encoding, or its DER SubjectPublicKeyInfo |
| `sha256_digest` | 32 raw SHA-256 bytes |
| `verification_callback` | Receives the peer's DER certificates in wire order, leaf first; returns `true` to accept |
| `client_hello_callback` | Inspects a server's incoming ClientHello and decides what to do with it |

### `context`

```cpp
class context
{
public:
    explicit context(role mode = role::client, trust_source trust = trust_source::system);
    ~context();
    context(context&&) noexcept;
    context& operator=(context&&) noexcept;
    context(const context&) = delete;
    context& operator=(const context&) = delete;

    void load_verify_file(const std::string& pem_file);
    void load_verify_pem(std::string_view certificates);
    void set_verify_pin(pin_type type, sha256_digest digest);
    void set_verify_callback(verification_callback verifier);
    void insecure_skip_verify();
    void use_certificate_chain(const std::string& certificate_chain_file, const std::string& private_key_file);
    void set_alpn_protocols(const std::vector<std::string>& protocols);
    void add_server_name(std::string name, const context& configuration);
    void set_server_name_policy(sni_policy missing, sni_policy unrecognized);
    void set_client_hello_callback(client_hello_callback callback);
};
```

A TLS configuration: role, trust, verification policy, certificate, ALPN and server-name routing. It is separate
from the `io_context` reactor, and one configuration can serve streams on several reactors.

**Freezing.** A context freezes when the first stream is constructed from it, when it is registered as an SNI target,
or when a ClientHello callback selects it. After that every setter throws `std::logic_error`, even once those
streams have closed; create a new context to change the configuration. Setters and freezing are serialized
internally. Moving or destroying the `context` object must not race with other use of that object; a moved-from
context throws `std::logic_error` from its setters and cannot create streams.

**Shared ownership.** Streams share ownership of their configuration and of its whole SNI table. The `context`
object may be moved or destroyed once streams exist: certificates, trust roots and callbacks stay alive until the
last stream or context using them is gone.

#### Trust

The constructor with `trust_source::system` (the default) loads OpenSSL's default trust paths on a client context
and throws `std::system_error` if that fails. `trust_source::application` starts with no trusted roots and never
loads the default paths, so a client can be configured even where the OpenSSL defaults are unavailable. Clients
verify the certificate chain and the expected DNS name or IP address by default.

- `load_verify_file(pem_file)` adds the roots of a PEM file.
- `load_verify_pem(certificates)` adds one or more in-memory `CERTIFICATE` PEM blocks separated by whitespace. It
  parses the whole input before adding anything and copies the certificates into the context, so the input may be
  released as soon as the call returns. Empty, malformed or mixed-content input throws `std::invalid_argument` (or
  `std::system_error` when OpenSSL rejects a block); duplicates are accepted.

```cpp
pace::tls::context client(pace::tls::role::client, pace::tls::trust_source::application);
client.load_verify_pem(application_ca_bundle); // Copied roots; the chain is still verified.
```

PACE does not import the Windows certificate store or the macOS Keychain. Export the platform certificates you
want to trust to PEM and pass them in, or supply a CA file. Wrong names, untrusted roots and expired certificates are
handshake errors with either trust source.

#### Client verification modes

Each of these setters replaces CA verification with one other mode. They are for client contexts only (a server
context throws `std::invalid_argument`); use `trust_source::application` when the mode needs no system trust.

- `set_verify_pin(pin_type::leaf_certificate, digest)` accepts the peer when the SHA-256 of its leaf certificate's
  DER encoding equals `digest`; `pin_type::subject_public_key_info` hashes the leaf's DER SubjectPublicKeyInfo. A
  matching pin accepts an untrusted or self-signed leaf: the pin replaces chain and validity checks.
- `set_verify_callback(verifier)` passes the peer's unverified DER certificates, leaf first, to `verifier`, which
  decides whether to trust the chain and may implement its own CA and pin checks. Returning `false` or throwing
  fails the handshake. The certificates are borrowed for the call. The callback runs synchronously on the stream's
  reactor, and concurrently when the context is shared across reactors. An empty function throws
  `std::invalid_argument`.
- `insecure_skip_verify()` disables certificate and name verification, for tests and interoperability only: the
  connection is encrypted but the peer is not authenticated.

Pin and callback modes still check the certificate's DNS or IP identity when a nonempty peer name is passed to
`connect` or `handshake`; pass an empty peer name to rely on the pin or callback alone.

```cpp
pace::tls::context client(pace::tls::role::client, pace::tls::trust_source::application);
client.set_verify_pin(pace::tls::pin_type::subject_public_key_info, expected_spki_sha256);
pace::tls::stream connection(io, client);
co_await connection.connect({"192.0.2.1", 443}, "", "relay.example.com");
```

#### Server certificate

`use_certificate_chain(certificate_chain_file, private_key_file)` loads a PEM chain (leaf first, then
intermediates) and its unencrypted PEM private key. Loading is synchronous and checks that the key matches before
installing the pair, so a missing or mismatched key leaves an earlier valid pair in place. Encrypted keys fail
instead of prompting for a password. A server serves this certificate unless SNI routing or a ClientHello callback
selects another context. This API does not request client certificates.

#### ALPN

`set_alpn_protocols(protocols)` sets the protocols to offer (client) or accept, in preference order (server). Each
identifier is 1–255 bytes and the encoded list at most 65535 bytes, otherwise `std::invalid_argument`. The server
picks its first protocol that the client offers; an offered list without overlap fails the handshake. An empty list
disables ALPN, and a peer that sends no ALPN completes with no selected protocol. Clients must check
`selected_protocol()` (for example `== "h2"`) before speaking the protocol; ALPN does not implement it.

#### Server name routing (SNI)

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
server.set_server_name_policy(pace::tls::sni_policy::use_default, pace::tls::sni_policy::reject);
// Missing SNI uses the default; unknown SNI fails this handshake.
```

`add_server_name(name, configuration)` routes ClientHellos whose SNI equals `name` to the server context
`configuration`, which supplies the certificate chain and ALPN policy for that connection. Names match exactly,
ignoring ASCII case; use ASCII labels (A-labels for IDNs). Wildcards, IP literals, trailing dots, empty or invalid
labels, duplicate names, client contexts, the context itself and targets that have their own SNI table throw
`std::invalid_argument`. One target may serve several names or several routing contexts.

`set_server_name_policy(missing, unrecognized)` chooses what happens without SNI and with an unknown name; both start
as `use_default`. A client whose peer name is an IP address sends no SNI and falls under `missing`. Malformed SNI is
rejected. With `use_default`, the client still checks the default certificate against the name it asked for. A
selected target that cannot complete the handshake fails that connection; it never falls back to the default.
Server contexts only.

Selection runs synchronously inside the handshake on the owning reactor. It only looks up the frozen table; it never
changes a shared context or loads files during a handshake. It applies equally to implicit TLS and to TCP streams
upgraded with STARTTLS. Contexts with an SNI table disable the session cache and session tickets (including TLS 1.3
tickets), because OpenSSL's servername callback runs after session resumption and resumption would bypass the
per-connection selection.

#### ClientHello callback

`set_client_hello_callback(callback)` installs a server-side callback that runs once per connection on the first
ClientHello, before SNI and certificate selection. It runs synchronously on the reactor, and concurrently on
several reactors when the context is shared. Its `client_hello_action` result decides what happens next; throwing
aborts the handshake. An empty function or a client context throws `std::invalid_argument`. Contexts with this
callback disable session resumption and tickets, so certificate selection is made afresh on each connection.

```cpp
server.set_client_hello_callback(
    [](const pace::tls::client_hello& hello)
    {
        if (hello.server_name == "upstream.example.com")
        {
            return pace::tls::client_hello_action::handoff();
        }
        return pace::tls::client_hello_action::proceed();
    }
);
```

### `client_hello` and `client_hello_key_share`

```cpp
struct client_hello_key_share
{
    std::uint16_t group = 0;
    std::string_view key_exchange;
};

struct client_hello
{
    std::string_view server_name;
    std::vector<std::string_view> alpn_protocols;
    std::vector<std::uint16_t> supported_versions;
    std::vector<std::uint16_t> cipher_suites;
    std::vector<client_hello_key_share> key_shares;
    std::uint16_t legacy_version = 0;
    std::string_view session_id;
    std::string_view raw_message;
};
```

The parsed ClientHello passed to the callback. Lists keep wire order and wire IDs. An absent SNI, ALPN or
supported_versions extension gives an empty field. `raw_message` is the exact handshake message, including its
four-byte handshake header; it may have spanned several TLS records and excludes record headers. Every view borrows
memory that is valid only until the callback returns.

### `client_hello_action`

```cpp
class client_hello_action
{
public:
    static client_hello_action proceed() noexcept;
    static client_hello_action select(const context& configuration);
    static client_hello_action reject() noexcept;
    static client_hello_action handoff() noexcept;
};
```

| Action | Effect |
|:--|:--|
| `proceed()` | Continue with this context's normal SNI policy and certificate |
| `select(configuration)` | Use the server context `configuration` (frozen and retained) for the certificate and ALPN. Takes precedence over SNI routing; a failed handshake closes the connection |
| `reject()` | Send a fatal handshake alert |
| `handoff()` | Stop before sending anything, so `handshake_with_handoff()` can return the transport |

`select` with a client context throws `std::invalid_argument`.

### `client_hello_handoff`

```cpp
template <class Transport>
struct client_hello_handoff
{
    Transport transport;
    std::string pre_read;
};
```

The result of a handoff: the original, still-open transport, and every byte read from it so far in wire order,
including the constructor's `pre_read` prefix and any bytes read past the ClientHello. Process `pre_read` before
reading more from `transport`.

### `stream`

```cpp
class stream
{
public:
    stream(io_context& io, const tls::context& configuration);
    stream(tcp::stream transport, const tls::context& configuration);
    stream(tcp::stream transport, const tls::context& configuration, std::string_view pre_read);
    template <byte_stream Transport>
    requires(!std::same_as<Transport, tcp::stream>)
    stream(Transport transport, const tls::context& configuration, std::string_view pre_read = {});
    ~stream();
    stream(stream&&) noexcept;
    stream& operator=(stream&&) noexcept;
    stream(const stream&) = delete;
    stream& operator=(const stream&) = delete;

    task<void> connect(tcp::endpoint endpoint, std::string peer_name);
    task<void> connect(tcp::endpoint endpoint, std::string peer_name, std::string server_name);
    task<void> handshake(std::string peer_name = {});
    task<void> handshake(std::string peer_name, std::string server_name);
    template <byte_stream Transport = tcp::stream>
    task<std::optional<client_hello_handoff<Transport>>> handshake_with_handoff();

    task<std::size_t> read_some(std::span<std::byte> buffer);
    task<void> write_all(std::span<const std::byte> bytes);
    task<void> shutdown_write();

    task<void> shutdown(std::chrono::milliseconds timeout = std::chrono::milliseconds(100));
    task<void> close_gracefully(std::chrono::milliseconds timeout = std::chrono::milliseconds(100));
    void close() noexcept;
    void set_no_delay(bool enabled = true);
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] io_context& context() const noexcept;
    [[nodiscard]] std::string selected_protocol() const;
};
```

A move-only TLS session over a transport it owns. Construction freezes the configuration. A `tls::stream` is itself a
[`pace::byte_stream`](../net/README.md#byte_stream), so it can carry another TLS session or be passed to
`pace::write` and `pace::read_exact`.

#### Construction

| Constructor | Transport |
|:--|:--|
| `stream(io, configuration)` | A new, unconnected TCP socket on `io`; call `connect` |
| `stream(tcp_stream, configuration)` | An accepted or connected TCP stream, moved in; call `handshake` |
| `stream(tcp_stream, configuration, pre_read)` | As above, after TLS bytes were already read from the socket |
| `stream(transport, configuration, pre_read = {})` | Any other connected [`pace::byte_stream`](#custom-transports), moved in |

Do not use a transport for plaintext I/O after moving it in. `pre_read` is for TLS bytes that protocol detection or
STARTTLS negotiation already consumed: the stream copies them before the constructor returns and replays them, in
wire order, before reading from the transport. The prefix may hold a complete ClientHello or end anywhere inside a
record.

#### Handshakes

```cpp
task<void> connect(tcp::endpoint endpoint, std::string peer_name);
task<void> connect(tcp::endpoint endpoint, std::string peer_name, std::string server_name);
task<void> handshake(std::string peer_name = {});
task<void> handshake(std::string peer_name, std::string server_name);
```

`connect` (client, TCP-backed streams only) connects the TCP socket to the numeric `endpoint` and performs the
handshake. `handshake` performs only the handshake on an already-connected transport: a client passes the peer name,
a server passes nothing.

`peer_name` is the identity expected in the certificate, independent of the address: a DNS name is checked against
the DNS names of the certificate and is also sent as SNI; an IP address is checked against its IP addresses and
sends no SNI. CA verification requires a nonempty `peer_name`. The `server_name` overloads send a separate ASCII DNS
name as SNI, or none when `server_name` is empty. Resolve host names first with the [DNS module](../dns/README.md).

```cpp
pace::tls::context configuration; // A verifying client by default.
pace::tls::stream connection(io, configuration);
co_await connection.connect({"192.0.2.1", 443}, "api.example.com");
```

For STARTTLS, finish the plaintext negotiation on the `tcp::stream`, move it into a `tls::stream` (with any TLS
bytes already read as `pre_read`) and call `handshake(peer_name)` on the client or `handshake()` on the server:

```cpp
pace::tls::stream connection(co_await listener.accept(), server_configuration);
co_await connection.handshake();
```

The handshake suspends on transport readiness. A failed or canceled handshake closes the transport. There is no
handshake timeout of its own; use a [deadline](../runtime/structured-concurrency.md) or close the stream.

#### `handshake_with_handoff`

```cpp
template <byte_stream Transport = tcp::stream>
task<std::optional<client_hello_handoff<Transport>>> handshake_with_handoff();
```

Server handshake that honours `client_hello_action::handoff()`. It returns an empty optional when the TLS handshake
completed. On a handoff it returns the original transport and every byte read so far, and leaves this stream
moved-from. `Transport` defaults to `tcp::stream`; for any other transport name the type passed to the constructor.
On a client context, or with a different `Transport` type, it throws `std::logic_error`. On `handshake()` and
`tls::accept()`, a handoff request instead closes the transport and throws a protocol error. While a ClientHello
callback may still ask for a handoff, reads are captured up to 1 MiB; beyond that the handshake fails.

```cpp
pace::tls::stream session(co_await listener.accept(), server);
if (auto handoff = co_await session.handshake_with_handoff())
{
    // Process pre_read in wire order before reading any more from the transport.
    co_await route_to_upstream(std::move(handoff->transport), std::move(handoff->pre_read));
}
else
{
    co_await handle_https(session);
}
```

#### Reading and writing

```cpp
task<std::size_t> read_some(std::span<std::byte> buffer);
task<void> write_all(std::span<const std::byte> bytes);
```

Available after a successful handshake (`std::logic_error` before). `read_some` returns decrypted bytes, possibly
fewer than requested. With a nonempty buffer, `0` means the peer sent a verified `close_notify`; a transport EOF
without it is a protocol error, so truncated TLS traffic is never taken as a clean end. An empty buffer returns `0`
without reading. `write_all` encrypts and sends the whole span, handling partial writes, and completes once the
ciphertext has been handed to the transport. Buffers are borrowed until the operation completes. Text goes through
[`pace::as_bytes` and `pace::as_writable_bytes`](../net/README.md#as_bytes-as_writable_bytes-and-as_text), and the
generic [`pace::write` and `pace::read_exact`](../net/README.md#functions) helpers work on `tls::stream`:

```cpp
co_await connection.write_all(pace::as_bytes("GET / HTTP/1.1\r\nHost: api.example.com\r\n\r\n"));
std::array<char, 4096> buffer{};
const auto count = co_await connection.read_some(pace::as_writable_bytes(buffer));
```

#### Shutdown and close

```cpp
task<void> shutdown(std::chrono::milliseconds timeout = std::chrono::milliseconds(100));
task<void> close_gracefully(std::chrono::milliseconds timeout = std::chrono::milliseconds(100));
void close() noexcept;
```

`shutdown` sends `close_notify`, waits for the peer's `close_notify`, then closes the transport. `close_gracefully` is
the same operation under the name `tcp::stream` uses. It requires a completed handshake (`std::logic_error`
otherwise; use `close()` to abandon a connection that never completed one).

- A cancellable reactor timer bounds the operation; reaching `timeout` closes normally without promising the peer
  received anything. A TLS error or cancellation closes the transport and propagates.
- A non-positive `timeout` makes one attempt on a TCP transport and closes other transports immediately.
- On a custom transport, a `shutdown(std::chrono::milliseconds) -> task<void>` member, if present, runs before
  `close()`. A QUIC-style transport (one with `finish_write()`) sends FIN with `shutdown_write()` and waits for the
  peer's FIN. Any other transport is closed.
- No other TLS operation may be active during shutdown. The timer is removed as soon as shutdown finishes.

```cpp
task<void> shutdown_write();
```

`shutdown_write` half-closes the TLS session: it sends `close_notify` and completes once the alert has been handed to
the transport, without waiting for the peer's. The peer's `read_some` then returns `0`, while `read_some` on this side
keeps returning the peer's data until the peer's own `close_notify` arrives (and then returns `0`). Writing afterwards
fails with `std::system_error` and, like any failed operation, closes the transport. It requires a completed
handshake and no other active operation; a TLS failure throws `std::system_error` with `protocol_error` and closes the
transport. The transport stays open: after reading to the end, `close()` the stream (or let it be destroyed).

```cpp
co_await connection.write_all(pace::as_bytes(request));
co_await connection.shutdown_write(); // close_notify: the request is complete
std::string reply;
std::array<char, 4096> buffer{};
while (const auto count = co_await connection.read_some(pace::as_writable_bytes(buffer)))
{
    reply.append(buffer.data(), count);
}
connection.close();
```

`close()` closes or resets the transport immediately, cancelling waits; it sends no `close_notify` and does not wait.
Suspended operations then finish with an error, so keep the `stream` alive until they have. The destructor also
closes immediately.

#### Accessors

| Member | Result |
|:--|:--|
| `context()` | The reactor of the transport; on a moved-from stream, the reactor it belonged to |
| `is_open()` | Whether the transport is open; not whether the handshake succeeded. `false` on a moved-from stream |
| `selected_protocol()` | An owned copy of the negotiated ALPN protocol, or an empty string |
| `set_no_delay(enabled)` | Sets `TCP_NODELAY` on a TCP transport; `std::logic_error` on other transports |

After a move, the source supports only destruction, assignment, `close()` and `is_open()`; the other members throw
`std::logic_error` (and `context()` must not be called).

### Custom transports

```cpp
template <byte_stream Transport>
requires(!std::same_as<Transport, tcp::stream>)
stream(Transport transport, const tls::context& configuration, std::string_view pre_read = {});
```

`tls::stream` can run over any connected, ordered, bidirectional byte stream that satisfies
[`pace::byte_stream`](../net/README.md#byte_stream): a bidirectional `quic::stream`, another `tls::stream` (nested
TLS), or an application adapter. The stream owns the transport it is given. Beyond the concept's contract:

- `context()` returns the reactor; the transport stays on it for its whole life.
- `read_some` returns `0` only at EOF; `write_all` accepts every byte before completing.
- `close()` must cancel outstanding operations with `operation_canceled`. TLS calls it to abandon the transport.
- Optional: `is_open() const noexcept` lets TLS observe that the transport closed.
- Optional: a `shutdown(std::chrono::milliseconds) -> task<void>` member runs before `close()` when the TLS session
  shuts down; a transport with `finish_write()` (QUIC) gets `shutdown_write()` and a drain to the peer's FIN instead.
- During shutdown TLS may run one transport read and one transport write at the same time to exchange alerts when
  buffers are full. An adapter that serializes I/O declares `bool supports_concurrent_io() const noexcept`
  returning `false`.

```cpp
auto bytes = co_await quic_connection.open_stream(pace::quic::stream_type::bidirectional);
pace::tls::stream inner(std::move(bytes), tls_configuration);
co_await inner.handshake("relay.example.com");
```

Keep a QUIC transport's parent connection alive for the whole life of the TLS stream. Ciphertext moves through
bounded 64 KiB buffers. Servers on such transports send no TLS 1.3 session tickets, so a bounded transport can carry
application data right after the handshake. `connect()` and `set_no_delay()` need a TCP transport, and so does
PACE's HTTP/2 driver over TLS. In nested TLS each session processes records one at a time.

`tls_transport` and `tls_transport_model` in the header are the type erasure behind this constructor; they are not
part of the API and are not exported by `import pace.tls;`.

## Functions

### `connect`

```cpp
task<stream> connect(io_context& io, const context& configuration, tcp::endpoint peer, std::string expected_name);
task<stream> connect(const context& configuration, tcp::endpoint peer, std::string expected_name);
task<stream> connect(io_context& io, tcp::endpoint peer, std::string expected_name);
task<stream> connect(tcp::endpoint peer, std::string expected_name);
```

Creates a stream, connects to the numeric `peer`, completes the handshake checking `expected_name` (as `peer_name`
above) and returns the stream. The overloads without `configuration` use a client context with system trust,
CA and name verification and no ALPN. A supplied `configuration` is borrowed until the task completes, so a stored
lazy task must not refer to a destroyed context; the returned stream shares the configuration's state. The
overloads without `io` use the context that runs the task, read when the task starts.

### `accept`

```cpp
task<stream> accept(tcp::stream transport, const context& configuration);
```

Wraps an accepted TCP stream (owned by the task) in a server stream and completes the handshake. `configuration` is
borrowed until the task completes.

## Errors

| Condition | Exception |
|:--|:--|
| Handshake failure, certificate or name mismatch, unclean EOF, OpenSSL error, a write after `shutdown_write` | `std::system_error` with `std::errc::protocol_error` and OpenSSL's diagnostic text |
| Failure to load default trust paths, a trust file or a certificate | `std::system_error` (`protocol_error`) |
| TCP connect failure in `connect` | The TCP `std::system_error`, unchanged |
| Invalid argument: empty path, bad PEM, bad ALPN list, invalid SNI name, empty callback, CA client without a peer name, a peer name passed to a server handshake | `std::invalid_argument` |
| Verification setter on a server context; SNI or ClientHello setter on a client context | `std::invalid_argument` |
| Setter on a frozen or moved-from context | `std::logic_error` |
| A second operation while one is active | `std::logic_error`; the active operation is not affected |
| Read, write, `shutdown_write` or shutdown before the handshake; a second handshake; `connect` on a non-TCP stream or with a server context; `set_no_delay` on a non-TCP stream; handoff misuse; moved-from stream | `std::logic_error` |
| Operation run on a context other than the transport's | `std::logic_error` |
| Context stopped, stream closed, task canceled | `std::system_error` (`operation_canceled`) |
| Deadline of the awaiting task expired | `std::system_error` (`timed_out`) |
| Operation on a closed transport | `std::system_error` (`bad_file_descriptor`) |

A failed operation closes the transport, except a rejected overlapping operation. Catch per-connection failures in
the connection's task so one peer cannot end the server.

## Threads and concurrency

- Run every stream operation on the reactor that owns the transport. `close()` and destruction must run there too,
  or while it is not running.
- **One asynchronous operation at a time per stream**, counting reads, writes, handshakes, `shutdown_write` and shutdown together.
  OpenSSL may need either direction for any operation, so the TCP rule of one read plus one write does not apply;
  independent reader and writer tasks must not share one TLS stream.
- A `context` may be used by streams on several reactors at once. Its verification and ClientHello callbacks then
  run concurrently and must be thread-safe. Configure it on one thread before the first stream.

## Lifetime and ownership

- A stream owns its transport and shares ownership of its configuration. The `context` object itself may be
  destroyed after the streams are created, except while a `tls::connect` or `tls::accept` task that borrows it is
  still running.
- Keep the stream at the same address, its reactor alive, and its buffers valid until each operation completes.
  Move it only while idle.
- `pre_read` is copied by the constructor. ClientHello views and verification-callback certificates are valid only
  during the callback.
- A handoff transfers the transport and its bytes to the caller and leaves the stream moved-from.

## Limits and defaults

| Item | Default or bound |
|:--|:--|
| `context` role / trust source | `client` / `system` |
| Client verification | CA chain plus DNS or IP name |
| Protocol versions | TLS 1.2 and 1.3 |
| SNI policies | `use_default` for missing and unrecognized |
| ALPN identifier length / encoded list | 1–255 bytes / at most 65535 bytes |
| SNI name length | at most 253 bytes, labels at most 63 |
| `shutdown` / `close_gracefully` timeout | 100 ms |
| Handoff capture of reads before the ClientHello decision | 1 MiB |
| Ciphertext buffers on non-TCP transports | 64 KiB per direction |
| Active operations per stream | 1 |
| Handshake timeout | none; use a deadline |

## See also

- [Networking](../net/README.md), [TCP](../net/tcp.md), [DNS](../dns/README.md)
- [QUIC](../quic/README.md), [WebSocket](../websocket/README.md), [HTTP/1](../http1/README.md),
  [HTTP/2](../http2/README.md)
- [Structured concurrency](../runtime/structured-concurrency.md) for deadlines
- Tutorial: [TLS and DNS](../../tutorial/10-tls-and-dns.md)
- Guide: [HTTP recipes](../../guides/http-recipes.md)
- OpenSSL: [SNI callback contract](https://docs.openssl.org/3.0/man3/SSL_CTX_set_tlsext_servername_callback/)
