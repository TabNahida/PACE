# DNS resolver

Short forms and owning helpers: [component conveniences](component-conveniences.md).

Enable `--dns=y`, depend on `pace_dns` (or the aggregate `pace`), and include
`<pace/dns.hpp>`. DNS is a Layer 2 extension backed by c-ares. The core TCP/UDP
library has no resolver dependency.

```cpp
pace::task<void> lookup(pace::io_context& io) {
    pace::dns::resolver resolver(io);
    const auto answer = co_await resolver.resolve("example.com");
    for (const auto& address : answer.addresses)
        std::cout << address.text << '\n';
    io.stop();
}
```

`resolve(std::string, family = family::any)` owns its hostname and returns a
`result` with `canonical_name` and deduplicated `addresses`. Each address has a
`family` (`ipv4` or `ipv6`) and numeric `text`. `any` queries A and AAAA. Numeric
addresses and hosts-file results can complete without DNS packets. Resolving an
AAAA record can be used with PACE's numeric IPv6 TCP/UDP endpoints. Address
selection and IPv4/IPv6 connection racing remain application responsibilities.

`options` selects the whole-lookup `timeout` (default 5 seconds), c-ares `tries`
(default 2), and optional comma-separated numeric DNS `servers`, including ports
such as `127.0.0.1:5353` and `[::1]:5353`. An empty server list follows system
configuration. Search-domain expansion is disabled; pass the exact hostname.
The implementation delegates DNS parsing, CNAME handling and UDP/TCP fallback
to c-ares. Network waits and retries run on PACE readiness/timers; there is no
blocking resolver worker. Loading local resolver/hosts configuration is
synchronous at lookup startup.

One resolver permits one active lookup. Use separate resolver instances for
concurrent requests. `resolve()` must execute on its owning context. `cancel()`
and destruction must occur there, or while the context is not running. They
cancel only that resolver's active lookup. Cancellation, resolver destruction
before a lazy task starts, and context shutdown report `operation_canceled`.
Whole-lookup expiry reports `timed_out`; other DNS failures preserve their c-ares
status in `pace::dns::error_category()`. A failed or canceled resolver is reusable
until destroyed. The context must outlive all lookup tasks.

The DNS adapter currently uses a bounded coroutine timer. Completed lookup state
may remain owned by that timer for up to 100 ms; it holds no active DNS sockets
after completion. This is a resource-lifetime bound, not polling for DNS packets.

`xmake run tutorial_dns` uses numeric loopback and is deterministic offline.
`xmake run tutorial_dns example.com` uses the configured DNS servers. The DNS
test suite runs its own local nameserver for A/AAAA/CNAME, UDP-to-TCP fallback, NXDOMAIN, refusal,
malformed replies, deadlines, cancellation, destruction and resolver reuse.

The adapter follows the [c-ares event-loop integration contract](https://c-ares.org/docs.html)
and [address lookup API](https://c-ares.org/docs/ares_getaddrinfo.html).
