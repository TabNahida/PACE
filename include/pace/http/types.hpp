#pragma once

#include <pace/runtime/ascii.hpp>
#include <pace/runtime/task.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Transport-independent messages shared by HTTP/1.1, HTTP/2 and HTTP/3.
namespace pace::http
{

/// One header or trailer field. Duplicates are kept as separate entries; names keep their spelling.
struct header_field
{
    /// Field name; HTTP/2 and HTTP/3 deliver it in lowercase.
    std::string name;
    /// Field value.
    std::string value;
};

/// A complete, owned HTTP request, shared by HTTP/1, HTTP/2 and HTTP/3.
struct request
{
    /// Method token, case-sensitive.
    std::string method;
    /// Origin-form target (path and query), never URL-decoded.
    std::string target;
    /// "HTTP/1.0", "HTTP/1.1", "HTTP/2" or "HTTP/3"; an HTTP/1 client treats empty as "HTTP/1.1".
    std::string version;
    /// Header fields other than the protocol-managed ones.
    std::vector<header_field> headers{};
    /// The complete de-framed body; may be binary.
    std::string body;
    /// HTTP/1 only: keep the connection open after this exchange.
    bool keep_alive = true;
    /// Trailer fields, never merged into headers.
    std::vector<header_field> trailers{};
    /// HTTP/1 only: the body uses chunked transfer coding.
    bool chunked = false;

    /// The first header whose name matches ignoring ASCII case, or an empty view. Borrows from this request.
    [[nodiscard]] std::string_view header(std::string_view name) const noexcept
    {
        for (const auto& field : headers)
        {
            if (ascii::iequals(field.name, name))
            {
                return field.value;
            }
        }
        return {};
    }
};

/// A complete, owned HTTP response, shared by HTTP/1, HTTP/2 and HTTP/3.
struct response
{
    /// Status code; a server response must be in [200, 599].
    unsigned status = 200;
    /// The complete body.
    std::string body;
    /// Sent as Content-Type (empty sends none); on a received response, the peer's Content-Type or empty.
    std::string content_type = "text/plain; charset=utf-8";
    /// Other header fields. Framing fields and Content-Type are managed by the protocol and must not be added.
    std::vector<header_field> headers{};
    /// HTTP/1 only: close the connection after this response; on a received one, the connection is not reusable.
    bool close = false;
    /// The peer's version on a received response; ignored on a server response.
    std::string version = "HTTP/1.1";
    /// Trailer fields; HTTP/1 sends them only with chunked framing.
    std::vector<header_field> trailers{};
    /// HTTP/1 only: send the body with chunked transfer coding.
    bool chunked = false;
    /// HTTP/1 only: Content-Length announced in a reply to HEAD whose body is empty; ignored when a body is carried.
    std::optional<std::uint64_t> representation_length{};

    /// The first header whose name matches ignoring ASCII case, or an empty view. Borrows from this response.
    [[nodiscard]] std::string_view header(std::string_view name) const noexcept
    {
        for (const auto& field : headers)
        {
            if (ascii::iequals(field.name, name))
            {
                return field.value;
            }
        }
        return {};
    }
};

/// A response owning `body` with Content-Type text/plain; charset=utf-8. Nothing is escaped.
inline response text(std::string body, unsigned status = 200)
{
    return {.status = status, .body = std::move(body)};
}

/// A response owning `body` with Content-Type text/html; charset=utf-8. Nothing is escaped.
inline response html(std::string body, unsigned status = 200)
{
    return {.status = status, .body = std::move(body), .content_type = "text/html; charset=utf-8"};
}

/// A response owning `body` with Content-Type application/json. `body` is sent as given, not validated.
inline response json(std::string body, unsigned status = 200)
{
    return {.status = status, .body = std::move(body), .content_type = "application/json"};
}

/// Thrown by a client when the peer's response is malformed, truncated or over its limits.
/// The connection is closed when it is thrown; transport errors are std::system_error instead.
class protocol_error : public std::runtime_error
{
public:
    /// Constructs the error with an explanatory message.
    explicit protocol_error(const std::string& message)
        : std::runtime_error(message)
    {
    }
};

/// Server handler of HTTP/1, HTTP/2 and HTTP/3: receives the complete request, returns the complete response.
/// A server copies it into every connection; copies may run concurrently on different worker threads.
using handler = std::function<task<response>(request)>;

} // namespace pace::http

namespace pace
{
/// Names the HTTP/1 version explicitly: pace::http1::get is pace::http::get.
namespace http1 = http;
} // namespace pace
