// Fuzz input: the bytes a client sends to an HTTP/1.1 server connection (request line, headers,
// chunked or length-delimited bodies, trailers, pipelined requests).

#include <pace/http1/protocol.hpp>
#include "fuzz/loopback.hpp"

#include <cstddef>
#include <cstdint>

namespace
{
pace::task<pace::http::response> respond(pace::http::request request)
{
    pace::http::response response;
    response.body = request.method + " " + request.target + " " + std::to_string(request.body.size());
    co_return response;
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    pace::http::limits bounds;
    bounds.max_body_bytes = 64 * 1024;
    bounds.max_requests_per_connection = 32;
    pace::fuzz::feed_server(
        pace::fuzz::as_text(data, size),
        [&](pace::tcp::stream stream) { return pace::http::serve_connection(std::move(stream), respond, bounds); }
    );
    return 0;
}
