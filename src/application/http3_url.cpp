#include <pace/http3.hpp>
#ifdef PACE_HAS_HTTP3
#include <pace/detail/http_url.hpp>
namespace pace::http3 {
namespace {
task<http::response> send_url(io_context& io, std::string url, http::request request, limits bounds,
        const quic::context* trust = nullptr) {
    const auto destination = pace::detail::http_url::parse_url(url);
    if (!destination.secure) throw std::invalid_argument("HTTP/3 requires an https URL");
    pace::detail::http_url::prepare_url_request(destination, request);
    std::optional<quic::context> defaults;
    if (!trust) { defaults.emplace(); defaults->set_alpn_protocols({"h3"}); trust = &*defaults; }
    const auto addresses = co_await pace::detail::http_url::resolve_url(io, destination);
    // QUIC connects to the first resolved address; no implicit request retry.
    const auto& peer = addresses.front();
    auto transport = co_await quic::connect(io, *trust, {peer.address, peer.port, peer.family, peer.scope_id}, destination.host);
    std::vector<http::request> requests; requests.push_back(std::move(request));
    auto replies = co_await async_exchange(std::move(transport), destination.authority, std::move(requests), bounds);
    co_return std::move(replies.front());
}
}
task<http::response> async_fetch(io_context& io, std::string url, http::request request, limits bounds) {
    return send_url(io, std::move(url), std::move(request), bounds);
}
task<http::response> async_fetch(io_context& io, const quic::context& trust, std::string url,
        http::request request, limits bounds) {
    return send_url(io, std::move(url), std::move(request), bounds, &trust);
}
}
#endif
