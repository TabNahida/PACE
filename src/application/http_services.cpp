#include <pace/http.hpp>
#include <pace/detail/http_url.hpp>
#include <pace/operation.hpp>
#include <pace/detail/content_hash.hpp>
#include <atomic>
#include <cstdio>
#include <unordered_set>

#include <algorithm>
#include <array>
#include <charconv>
#include <ctime>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_map>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pace::http {
namespace {
using clock_type = std::chrono::steady_clock;
std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return out;
}
bool equal_ci(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        auto fold = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; };
        if (fold(a[i]) != fold(b[i])) return false;
    }
    return true;
}
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}
std::vector<std::string> tokens(std::string_view s) {
    std::vector<std::string> result;
    while (!s.empty()) {
        const auto comma = s.find(',');
        result.push_back(lower(trim(s.substr(0, comma))));
        if (comma == s.npos) break;
        s.remove_prefix(comma + 1);
    }
    return result;
}
std::string prefix_path(std::string p) {
    if (p.empty() || p.front() != '/' || p.find_first_of("?#\\%") != p.npos)
        throw std::invalid_argument("invalid service prefix");
    for (unsigned char c : p) if (c <= 32 || c == 127) throw std::invalid_argument("invalid service prefix");
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}
bool matches(std::string_view path, std::string_view prefix) {
    return path.starts_with(prefix) && (prefix == "/" || path.size() == prefix.size() || path[prefix.size()] == '/');
}
std::string http_date(std::time_t value) {
    std::tm tm{};
#ifdef _WIN32
    if (gmtime_s(&tm, &value) != 0) throw std::runtime_error("invalid file timestamp");
#else
    if (!gmtime_r(&value, &tm)) throw std::runtime_error("invalid file timestamp");
#endif
    constexpr const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    constexpr const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    std::array<char, 64> text{};
    const auto n = std::snprintf(text.data(), text.size(), "%s, %02d %s %04d %02d:%02d:%02d GMT",
        days[tm.tm_wday], tm.tm_mday, months[tm.tm_mon], tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
    if (n < 0 || static_cast<std::size_t>(n) >= text.size()) throw std::runtime_error("invalid file timestamp");
    return {text.data(), static_cast<std::size_t>(n)};
}
std::string mime(std::string_view name) {
    auto ext = lower(name.substr(name.find_last_of('.') == name.npos ? name.size() : name.find_last_of('.')));
    for (const auto& [suffix, type] : std::array<std::pair<std::string_view, std::string_view>, 15>{{
        {".html", "text/html; charset=utf-8"}, {".htm", "text/html; charset=utf-8"},
        {".css", "text/css; charset=utf-8"}, {".js", "text/javascript; charset=utf-8"},
        {".json", "application/json"}, {".txt", "text/plain; charset=utf-8"},
        {".svg", "image/svg+xml"}, {".png", "image/png"}, {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"}, {".gif", "image/gif"}, {".ico", "image/x-icon"},
        {".wasm", "application/wasm"}, {".woff2", "font/woff2"}, {".pdf", "application/pdf"}}})
        if (ext == suffix) return std::string(type);
    return "application/octet-stream";
}
struct string_hash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};
std::size_t cache_cost(std::string_view key, const response& value) {
    std::size_t bytes = sizeof(response) + 192 + key.size() * 2 + value.body.capacity() + value.content_type.capacity() + value.version.capacity();
    for (const auto& f : value.headers) bytes += sizeof(f) + f.name.capacity() + f.value.capacity();
    for (const auto& f : value.trailers) bytes += sizeof(f) + f.name.capacity() + f.value.capacity();
    return bytes;
}
}

struct response_cache::implementation {
    struct slot {
        std::string key;
        std::shared_ptr<const response> value;
        clock_type::time_point expiry{}, inserted{};
        std::size_t cost = 0;
        bool referenced = false;
    };
    struct alignas(128) shard {
        // Invalidation is written only on mutation, not on a cache hit. Keep it
        // away from the mutex/stats modified by shared-path readers.
        alignas(128) std::atomic<std::uint64_t> generation{0};
        alignas(128) mutable std::mutex mutex;
        std::vector<slot> slots;
        std::unordered_map<std::string, std::size_t, string_hash, std::equal_to<>> index;
        std::size_t hand = 0, budget = 0;
        cache_statistics stats;
        void invalidate() noexcept {
            const auto value = generation.load(std::memory_order_relaxed);
            // Saturation permanently disables memoization, preventing ABA.
            if (value != UINT64_MAX) generation.store(value + 1, std::memory_order_release);
        }
        void erase(std::size_t n) {
            auto& s = slots[n];
            if (!s.value) return;
            index.erase(s.key); stats.bytes -= s.cost; --stats.entries;
            s = {};
            invalidate();
        }
    };
    struct reader {
        std::shared_ptr<implementation> owner;
        std::string key;
        std::shared_ptr<const response> value;
        shard* source = nullptr;
        std::uint64_t generation = UINT64_MAX;
        clock_type::time_point expiry{}, inserted{};
        // Only this reactor writes this counter. statistics() may sample it.
        alignas(128) std::atomic<std::uint64_t> hits{0};
        explicit reader(std::shared_ptr<implementation> p) : owner(std::move(p)) {
            std::lock_guard lock(owner->readers_mutex); owner->readers.insert(this);
        }
        ~reader() {
            std::lock_guard lock(owner->readers_mutex);
            owner->retired_hits += hits.load(std::memory_order_relaxed);
            owner->readers.erase(this);
        }
    };
    mutable std::mutex readers_mutex;
    std::unordered_set<reader*> readers;
    std::uint64_t retired_hits = 0;
    cache_options options;
    std::vector<std::unique_ptr<shard>> shards;
    explicit implementation(cache_options o) : options(o) {
        if (o.ttl.count() < 0 || !o.shards || o.shards > 256 || !o.max_entries)
            throw std::invalid_argument("invalid cache limits");
        const auto count = std::min(o.shards, o.max_entries);
        for (std::size_t i = 0; i < count; ++i) {
            auto s = std::make_unique<shard>();
            s->budget = o.max_bytes / count + (i < o.max_bytes % count);
            if (o.max_bytes) {
                s->slots.resize(o.max_entries / count + (i < o.max_entries % count));
                s->index.reserve(s->slots.size());
            }
            shards.push_back(std::move(s));
        }
    }
    shard& select(std::string_view key) const { return *shards[string_hash{}(key) % shards.size()]; }
};
response_cache::response_cache(cache_options options) : impl_(std::make_shared<implementation>(options)) {}
response_cache::~response_cache() = default;
std::shared_ptr<const response> response_cache::get(std::string_view key, std::chrono::seconds* resident_age) {
    auto& s = impl_->select(key);
    std::lock_guard lock(s.mutex);
    const auto it = s.index.find(key);
    if (it != s.index.end()) {
        auto& value = s.slots[it->second];
        const auto now = clock_type::now();
        if (now < value.expiry) {
            if (resident_age) *resident_age = std::chrono::duration_cast<std::chrono::seconds>(now - value.inserted);
            ++s.stats.hits; value.referenced = true; return value.value;
        }
        s.erase(it->second);
    }
    ++s.stats.misses;
    return {};
}
std::optional<response> response_cache::get_copy(std::string_view key, std::chrono::seconds* resident_age) {
    auto* io = io_context::current();
    if (!io) {
        if (auto snapshot = get(key, resident_age)) return *snapshot;
        return {};
    }
    auto* local = static_cast<implementation::reader*>(io->find_local_state(impl_.get()));
    if (!local) local = static_cast<implementation::reader*>(io->local_state(impl_, [&] {
        return std::make_shared<implementation::reader>(impl_);
    }).get());
    const auto now = clock_type::now();
    if (local->value && local->key == key && now < local->expiry && local->generation != UINT64_MAX &&
        local->source->generation.load(std::memory_order_acquire) == local->generation) {
        local->hits.fetch_add(1, std::memory_order_relaxed);
        if (resident_age) *resident_age = std::chrono::duration_cast<std::chrono::seconds>(now - local->inserted);
        return *local->value;
    }
    local->value.reset();
    auto& s = impl_->select(key);
    {
        std::lock_guard lock(s.mutex);
        const auto it = s.index.find(key);
        if (it == s.index.end()) { ++s.stats.misses; return {}; }
        auto& value = s.slots[it->second];
        // Read time after acquiring the lock: contention must not extend TTL.
        if (clock_type::now() >= value.expiry) { s.erase(it->second); ++s.stats.misses; return {}; }
        local->key = key;
        local->value = value.value;
        local->source = &s;
        local->generation = s.generation.load(std::memory_order_relaxed);
        local->expiry = value.expiry; local->inserted = value.inserted;
        ++s.stats.hits; value.referenced = true;
    }
    if (resident_age) *resident_age = std::chrono::duration_cast<std::chrono::seconds>(clock_type::now() - local->inserted);
    return *local->value;
}
void response_cache::put(std::string key, response value, std::chrono::seconds ttl) {
    if (value.body.size() > impl_->options.max_object_bytes || ttl.count() <= 0 || !impl_->options.max_bytes) return;
    ttl = std::min(ttl, impl_->options.ttl);
    if (ttl.count() <= 0) return;
    const auto cost = cache_cost(key, value);
    auto& s = impl_->select(key);
    if (cost > s.budget) return;
    auto snapshot = std::make_shared<const response>(std::move(value));
    std::lock_guard lock(s.mutex);
    if (const auto it = s.index.find(key); it != s.index.end()) s.erase(it->second);
    while (s.stats.bytes > s.budget - cost || s.stats.entries == s.slots.size()) {
        auto& slot = s.slots[s.hand];
        if (slot.value) {
            if (slot.referenced && slot.expiry > clock_type::now()) slot.referenced = false;
            else { s.erase(s.hand); ++s.stats.evictions; }
        }
        s.hand = (s.hand + 1) % s.slots.size();
    }
    while (s.slots[s.hand].value) s.hand = (s.hand + 1) % s.slots.size();
    const auto now = clock_type::now();
    const auto maximum = std::chrono::duration_cast<std::chrono::seconds>(clock_type::time_point::max() - now);
    const auto expiry = ttl >= maximum ? clock_type::time_point::max() : now + ttl;
    s.index.emplace(key, s.hand);
    s.slots[s.hand] = {std::move(key), std::move(snapshot), expiry, now, cost, true};
    s.stats.bytes += cost; ++s.stats.entries;
    s.invalidate();
}
void response_cache::clear() {
    for (auto& pointer : impl_->shards) {
        auto& s = *pointer; std::lock_guard lock(s.mutex);
        for (std::size_t i = 0; i < s.slots.size(); ++i) s.erase(i);
    }
}
cache_statistics response_cache::statistics() const {
    cache_statistics result;
    {
        std::lock_guard lock(impl_->readers_mutex);
        result.hits = impl_->retired_hits;
        for (const auto* reader : impl_->readers) result.hits += reader->hits.load(std::memory_order_relaxed);
    }
    for (const auto& pointer : impl_->shards) {
        const auto& s = *pointer; std::lock_guard lock(s.mutex);
        result.hits += s.stats.hits; result.misses += s.stats.misses; result.evictions += s.stats.evictions;
        result.bytes += s.stats.bytes; result.entries += s.stats.entries;
    }
    return result;
}

namespace {
std::vector<std::string> path_parts(std::string_view input, bool dotfiles) {
    std::string decoded;
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < input.size(); ++i) {
        unsigned char c = input[i];
        if (c == '%') {
            if (i + 2 >= input.size() || hex(input[i+1]) < 0 || hex(input[i+2]) < 0) throw std::invalid_argument("bad path escape");
            c = static_cast<unsigned char>(hex(input[i+1]) * 16 + hex(input[i+2])); i += 2;
        }
        if (c < 32 || c == 127 || c == '\\' || c == ':' || c == '#') throw std::invalid_argument("bad path character");
        decoded += static_cast<char>(c);
    }
    std::vector<std::string> parts;
    std::string_view remaining(decoded);
    while (!remaining.empty()) {
        const auto slash = remaining.find('/');
        const auto part = remaining.substr(0, slash);
        if (!part.empty()) {
            if (part == "." || part == ".." || (!dotfiles && part.front() == '.') || part.back() == '.' || part.back() == ' ')
                throw std::invalid_argument("unsafe path segment");
            parts.emplace_back(part);
        }
        if (slash == remaining.npos) break;
        remaining.remove_prefix(slash + 1);
    }
    return parts;
}
#ifdef _WIN32
struct file_handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~file_handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
std::wstring final_path(HANDLE h) {
    std::wstring result(32768, L'\0');
    const auto n = GetFinalPathNameByHandleW(h, result.data(), static_cast<DWORD>(result.size()), FILE_NAME_NORMALIZED);
    if (!n || n >= result.size()) throw std::runtime_error("cannot resolve file handle");
    result.resize(n); return result;
}
#else
struct file_handle {
    int value = -1;
    ~file_handle() { if (value >= 0) ::close(value); }
};
#endif
response condition_file(response result, const request& req) {
    if (result.status != 200) return result;
    const auto etag = result.header("ETag");
    const auto none = req.header("If-None-Match");
    bool unchanged = false;
    if (!none.empty()) {
        auto remaining = none;
        while (!remaining.empty()) {
            auto comma = remaining.find(','); auto part = trim(remaining.substr(0, comma));
            if (part.starts_with("W/")) part.remove_prefix(2);
            if (part == "*" || part == etag) unchanged = true;
            if (comma == remaining.npos) break;
            remaining.remove_prefix(comma + 1);
        }
    } else if (!req.header("If-Modified-Since").empty())
        unchanged = req.header("If-Modified-Since") == result.header("Last-Modified");
    if (unchanged) { result.status = 304; result.body.clear(); return result; }
    // Range applies only to GET; HEAD describes the complete representation.
    if (req.method != "GET") return result;
    auto range = req.header("Range");
    const auto if_range = req.header("If-Range");
    if (!if_range.empty() && if_range != etag && if_range != result.header("Last-Modified")) return result;
    // Ignore unsupported units/multiple ranges; never build a multipart body.
    if (!range.starts_with("bytes=") || range.find(',') != range.npos) return result;
    range.remove_prefix(6);
    auto dash = range.find('-');
    if (dash == range.npos) return result;
    auto number = [](std::string_view s, std::size_t& value) {
        auto [p, e] = std::from_chars(s.data(), s.data() + s.size(), value);
        return !s.empty() && e == std::errc{} && p == s.data() + s.size();
    };
    const auto size = result.body.size();
    std::size_t first = 0, last = size ? size - 1 : 0;
    bool valid;
    if (dash == 0) {
        std::size_t suffix = 0; valid = number(range.substr(1), suffix) && suffix != 0;
        first = suffix >= size ? 0 : size - suffix;
    } else {
        valid = number(range.substr(0, dash), first);
        if (dash + 1 < range.size()) valid = number(range.substr(dash + 1), last) && valid;
        last = size ? std::min(last, size - 1) : 0;
    }
    if (!valid) return result;
    if (!size || first >= size || first > last) {
        result.status = 416; result.body.clear();
        result.headers.push_back({"Content-Range", "bytes */" + std::to_string(size)});
    } else {
        result.status = 206;
        result.body = result.body.substr(first, last - first + 1);
        result.headers.push_back({"Content-Range", "bytes " + std::to_string(first) + "-" + std::to_string(last) + "/" + std::to_string(size)});
    }
    return result;
}
}
struct static_files::implementation {
    std::string prefix;
    std::filesystem::path root;
    static_options options;
    response_cache cache;
    file_handle root_handle;
#ifdef _WIN32
    std::wstring root_name;
#endif
    implementation(std::string p, std::filesystem::path directory, static_options o)
        : prefix(prefix_path(std::move(p))), root(std::move(directory)), options(std::move(o)), cache(options.cache) {
        if (!options.index.empty() && (options.index.find_first_of("/\\:%") != options.index.npos || options.index == "." || options.index == ".."))
            throw std::invalid_argument("index must be a filename");
        for (unsigned char c : options.cache_control) if (c < 32 || c == 127) throw std::invalid_argument("invalid cache-control");
#ifdef _WIN32
        root_handle.value = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (root_handle.value == INVALID_HANDLE_VALUE) throw std::system_error(GetLastError(), std::system_category(), "static root");
        root_name = final_path(root_handle.value);
        if (root_name.back() != L'\\') root_name += L'\\';
#else
        root_handle.value = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (root_handle.value < 0) throw std::system_error(errno, std::generic_category(), "static root");
#endif
    }
    response load(std::vector<std::string> parts) {
        file_handle file;
        std::uint64_t size = 0;
        std::time_t modified = 0;
#ifdef _WIN32
        auto name = root;
        for (const auto& part : parts) {
            name /= std::filesystem::u8path(part);
            const auto attrs = GetFileAttributesW(name.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return text("Not Found\n", 404);
        }
        if (GetFileAttributesW(name.c_str()) & FILE_ATTRIBUTE_DIRECTORY) {
            if (options.index.empty()) return text("Not Found\n", 404);
            name /= std::filesystem::u8path(options.index); parts.push_back(options.index);
        }
        file.value = CreateFileW(name.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file.value == INVALID_HANDLE_VALUE) return text("Not Found\n", 404);
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file.value, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) || GetFileType(file.value) != FILE_TYPE_DISK)
            return text("Not Found\n", 404);
        const auto resolved = final_path(file.value);
        if (resolved.size() <= root_name.size() || _wcsnicmp(resolved.c_str(), root_name.c_str(), root_name.size()) != 0) return text("Not Found\n", 404);
        size = (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        const auto timestamp = (std::uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
        modified = static_cast<std::time_t>(timestamp / 10000000 - 11644473600ULL);
#else
        // Borrow the retained root until the first component is opened.
        // No dup/close pair is needed for ordinary file paths.
        for (std::size_t i = 0; i < parts.size(); ++i) {
            const auto fd = ::openat(i == 0 ? root_handle.value : file.value, parts[i].c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC | (i + 1 < parts.size() ? O_DIRECTORY : 0));
            if (fd < 0) return text("Not Found\n", 404);
            if (file.value >= 0) ::close(file.value);
            file.value = fd;
        }
        struct stat st{};
        if (::fstat(file.value < 0 ? root_handle.value : file.value, &st) < 0) return text("Not Found\n", 404);
        if (S_ISDIR(st.st_mode)) {
            if (options.index.empty()) return text("Not Found\n", 404);
            const auto fd = ::openat(file.value < 0 ? root_handle.value : file.value, options.index.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0) return text("Not Found\n", 404);
            if (file.value >= 0) ::close(file.value);
            file.value = fd; parts.push_back(options.index);
            if (::fstat(file.value, &st) < 0) return text("Not Found\n", 404);
        }
        if (!S_ISREG(st.st_mode) || st.st_size < 0) return text("Not Found\n", 404);
        size = static_cast<std::uint64_t>(st.st_size); modified = st.st_mtime;
#endif
        if (size > options.max_file_bytes || size > std::numeric_limits<std::size_t>::max()) return text("File exceeds configured limit\n", 413);
        response result;
        result.body.resize(static_cast<std::size_t>(size));
        std::size_t offset = 0;
        while (offset < result.body.size()) {
#ifdef _WIN32
            DWORD n = 0;
            if (!ReadFile(file.value, result.body.data() + offset, static_cast<DWORD>(std::min<std::size_t>(result.body.size() - offset, 65536)), &n, nullptr))
                return text("File read failed\n", 500);
#else
            const auto n = ::read(file.value, result.body.data() + offset, result.body.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) return text("File read failed\n", 500);
#endif
            if (!n) return text("File changed during read\n", 503);
            offset += static_cast<std::size_t>(n);
        }
        const auto hash = pace::detail::content_hash(result.body);
        std::array<char, 32> buffer{};
        auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), hash, 16);
        (void)error;
        result.content_type = mime(parts.empty() ? options.index : parts.back());
        result.headers = {{"ETag", "\"" + std::string(buffer.data(), end) + "-" + std::to_string(size) + "\""},
            {"Last-Modified", http_date(modified)}, {"Accept-Ranges", "bytes"}, {"X-Content-Type-Options", "nosniff"}};
        if (!options.cache_control.empty()) result.headers.push_back({"Cache-Control", options.cache_control});
        return result;
    }
    static task<response> serve(std::shared_ptr<implementation> self, request req) {
        if (req.method != "GET" && req.method != "HEAD")
            co_return response{.status=405, .body="Method Not Allowed\n", .headers={{"Allow", "GET, HEAD"}}};
        const auto path = std::string_view(req.target).substr(0, req.target.find('?'));
        if (!matches(path, self->prefix)) co_return text("Not Found\n", 404);
        std::vector<std::string> parts;
        try { parts = path_parts(path.substr(self->prefix == "/" ? 1 : self->prefix.size()), self->options.allow_dotfiles); }
        catch (const std::invalid_argument&) { co_return text("Invalid path\n", 400); }
        std::string key;
        for (const auto& part : parts) { key += '/'; key += part; }
        const bool use_cache = self->options.cache.max_bytes && req.header("Cache-Control").empty() && req.header("Pragma").empty();
        if (use_cache) if (auto cached = self->cache.get_copy(key)) co_return condition_file(std::move(*cached), req);
        auto result = self->load(std::move(parts));
        if (use_cache && result.status == 200) self->cache.put(std::move(key), result, self->options.cache.ttl);
        co_return condition_file(std::move(result), req);
    }
};
static_files::static_files(std::string prefix, std::filesystem::path directory, static_options options)
    : impl_(std::make_shared<implementation>(std::move(prefix), std::move(directory), std::move(options))) {}
task<response> static_files::operator()(request incoming) const { return implementation::serve(impl_, std::move(incoming)); }
void static_files::clear_cache() const { impl_->cache.clear(); }
cache_statistics static_files::cache_stats() const { return impl_->cache.statistics(); }

namespace {
void strip_headers(std::vector<header_field>& headers, bool outgoing_request) {
    std::vector<std::string> named;
    for (const auto& f : headers) if (equal_ci(f.name, "Connection")) {
        auto more = tokens(f.value); named.insert(named.end(), more.begin(), more.end());
    }
    std::erase_if(headers, [&](const header_field& f) {
        const std::string_view name = f.name;
        if (equal_ci(name, "connection") || equal_ci(name, "keep-alive") || equal_ci(name, "proxy-connection") || equal_ci(name, "proxy-authenticate") ||
            equal_ci(name, "proxy-authorization") || equal_ci(name, "te") || equal_ci(name, "trailer") || equal_ci(name, "transfer-encoding") ||
            equal_ci(name, "upgrade") || equal_ci(name, "content-length") || name.starts_with(':')) return true;
        if (std::any_of(named.begin(), named.end(), [&](const auto& token) { return equal_ci(token, name); })) return true;
        if (outgoing_request) return equal_ci(name, "host") || equal_ci(name, "expect") || equal_ci(name, "forwarded") || equal_ci(name.substr(0, 12), "x-forwarded-");
        return equal_ci(name, "content-type");
    });
}
bool cache_request(const request& req) {
    if (req.method != "GET" || !req.body.empty() || req.target.size() > 4096) return false;
    for (const auto& f : req.headers) {
        const std::string_view name = f.name;
        if (equal_ci(name, "authorization") || equal_ci(name, "cookie") || equal_ci(name, "range") || equal_ci(name.substr(0, 3), "if-") ||
            equal_ci(name, "cache-control") || equal_ci(name, "pragma")) return false;
    }
    return true;
}
std::chrono::seconds origin_age(const response& result) {
    std::uint64_t age = 0;
    if (!result.header("Age").empty()) {
        const auto value = result.header("Age");
        auto [p, e] = std::from_chars(value.data(), value.data() + value.size(), age);
        if (e != std::errc{} || p != value.data() + value.size() || age > 31536000) return std::chrono::seconds::max();
    }
    if (!result.header("Date").empty()) {
        std::istringstream input{std::string(result.header("Date"))};
        input.imbue(std::locale::classic()); std::tm tm{};
        input >> std::get_time(&tm, "%a, %d %b %Y %H:%M:%S GMT");
        if (input.fail()) return std::chrono::seconds::max();
#ifdef _WIN32
        const auto timestamp = _mkgmtime(&tm);
#else
        const auto timestamp = timegm(&tm);
#endif
        if (timestamp < 0) return std::chrono::seconds::max();
        const auto now = std::time(nullptr);
        if (now > timestamp) age = std::max(age, static_cast<std::uint64_t>(now - timestamp));
    }
    return std::chrono::seconds(age);
}
std::chrono::seconds cache_ttl(const response& result, std::chrono::seconds configured) {
    if (result.status != 200 || result.close) return {};
    std::optional<std::chrono::seconds> max_age, shared_age;
    bool public_response = false;
    for (const auto& field : result.headers) {
        const auto name = lower(field.name);
        if (name == "set-cookie" || name == "vary" || name == "content-range" || name == "pragma") return {};
        if (name == "cache-control") for (auto directive : tokens(field.value)) {
            if (directive == "public") public_response = true;
            else if (directive.starts_with("max-age=") || directive.starts_with("s-maxage=")) {
                auto value = std::string_view(directive).substr(directive.find('=') + 1);
                std::uint64_t seconds = 0;
                auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), seconds);
                if (error != std::errc{} || end != value.data() + value.size() || seconds > 31536000) return {};
                if (directive.starts_with("s-maxage=")) shared_age = std::chrono::seconds(seconds);
                else max_age = std::chrono::seconds(seconds);
            } else return {}; // Unknown/no-store/private/no-cache directives stay uncacheable.
        }
    }
    if (!public_response && !shared_age) return {};
    auto ttl = std::min(configured, shared_age.value_or(max_age.value_or(configured)));
    const auto age = origin_age(result);
    if (age >= ttl) return {};
    ttl -= age;
    return ttl;
}
}
struct reverse_proxy::implementation {
    using destination = pace::detail::http_url::destination;
    struct connection {
        std::unique_ptr<client> plain;
#ifdef PACE_HAS_TLS
        std::unique_ptr<secure_client> secure;
#endif
        clock_type::time_point used{};
        bool open() const {
#ifdef PACE_HAS_TLS
            if (secure) return secure->is_open();
#endif
            return plain && plain->is_open();
        }
        task<response> send(request req) {
#ifdef PACE_HAS_TLS
            if (secure) co_return co_await secure->send(std::move(req));
#endif
            co_return co_await plain->send(std::move(req));
        }
    };
    struct upstream_state {
        std::vector<std::unique_ptr<connection>> idle;
        std::size_t active = 0;
        clock_type::time_point failed_until{};
    };
    struct worker_state {
        std::vector<upstream_state> upstreams;
        std::size_t cursor = 0;
#ifdef PACE_HAS_TLS
        std::optional<tls::context> tls_config;
#endif
        explicit worker_state(std::size_t n) : upstreams(n) {}
    };
    std::string prefix;
    proxy_options options;
    std::vector<destination> upstreams;
    response_cache cache;
    implementation(std::string p, std::vector<std::string> urls, proxy_options o)
        : prefix(prefix_path(std::move(p))), options(o), cache(o.cache) {
        if (urls.empty() || !o.max_connections || o.max_idle_connections > o.max_connections ||
            o.timeout.count() <= 0 || o.idle_timeout.count() < 0 || o.fail_timeout.count() < 0)
            throw std::invalid_argument("invalid proxy limits");
        for (const auto& url : urls) {
            if (url.find_first_of("?#") != url.npos) throw std::invalid_argument("upstream base must not contain query/fragment");
            auto target = pace::detail::http_url::parse_url(url);
#ifndef PACE_HAS_TLS
            if (target.secure) throw std::invalid_argument("HTTPS upstream requires TLS module");
#endif
#ifndef PACE_HAS_DNS
            if (!target.numeric) throw std::invalid_argument("hostname upstream requires DNS module");
#endif
            upstreams.push_back(std::move(target));
        }
    }
    task<response> exchange(worker_state& worker, std::size_t index, request req) {
        auto& state = worker.upstreams[index];
        std::unique_ptr<connection> transport;
        const auto now = clock_type::now();
        while (!state.idle.empty()) {
            transport = std::move(state.idle.back()); state.idle.pop_back();
            if (transport->open() && now - transport->used < options.idle_timeout) break;
            transport.reset();
        }
        if (!transport) {
            transport = std::make_unique<connection>();
            const auto& dest = upstreams[index];
            auto stream = co_await pace::detail::http_url::connect_url(this_context(), dest);
            stream.set_no_delay();
            limits bounds; bounds.max_body_bytes = options.max_response_bytes;
#ifdef PACE_HAS_TLS
            if (dest.secure) {
                if (!worker.tls_config) { worker.tls_config.emplace(); worker.tls_config->set_alpn_protocols({"http/1.1"}); }
                tls::stream secure(std::move(stream), *worker.tls_config);
                co_await secure.handshake(dest.host);
                transport->secure = std::make_unique<secure_client>(std::move(secure), bounds);
            } else
#endif
                transport->plain = std::make_unique<client>(std::move(stream), bounds);
        }
        auto result = co_await transport->send(std::move(req));
        if (transport->open() && state.idle.size() < options.max_idle_connections) {
            transport->used = clock_type::now(); state.idle.push_back(std::move(transport));
        }
        co_return result;
    }
    static task<response> serve(std::shared_ptr<implementation> self, request req) {
        const auto path = std::string_view(req.target).substr(0, req.target.find('?'));
        if (!matches(path, self->prefix)) co_return text("Not Found\n", 404);
        if (req.method == "CONNECT" || !req.header("Upgrade").empty()) co_return text("Upgrade/tunnel unsupported\n", 501);
        if (req.target.find('#') != req.target.npos) co_return text("Invalid target\n", 400);
        const auto parent = co_await this_task::cancellation();
        parent.throw_if_cancelled();
        const bool eligible = self->options.cache.max_bytes && cache_request(req);
        const std::string key = eligible ? std::string(req.header("Host")) + "\n" + req.target : std::string{};
        if (eligible) {
            std::chrono::seconds age;
            if (auto cached = self->cache.get_copy(key, &age)) {
                auto result = std::move(*cached);
                const auto stored_age = result.header("Age");
                std::uint64_t initial = 0;
                if (!stored_age.empty()) (void)std::from_chars(stored_age.data(), stored_age.data() + stored_age.size(), initial);
                // Date/Age were normalized on insertion. Resident time uses a
                // monotonic clock; do not parse the same Date on every hit.
                age += std::chrono::seconds(initial);
                std::erase_if(result.headers, [](const header_field& f) { return equal_ci(f.name, "Age"); });
                result.headers.push_back({"Age", std::to_string(age.count())});
                co_return result;
            }
        }
        auto& io = this_context();
        auto* worker = static_cast<worker_state*>(io.find_local_state(self.get()));
        if (!worker) worker = static_cast<worker_state*>(io.local_state(self, [&] {
            return std::make_shared<worker_state>(self->upstreams.size());
        }).get());
        std::size_t chosen = self->upstreams.size();
        const auto now = clock_type::now();
        for (std::size_t i = 0; i < worker->upstreams.size(); ++i) {
            const auto n = (worker->cursor + i) % worker->upstreams.size();
            const auto& state = worker->upstreams[n];
            if (state.failed_until > now || state.active >= self->options.max_connections) continue;
            if (chosen == self->upstreams.size() || state.active < worker->upstreams[chosen].active) chosen = n;
            if (self->options.balance == load_balance::round_robin) break;
        }
        if (chosen == self->upstreams.size()) co_return text("No available upstream\n", 503);
        worker->cursor = (chosen + 1) % worker->upstreams.size();
        auto& state = worker->upstreams[chosen];
        ++state.active;
        struct release { upstream_state& state; ~release() { --state.active; } } active{state};
        auto suffix = self->options.strip_prefix && self->prefix != "/" ? req.target.substr(self->prefix.size()) : req.target;
        if (suffix.empty() || suffix.front() == '?') suffix.insert(suffix.begin(), '/');
        auto base = self->upstreams[chosen].target;
        if (base.ends_with('/')) base.pop_back();
        req.target = base + suffix;
        const bool head = req.method == "HEAD";
        strip_headers(req.headers, true);
        req.headers.push_back({"Host", self->upstreams[chosen].authority});
        req.version = "HTTP/1.1"; req.chunked = false; req.trailers.clear(); req.keep_alive = true;
        response result;
        try {
            result = co_await with_deadline(deadline::after(self->options.timeout),
                self->exchange(*worker, chosen, std::move(req)));
        } catch (const std::system_error& error) {
            parent.throw_if_cancelled();
            if (error.code() == std::errc::operation_canceled) throw;
            state.failed_until = deadline::after(self->options.fail_timeout).time();
            state.idle.clear();
            co_return text(error.code() == std::errc::timed_out ? "Upstream timeout\n" : "Upstream unavailable\n",
                           error.code() == std::errc::timed_out ? 504 : 502);
        } catch (const protocol_error&) {
            state.failed_until = deadline::after(self->options.fail_timeout).time();
            state.idle.clear(); co_return text("Invalid upstream response\n", 502);
        }
        if (head) {
            const auto length = result.header("Content-Length"); std::uint64_t size = 0;
            const auto [p, e] = std::from_chars(length.data(), length.data() + length.size(), size);
            if (!length.empty() && e == std::errc{} && p == length.data() + length.size()) result.representation_length = size;
        }
        const auto ttl = eligible ? cache_ttl(result, self->options.cache.ttl) : std::chrono::seconds{};
        strip_headers(result.headers, false);
        result.close = false; result.chunked = false; result.trailers.clear(); result.version = "HTTP/1.1";
        if (eligible && ttl.count() > 0) {
            const auto age = origin_age(result);
            std::erase_if(result.headers, [](const header_field& f) { return equal_ci(f.name, "Age"); });
            result.headers.push_back({"Age", std::to_string(age.count())});
            self->cache.put(key, result, ttl);
        }
        co_return result;
    }
};
reverse_proxy::reverse_proxy(std::string prefix, std::vector<std::string> upstreams, proxy_options options)
    : impl_(std::make_shared<implementation>(std::move(prefix), std::move(upstreams), options)) {}
task<response> reverse_proxy::operator()(request incoming) const { return implementation::serve(impl_, std::move(incoming)); }
void reverse_proxy::clear_cache() const { impl_->cache.clear(); }
cache_statistics reverse_proxy::cache_stats() const { return impl_->cache.statistics(); }
void app::run_workers(std::size_t workers, tcp::endpoint endpoint, limits bounds) const {
    runtime pool(workers);
    std::vector<tcp::listener> listeners;
    listeners.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) {
        listeners.emplace_back(pool.context(i), endpoint, tcp::listen_options{.reuse_port = workers > 1});
        if (endpoint.port == 0) endpoint.port = listeners.back().local_endpoint().port;
        pool.context(i).spawn(serve(listeners.back(), bounds));
    }
    pool.run_on_current_thread();
}
} // namespace pace::http
