#include <pace/http.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace pace::http {
namespace {
struct protocol_error {
    unsigned status;
};

char lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

bool equal_ci(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
                                             [](char x, char y) { return lower(x) == lower(y); });
}

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

bool token_char(unsigned char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) != std::string_view::npos;
}

bool valid_token(std::string_view text) noexcept {
    return !text.empty() && std::all_of(text.begin(), text.end(), token_char);
}

bool valid_value(std::string_view text) noexcept {
    return std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return c == '\t' || (c >= 0x20 && c != 0x7f);
    });
}

bool contains_token(std::string_view value, std::string_view token) noexcept {
    while (!value.empty()) {
        const auto comma = value.find(',');
        if (equal_ci(trim(value.substr(0, comma)), token)) return true;
        if (comma == std::string_view::npos) break;
        value.remove_prefix(comma + 1);
    }
    return false;
}

std::string_view reason(unsigned status) noexcept {
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 205: return "Reset Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Content Too Large";
    case 417: return "Expectation Failed";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default: return "Response";
    }
}

request parse_head(std::string_view head, std::size_t& content_length, limits bounds) {
    request result;
    const auto line_end = head.find("\r\n");
    if (line_end == std::string_view::npos) throw protocol_error{400};
    const auto first = head.substr(0, line_end);
    const auto space1 = first.find(' ');
    if (space1 == std::string_view::npos) throw protocol_error{400};
    const auto space2 = first.find(' ', space1 + 1);
    if (space2 == std::string_view::npos || first.find(' ', space2 + 1) != std::string_view::npos)
        throw protocol_error{400};
    const auto method = first.substr(0, space1);
    const auto target = first.substr(space1 + 1, space2 - space1 - 1);
    const auto version = first.substr(space2 + 1);
    if (!valid_token(method) || target.empty() ||
        !std::all_of(target.begin(), target.end(), [](unsigned char c) { return c > 0x20 && c != 0x7f; }))
        throw protocol_error{400};
    if (version != "HTTP/1.1" && version != "HTTP/1.0") throw protocol_error{505};
    if (method == "CONNECT") throw protocol_error{501};
    result.method = method;
    result.target = target;
    result.version = version;
    result.keep_alive = version == "HTTP/1.1";
    head.remove_prefix(line_end + 2);

    bool has_length = false, has_transfer = false, has_host = false;
    bool connection_close = false, connection_keep_alive = false, expect = false, upgrade = false;
    content_length = 0;
    while (!head.empty()) {
        const auto end = head.find("\r\n");
        if (end == std::string_view::npos || end == 0) throw protocol_error{400};
        const auto line = head.substr(0, end);
        head.remove_prefix(end + 2);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) throw protocol_error{400};
        const auto name = line.substr(0, colon);
        const auto value = trim(line.substr(colon + 1));
        if (!valid_token(name) || !valid_value(value)) throw protocol_error{400};
        if (equal_ci(name, "content-length")) {
            if (has_length || value.empty()) throw protocol_error{400};
            has_length = true;
            const auto [ptr, error] = std::from_chars(value.data(), value.data() + value.size(), content_length);
            if (error != std::errc{} || ptr != value.data() + value.size()) throw protocol_error{400};
            if (content_length > bounds.max_body_bytes) throw protocol_error{413};
        } else if (equal_ci(name, "transfer-encoding")) {
            has_transfer = true;
        } else if (equal_ci(name, "host")) {
            if (has_host || value.empty() || value.find_first_of(" ,\t") != std::string_view::npos)
                throw protocol_error{400};
            has_host = true;
        } else if (equal_ci(name, "connection")) {
            connection_close |= contains_token(value, "close");
            connection_keep_alive |= contains_token(value, "keep-alive");
            upgrade |= contains_token(value, "upgrade");
        } else if (equal_ci(name, "expect")) {
            expect = true;
        } else if (equal_ci(name, "upgrade")) {
            upgrade = true;
        }
        result.headers.push_back({std::string(name), std::string(value)});
    }
    if (version == "HTTP/1.1" && !has_host) throw protocol_error{400};
    if (has_transfer) throw protocol_error{has_length ? 400u : 501u};
    if (upgrade) throw protocol_error{501};
    if (expect) throw protocol_error{417};
    result.keep_alive = !connection_close && (result.keep_alive || connection_keep_alive);
    return result;
}

task<std::optional<request>> read_request(tcp::stream& stream, std::string& pending,
                                        limits bounds, bool& head_request) {
    std::array<char, 8192> buffer{};
    // HEAD responses never carry payloads, including errors discovered after
    // recognizing the method but before the full request has been validated.
    head_request = pending.starts_with("HEAD ");
    auto delimiter = pending.find("\r\n\r\n");
    while (delimiter == std::string::npos) {
        if (pending.size() >= bounds.max_header_bytes) throw protocol_error{431};
        const auto allowed = std::min(buffer.size(), bounds.max_header_bytes - pending.size());
        const auto count = co_await stream.read_some(std::span<char>(buffer.data(), allowed));
        if (count == 0) {
            if (pending.empty()) co_return std::nullopt;
            throw protocol_error{400};
        }
        pending.append(buffer.data(), count);
        head_request = pending.starts_with("HEAD ");
        delimiter = pending.find("\r\n\r\n");
    }
    const auto head_size = delimiter + 4;
    if (head_size > bounds.max_header_bytes) throw protocol_error{431};
    std::size_t length = 0;
    auto result = parse_head(std::string_view(pending).substr(0, head_size - 2), length, bounds);
    // Consume the head before the body, avoiding arithmetic overflow for custom limits.
    pending.erase(0, head_size);
    while (pending.size() < length) {
        const auto wanted = std::min(buffer.size(), length - pending.size());
        const auto count = co_await stream.read_some(std::span<char>(buffer.data(), wanted));
        if (count == 0) throw protocol_error{400};
        pending.append(buffer.data(), count);
    }
    result.body.assign(pending.data(), length);
    pending.erase(0, length);
    co_return result;
}

std::string encode_response(const response& result, bool keep_alive, bool head_request) {
    if (result.status < 200 || result.status > 599)
        throw std::invalid_argument("HTTP final response status must be in [200, 599]");
    if (!valid_value(result.content_type)) throw std::invalid_argument("invalid HTTP content type");
    const bool omit_length = result.status == 204 || result.status == 304;
    const bool no_body = omit_length || result.status == 205;
    std::string output = "HTTP/1.1 " + std::to_string(result.status) + " " + std::string(reason(result.status)) + "\r\n";
    if (!omit_length)
        output += "Content-Length: " + std::to_string(result.status == 205 ? 0 : result.body.size()) + "\r\n";
    if (!result.content_type.empty()) output += "Content-Type: " + result.content_type + "\r\n";
    output += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    for (const auto& field : result.headers) {
        if (!valid_token(field.name) || !valid_value(field.value))
            throw std::invalid_argument("invalid HTTP response header");
        if (equal_ci(field.name, "content-length") || equal_ci(field.name, "transfer-encoding") ||
            equal_ci(field.name, "connection") || equal_ci(field.name, "content-type"))
            throw std::invalid_argument("HTTP framing and content-type headers are managed by PACE");
        output += field.name + ": " + field.value + "\r\n";
    }
    output += "\r\n";
    if (!head_request && !no_body) output += result.body;
    return output;
}

task<void> process_connection(tcp::stream& stream, const handler& handle, limits bounds) {
    std::string pending;
    for (std::size_t count = 0; count < bounds.max_requests_per_connection; ++count) {
        std::optional<request> incoming;
        response outgoing;
        bool keep_alive = false, head_request = false;
        unsigned error_status = 0;
        try {
            incoming = co_await read_request(stream, pending, bounds, head_request);
        } catch (const protocol_error& error) {
            error_status = error.status;
        }
        if (error_status != 0) {
            outgoing.status = error_status;
            outgoing.body = std::string(reason(error_status)) + "\n";
        } else {
            if (!incoming) co_return;
            keep_alive = incoming->keep_alive && count + 1 < bounds.max_requests_per_connection;
            head_request = incoming->method == "HEAD";
            try {
                outgoing = co_await handle(std::move(*incoming));
                keep_alive = keep_alive && !outgoing.close;
            } catch (const std::system_error& error) {
                if (error.code() == std::errc::operation_canceled) throw;
                error_status = 500;
            } catch (...) {
                error_status = 500;
            }
            if (error_status != 0) {
                outgoing = response{.status = 500, .body = "Internal Server Error\n"};
                keep_alive = false;
            }
        }
        std::string encoded;
        try {
            encoded = encode_response(outgoing, keep_alive, head_request);
        } catch (const std::invalid_argument&) {
            keep_alive = false;
            encoded = encode_response(response{.status = 500, .body = "Internal Server Error\n"}, false, head_request);
        }
        co_await stream.write_all(encoded);
        if (!keep_alive) {
            co_await stream.close_gracefully();
            co_return;
        }
    }
}
} // namespace

std::string_view request::header(std::string_view name) const noexcept {
    for (const auto& field : headers)
        if (equal_ci(field.name, name)) return field.value;
    return {};
}

task<void> serve_connection(tcp::stream stream, handler handle, limits bounds) {
    try {
        stream.set_no_delay();
        co_await process_connection(stream, handle, bounds);
    } catch (const std::system_error&) {
        // EOF/reset, broken pipe and context cancellation are per-client events.
        // The owning stream closes here, unregistering both directions.
    }
}

task<void> serve(tcp::listener& listener, handler handle, limits bounds) {
    for (;;) {
        try {
            auto stream = co_await listener.accept();
            listener.context().spawn(serve_connection(std::move(stream), handle, bounds));
        } catch (const std::system_error& error) {
            if (error.code() == std::errc::operation_canceled || error.code() == std::errc::bad_file_descriptor)
                co_return;
            throw;
        }
    }
}

} // namespace pace::http
