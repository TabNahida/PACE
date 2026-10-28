#pragma once

#include <pace/task.hpp>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Transport-independent messages shared by HTTP/1.1, HTTP/2 and HTTP/3.
namespace pace::http {

struct header_field {
    std::string name;
    std::string value;
};

struct request {
    std::string method;
    std::string target;
    std::string version;
    std::vector<header_field> headers{};
    std::string body;
    bool keep_alive = true;
    std::vector<header_field> trailers{};
    bool chunked = false;

    [[nodiscard]] std::string_view header(std::string_view name) const noexcept {
        const auto equal = [](std::string_view a, std::string_view b) {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i) {
                const auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
                if (lower(a[i]) != lower(b[i])) return false;
            }
            return true;
        };
        for (const auto& field : headers) if (equal(field.name, name)) return field.value;
        return {};
    }
};

struct response {
    unsigned status = 200;
    std::string body;
    std::string content_type = "text/plain; charset=utf-8";
    std::vector<header_field> headers{};
    bool close = false;
    std::string version = "HTTP/1.1";
    std::vector<header_field> trailers{};
    bool chunked = false;

    [[nodiscard]] std::string_view header(std::string_view name) const noexcept {
        const auto equal = [](std::string_view a, std::string_view b) {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i) {
                const auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
                if (lower(a[i]) != lower(b[i])) return false;
            }
            return true;
        };
        for (const auto& field : headers) if (equal(field.name, name)) return field.value;
        return {};
    }
};

// Own already-serialized content. json() does not serialize or validate JSON.
inline response text(std::string body, unsigned status = 200) {
    return {.status = status, .body = std::move(body)};
}
inline response html(std::string body, unsigned status = 200) {
    return {.status = status, .body = std::move(body), .content_type = "text/html; charset=utf-8"};
}
inline response json(std::string body, unsigned status = 200) {
    return {.status = status, .body = std::move(body), .content_type = "application/json"};
}

// Peer protocol/framing failure reported by the client. The connection closes.
class protocol_error : public std::runtime_error {
public:
    explicit protocol_error(const std::string& message) : std::runtime_error(message) {}
};

using handler = std::function<task<response>(request)>;

} // namespace pace::http

// Explicit version spelling; the original pace::http name remains available.
namespace pace { namespace http1 = http; }
