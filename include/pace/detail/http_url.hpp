#pragma once
#include <pace/http_types.hpp>
#include <pace/tcp.hpp>
#include <pace/detail/socket_address.hpp>
#ifdef PACE_HAS_DNS
#include <pace/dns.hpp>
#endif
#include <charconv>
namespace pace::detail::http_url {
struct destination {
    bool secure = false;
    bool numeric = false;
    std::string host, authority, target;
    std::uint16_t port = 80;
};

inline destination parse_url(std::string_view url) {
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

inline task<std::vector<tcp::endpoint>> resolve_url(io_context& io, const destination& url) {
    std::vector<tcp::endpoint> result;
    if (url.numeric) { result.push_back({url.host, url.port}); co_return result; }
#ifdef PACE_HAS_DNS
    dns::resolver resolver(io);
    const auto resolved = co_await resolver.resolve(url.host);
    for (const auto& address : resolved.addresses) result.push_back({address.text, url.port});
    if (result.empty()) throw std::system_error(std::make_error_code(std::errc::host_unreachable), "DNS returned no addresses");
    co_return result;
#else
    (void)io;
    throw std::system_error(std::make_error_code(std::errc::not_supported), "hostname URLs require PACE DNS support");
#endif
}
inline task<tcp::stream> connect_url(io_context& io, const destination& url) {
    const auto addresses = co_await resolve_url(io, url);
    std::exception_ptr last;
    for (const auto& address : addresses) {
        try { co_return co_await tcp::connect(io, address); }
        catch (const std::system_error& error) {
            if (error.code() == std::errc::operation_canceled || error.code() == std::errc::timed_out) throw;
            last = std::current_exception();
        }
    }
    std::rethrow_exception(last);
}
inline void prepare_url_request(const destination& url, http::request& request) {
    request.target = url.target;
    request.keep_alive = false;
    for (auto& header : request.headers) {
        auto name = header.name;
        for (char& c : name) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        if (name == "host" || name.starts_with(':')) throw std::invalid_argument("URL client manages authority and target");
    }
}
}
