#pragma once

#include <pace/http/types.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace pace::http
{

/// Bounds of a response_cache. Invalid values throw std::invalid_argument from the cache's constructor.
struct cache_options
{
    /// Budget for stored entries (body, key and fields); 0 disables the cache. Not a process memory limit.
    std::size_t max_bytes = 0;
    /// Number of entries; at least 1.
    std::size_t max_entries = 1024;
    /// Largest body stored.
    std::size_t max_object_bytes = std::size_t{1024} * 1024;
    /// Longest lifetime of an entry; not negative.
    std::chrono::seconds ttl{60};
    /// Independently locked shards, 1 to 256; budgets are divided equally among them.
    std::size_t shards = 16;
};

/// Counters of a response_cache, summed shard by shard (not one atomic snapshot).
struct cache_statistics
{
    /// Hits, misses and evictions since construction; clear() keeps them.
    std::uint64_t hits = 0, misses = 0, evictions = 0;
    /// Current entries and accounted bytes.
    std::size_t entries = 0, bytes = 0;
};

/// Thread-safe, bounded, sharded CLOCK cache of immutable responses by key. Stores whatever it is given; HTTP
/// caching rules belong to its users. Snapshots already returned stay valid after eviction or clear().
class response_cache
{
public:
    /// Creates the cache. Throws std::invalid_argument for zero or more than 256 shards, zero entries or negative ttl.
    explicit response_cache(cache_options options = {});
    /// Destroys the cache; snapshots held elsewhere stay valid.
    ~response_cache();
    response_cache(const response_cache&) = delete;
    response_cache& operator=(const response_cache&) = delete;
    /// The stored snapshot, or null when missing or expired. Writes the time since insertion to *resident_age.
    std::shared_ptr<const response> get(std::string_view key, std::chrono::seconds* resident_age = nullptr);
    /// An owned copy, or empty. On a running context, remembers the last snapshot per cache and context so repeated
    /// hits skip the shard lock; that snapshot may outlive its eviction until the next lookup there or the
    /// context's destruction. Outside a context it copies get().
    std::optional<response> get_copy(std::string_view key, std::chrono::seconds* resident_age = nullptr);
    /// Stores `value` for min(ttl, options.ttl), replacing the same key and evicting as needed. Ignored when the
    /// cache is disabled, ttl is not positive, or the body or entry is too large.
    void put(std::string key, response value, std::chrono::seconds ttl);
    /// Removes every entry; statistics are kept. A racing miss may store its result again.
    void clear();
    /// Current counters.
    [[nodiscard]] cache_statistics statistics() const;

private:
    struct implementation;
    std::shared_ptr<implementation> impl_;
};

/// Options of static_files.
struct static_options
{
    /// File served for a directory: one file name; empty means directories get 404.
    std::string index = "index.html";
    /// Largest file served; larger files get 413.
    std::size_t max_file_bytes = std::size_t{8} * 1024 * 1024;
    /// Serve path segments that start with '.'.
    bool allow_dotfiles = false;
    /// Cache-Control sent to clients; empty sends none. Independent of the local cache.
    std::string cache_control = "no-cache";
    /// Local cache of file responses; disabled while cache.max_bytes is 0.
    cache_options cache{};
};

/// Serves regular files under a directory for GET and HEAD, with index files, MIME types, ETag/Last-Modified
/// validators and single byte ranges. Files are read synchronously on the serving context and buffered. Symbolic
/// links and reparse points are never followed. Copies share one service and its cache; usable as a handler.
class static_files
{
public:
    /// Serves `directory` for paths under `prefix`. Throws std::invalid_argument for an invalid prefix, index or
    /// cache_control and std::system_error when the directory cannot be opened.
    static_files(std::string prefix, std::filesystem::path directory, static_options options = {});
    /// Answers one request: 405 for other methods, 400 for unsafe paths, 404, 413, 304, 206, 416 or 200.
    task<response> operator()(request incoming) const;
    /// Empties the local cache; thread-safe.
    void clear_cache() const;
    /// The local cache's counters; thread-safe.
    cache_statistics cache_stats() const;

private:
    struct implementation;
    std::shared_ptr<implementation> impl_;
};

/// How reverse_proxy picks an upstream, per worker context.
enum class load_balance
{
    /// The next upstream that is neither cooling down nor saturated.
    round_robin,
    /// The eligible upstream with the fewest active exchanges, rotating among ties.
    least_connections
};

/// Options of reverse_proxy. Invalid values throw std::invalid_argument from its constructor.
struct proxy_options
{
    /// Upstream selection.
    load_balance balance = load_balance::round_robin;
    /// Remove the mount prefix from the target before forwarding.
    bool strip_prefix = true;
    /// Concurrent exchanges per upstream per worker context; at least 1. Beyond it the upstream is skipped.
    std::size_t max_connections = 64;
    /// Idle keep-alive connections kept per upstream per worker context; at most max_connections.
    std::size_t max_idle_connections = 16;
    /// Largest upstream response body.
    std::size_t max_response_bytes = std::size_t{8} * 1024 * 1024;
    /// One positive deadline for DNS, connect, TLS and the exchange; expiry gives 504.
    std::chrono::milliseconds timeout{30000};
    /// Idle connections older than this are discarded instead of reused.
    std::chrono::seconds idle_timeout{30};
    /// Cooldown of an upstream after a transport failure, timeout or malformed response.
    std::chrono::milliseconds fail_timeout{1000};
    /// Shared cache; only public or s-maxage GET 200 responses without Vary or cookies are stored.
    cache_options cache{};
};

/// Forwards requests under a prefix to fixed HTTP/1.1 upstreams: keeps method, body and query, replaces Host and
/// removes hop-by-hop and forwarding fields. Nothing is queued, retried or replayed; CONNECT and Upgrade get 501.
/// Connection pools and balancing state belong to each worker context; the cache is shared. Copies share one
/// service; usable as a handler.
class reverse_proxy
{
public:
    /// Throws std::invalid_argument for an invalid prefix or options, no upstream, or an upstream URL that is
    /// invalid, has a query or fragment, or needs TLS or DNS support the build lacks.
    reverse_proxy(std::string prefix, std::vector<std::string> upstreams, proxy_options options = {});
    /// Forwards one request. No eligible upstream gives 503, a transport or response failure 502, a timeout 504.
    task<response> operator()(request incoming) const;
    /// Empties the proxy cache; thread-safe.
    void clear_cache() const;
    /// The proxy cache's counters; thread-safe.
    cache_statistics cache_stats() const;

private:
    struct implementation;
    std::shared_ptr<implementation> impl_;
};
} // namespace pace::http
