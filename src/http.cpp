#include <pace/http.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace pace::http {
namespace {
struct parse_failure {
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

struct framing {
    std::optional<std::size_t> length;
    bool chunked = false;
    bool keep_alive = false;
    std::vector<std::string> connection_fields;
};

bool forbidden_trailer(std::string_view name) {
    constexpr std::string_view forbidden[]{"content-length", "transfer-encoding", "host", "connection",
        "trailer", "te", "upgrade", "expect", "authorization", "proxy-authorization", "proxy-authenticate",
        "www-authenticate", "cookie", "set-cookie", "content-type", "content-encoding", "content-range",
        "cache-control", "max-forwards", "range"};
    return std::any_of(std::begin(forbidden), std::end(forbidden),
                       [&](auto value) { return equal_ci(name, value); });
}

std::vector<std::string> token_list(std::string_view value) {
    std::vector<std::string> result;
    for (;;) {
        const auto comma = value.find(',');
        auto token = trim(value.substr(0, comma));
        if (!valid_token(token)) throw parse_failure{400};
        result.emplace_back(token);
        if (comma == std::string_view::npos) return result;
        value.remove_prefix(comma + 1);
    }
}

header_field parse_field(std::string_view line) {
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) throw parse_failure{400};
    const auto name = line.substr(0, colon);
    const auto value = trim(line.substr(colon + 1));
    if (!valid_token(name) || !valid_value(value)) throw parse_failure{400};
    return {std::string(name), std::string(value)};
}

std::vector<header_field> parse_fields(std::string_view text, std::size_t maximum) {
    std::vector<header_field> fields;
    while (!text.empty()) {
        const auto end = text.find("\r\n");
        if (end == std::string_view::npos || end == 0) throw parse_failure{400};
        if (fields.size() >= maximum) throw parse_failure{431};
        fields.push_back(parse_field(text.substr(0, end)));
        text.remove_prefix(end + 2);
    }
    return fields;
}

framing parse_framing(const std::vector<header_field>& fields, std::string_view version,
                       bool request_message) {
    framing result;
    result.keep_alive = version == "HTTP/1.1";
    bool has_transfer = false, host = false, close = false, persistent = false;
    std::vector<std::string> declared_trailers;
    for (const auto& field : fields) {
        const auto name = std::string_view(field.name), value = std::string_view(field.value);
        if (equal_ci(name, "content-length")) {
            if (result.length || value.empty()) throw parse_failure{400};
            std::size_t length = 0;
            auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), length);
            if (error != std::errc{} || end != value.data() + value.size()) throw parse_failure{400};
            result.length = length;
        } else if (equal_ci(name, "transfer-encoding")) {
            if (has_transfer) throw parse_failure{400};
            has_transfer = true;
            const auto codings = token_list(value);
            if (codings.size() != 1 || !equal_ci(codings.front(), "chunked")) {
                // Ambiguous framing still takes precedence over unsupported coding.
                for (const auto& candidate : fields)
                    if (equal_ci(candidate.name, "content-length")) throw parse_failure{400};
                throw parse_failure{501};
            }
            result.chunked = true;
        } else if (equal_ci(name, "host") && request_message) {
            if (host || value.empty() || value.find_first_of(" ,\t") != std::string_view::npos)
                throw parse_failure{400};
            host = true;
        } else if (equal_ci(name, "connection")) {
            for (auto& token : token_list(value)) {
                if (equal_ci(token, "content-length") || equal_ci(token, "transfer-encoding") ||
                    equal_ci(token, "host") || equal_ci(token, "trailer")) throw parse_failure{400};
                close |= equal_ci(token, "close");
                persistent |= equal_ci(token, "keep-alive");
                if (request_message && equal_ci(token, "upgrade")) throw parse_failure{501};
                result.connection_fields.push_back(std::move(token));
            }
        } else if (equal_ci(name, "trailer")) {
            for (auto& token : token_list(value)) declared_trailers.push_back(std::move(token));
        } else if (request_message && equal_ci(name, "expect")) {
            throw parse_failure{417};
        } else if (request_message && equal_ci(name, "upgrade")) {
            throw parse_failure{501};
        }
    }
    if (request_message && version == "HTTP/1.1" && !host) throw parse_failure{400};
    if (has_transfer && (result.length || version != "HTTP/1.1")) throw parse_failure{400};
    for (const auto& name : declared_trailers) {
        if (!result.chunked || forbidden_trailer(name) ||
            std::any_of(result.connection_fields.begin(), result.connection_fields.end(),
                        [&](const auto& field) { return equal_ci(field, name); })) throw parse_failure{400};
    }
    result.keep_alive = !close && (result.keep_alive || persistent);
    return result;
}

template<class Transport>
task<std::optional<std::string>> read_head(Transport& stream, std::string& pending,
                                           limits bounds, bool* head_request = nullptr) {
    std::array<char, 8192> buffer{};
    for (;;) {
        if (head_request) *head_request = pending.starts_with("HEAD ");
        const auto end = pending.find("\r\n\r\n");
        if (end != std::string::npos) {
            if (end + 4 > bounds.max_header_bytes) throw parse_failure{431};
            auto head = pending.substr(0, end + 2);
            pending.erase(0, end + 4);
            co_return head;
        }
        if (pending.size() >= bounds.max_header_bytes) throw parse_failure{431};
        const auto wanted = std::min(buffer.size(), bounds.max_header_bytes - pending.size());
        const auto count = co_await stream.read_some(std::span<char>(buffer.data(), wanted));
        if (!count) {
            if (pending.empty()) co_return std::nullopt;
            throw parse_failure{400};
        }
        pending.append(buffer.data(), count);
    }
}

template<class Transport>
task<void> ensure_bytes(Transport& stream, std::string& pending, std::size_t size) {
    std::array<char, 8192> buffer{};
    while (pending.size() < size) {
        const auto wanted = std::min(buffer.size(), size - pending.size());
        const auto count = co_await stream.read_some(std::span<char>(buffer.data(), wanted));
        if (!count) throw parse_failure{400};
        pending.append(buffer.data(), count);
    }
}

template<class Transport>
task<std::string> read_line(Transport& stream, std::string& pending, std::size_t maximum) {
    std::array<char, 1024> buffer{};
    for (;;) {
        const auto end = pending.find("\r\n");
        if (end != std::string::npos) {
            if (end + 2 > maximum) throw parse_failure{431};
            auto line = pending.substr(0, end);
            pending.erase(0, end + 2);
            co_return line;
        }
        if (pending.size() >= maximum) throw parse_failure{431};
        const auto count = co_await stream.read_some(std::span<char>(buffer.data(), std::min(buffer.size(), maximum - pending.size())));
        if (!count) throw parse_failure{400};
        pending.append(buffer.data(), count);
    }
}

std::size_t chunk_size(std::string_view line) {
    const auto semicolon = line.find(';');
    auto digits = line.substr(0, semicolon);
    if (semicolon != std::string_view::npos)
        while (!digits.empty() && (digits.back() == ' ' || digits.back() == '\t')) digits.remove_suffix(1);
    // Strict hexadecimal: no sign, prefix, whitespace or empty number.
    if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        })) throw parse_failure{400};
    std::size_t size = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), size, 16);
    if (error != std::errc{} || end != digits.data() + digits.size()) throw parse_failure{400};
    if (semicolon == std::string_view::npos) return size;
    line.remove_prefix(semicolon);
    // Extensions are syntactically checked and ignored; line bytes are bounded.
    while (!line.empty()) {
        if (line.front() != ';') throw parse_failure{400};
        line.remove_prefix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        std::size_t length = 0;
        while (length < line.size() && token_char(static_cast<unsigned char>(line[length]))) ++length;
        if (length == 0) throw parse_failure{400};
        line.remove_prefix(length);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (!line.empty() && line.front() == '=') {
            line.remove_prefix(1);
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
            if (line.empty()) throw parse_failure{400};
            if (line.front() == '"') {
                line.remove_prefix(1);
                bool ended = false;
                while (!line.empty()) {
                    unsigned char c = static_cast<unsigned char>(line.front()); line.remove_prefix(1);
                    if (c == '"') { ended = true; break; }
                    if (c == '\\') {
                        if (line.empty()) throw parse_failure{400};
                        c = static_cast<unsigned char>(line.front()); line.remove_prefix(1);
                    }
                    if (!(c == '\t' || (c >= 0x20 && c != 0x7f))) throw parse_failure{400};
                }
                if (!ended) throw parse_failure{400};
            } else {
                length = 0;
                while (length < line.size() && token_char(static_cast<unsigned char>(line[length]))) ++length;
                if (length == 0) throw parse_failure{400};
                line.remove_prefix(length);
            }
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        }
    }
    return size;
}

template<class Transport>
task<void> read_body(Transport& stream, std::string& pending, const framing& frame,
                     limits bounds, std::string& body, std::vector<header_field>& trailers) {
    if (!frame.chunked) {
        const auto length = frame.length.value_or(0);
        if (length > bounds.max_body_bytes) throw parse_failure{413};
        co_await ensure_bytes(stream, pending, length);
        body.assign(pending.data(), length);
        pending.erase(0, length);
        co_return;
    }
    for (std::size_t chunks = 0;; ++chunks) {
        if (chunks >= bounds.max_chunks) throw parse_failure{413};
        const auto line = co_await read_line(stream, pending, bounds.max_chunk_line_bytes);
        const auto size = chunk_size(line);
        if (size > bounds.max_body_bytes - body.size()) throw parse_failure{413};
        if (!size) break;
        // Read data and terminator independently to avoid size+2 overflow.
        co_await ensure_bytes(stream, pending, size);
        body.append(pending.data(), size);
        pending.erase(0, size);
        co_await ensure_bytes(stream, pending, 2);
        if (!pending.starts_with("\r\n")) throw parse_failure{400};
        pending.erase(0, 2);
    }
    std::size_t trailer_bytes = 0;
    for (;;) {
        auto line = co_await read_line(stream, pending, bounds.max_trailer_bytes - trailer_bytes);
        trailer_bytes += line.size() + 2;
        if (line.empty()) break;
        if (trailers.size() >= bounds.max_trailer_fields) throw parse_failure{431};
        auto field = parse_field(line);
        if (forbidden_trailer(field.name) || std::any_of(frame.connection_fields.begin(), frame.connection_fields.end(),
                [&](const auto& name) { return equal_ci(name, field.name); })) throw parse_failure{400};
        trailers.push_back(std::move(field));
    }
}

template<class Transport>
task<std::optional<request>> read_request(Transport& stream, std::string& pending,
                                         limits bounds, bool& head_request) {
    auto head = co_await read_head(stream, pending, bounds, &head_request);
    if (!head) co_return std::nullopt;
    const auto line_end = head->find("\r\n");
    const auto first = std::string_view(*head).substr(0, line_end);
    const auto space1 = first.find(' '), space2 = first.find(' ', space1 == std::string_view::npos ? 0 : space1 + 1);
    if (space1 == std::string_view::npos || space2 == std::string_view::npos || first.find(' ', space2 + 1) != std::string_view::npos)
        throw parse_failure{400};
    request result;
    result.method = first.substr(0, space1);
    result.target = first.substr(space1 + 1, space2 - space1 - 1);
    result.version = first.substr(space2 + 1);
    if (!valid_token(result.method) || result.target.empty() || !std::all_of(result.target.begin(), result.target.end(),
            [](unsigned char c) { return c > 0x20 && c != 0x7f; })) throw parse_failure{400};
    if (result.version != "HTTP/1.1" && result.version != "HTTP/1.0") throw parse_failure{505};
    if (result.method == "CONNECT") throw parse_failure{501};
    result.headers = parse_fields(std::string_view(*head).substr(line_end + 2), bounds.max_header_fields);
    const auto frame = parse_framing(result.headers, result.version, true);
    result.keep_alive = frame.keep_alive;
    result.chunked = frame.chunked;
    co_await read_body(stream, pending, frame, bounds, result.body, result.trailers);
    co_return result;
}

void validate_output_field(const header_field& field) {
    if (!valid_token(field.name) || !valid_value(field.value))
        throw std::invalid_argument("invalid HTTP header");
}

bool managed_field(std::string_view name) {
    return equal_ci(name, "content-length") || equal_ci(name, "transfer-encoding") ||
           equal_ci(name, "connection") || equal_ci(name, "trailer");
}

std::string trailer_declaration(const std::vector<header_field>& trailers) {
    std::string result;
    for (const auto& field : trailers) {
        validate_output_field(field);
        if (forbidden_trailer(field.name)) throw std::invalid_argument("forbidden HTTP trailer");
        if (!result.empty()) result += ", ";
        result += field.name;
    }
    return result.empty() ? "" : "Trailer: " + result + "\r\n";
}

std::string encode_chunks(std::string_view body, const std::vector<header_field>& trailers) {
    std::string result;
    if (!body.empty()) {
        std::array<char, 2 * sizeof(std::size_t)> hex{};
        const auto [end, error] = std::to_chars(hex.data(), hex.data() + hex.size(), body.size(), 16);
        if (error != std::errc{}) throw std::length_error("HTTP body length");
        result.append(hex.data(), end);
        result += "\r\n"; result += body; result += "\r\n";
    }
    result += "0\r\n";
    for (const auto& field : trailers) result += field.name + ": " + field.value + "\r\n";
    result += "\r\n";
    return result;
}

std::string encode_response(const response& result, bool keep_alive, bool head_request, bool http10 = false) {
    if (result.status < 200 || result.status > 599)
        throw std::invalid_argument("HTTP final response status must be in [200, 599]");
    if (!valid_value(result.content_type)) throw std::invalid_argument("invalid HTTP content type");
    const bool omit_length = result.status == 204 || result.status == 304;
    const bool no_body = omit_length || result.status == 205;
    if (!result.trailers.empty() && (!result.chunked || http10))
        throw std::invalid_argument("trailers require an HTTP/1.1 chunked body");
    const bool chunked = result.chunked && !no_body && !http10;
    const auto declared = trailer_declaration(result.trailers);
    std::string output = "HTTP/1.1 " + std::to_string(result.status) + " " + std::string(reason(result.status)) + "\r\n";
    if (chunked) output += "Transfer-Encoding: chunked\r\n" + (head_request ? std::string{} : declared);
    else if (!omit_length) output += "Content-Length: " + std::to_string(result.status == 205 ? 0 : result.body.size()) + "\r\n";
    if (!result.content_type.empty()) output += "Content-Type: " + result.content_type + "\r\n";
    output += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    for (const auto& field : result.headers) {
        validate_output_field(field);
        if (managed_field(field.name) || equal_ci(field.name, "content-type"))
            throw std::invalid_argument("HTTP framing and content-type headers are managed by PACE");
        output += field.name + ": " + field.value + "\r\n";
    }
    output += "\r\n";
    if (!head_request && !no_body) output += chunked ? encode_chunks(result.body, result.trailers) : result.body;
    return output;
}

std::string encode_request(const request& outgoing) {
    const auto version = outgoing.version.empty() ? std::string_view("HTTP/1.1") : std::string_view(outgoing.version);
    if (!valid_token(outgoing.method) || outgoing.method == "CONNECT" || outgoing.target.empty() ||
        !std::all_of(outgoing.target.begin(), outgoing.target.end(), [](unsigned char c) { return c > 0x20 && c != 0x7f; }) ||
        (version != "HTTP/1.1" && version != "HTTP/1.0")) throw std::invalid_argument("invalid HTTP request line");
    if ((!outgoing.trailers.empty() && !outgoing.chunked) || (outgoing.chunked && version != "HTTP/1.1"))
        throw std::invalid_argument("trailers/chunks require HTTP/1.1 chunked framing");
    bool host = false;
    std::string result = outgoing.method + " " + outgoing.target + " " + std::string(version) + "\r\n";
    for (const auto& field : outgoing.headers) {
        validate_output_field(field);
        if (managed_field(field.name) || equal_ci(field.name, "expect") || equal_ci(field.name, "upgrade"))
            throw std::invalid_argument("HTTP framing/upgrade/expect header is managed or unsupported");
        if (equal_ci(field.name, "host")) {
            if (host || field.value.empty() || field.value.find_first_of(" ,\t") != std::string::npos)
                throw std::invalid_argument("invalid HTTP Host");
            host = true;
        }
        result += field.name + ": " + field.value + "\r\n";
    }
    if (!host && version == "HTTP/1.1") throw std::invalid_argument("HTTP/1.1 client requires Host");
    result += outgoing.keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    if (outgoing.chunked) result += "Transfer-Encoding: chunked\r\n" + trailer_declaration(outgoing.trailers);
    else result += "Content-Length: " + std::to_string(outgoing.body.size()) + "\r\n";
    result += "\r\n";
    result += outgoing.chunked ? encode_chunks(outgoing.body, outgoing.trailers) : outgoing.body;
    return result;
}

template<class Transport>
task<response> read_response(Transport& stream, std::string& pending, limits bounds, std::string_view method) {
    for (std::size_t informational = 0;;) {
        auto head = co_await read_head(stream, pending, bounds);
        if (!head) throw parse_failure{400};
        const auto end = head->find("\r\n");
        const auto first = std::string_view(*head).substr(0, end);
        if (first.size() < 13 || (first.substr(0, 8) != "HTTP/1.1" && first.substr(0, 8) != "HTTP/1.0") ||
            first[8] != ' ' || first[12] != ' ' || !valid_value(first.substr(13))) throw parse_failure{400};
        response result;
        result.version = first.substr(0, 8);
        const auto [finish, error] = std::from_chars(first.data() + 9, first.data() + 12, result.status);
        if (error != std::errc{} || finish != first.data() + 12 || result.status < 100 || result.status > 599)
            throw parse_failure{400};
        result.headers = parse_fields(std::string_view(*head).substr(end + 2), bounds.max_header_fields);
        const auto frame = parse_framing(result.headers, result.version, false);
        result.content_type = result.header("content-type");
        result.chunked = frame.chunked;
        result.close = !frame.keep_alive;
        if (result.status < 200) {
            if (result.status == 101 || frame.length || frame.chunked || ++informational > bounds.max_informational_responses)
                throw parse_failure{400};
            continue;
        }
        if (result.status == 204 && (frame.length || frame.chunked)) throw parse_failure{400};
        if (method == "HEAD" || result.status == 204 || result.status == 304) co_return result;
        if (frame.chunked || frame.length) {
            co_await read_body(stream, pending, frame, bounds, result.body, result.trailers);
        } else {
            // An unframed response is delimited by EOF and can never be reused.
            result.close = true;
            if (pending.size() > bounds.max_body_bytes) throw parse_failure{413};
            result.body = std::move(pending); pending.clear();
            std::array<char, 8192> buffer{};
            for (;;) {
                const auto count = co_await stream.read_some(buffer);
                if (!count) break;
                if (count > bounds.max_body_bytes - result.body.size()) throw parse_failure{413};
                result.body.append(buffer.data(), count);
            }
        }
        if (result.status == 205 && !result.body.empty()) throw parse_failure{400};
        co_return result;
    }
}

template<class Transport>
task<void> process_connection(Transport& stream, const handler& handle, limits bounds) {
    std::string pending;
    for (std::size_t count = 0; count < bounds.max_requests_per_connection; ++count) {
        std::optional<request> incoming;
        response outgoing;
        bool keep_alive = false, head_request = false, http10 = false;
        unsigned error_status = 0;
        try { incoming = co_await read_request(stream, pending, bounds, head_request); }
        catch (const parse_failure& error) { error_status = error.status; }
        if (error_status) {
            outgoing.status = error_status;
            outgoing.body = std::string(reason(error_status)) + "\n";
        } else {
            if (!incoming) co_return;
            keep_alive = incoming->keep_alive && count + 1 < bounds.max_requests_per_connection;
            head_request = incoming->method == "HEAD";
            http10 = incoming->version == "HTTP/1.0";
            try {
                outgoing = co_await handle(std::move(*incoming));
                keep_alive = keep_alive && !outgoing.close;
            } catch (const std::system_error& error) {
                if (error.code() == std::errc::operation_canceled) throw;
                error_status = 500;
            } catch (...) { error_status = 500; }
            if (error_status) {
                outgoing = response{.status = 500, .body = "Internal Server Error\n"};
                keep_alive = false;
            }
        }
        std::string encoded;
        try { encoded = encode_response(outgoing, keep_alive, head_request, http10); }
        catch (const std::invalid_argument&) {
            keep_alive = false;
            encoded = encode_response(response{.status = 500, .body = "Internal Server Error\n"}, false, head_request);
        }
        co_await stream.write_all(encoded);
        if (!keep_alive) { co_await stream.close_gracefully(); co_return; }
    }
}

void default_host(request& outgoing, std::string host, std::uint16_t port, std::uint16_t default_port) {
    const bool exists = std::any_of(outgoing.headers.begin(), outgoing.headers.end(),
                                  [](const auto& field) { return equal_ci(field.name, "host"); });
    if (!exists) {
        if (port != default_port) host += ":" + std::to_string(port);
        outgoing.headers.push_back({"Host", std::move(host)});
    }
}

void check_http_protocol(const tcp::stream&) {}
#ifdef PACE_HAS_TLS
void check_http_protocol(const tls::stream& stream) {
    const auto protocol = stream.selected_protocol();
    if (!protocol.empty() && protocol != "http/1.1")
        throw std::system_error(std::make_error_code(std::errc::protocol_error), "HTTP/1.1 transport negotiated another ALPN protocol");
}
#endif
} // namespace

std::string_view request::header(std::string_view name) const noexcept {
    for (const auto& field : headers) if (equal_ci(field.name, name)) return field.value;
    return {};
}
std::string_view response::header(std::string_view name) const noexcept {
    for (const auto& field : headers) if (equal_ci(field.name, name)) return field.value;
    return {};
}

template<class Transport>
task<response> basic_client<Transport>::send(request outgoing) {
    if (io_context::current() != &stream_.context())
        throw std::logic_error("HTTP client exchange must execute on its owning io_context");
    if (busy_) throw std::logic_error("HTTP client permits one exchange at a time");
    if (!is_open()) throw std::logic_error("HTTP client connection is closed");
    // Validation precedes any bytes or busy state; callers may repair local input.
    const auto encoded = encode_request(outgoing);
    busy_ = true;
    struct reset { bool& value; ~reset() { value = false; } } guard{busy_};
    try {
        check_http_protocol(stream_);
        co_await stream_.write_all(encoded);
        auto incoming = co_await read_response(stream_, pending_, bounds_, outgoing.method);
        if (incoming.close || !outgoing.keep_alive) {
            incoming.close = true;
            reusable_ = false;
            co_await stream_.close_gracefully();
        }
        co_return incoming;
    } catch (const parse_failure& error) {
        close();
        throw protocol_error("invalid or oversized HTTP response (" + std::to_string(error.status) + ")");
    } catch (...) {
        close();
        throw;
    }
}
template class basic_client<tcp::stream>;

task<void> serve_connection(tcp::stream stream, handler handle, limits bounds) {
    try {
        stream.set_no_delay();
        co_await process_connection(stream, handle, bounds);
    } catch (const std::system_error&) { /* A transport error belongs to this client. */ }
}

task<void> serve(tcp::listener& listener, handler handle, limits bounds) {
    for (;;) {
        try { listener.context().spawn(serve_connection(co_await listener.accept(), handle, bounds)); }
        catch (const std::system_error& error) {
            if (error.code() == std::errc::operation_canceled || error.code() == std::errc::bad_file_descriptor) co_return;
            throw;
        }
    }
}

task<response> fetch(io_context& context, tcp::endpoint endpoint, request outgoing, limits bounds) {
    default_host(outgoing, endpoint.address, endpoint.port, 80);
    outgoing.keep_alive = false;
    tcp::stream stream(context);
    co_await stream.connect(std::move(endpoint));
    client connection(std::move(stream), bounds);
    co_return co_await connection.send(std::move(outgoing));
}

task<response> get(io_context& context, tcp::endpoint endpoint, std::string target, limits bounds) {
    request outgoing{.method = "GET", .target = std::move(target), .version = "HTTP/1.1", .body = {}};
    co_return co_await fetch(context, std::move(endpoint), std::move(outgoing), bounds);
}

#ifdef PACE_HAS_TLS
template class basic_client<tls::stream>;

task<void> serve_connection(tls::stream stream, handler handle, limits bounds) {
    try {
        check_http_protocol(stream);
        stream.set_no_delay();
        co_await process_connection(stream, handle, bounds);
    } catch (const std::system_error&) { /* TLS/transport failure is per connection. */ }
}

namespace {
task<void> handshake_and_serve(tls::stream stream, handler handle, limits bounds) {
    try {
        co_await stream.handshake();
        co_await serve_connection(std::move(stream), std::move(handle), bounds);
    } catch (const std::system_error&) { /* A bad TLS peer must not stop the listener. */ }
}
}

task<void> serve(tcp::listener& listener, const tls::context& configuration, handler handle, limits bounds) {
    for (;;) {
        try {
            tls::stream stream(co_await listener.accept(), configuration);
            listener.context().spawn(handshake_and_serve(std::move(stream), handle, bounds));
        } catch (const std::system_error& error) {
            if (error.code() == std::errc::operation_canceled || error.code() == std::errc::bad_file_descriptor) co_return;
            throw;
        }
    }
}

task<response> fetch(io_context& context, const tls::context& configuration,
                     tcp::endpoint endpoint, std::string peer_name, request outgoing, limits bounds) {
    default_host(outgoing, peer_name, endpoint.port, 443);
    outgoing.keep_alive = false;
    tls::stream stream(context, configuration);
    co_await stream.connect(std::move(endpoint), std::move(peer_name));
    secure_client connection(std::move(stream), bounds);
    co_return co_await connection.send(std::move(outgoing));
}

task<response> get(io_context& context, const tls::context& configuration,
                   tcp::endpoint endpoint, std::string peer_name, std::string target, limits bounds) {
    request outgoing{.method = "GET", .target = std::move(target), .version = "HTTP/1.1", .body = {}};
    co_return co_await fetch(context, configuration, std::move(endpoint), std::move(peer_name), std::move(outgoing), bounds);
}
#endif
} // namespace pace::http
