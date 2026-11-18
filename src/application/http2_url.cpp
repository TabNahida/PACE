#include <pace/http2.hpp>
#ifdef PACE_HAS_HTTP2
#include <pace/detail/http_url.hpp>
namespace pace::http2 {
namespace {
task<http::response> send_url(io_context& io, std::string url, http::request request, limits bounds
#ifdef PACE_HAS_TLS
        , const tls::context* trust = nullptr
#endif
        ) {
    const auto destination = pace::detail::http_url::parse_url(url);
    pace::detail::http_url::prepare_url_request(destination, request);
    std::vector<http::request> requests; requests.push_back(std::move(request));
#ifdef PACE_HAS_TLS
    if (trust && !destination.secure) throw std::invalid_argument("TLS configuration requires https URL");
    if (destination.secure) {
        std::optional<tls::context> defaults;
        if (!trust) { defaults.emplace(); defaults->set_alpn_protocols({"h2"}); trust = &*defaults; }
        tls::stream transport(co_await pace::detail::http_url::connect_url(io, destination), *trust);
        co_await transport.handshake(destination.host);
        auto replies = co_await async_exchange(std::move(transport), destination.authority, std::move(requests), bounds);
        co_return std::move(replies.front());
    }
#else
    if (destination.secure) throw std::system_error(std::make_error_code(std::errc::not_supported), "HTTPS requires TLS support");
#endif
    auto replies = co_await async_exchange(co_await pace::detail::http_url::connect_url(io, destination),
        destination.authority, std::move(requests), bounds);
    co_return std::move(replies.front());
}
}
task<http::response> async_fetch(io_context& io, std::string url, http::request request, limits bounds) {
    return send_url(io, std::move(url), std::move(request), bounds);
}
#ifdef PACE_HAS_TLS
task<http::response> async_fetch(io_context& io, const tls::context& trust, std::string url,
        http::request request, limits bounds) {
    return send_url(io, std::move(url), std::move(request), bounds, &trust);
}
#endif
}
#endif
