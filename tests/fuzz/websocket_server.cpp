// Fuzz input: WebSocket frames a client sends after a valid opening handshake (masking,
// fragmentation, control frames, close codes, UTF-8 validation, size limits).
//
// The handshake itself is fixed so that the fuzzer spends its time in frame parsing; the HTTP/1
// targets cover the request parser.

#include <pace/websocket/stream.hpp>
#include "fuzz/loopback.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace
{
constexpr std::string_view handshake = "GET /chat HTTP/1.1\r\n"
                                       "Host: 127.0.0.1\r\n"
                                       "Upgrade: websocket\r\n"
                                       "Connection: Upgrade\r\n"
                                       "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                       "Sec-WebSocket-Version: 13\r\n"
                                       "\r\n";
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    std::string input(handshake);
    input.append(pace::fuzz::as_text(data, size));
    pace::websocket::limits bounds;
    bounds.max_message_bytes = 64 * 1024;
    pace::fuzz::feed_server(
        input,
        [&](pace::tcp::stream stream) -> pace::task<void>
        {
            auto socket = co_await pace::websocket::accept(std::move(stream), {}, {}, bounds);
            for (int messages = 0; messages < 64; ++messages)
            {
                auto message = co_await socket.receive();
                if (!message)
                {
                    break;
                }
                co_await socket.send(std::move(*message));
            }
        }
    );
    return 0;
}
