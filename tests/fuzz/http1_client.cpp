// Fuzz input: the bytes an HTTP/1.1 server answers to one GET (status line, informational
// responses, headers, chunked / length / EOF-delimited bodies, trailers).

#include "fuzz/loopback.hpp"
#include <pace/http.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    pace::http::limits bounds;
    bounds.max_body_bytes = 64 * 1024;
    pace::fuzz::feed_client(
        pace::fuzz::as_text(data, size),
        [&](pace::io_context& io, pace::tcp::endpoint server) -> pace::task<void>
        {
            const auto url = "http://127.0.0.1:" + std::to_string(server.port) + "/fuzz";
            (void)co_await pace::http::async_get(io, url, bounds);
        }
    );
    return 0;
}
