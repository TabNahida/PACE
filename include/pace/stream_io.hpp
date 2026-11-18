#pragma once
#include <pace/task.hpp>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>

namespace pace {
// Own bytes across suspension. The stream is borrowed and must remain alive,
// unmoved, and on its owning loop until this operation finishes.
template<class Stream>
task<void> write(Stream& stream, std::string bytes) {
    co_await stream.write_all(bytes);
}
// TCP/TLS/QUIC streams expose the same byte-stream operations. Fill exactly the
// requested span, without consuming the following message. Failed reads may
// modify a prefix. Do not overlap another read (or any TLS operation).
template<class Stream>
task<void> read_exact(Stream& stream, std::span<char> bytes) {
    while (!bytes.empty()) {
        const auto size = co_await stream.read_some(bytes);
        if (!size) throw std::system_error(std::make_error_code(std::errc::connection_reset), "EOF before read_exact completed");
        bytes = bytes.subspan(size);
    }
}
template<class Stream>
task<void> read_exact(Stream& stream, std::span<std::byte> bytes) {
    co_await read_exact(stream, std::span<char>(reinterpret_cast<char*>(bytes.data()), bytes.size()));
}
template<class Stream>
task<std::string> read_exact(Stream& stream, std::size_t size, std::size_t maximum = 1024 * 1024) {
    if (size > maximum) throw std::length_error("read_exact exceeds maximum");
    std::string bytes(size, '\0');
    co_await read_exact(stream, std::span<char>(bytes));
    co_return bytes;
}
} // namespace pace
