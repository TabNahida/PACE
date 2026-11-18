#pragma once

#include <pace/http_types.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace pace::http {

struct cache_options {
    std::size_t max_bytes = 0; // Disabled by default. Accounted entry budget; slot storage is bounded by max_entries.
    std::size_t max_entries = 1024;
    std::size_t max_object_bytes = 1024 * 1024;
    std::chrono::seconds ttl{60};
    std::size_t shards = 16;
};
struct cache_statistics {
    std::uint64_t hits = 0, misses = 0, evictions = 0;
    std::size_t entries = 0, bytes = 0;
};
// Bounded, sharded CLOCK cache. Values are immutable; readers own their snapshot.
// clear() invalidates stored entries; snapshots already being sent remain valid.
class response_cache {
public:
    explicit response_cache(cache_options options = {});
    ~response_cache();
    response_cache(const response_cache&) = delete;
    std::shared_ptr<const response> get(std::string_view key, std::chrono::seconds* resident_age = nullptr);
    // Owned copy. On a running reactor, memoizes one immutable snapshot per
    // cache/context: repeated hits avoid the shared mutex and refcount writes.
    // Outside a reactor this uses get(). Each reactor may pin one max_object_bytes
    // snapshot after eviction; it is released on replacement/context destruction.
    std::optional<response> get_copy(std::string_view key, std::chrono::seconds* resident_age = nullptr);
    void put(std::string key, response value, std::chrono::seconds ttl);
    void clear();
    [[nodiscard]] cache_statistics statistics() const;
private:
    struct implementation;
    std::shared_ptr<implementation> impl_;
};

struct static_options {
    std::string index = "index.html";
    std::size_t max_file_bytes = 8 * 1024 * 1024;
    bool allow_dotfiles = false;
    std::string cache_control = "no-cache";
    cache_options cache{};
};
// GET/HEAD, index files, MIME types, validators and single byte ranges.
// Files are bounded and buffered. Disk reads run on the caller's reactor.
// Symlinks/reparse points are rejected, including intermediate directories.
class static_files {
public:
    static_files(std::string prefix, std::filesystem::path directory, static_options options = {});
    task<response> operator()(request incoming) const;
    void clear_cache() const;
    cache_statistics cache_stats() const;
private:
    struct implementation;
    std::shared_ptr<implementation> impl_;
};

enum class load_balance { round_robin, least_connections };
struct proxy_options {
    load_balance balance = load_balance::round_robin;
    bool strip_prefix = true;
    std::size_t max_connections = 64; // Per upstream, per reactor; excess returns 503.
    std::size_t max_idle_connections = 16;
    std::size_t max_response_bytes = 8 * 1024 * 1024;
    std::chrono::milliseconds timeout{30000};
    std::chrono::seconds idle_timeout{30};
    std::chrono::milliseconds fail_timeout{1000};
    cache_options cache{}; // Only public, unvarying GET 200 responses are stored.
};
// Fixed upstreams; preserve method/body/query, replace Host, remove hop-by-hop
// and untrusted forwarding headers. No automatic replay, upgrades or CONNECT.
// Connections and balancing state belong to each reactor; cache is shared.
class reverse_proxy {
public:
    reverse_proxy(std::string prefix, std::vector<std::string> upstreams, proxy_options options = {});
    task<response> operator()(request incoming) const;
    void clear_cache() const;
    cache_statistics cache_stats() const;
private:
    struct implementation;
    std::shared_ptr<implementation> impl_;
};
} // namespace pace::http
