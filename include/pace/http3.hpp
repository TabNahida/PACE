#pragma once
#include <pace/http_types.hpp>
#include <pace/quic.hpp>

#ifdef PACE_HAS_HTTP3
namespace pace::http3 {
struct limits {
    std::size_t max_header_bytes = 16 * 1024;
    std::size_t max_header_fields = 100;
    std::size_t max_body_bytes = 1024 * 1024;
    std::size_t max_concurrent_streams = 32;
    std::size_t max_requests_per_connection = 1000;
    // Cooperative application tasks per connection; 1 serializes handlers.
    // The effective cap is min(this value, max_concurrent_streams).
    std::size_t max_concurrent_handlers = 32;
};

// RFC 9114 over an authenticated QUIC connection with negotiated ALPN "h3".
// Owns the connection for a multiplexed batch; results preserve input order.
// Bodies are buffered within the per-stream limits. No CONNECT or server push.
task<std::vector<http::response>> exchange(quic::connection transport, std::string authority,
                                          std::vector<http::request> requests, limits bounds = {});
task<void> serve_connection(quic::connection transport, http::handler handle, limits bounds = {});
// The QUIC listener must use a server context configured with certificates and
// ALPN {"h3"}. Each connection owns its control and QPACK streams.
task<void> serve(quic::listener& listener, http::handler handle, limits bounds = {});
// One GET over an owned connection. For multiplexing, use exchange() with a batch.
inline task<http::response> get(quic::connection transport, std::string authority,
                         std::string target = "/", limits bounds = {}) {
    std::vector<http::request> requests(1);
    requests.front().method = "GET";
    requests.front().target = std::move(target);
    auto replies = co_await exchange(std::move(transport), std::move(authority), std::move(requests), bounds);
    co_return std::move(replies.front());
}
} // namespace pace::http3
#endif
