// Independent Linux load generator: no PACE headers or runtime. One outstanding
// request per connection, no pipelining, full status/length/body validation.
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <time.h>
using timer = std::chrono::steady_clock;
struct fd_owner { int value = -1; ~fd_owner() { if (value >= 0) close(value); } };
struct connection {
    int fd = -1;
    std::size_t sent = 0, completed = 0, header = 0;
    std::string input;
    timer::time_point begin;
};
const std::string query = "GET /data.bin HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
void fail(const char* message) { throw std::runtime_error(std::string(message) + ": " + std::strerror(errno)); }
double cpu_time() { timespec ts{}; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }
int main(int argc, char** argv) {
    std::vector<connection> clients;
    try {
        if (argc != 6) throw std::invalid_argument("usage: http_load PORT CONNECTIONS ITERATIONS WARMUP PAYLOAD_BYTES");
        const auto port = std::stoul(argv[1]); const auto count = std::stoul(argv[2]);
        const auto iterations = std::stoul(argv[3]); const auto warmup = std::stoul(argv[4]); const auto bytes = std::stoul(argv[5]);
        if (!count || count > 4096 || !iterations || bytes > 16*1024*1024) throw std::invalid_argument("invalid workload");
        clients.resize(count);
        fd_owner ep{epoll_create1(EPOLL_CLOEXEC)}; if (ep.value < 0) fail("epoll_create1");
        auto interest = [&](std::size_t i, unsigned flags, int op = EPOLL_CTL_MOD) {
            epoll_event event{}; event.events = flags; event.data.u32 = static_cast<unsigned>(i);
            if (epoll_ctl(ep.value, op, clients[i].fd, &event) < 0) fail("epoll_ctl");
        };
        for (std::size_t i = 0; i < count; ++i) {
            auto& c = clients[i]; c.fd = socket(AF_INET, SOCK_STREAM, 0); if (c.fd < 0) fail("socket");
            sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(static_cast<unsigned short>(port)); addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (connect(c.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) fail("connect");
            int yes = 1; setsockopt(c.fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
            if (fcntl(c.fd, F_SETFL, O_NONBLOCK) < 0) fail("fcntl");
            c.input.reserve(bytes + 4096); interest(i, EPOLLIN, EPOLL_CTL_ADD);
        }
        std::vector<double> latency; latency.reserve(count * iterations);
        auto phase = [&](std::size_t rounds, bool measure) {
            if (!rounds) return 0.0;
            const auto started = timer::now();
            for (std::size_t i = 0; i < count; ++i) {
                clients[i].sent = clients[i].completed = clients[i].header = 0;
                clients[i].input.clear(); clients[i].begin = started; interest(i, EPOLLIN | EPOLLOUT);
            }
            std::size_t total = 0;
            std::array<epoll_event, 256> events{};
            std::array<char, 65536> buffer{};
            while (total < count * rounds) {
                const int n = epoll_wait(ep.value, events.data(), events.size(), 15000);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) throw std::runtime_error("load generator readiness timeout/error");
                for (int e = 0; e < n; ++e) {
                    const auto index = events[e].data.u32; auto& c = clients[index];
                    if (events[e].events & EPOLLOUT) {
                        while (c.sent < query.size()) {
                            const auto sent = send(c.fd, query.data() + c.sent, query.size() - c.sent, MSG_NOSIGNAL);
                            if (sent < 0 && errno == EINTR) continue;
                            if (sent < 0 && errno == EAGAIN) break;
                            if (sent <= 0) fail("send");
                            c.sent += static_cast<std::size_t>(sent);
                        }
                        if (c.sent == query.size()) interest(index, EPOLLIN);
                    }
                    if (!(events[e].events & (EPOLLIN | EPOLLERR | EPOLLHUP))) continue;
                    bool finished = false;
                    for (;;) {
                        const auto received = recv(c.fd, buffer.data(), buffer.size(), 0);
                        if (received < 0 && errno == EINTR) continue;
                        if (received < 0 && errno == EAGAIN) break;
                        if (received <= 0) throw std::runtime_error("premature response EOF");
                        c.input.append(buffer.data(), static_cast<std::size_t>(received));
                        if (c.input.size() > bytes + 65536) throw std::runtime_error("oversized response");
                        if (!c.header) {
                            const auto boundary = c.input.find("\r\n\r\n");
                            if (boundary == c.input.npos) continue;
                            c.header = boundary + 4;
                            if (!c.input.starts_with("HTTP/1.1 200 ")) throw std::runtime_error("non-200 response");
                            std::string head = c.input.substr(0, boundary);
                            for (auto& ch : head) if (ch >= 'A' && ch <= 'Z') ch += 32;
                            auto field = head.find("\r\ncontent-length:");
                            if (field == head.npos || head.find("\r\ntransfer-encoding:") != head.npos) throw std::runtime_error("unexpected response framing");
                            field += 17; while (field < head.size() && head[field] == ' ') ++field;
                            std::size_t length = 0; const auto end = head.find("\r\n", field);
                            const auto span = std::string_view(head).substr(field, end == head.npos ? head.npos : end - field);
                            const auto [ptr, error] = std::from_chars(span.data(), span.data() + span.size(), length);
                            if (error != std::errc{} || ptr != span.data() + span.size() || length != bytes) throw std::runtime_error("response length mismatch");
                        }
                        if (c.input.size() < c.header + bytes) continue;
                        if (c.input.size() != c.header + bytes || !std::all_of(c.input.begin() + c.header, c.input.end(), [](char ch) { return ch == 'x'; }))
                            throw std::runtime_error("response bytes mismatch");
                        const auto now = timer::now();
                        if (measure) latency.push_back(std::chrono::duration<double, std::micro>(now - c.begin).count());
                        ++total; ++c.completed; finished = true; break;
                    }
                    if (finished && c.completed < rounds) {
                        c.input.clear(); c.sent = c.header = 0; c.begin = timer::now(); interest(index, EPOLLIN | EPOLLOUT);
                    }
                }
            }
            return std::chrono::duration<double>(timer::now() - started).count();
        };
        phase(warmup, false);
        const auto cpu_start = cpu_time(); const auto elapsed = phase(iterations, true); const auto cpu = cpu_time() - cpu_start;
        std::sort(latency.begin(), latency.end());
        auto percentile = [&](double p) { return latency[static_cast<std::size_t>((latency.size() - 1) * p)]; };
        std::cout << "{\"validated\":true,\"connections\":" << count << ",\"completed_requests\":" << count * iterations
                  << ",\"payload_bytes\":" << bytes << ",\"elapsed_seconds\":" << elapsed << ",\"requests_per_second\":" << count * iterations / elapsed
                  << ",\"p50_us\":" << percentile(.5) << ",\"p95_us\":" << percentile(.95) << ",\"p99_us\":" << percentile(.99)
                  << ",\"client_cpu_seconds\":" << cpu << "}\n";
        for (auto& c : clients) close(c.fd);
    } catch (const std::exception& error) {
        for (auto& c : clients) if (c.fd >= 0) close(c.fd);
        std::cerr << error.what() << '\n'; return 1;
    }
}
