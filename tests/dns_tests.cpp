#include <pace/dns.hpp>
#include <pace/udp.hpp>
#include <pace/tcp.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
using namespace std::chrono_literals;
void require(bool value, const char* what) { if (!value) throw std::runtime_error(what); }
struct watchdog {
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    std::thread thread{[this] {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, 10s, [this] { return done; })) std::abort();
    }};
    ~watchdog() { { std::lock_guard lock(mutex); done = true; } changed.notify_one(); thread.join(); }
};
unsigned get16(std::string_view data, std::size_t offset) {
    return static_cast<unsigned char>(data[offset]) * 256 + static_cast<unsigned char>(data[offset + 1]);
}
void put16(std::string& data, unsigned value) { data += static_cast<char>(value >> 8); data += static_cast<char>(value); }
std::string dns_name(std::string_view name) {
    std::string result;
    while (!name.empty()) {
        const auto dot = name.find('.'); const auto label = name.substr(0, dot);
        result += static_cast<char>(label.size()); result += label;
        if (dot == std::string_view::npos) break;
        name.remove_prefix(dot + 1);
    }
    result += '\0'; return result;
}
std::string make_reply(std::string_view query, std::string& hostname, unsigned& type) {
    require(query.size() >= 17 && get16(query, 4) == 1, "invalid test DNS question");
    std::size_t offset = 12;
    while (offset < query.size() && query[offset]) {
        const auto length = static_cast<unsigned char>(query[offset++]);
        require(length <= 63 && length <= query.size() - offset, "invalid test DNS label");
        if (!hostname.empty()) hostname += '.';
        for (unsigned i = 0; i < length; ++i) {
            char c = query[offset++]; if (c >= 'A' && c <= 'Z') c += 'a' - 'A'; hostname += c;
        }
    }
    require(offset + 5 <= query.size(), "incomplete test DNS question");
    ++offset; type = get16(query, offset); offset += 4;
    std::string response(query.substr(0, 2));
    put16(response, hostname == "missing.pace.test" ? 0x8183 : hostname == "refused.pace.test" ? 0x8185 : 0x8180);
    put16(response, 1);
    const bool no_answer = hostname == "missing.pace.test" || hostname == "refused.pace.test";
    put16(response, no_answer ? 0 : hostname == "alias.pace.test" ? 2 : 1);
    put16(response, 0); put16(response, 0);
    response.append(query.substr(12, offset - 12));
    if (no_answer) return response;
    if (hostname == "alias.pace.test") {
        response.append("\xc0\x0c", 2); put16(response, 5); put16(response, 1);
        response.append("\0\0\0\x3c", 4);
        const auto alias = dns_name("address.pace.test"); put16(response, alias.size()); response += alias;
        response += alias;
    } else response.append("\xc0\x0c", 2);
    put16(response, type); put16(response, 1); response.append("\0\0\0\x3c", 4);
    if (hostname == "malformed.pace.test") { put16(response, 4); response += '\x7f'; return response; }
    if (type == 1) { put16(response, 4); response.append("\xc0\0\x02\x2a", 4); }
    else { put16(response, 16); response.append("\x20\x01\x0d\xb8", 4); response.append(11, '\0'); response += '\x2a'; }
    return response;
}
pace::task<void> nameserver(pace::udp::socket& socket, unsigned& queries) {
    std::array<char, 4096> buffer{};
    try {
        for (;;) {
            const auto packet = co_await socket.receive_from(buffer);
            ++queries; std::string hostname; unsigned type = 0;
            auto reply = make_reply(std::string_view(buffer.data(), packet.size), hostname, type);
            if (hostname == "silent.pace.test") continue;
            if (hostname == "tcp.pace.test") {
                // A legitimate truncated UDP response forces a TCP connection
                // and exercises c-ares socket-direction/descriptor transitions.
                reply[2] = static_cast<char>(0x83); reply[6] = 0; reply[7] = 0;
                reply.resize(12 + dns_name(hostname).size() + 4);
            }
            co_await socket.send_to(reply, packet.sender);
        }
    } catch (const std::system_error& error) {
        if (error.code() != std::errc::operation_canceled) throw;
    }
}
pace::task<void> tcp_nameserver(pace::tcp::listener& listener, unsigned& queries) {
    auto socket = co_await listener.accept();
    std::array<char, 2> prefix{};
    for (std::size_t n = 0; n < prefix.size();) {
        const auto count = co_await socket.read_some(std::span<char>(prefix).subspan(n));
        require(count != 0, "truncated TCP DNS prefix"); n += count;
    }
    std::string query(get16(std::string_view(prefix.data(), prefix.size()), 0), '\0');
    for (std::size_t n = 0; n < query.size();) {
        const auto count = co_await socket.read_some(std::span<char>(query).subspan(n));
        require(count != 0, "truncated TCP DNS question"); n += count;
    }
    ++queries; std::string hostname; unsigned type = 0;
    const auto answer = make_reply(query, hostname, type);
    std::string framed; put16(framed, answer.size()); framed += answer;
    co_await socket.write_all(framed);
}
pace::dns::options configuration(const pace::udp::socket& socket) {
    return {.timeout = 150ms, .tries = 1, .servers = "127.0.0.1:" + std::to_string(socket.local_endpoint().port)};
}
bool contains(const pace::dns::result& value, pace::dns::family type, std::string_view address) {
    for (const auto& item : value.addresses) if (item.type == type && item.text == address) return true;
    return false;
}
pace::task<void> checks(pace::io_context& io, pace::dns::options options, unsigned& queries, unsigned& tcp_queries) {
    pace::dns::resolver resolver(io, options);
    auto ipv4 = co_await resolver.resolve("address.pace.test", pace::dns::family::ipv4);
    require(ipv4.addresses.size() == 1 && contains(ipv4, pace::dns::family::ipv4, "192.0.2.42"), "A answer");
    auto ipv6 = co_await resolver.resolve("address.pace.test", pace::dns::family::ipv6);
    require(ipv6.addresses.size() == 1 && contains(ipv6, pace::dns::family::ipv6, "2001:db8::2a"), "AAAA answer");
    auto both = co_await resolver.resolve("address.pace.test");
    require(both.addresses.size() == 2 && contains(both, pace::dns::family::ipv4, "192.0.2.42") &&
            contains(both, pace::dns::family::ipv6, "2001:db8::2a"), "dual-stack answer");
    auto alias = co_await resolver.resolve("alias.pace.test", pace::dns::family::ipv4);
    require(contains(alias, pace::dns::family::ipv4, "192.0.2.42"), "CNAME answer");
    require(alias.canonical_name == "address.pace.test", "CNAME canonical name");
    auto fallback = co_await resolver.resolve("tcp.pace.test", pace::dns::family::ipv4);
    require(tcp_queries == 1 && contains(fallback, pace::dns::family::ipv4, "192.0.2.42"), "UDP to TCP DNS fallback");
    const auto before = queries;
    require(contains(co_await resolver.resolve("127.0.0.1"), pace::dns::family::ipv4, "127.0.0.1"), "numeric IPv4");
    require(contains(co_await resolver.resolve("::1"), pace::dns::family::ipv6, "::1"), "numeric IPv6");
    require(queries == before, "numeric lookups generated DNS traffic");
    for (const auto* name : {"missing.pace.test", "refused.pace.test", "malformed.pace.test", "silent.pace.test"}) {
        bool failed = false;
        try { (void)co_await resolver.resolve(name, pace::dns::family::ipv4); }
        catch (const std::system_error& e) {
            failed = e.code().category() == pace::dns::error_category() || e.code() == std::errc::timed_out;
            if (std::string_view(name) == "silent.pace.test") {
                require(e.code() == std::errc::timed_out, "silent DNS lookup did not reach deadline");
            }
        }
        require(failed, "malformed/error/timeout DNS answer was accepted");
    }
    // Failed queries leave the resolver reusable.
    require(contains(co_await resolver.resolve("address.pace.test", pace::dns::family::ipv4),
                     pace::dns::family::ipv4, "192.0.2.42"), "reuse after failure");
    for (std::string name : {std::string{}, std::string("bad name"), std::string("good\0evil", 9)}) {
        bool rejected = false;
        try { (void)co_await resolver.resolve(std::move(name)); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "hostname validation");
    }
    io.stop();
}
pace::task<void> pending(pace::task<pace::dns::result> lookup, bool& canceled) {
    try { (void)co_await std::move(lookup); }
    catch (const std::system_error& error) { canceled = error.code() == std::errc::operation_canceled; }
}
pace::task<void> cancel_lookup(pace::io_context& io, std::unique_ptr<pace::dns::resolver>& resolver,
                              bool& canceled, unsigned mode) {
    co_await io.yield();
    bool overlap = false;
    try { (void)co_await resolver->resolve("127.0.0.1"); } catch (const std::logic_error&) { overlap = true; }
    require(overlap, "overlapping DNS lookup was accepted");
    if (mode == 0) resolver->cancel();
    else if (mode == 1) resolver.reset();
    else { io.stop(); co_return; }
    require(canceled && !io.stopped(), "DNS cancel/destruction did not isolate query");
    if (resolver) require(contains(co_await resolver->resolve("127.0.0.1"), pace::dns::family::ipv4, "127.0.0.1"), "reuse after cancel");
    // Let canceled readiness tasks and the detached bounded timer finish.
    co_await io.sleep_for(120ms);
    io.stop();
}
void cancellation(unsigned mode) {
    pace::io_context io; pace::udp::socket server(io, {"127.0.0.1", 0}); unsigned queries = 0;
    auto resolver = std::make_unique<pace::dns::resolver>(io, configuration(server)); bool canceled = false;
    io.spawn(nameserver(server, queries));
    io.spawn(pending(resolver->resolve("silent.pace.test", pace::dns::family::ipv4), canceled));
    io.spawn(cancel_lookup(io, resolver, canceled, mode));
    io.run(); require(canceled, "DNS cancellation failed");
}
pace::task<void> expired_owner(pace::io_context& io, pace::task<pace::dns::result> work, bool& canceled) {
    co_await pending(std::move(work), canceled); io.stop();
}
pace::task<void> reentrant_lookup(pace::io_context& io, std::unique_ptr<pace::dns::resolver>& resolver,
                                 bool destroy, bool& canceled, bool& completed) {
    try { (void)co_await resolver->resolve("silent.pace.test", pace::dns::family::ipv4); }
    catch (const std::system_error& error) { canceled = error.code() == std::errc::operation_canceled; }
    require(canceled, "reentrant lookup cancellation");
    // cancel() resumes this continuation synchronously. Exercise both deletion
    // of its resolver and replacing the active query before cancel() returns.
    if (destroy) resolver.reset();
    else {
        require(contains(co_await resolver->resolve("address.pace.test", pace::dns::family::ipv4),
                         pace::dns::family::ipv4, "192.0.2.42"), "reentrant network lookup after cancel");
    }
    completed = true; io.stop();
}
pace::task<void> reentrant_cancel(pace::io_context& io, std::unique_ptr<pace::dns::resolver>& resolver,
                                 bool& canceled, bool& returned) {
    co_await io.yield();
    resolver->cancel();
    require(canceled, "cancel did not resume the lookup");
    returned = true;
}
void reentrant_cancellation(bool destroy) {
    pace::io_context io; pace::udp::socket server(io, {"127.0.0.1", 0}); unsigned queries = 0;
    auto resolver = std::make_unique<pace::dns::resolver>(io, configuration(server));
    bool canceled = false, completed = false, returned = false;
    io.spawn(nameserver(server, queries));
    io.spawn(reentrant_lookup(io, resolver, destroy, canceled, completed));
    io.spawn(reentrant_cancel(io, resolver, canceled, returned));
    io.run(); require(completed && returned, "reentrant cancellation incomplete");
    if (!destroy) require(queries >= 2, "reentrant lookup did not reach network");
}
pace::task<void> stalled_tcp_nameserver(pace::tcp::listener& listener, bool& accepted, bool& closed) {
    auto socket = co_await listener.accept();
    std::array<char, 4096> buffer{};
    require(co_await socket.read_some(buffer) != 0, "TCP fallback did not send query");
    accepted = true;
    while (co_await socket.read_some(buffer)) {}
    closed = true;
}
pace::task<void> cancel_tcp_lookup(pace::io_context& io, pace::dns::resolver& resolver,
                                  bool& accepted, bool& closed, bool& canceled) {
    while (!accepted) co_await io.sleep_for(1ms);
    resolver.cancel(); require(canceled, "TCP fallback cancellation");
    const auto deadline = std::chrono::steady_clock::now() + 200ms;
    while (!closed && std::chrono::steady_clock::now() < deadline) co_await io.sleep_for(1ms);
    require(closed, "canceled TCP fallback retained its socket");
    // A reused descriptor must not be affected by stale canceled watchers.
    require(contains(co_await resolver.resolve("address.pace.test", pace::dns::family::ipv4),
                     pace::dns::family::ipv4, "192.0.2.42"), "reuse after canceled TCP fallback");
    io.stop();
}
void tcp_cancellation() {
    pace::io_context io; pace::udp::socket server(io, {"127.0.0.1", 0}); unsigned queries = 0;
    pace::tcp::listener listener(io, {"127.0.0.1", server.local_endpoint().port});
    pace::dns::resolver resolver(io, configuration(server));
    bool accepted = false, closed = false, canceled = false;
    io.spawn(nameserver(server, queries)); io.spawn(stalled_tcp_nameserver(listener, accepted, closed));
    io.spawn(pending(resolver.resolve("tcp.pace.test", pace::dns::family::ipv4), canceled));
    io.spawn(cancel_tcp_lookup(io, resolver, accepted, closed, canceled));
    io.run(); require(accepted && closed && canceled, "TCP fallback cancellation incomplete");
}
}
int main() {
    watchdog deadline;
    try {
        { pace::io_context io; pace::udp::socket server(io, {"127.0.0.1", 0}); unsigned queries = 0, tcp_queries = 0;
          pace::tcp::listener tcp_server(io, {"127.0.0.1", server.local_endpoint().port});
          io.spawn(tcp_nameserver(tcp_server, tcp_queries));
          io.spawn(nameserver(server, queries)); io.spawn(checks(io, configuration(server), queries, tcp_queries)); io.run(); }
        for (unsigned mode = 0; mode < 3; ++mode) cancellation(mode);
        reentrant_cancellation(false); reentrant_cancellation(true); tcp_cancellation();
        { pace::io_context io; bool canceled = false; pace::task<pace::dns::result> work;
          { pace::dns::resolver resolver(io); work = resolver.resolve("127.0.0.1"); }
          io.spawn(expired_owner(io, std::move(work), canceled)); io.run(); require(canceled, "lazy destroyed resolver"); }
        std::cout << "DNS A/AAAA/CNAME, TCP fallback, local errors, malformed replies, timeout, cancellation reentrancy and lifetime passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
