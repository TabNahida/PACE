# DNS (`pace/dns.hpp`)

[简体中文](README.zh.md) · [Reference](../README.md)

## Overview

The DNS module resolves host names to numeric IPv4 and IPv6 addresses asynchronously on the PACE reactor, using
[c-ares](https://c-ares.org/) without a resolver thread.

| Header | Target | Build option |
|:--|:--|:--|
| `<pace/dns.hpp>` | `pace_dns` | `--dns=y` |

`<pace/dns.hpp>` includes `<pace/dns/resolver.hpp>`. The build option defines `PACE_HAS_DNS` and links c-ares;
without it the header declares nothing, and the TCP and UDP modules never depend on a resolver. With `--modules=y`
the names are exported by `import pace.dns;`. The checked-in example is `examples/tutorial_dns.cpp`:
`xmake run tutorial_dns` resolves `127.0.0.1` without network access, `xmake run tutorial_dns example.com` uses the
configured name servers.

## Synopsis

```cpp
#include <pace/dns.hpp>
#include <pace/net.hpp>
#include <pace/runtime.hpp>

#include <iostream>

pace::task<void> lookup(pace::io_context& io)
{
    const auto answer = co_await pace::dns::resolve(io, "localhost", pace::dns::family::any);
    for (const auto& address : answer.addresses)
    {
        std::cout << (address.type == pace::dns::family::ipv4 ? "IPv4 " : "IPv6 ") << address.text << '\n';
    }
    if (!answer.addresses.empty())
    {
        pace::tcp::endpoint server{answer.addresses.front().text, 80};
        std::cout << "would connect to " << server.address << '\n';
    }
}

int main()
{
    pace::run(lookup);
}
```

## Types

### `family`

```cpp
enum class family
{
    any,
    ipv4,
    ipv6
};
```

The requested address family: `any` asks for A and AAAA records, `ipv4` for A only, `ipv6` for AAAA only. In a
result, each address is `ipv4` or `ipv6`, never `any`.

### `address`

```cpp
struct address
{
    family type = family::ipv4;
    std::string text;
};
```

One resolved address: its family in `type` and its numeric text in `text`, ready for a
[`tcp::endpoint`](../net/tcp.md) or [`udp::endpoint`](../net/udp.md).

### `result`

```cpp
struct result
{
    std::string canonical_name;
    std::vector<address> addresses;
};
```

`canonical_name` is the canonical name reported by the lookup (after CNAMEs) and `addresses` the resolved
addresses, without duplicates, in the order c-ares returned them (no RFC 6724 sorting). A successful lookup always
has at least one address. Choosing an address, trying several, and racing IPv4 against IPv6 are up to the
application.

### `options`

```cpp
struct options
{
    std::chrono::milliseconds timeout{5000};
    unsigned tries = 2;
    std::string servers;
};
```

| Member | Meaning |
|:--|:--|
| `timeout` | Bound on the whole lookup, including retries. Must be positive and at most 24 hours |
| `tries` | Attempts per name server (c-ares `tries`), 1 to 10. Each attempt waits at most 1 s (less if `timeout` is shorter) |
| `servers` | Empty uses the system configuration. Otherwise numeric servers separated by commas, with optional ports, such as `127.0.0.1:5353,[::1]:5353`; never host names |

Invalid options throw `std::invalid_argument` from the `resolver` constructor. Search-domain expansion is disabled:
pass the exact name to resolve. The hosts file is still consulted.

### `resolver`

```cpp
class resolver
{
public:
    explicit resolver(io_context& io, options configuration = {});
    ~resolver();
    resolver(const resolver&) = delete;
    resolver& operator=(const resolver&) = delete;
    task<result> resolve(std::string hostname, family requested = family::any);
    void cancel() noexcept;
    io_context& context() const noexcept;
};
```

A resolver bound to one `io_context`, running one lookup at a time. It is neither copyable nor movable.

- `resolve(hostname, requested)` looks up `hostname` and returns its addresses. The task owns the name. Numeric
  addresses and hosts-file entries complete without sending DNS packets. Starting a lookup reads the system resolver
  and hosts configuration synchronously; network waits and retries then run on the reactor's readiness and timers.
  DNS parsing, CNAME chains and the UDP-to-TCP fallback are c-ares's. The hostname must be 1 to 253 bytes without
  spaces or control characters (`std::invalid_argument`).
- `cancel()` cancels the active lookup, if any, which then throws `operation_canceled`. Lookups not yet started are
  not affected.
- `context()` returns the owning context.

A resolver whose lookup failed, timed out or was canceled can be used for the next lookup.

## Functions

### `resolve`

```cpp
task<result> resolve(io_context& io, std::string hostname, family requested = family::any, options configuration = {});
task<result> resolve(std::string hostname, family requested = family::any, options configuration = {});
```

One-shot lookup: the task owns a `resolver`, the name and the options until it completes, so it can run
concurrently with other `resolve` calls. The overload without `io` uses the context that runs the task, read when
the task starts; outside a running context, pass it to `pace::run`, for example
`pace::run(pace::dns::resolve("127.0.0.1"))`. Use an explicit `resolver` when you need `cancel()`. `resolve` does not
pick a port, connect, or try several addresses.

### `error_category`

```cpp
const std::error_category& error_category() noexcept;
```

The category (`name()` is `"pace.dns"`) of DNS failures. Its values are c-ares status codes (`ARES_ENOTFOUND`,
`ARES_ENODATA`, `ARES_ESERVFAIL`, `ARES_EREFUSED`, …) and `message()` is c-ares's description.

## Errors

| Condition | Exception |
|:--|:--|
| Invalid `options` | `std::invalid_argument` from the `resolver` constructor |
| Empty, too long or malformed hostname, invalid `family` value | `std::invalid_argument` |
| `resolve` while another lookup on the same resolver is active | `std::logic_error` |
| `resolve` run on a context other than the resolver's | `std::logic_error` |
| Name not found, no address of the requested family, server failure, refusal, malformed reply | `std::system_error` in `dns::error_category()` with the c-ares status |
| `cancel()`, resolver destroyed, context stopped, or the awaiting task canceled | `std::system_error` (`std::errc::operation_canceled`) |
| `options::timeout` or the awaiting task's deadline expired | `std::system_error` (`std::errc::timed_out`) |

A name that exists but has no address of the requested family reports `ARES_ENODATA`.

## Threads and concurrency

- `resolve` must run on the resolver's context. `cancel()` and the destructor must run on that context, or while it
  is not running.
- A resolver allows one active lookup. For concurrent lookups use separate `resolver` objects or the free
  `resolve` functions, each of which owns its resolver.

## Lifetime and ownership

- The `io_context` must outlive every lookup task, including tasks of a destroyed resolver.
- `resolve` owns its hostname and its lookup state; that state is shared with the task and survives the destruction
  of the `resolver`. Destroying the resolver cancels an active lookup.
- After a lookup finishes, its internal timer coroutine may keep the completed lookup state for up to 100 ms. It holds
  no DNS socket during that time.

## Limits and defaults

| Item | Default or bound |
|:--|:--|
| `options::timeout` | 5 s; allowed (0, 24 h] |
| `options::tries` | 2; allowed 1–10 |
| Wait per attempt | min(1 s, `timeout`) |
| `options::servers` | empty (system configuration) |
| Hostname length | 1–253 bytes |
| Active lookups per `resolver` | 1 |
| Search-domain expansion | disabled |
| Completed state retained by the lookup timer | at most 100 ms |

## See also

- [TCP](../net/tcp.md), [UDP](../net/udp.md) for connecting to the resolved addresses
- [TLS](../tls/README.md): the peer name is passed separately from the numeric address
- [HTTP clients](../http/clients.md), which resolve URL host names with this module
- [Structured concurrency](../runtime/structured-concurrency.md) for deadlines
- Tutorial: [TLS and DNS](../../tutorial/10-tls-and-dns.md)
- c-ares: [event-loop integration](https://c-ares.org/docs.html), [`ares_getaddrinfo`](https://c-ares.org/docs/ares_getaddrinfo.html)
