#pragma once
#include <pace/io_context.hpp>
#include <pace/task.hpp>
#include <pace/this_context.hpp>
#ifdef PACE_HAS_DNS
#include <chrono>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace pace::dns {
enum class family { any, ipv4, ipv6 };
struct address {
    family type = family::ipv4;
    std::string text;
};
struct result {
    std::string canonical_name;
    std::vector<address> addresses;
};
struct options {
    std::chrono::milliseconds timeout{5000}; // Whole lookup, including retries.
    unsigned tries = 2;
    // Empty uses system configuration. Numeric servers, e.g. "127.0.0.1:5353"
    // or "[::1]:5353", separated by commas; never hostnames.
    std::string servers;
};
// DNS failures retain c-ares status codes in this error category. Cancellation
// and whole-lookup expiry use generic operation_canceled and timed_out instead.
const std::error_category& error_category() noexcept;

// One active lookup per resolver; use independent instances for concurrency.
// Starting a lookup reads system resolver/hosts configuration synchronously. Network
// DNS and retries use io_context readiness/timers, without a resolver thread.
// resolve() owns its hostname and shared operation state, including across
// resolver destruction. cancel()/destruction must execute on the owning context
// (or while it is not running); the context must outlive all submitted tasks.
class resolver {
public:
    explicit resolver(io_context& io, options configuration = {});
    ~resolver();
    resolver(const resolver&) = delete;
    resolver& operator=(const resolver&) = delete;
    task<result> resolve(std::string hostname, family requested = family::any);
    void cancel() noexcept;
    io_context& context() const noexcept;
private:
    struct implementation;
    std::shared_ptr<implementation> impl_;
    static task<result> resolve_impl(std::shared_ptr<implementation> state,
                                    std::string hostname, family requested);
};
// One-shot resolver: hostname, configuration and resolver are owned by the task.
inline task<result> resolve(io_context& io, std::string hostname,
                            family requested = family::any, options configuration = {}) {
    resolver lookup(io, std::move(configuration));
    co_return co_await lookup.resolve(std::move(hostname), requested);
}
inline task<result> resolve(std::string hostname, family requested = family::any, options configuration = {}) {
    co_return co_await resolve(this_context(), std::move(hostname), requested, std::move(configuration));
}
} // namespace pace::dns
#endif
