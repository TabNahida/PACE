#pragma once
#include <pace/runtime/io_context.hpp>
#include <pace/runtime/task.hpp>
#include <pace/runtime/this_context.hpp>
#ifdef PACE_HAS_DNS
#include <chrono>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace pace::dns
{
/// Address family requested from, or reported by, a lookup.
enum class family
{
    any,  ///< A and AAAA records (request only).
    ipv4, ///< A records.
    ipv6  ///< AAAA records.
};

/// One resolved address.
struct address
{
    /// ipv4 or ipv6, never any.
    family type = family::ipv4;
    /// Numeric address text, usable in tcp::endpoint and udp::endpoint.
    std::string text;
};

/// The result of a successful lookup: at least one address, deduplicated, in c-ares order.
struct result
{
    /// Canonical name after CNAMEs.
    std::string canonical_name;
    /// Resolved addresses.
    std::vector<address> addresses;
};

/// Resolver configuration; invalid values throw std::invalid_argument from the resolver constructor.
struct options
{
    /// Whole lookup, including retries; (0, 24 h].
    std::chrono::milliseconds timeout{5000};
    /// Attempts per name server, 1..10; each waits at most min(1 s, timeout).
    unsigned tries = 2;
    /// Empty uses system configuration. Numeric servers, e.g. "127.0.0.1:5353"
    /// or "[::1]:5353", separated by commas; never hostnames.
    std::string servers;
};

/// DNS failures retain c-ares status codes in this error category. Cancellation
/// and whole-lookup expiry use generic operation_canceled and timed_out instead.
const std::error_category& error_category() noexcept;

/// One active lookup per resolver; use independent instances for concurrency.
/// Starting a lookup reads system resolver/hosts configuration synchronously. Network
/// DNS and retries use io_context readiness/timers, without a resolver thread.
/// resolve() owns its hostname and shared operation state, including across
/// resolver destruction. cancel()/destruction must execute on the owning context
/// (or while it is not running); the context must outlive all submitted tasks.
class resolver
{
public:
    /// Binds the resolver to `io`. Throws std::invalid_argument for invalid options.
    explicit resolver(io_context& io, options configuration = {});
    /// Cancels the active lookup, if any.
    ~resolver();
    resolver(const resolver&) = delete;
    resolver& operator=(const resolver&) = delete;
    /// Resolves `hostname` (1..253 bytes, exact name, no search domains) on the owning context.
    /// Throws std::system_error in error_category() for DNS failures, operation_canceled or timed_out, and
    /// std::logic_error for a second concurrent lookup or a call on another context.
    task<result> resolve(std::string hostname, family requested = family::any);
    /// Cancels the active lookup, which then throws operation_canceled. Run on the owning context.
    void cancel() noexcept;
    /// The owning context.
    io_context& context() const noexcept;

private:
    struct implementation;
    std::shared_ptr<implementation> impl_;
    static task<result> resolve_impl(std::shared_ptr<implementation> state, std::string hostname, family requested);
};

/// One-shot resolver: hostname, configuration and resolver are owned by the task.
inline task<result> resolve(
    io_context& io,
    std::string hostname,
    family requested = family::any,
    options configuration = {}
)
{
    resolver lookup(io, std::move(configuration));
    co_return co_await lookup.resolve(std::move(hostname), requested);
}

/// As above, on the context that runs the task (read when the task starts).
inline task<result> resolve(std::string hostname, family requested = family::any, options configuration = {})
{
    co_return co_await resolve(this_context(), std::move(hostname), requested, std::move(configuration));
}
} // namespace pace::dns
#endif
