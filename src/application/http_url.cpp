#include <pace/http.hpp>
#include <pace/detail/socket_address.hpp>
#ifdef PACE_HAS_DNS
#include <pace/dns.hpp>
#endif
#include <algorithm>
#include <charconv>

namespace pace::http {
namespace {
struct destination {
    bool secure = false;
    bool numeric = false;
    std::string host, authority, target;
    std::uint16_t port = 80;
};

destination parse_url(std::string_view url) {
    destination result;
    for (unsigned char c : url)
        if (c <= 32 || c >= 127 || c == '\\') throw std::invalid_argument("URL must be ASCII without spaces or backslashes");
    const auto scheme_end = url.find("://");
    auto scheme = std::string(url.substr(0, scheme_end));
    for (char& c : scheme) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    if (scheme_end == url.npos || (scheme != "http" && scheme != "https"))
        throw std::invalid_argument("expected an absolute http or https URL");
    result.secure = scheme == "https";
    result.port = result.secure ? 443 : 80;
    url.remove_prefix(scheme_end + 3);
    const auto authority_end = url.find_first_of("/?#");
    const auto authority = url.substr(0, authority_end);
    if (authority.empty() || authority.find('@') != authority.npos)
        throw std::invalid_argument("URL needs a host; userinfo is unsupported");
    result.authority = authority;
    std::string_view host, port;
    bool explicit_port = false, bracketed = authority.front() == '[';
    if (bracketed) {
        const auto end = authority.find(']');
        if (end == authority.npos) throw std::invalid_argument("unterminated IPv6 URL host");
        host = authority.substr(1, end - 1);
        if (end + 1 != authority.size()) {
            if (authority[end + 1] != ':') throw std::invalid_argument("invalid URL authority");
            explicit_port = true; port = authority.substr(end + 2);
        }
    } else {
        const auto colon = authority.find(':');
        host = authority.substr(0, colon);
        if (colon != authority.npos) { explicit_port = true; port = authority.substr(colon + 1); }
    }
    if (explicit_port) {
        unsigned parsed = 0;
        const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), parsed);
        if (error != std::errc{} || end != port.data() + port.size() || parsed == 0 || parsed > 65535)
            throw std::invalid_argument("invalid URL port");
        result.port = static_cast<std::uint16_t>(parsed);
    }
    result.host = host;
    try {
        const auto address = pace::detail::parse_socket_address(result.host, result.port);
        if (bracketed != (address.family() == address_family::ipv6))
            throw std::invalid_argument("IPv6 URL hosts require brackets");
        result.numeric = true;
    } catch (const std::invalid_argument&) {
        if (bracketed || host.empty() || host.size() > 253 ||
            host.find_first_not_of("0123456789.") == host.npos)
            throw std::invalid_argument("invalid URL host");
        auto remaining = host;
        while (!remaining.empty()) {
            const auto dot = remaining.find('.');
            const auto label = remaining.substr(0, dot);
            if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-')
                throw std::invalid_argument("invalid URL hostname label");
            for (char c : label)
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-')) throw std::invalid_argument("invalid URL hostname");
            if (dot == remaining.npos) break;
            remaining.remove_prefix(dot + 1);
        }
    }
    auto target = authority_end == url.npos ? std::string_view{} : url.substr(authority_end);
    target = target.substr(0, target.find('#'));
    result.target = target.empty() || target.front() == '?' ? "/" + std::string(target) : std::string(target);
    return result;
}

task<tcp::stream> connect_url(io_context& io, const destination& url) {
    if (url.numeric) co_return co_await tcp::connect(io, {url.host, url.port});
#ifdef PACE_HAS_DNS
    dns::resolver resolver(io);
    const auto addresses = co_await resolver.resolve(url.host);
    std::exception_ptr last;
    for (const auto& address : addresses.addresses) {
        try { co_return co_await tcp::connect(io, {address.text, url.port}); }
        catch (const std::system_error& error) {
            if (error.code() == std::errc::operation_canceled) throw;
            last = std::current_exception();
        }
    }
    if (last) std::rethrow_exception(last);
    throw std::system_error(std::make_error_code(std::errc::host_unreachable), "DNS returned no addresses");
#else
    throw std::system_error(std::make_error_code(std::errc::not_supported), "hostname URLs require PACE DNS support");
#endif
}

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
