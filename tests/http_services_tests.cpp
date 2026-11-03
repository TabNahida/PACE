#include <pace/http.hpp>
#include <pace/task_group.hpp>
#include <pace/operation.hpp>
#include <atomic>
#include <fstream>
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
using namespace std::chrono_literals;
namespace h = pace::http;
namespace fs = std::filesystem;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct watchdog {
    std::mutex mutex; std::condition_variable cv; bool done = false;
    std::thread thread{[this] { std::unique_lock lock(mutex); if (!cv.wait_for(lock, 30s, [&] { return done; })) std::abort(); }};
    ~watchdog() { { std::lock_guard lock(mutex); done = true; } cv.notify_one(); thread.join(); }
};
h::request request(std::string target, std::string method = "GET") {
    return {.method=std::move(method), .target=std::move(target), .version="HTTP/1.1", .headers={{"Host", "example.test"}}, .body={}};
}
void cache_test() {
    h::response_cache cache({.max_bytes=16384, .max_entries=8, .max_object_bytes=2048, .ttl=1s, .shards=2});
    cache.put("one", h::text("snapshot"), 1s);
    auto pinned = cache.get("one"); require(pinned && pinned->body == "snapshot", "cache insertion");
    std::vector<std::thread> threads;
    for (int n = 0; n < 4; ++n) threads.emplace_back([&, n] {
        for (int i = 0; i < 200; ++i) {
            const auto key = std::to_string(n * 1000 + i);
            cache.put(key, h::text(std::string(128, 'x')), 1s);
            (void)cache.get(key);
        }
    });
    for (auto& thread : threads) thread.join();
    auto stats = cache.statistics();
    require(stats.bytes <= 16384 && stats.entries <= 8 && stats.evictions > 0, "bounded concurrent CLOCK eviction");
    cache.clear(); require(cache.statistics().bytes == 0 && pinned->body == "snapshot", "snapshot survives clear/eviction");
    cache.put("ttl", h::text("short"), 1s);
    std::this_thread::sleep_for(1100ms);
    require(!cache.get("ttl"), "expired entries must not be served");
    cache.put("large", h::text(std::string(2049, 'x')), 1s);
    require(!cache.get("large"), "object size bound");
}
struct directory {
    fs::path path = fs::temp_directory_path() / ("pace-services-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    directory() { fs::create_directories(path / "sub"); }
    ~directory() { std::error_code e; fs::remove_all(path, e); }
    void write(const std::string& name, std::string_view value) { std::ofstream out(path / name, std::ios::binary); out << value; }
};
pace::task<void> static_test(directory& dir) {
    h::static_options options; options.cache = {.max_bytes=65536, .max_entries=8, .ttl=30s, .shards=1};
    h::static_files files("/assets", dir.path, options);
    auto first = co_await files(request("/assets/hello.txt?x=1"));
    require(first.status == 200 && first.body == "hello world" && first.content_type.starts_with("text/plain"), "static bytes/MIME/query");
    auto head = co_await files(request("/assets/hello.txt", "HEAD"));
    require(head.body.size() == first.body.size(), "HEAD representation length retained");
    auto conditional = request("/assets/hello.txt"); conditional.headers.push_back({"If-None-Match", "W/" + std::string(first.header("ETag"))});
    auto unchanged = co_await files(conditional); require(unchanged.status == 304 && unchanged.body.empty(), "weak conditional match");
    conditional.headers.back() = {"If-Modified-Since", std::string(first.header("Last-Modified"))};
    require((co_await files(conditional)).status == 304, "modification validator");
    auto range = request("/assets/hello.txt"); range.headers.push_back({"Range", "bytes=1-4"});
    auto head_range = range; head_range.method = "HEAD";
    auto whole_head = co_await files(head_range);
    require(whole_head.status == 200 && whole_head.body.size() == 11, "HEAD ignores Range");
    auto slice = co_await files(range); require(slice.status == 206 && slice.body == "ello" && slice.header("Content-Range") == "bytes 1-4/11", "single range");
    range.headers.back().value = "bytes=-5"; require((co_await files(range)).body == "world", "suffix range");
    range.headers.back().value = "bytes=50-"; require((co_await files(range)).status == 416, "unsatisfiable range");
    range.headers.push_back({"If-Range", "\"old\""}); require((co_await files(range)).status == 200, "If-Range mismatch");
    require((co_await files(request("/assets/sub/"))).body == "index", "directory index");
    require((co_await files(request("/assets/hello.txt", "POST"))).status == 405, "static method restriction");
    for (const auto& target : {"/assets/../secret", "/assets/%2e%2e/secret", "/assets/%00", "/assets/%5csecret", "/assets/.env", "/assets/a:b", "/assets/%xx"})
        require((co_await files(request(target))).status == 400, "unsafe path rejected");
    auto missing = request("/assets/missing"); missing.headers.push_back({"If-None-Match", "*"});
    require((co_await files(missing)).status == 404, "missing file must not become 304");
#ifndef _WIN32
    require((co_await files(request("/assets/link"))).status == 404, "symlink escape rejected");
    require((co_await files(request("/assets/directory-link/hello.txt"))).status == 404, "intermediate symlink rejected");
#endif
    dir.write("hello.txt", "updated");
    require((co_await files(request("/assets/hello.txt"))).body == "hello world", "explicit static TTL");
    files.clear_cache(); require((co_await files(request("/assets/hello.txt"))).body == "updated", "explicit cache invalidation");
    h::static_options tiny; tiny.max_file_bytes = 2;
    h::static_files limited("/", dir.path, tiny);
    require((co_await limited(request("/hello.txt"))).status == 413, "file allocation bound");
    h::app app;
    app.mount("/assets", dir.path).mount("/", dir.path).get("/assets/hello.txt", [] { return h::text("exact"); });
    auto snapshot = app.handler();
    app.get("/new", [] { return h::text("new"); });
    require((co_await snapshot(request("/assets/hello.txt"))).body == "exact", "exact route wins");
    require((co_await snapshot(request("/assets/sub"))).body == "index", "longest segment mount");
    require((co_await snapshot(request("/new"))).status == 404, "route snapshot lifetime");
    auto temporary = h::static_files("/", dir.path)(request("/hello.txt"));
    require((co_await std::move(temporary)).body == "updated", "lazy task owns service");
}
struct upstream {
    std::string id; unsigned calls = 0; unsigned connections = 0;
    pace::task<h::response> handle(h::request req) {
        ++calls;
        if (req.target == "/slow") co_await pace::sleep_for(200ms);
        auto res = h::text(id + " " + req.target + " " + req.body);
        res.headers = {{"Cache-Control", "public, max-age=30"}};
        if (req.target == "/aged") res.headers.push_back({"Age", "10"});
        if (req.target == "/stale") res.headers.push_back({"Age", "60"});
        if (req.target == "/private") res.headers = {{"Cache-Control", "private"}};
        if (req.target == "/cookie") res.headers.push_back({"Set-Cookie", "session=1"});
        if (req.target == "/vary") res.headers.push_back({"Vary", "Accept-Language"});
        if (req.target == "/headers") {
            require(req.header("Connection") == "keep-alive", "upstream keep-alive");
            require(req.header("X-Remove").empty() && req.header("Forwarded").empty() && req.header("X-Forwarded-For").empty(), "proxy strips untrusted/hop headers");
            require(req.header("Host") != "example.test", "upstream authority replaced");
        }
        co_return res;
    }
};
pace::task<void> accept_upstream(pace::tcp::listener& listener, upstream& state) {
    co_await pace::with_task_group([&](pace::task_group& group) -> pace::task<void> {
        try {
            for (;;) {
                auto socket = co_await listener.accept(); ++state.connections;
                (void)group.spawn(h::serve_connection(std::move(socket), [&](h::request req) { return state.handle(std::move(req)); }));
            }
        } catch (const std::system_error& e) {
            if (e.code() != std::errc::operation_canceled) throw;
        }
    });
}
std::string url(const pace::tcp::listener& listener) { return "http://127.0.0.1:" + std::to_string(listener.local_endpoint().port); }
pace::task<void> proxy_test(pace::io_context& io, pace::tcp::listener& a, pace::tcp::listener& b, upstream& first, upstream& second) {
    h::reverse_proxy proxy("/api", {url(a), url(b)});
    require((co_await proxy(request("/api/one?q=1"))).body == "a /one?q=1 ", "proxy prefix/query/round robin first");
    require((co_await proxy(request("/api/two"))).body == "b /two ", "round robin second");
    auto post = request("/api/echo", "POST"); post.body = "body";
    require((co_await proxy(post)).body == "a /echo body", "proxy method/body");
    require(first.connections == 1 && second.connections == 1, "worker-local connection reuse");
    auto headers = request("/api/headers"); headers.headers.push_back({"Connection", "keep-alive, X-Remove"});
    headers.headers.push_back({"X-Remove", "secret"}); headers.headers.push_back({"Forwarded", "spoofed"});
    headers.headers.push_back({"X-Forwarded-For", "spoofed"});
    require((co_await proxy(headers)).status == 200, "forwarded header sanitization");
    h::proxy_options options; options.cache = {.max_bytes=65536, .max_entries=16, .ttl=30s, .shards=1};
    h::reverse_proxy cached("/", {url(a)}, options);
    const auto before = first.calls;
    auto r = co_await cached(request("/cached"));
    require((co_await cached(request("/cached"))).body == r.body && first.calls == before + 1, "public proxy cache hit");
    auto other_host = request("/cached"); other_host.headers.front().value = "another.test";
    (void)co_await cached(other_host); require(first.calls == before + 2, "cache separates downstream hosts");
    cached.clear_cache(); (void)co_await cached(request("/cached"));
    require(first.calls == before + 3, "explicit proxy invalidation");
    (void)co_await cached(request("/aged"));
    auto aged = co_await cached(request("/aged"));
    require(!aged.header("Age").empty() && std::stoul(std::string(aged.header("Age"))) >= 10, "cache preserves upstream Age");
    const auto authenticated_before = first.calls;
    auto auth = request("/cached"); auth.headers.push_back({"Authorization", "Bearer private"});
    (void)co_await cached(auth); require(first.calls == authenticated_before + 1, "authorization cache bypass");
    for (const auto& path : {"/private", "/cookie", "/vary", "/stale"}) {
        const auto n = first.calls;
        (void)co_await cached(request(path)); (void)co_await cached(request(path));
        require(first.calls == n + 2, "private/cookie/vary response must not enter shared cache");
    }
    auto upgrade = request("/"); upgrade.headers.push_back({"Upgrade", "websocket"});
    require((co_await cached(upgrade)).status == 501, "no accidental websocket upgrade");
    h::proxy_options slow; slow.timeout = 10ms; slow.fail_timeout = 1s;
    h::reverse_proxy timeout("/", {url(a)}, slow);
    require((co_await timeout(request("/slow"))).status == 504, "upstream deadline");
    require((co_await timeout(request("/next"))).status == 503, "passive failure cooldown");
    h::proxy_options least; least.balance = h::load_balance::least_connections;
    least.max_connections = 1; least.max_idle_connections = 1;
    h::reverse_proxy balanced("/", {url(a), url(b)}, least);
    co_await pace::with_task_group([&](pace::task_group& group) -> pace::task<void> {
        auto long_request = group.spawn(balanced(request("/slow")));
        co_await pace::sleep_for(10ms);
        auto quick = co_await balanced(request("/fast"));
        require(quick.body == "b /fast ", "least-connections bypasses busy upstream");
        require((co_await long_request).status == 200, "active request lifecycle");
    });
    h::reverse_proxy bound("/", {url(a)}, least);
    co_await pace::with_task_group([&](pace::task_group& group) -> pace::task<void> {
        auto long_request = group.spawn(bound(request("/slow")));
        co_await pace::sleep_for(10ms);
        require((co_await bound(request("/fast"))).status == 503, "per-upstream admission limit");
        (void)co_await long_request;
    });
    // HEAD must preserve the upstream representation size without leaking body
    // bytes into the next response on the downstream persistent connection.
    pace::tcp::listener gateway(io, {"127.0.0.1", 0});
    co_await pace::with_task_group([&](pace::task_group& group) -> pace::task<void> {
        (void)group.spawn(h::serve(gateway, proxy));
        auto socket = co_await pace::tcp::connect(gateway.local_endpoint());
        h::client client(std::move(socket));
        auto head = co_await client.send(request("/api/wire", "HEAD"));
        require(head.body.empty() && head.header("Content-Length") == "8", "proxied HEAD wire length");
        auto get = co_await client.send(request("/api/wire"));
        require(get.status == 200 && get.body.size() == 8, "HEAD must not corrupt next response");
        client.close(); gateway.close();
    });
    h::reverse_proxy scoped("/", {url(a)});
    bool expired = false;
    try { (void)co_await pace::with_deadline(pace::deadline::after(10ms), scoped(request("/slow"))); }
    catch (const std::system_error& e) { expired = e.code() == std::errc::timed_out; }
    require(expired && (co_await scoped(request("/fast"))).status == 200,
            "parent deadline must propagate without marking upstream unhealthy");
    io.stop();
}
}
int main() {
    try {
        watchdog guard;
        cache_test();
        directory dir; dir.write("hello.txt", "hello world"); dir.write("sub/index.html", "index"); dir.write(".env", "private");
#ifndef _WIN32
        fs::create_symlink("/etc/passwd", dir.path / "link");
        fs::create_directory_symlink(dir.path, dir.path / "directory-link");
#endif
        pace::run([&] { return static_test(dir); });
        pace::io_context io;
        pace::tcp::listener a(io, {"127.0.0.1", 0}), b(io, {"127.0.0.1", 0});
        upstream first{"a"}, second{"b"};
        io.spawn(accept_upstream(a, first)); io.spawn(accept_upstream(b, second));
        io.spawn(proxy_test(io, a, b, first, second)); io.run();
        std::cout << "Static mounts, cache isolation/eviction, pooled proxy and balancing passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
