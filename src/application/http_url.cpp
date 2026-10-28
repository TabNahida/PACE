#include <pace/detail/http_url.hpp>
#include <pace/http.hpp>
#include <pace/detail/socket_address.hpp>
#ifdef PACE_HAS_DNS
#include <pace/dns.hpp>
#endif
#include <algorithm>
#include <charconv>

namespace pace::http {
namespace {
using pace::detail::http_url::parse_url;
using pace::detail::http_url::connect_url;
task<response> send_url(io_context& io, std::string url, request outgoing, limits bounds, client_options options
#ifdef PACE_HAS_TLS
                       , const tls::context* configuration = nullptr
#endif
                       ) {
    const auto destination = parse_url(url);
#ifndef PACE_HAS_TLS
    if (destination.secure) throw std::system_error(std::make_error_code(std::errc::not_supported), "https URLs require PACE TLS support");
#endif
    if (options.close_timeout.count() < 0) throw std::invalid_argument("negative close timeout");
    outgoing.target = destination.target;
    outgoing.keep_alive = false;
    // The URL is authoritative for both Host and TLS identity.
    for (const auto& header : outgoing.headers) {
        auto name = header.name;
        for (char& c : name) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        if (name == "host") throw std::invalid_argument("URL fetch manages Host");
    }
    outgoing.headers.push_back({"Host", destination.authority});
#ifdef PACE_HAS_TLS
    if (configuration && !destination.secure) throw std::invalid_argument("TLS configuration requires an https URL");
    if (destination.secure) {
        std::optional<tls::context> defaults;
        if (!configuration) {
            defaults.emplace();
            defaults->set_alpn_protocols({"http/1.1"});
            configuration = &*defaults;
        }
        tls::stream transport(co_await connect_url(io, destination), *configuration);
        co_await transport.handshake(destination.host);
        secure_client connection(std::move(transport), bounds, options);
        co_return co_await connection.send(std::move(outgoing));
    }
#endif
    client connection(co_await connect_url(io, destination), bounds, options);
    co_return co_await connection.send(std::move(outgoing));
}
} // namespace

task<response> async_fetch(io_context& io, std::string url, request outgoing, limits bounds, client_options options) {
    return send_url(io, std::move(url), std::move(outgoing), bounds, options);
}
task<response> async_get(io_context& io, std::string url, limits bounds, client_options options) {
    return async_fetch(io, std::move(url), request{.method = "GET", .target = {}, .version = "HTTP/1.1", .body = {}}, bounds, options);
}
task<response> async_post(io_context& io, std::string url, std::string body, std::string type, limits bounds, client_options options) {
    return async_fetch(io, std::move(url), request{.method = "POST", .target = {}, .version = "HTTP/1.1", .headers = {{"Content-Type", std::move(type)}}, .body = std::move(body)}, bounds, options);
}
task<response> async_fetch(std::string url, request outgoing, limits bounds, client_options options) {
    co_return co_await async_fetch(this_context(), std::move(url), std::move(outgoing), bounds, options);
}
task<response> async_get(std::string url, limits bounds, client_options options) {
    co_return co_await async_get(this_context(), std::move(url), bounds, options);
}
task<response> async_post(std::string url, std::string body, std::string type, limits bounds, client_options options) {
    co_return co_await async_post(this_context(), std::move(url), std::move(body), std::move(type), bounds, options);
}
#ifdef PACE_HAS_TLS
task<response> async_fetch(io_context& io, const tls::context& configuration, std::string url, request outgoing,
                     limits bounds, client_options options) {
    return send_url(io, std::move(url), std::move(outgoing), bounds, options, &configuration);
}
task<response> async_get(io_context& io, const tls::context& configuration, std::string url, limits bounds, client_options options) {
    return async_fetch(io, configuration, std::move(url), request{.method = "GET", .target = {}, .version = "HTTP/1.1", .body = {}}, bounds, options);
}
#endif
} // namespace pace::http
