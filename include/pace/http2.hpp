#pragma once
#include <pace/http_types.hpp>
#include <pace/tcp.hpp>
#include <pace/tls.hpp>

#ifdef PACE_HAS_HTTP2
namespace pace::http2 {
struct limits {
    std::size_t max_header_bytes = 16 * 1024;
    std::size_t max_header_fields = 100;
    std::size_t max_body_bytes = 1024 * 1024;
    std::size_t max_concurrent_streams = 100;
    std::size_t max_requests_per_connection = 1000;
    // Cooperative application tasks per connection; 1 serializes handlers.
    // The effective cap is min(this value, max_concurrent_streams).
    std::size_t max_concurrent_handlers = 100;
};

// HTTP/2 prior knowledge on cleartext TCP. Owns the connection for one batch;
// requests are submitted together and results preserve input order. Bodies are
// buffered with per-stream bounds. No HTTP/1 Upgrade or CONNECT tunnel support.
task<std::vector<http::response>> exchange(tcp::stream transport, std::string authority,
                                          std::vector<http::request> requests, limits bounds = {});
task<void> serve_connection(tcp::stream transport, http::handler handle, limits bounds = {});
task<void> serve(tcp::listener& listener, http::handler handle, limits bounds = {});

#ifdef PACE_HAS_TLS
// Transport must have completed a verified TLS handshake with ALPN "h2".
task<std::vector<http::response>> exchange(tls::stream transport, std::string authority,
                                          std::vector<http::request> requests, limits bounds = {});
task<void> serve_connection(tls::stream transport, http::handler handle, limits bounds = {});
// Configure certificates and ALPN {"h2"} before entering serve(). Configuration
// and listener must outlive the server. Each accepted stream performs handshake.
task<void> serve(tcp::listener& listener, const tls::context& configuration,
                 http::handler handle, limits bounds = {});
#endif
} // namespace pace::http2
#endif
