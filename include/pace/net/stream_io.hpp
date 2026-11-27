#pragma once
#include <pace/runtime/task.hpp>

#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>

namespace pace
{
/// Writes every byte of `bytes` with `stream.write_all`; the task owns the string across suspension.
/// The stream is borrowed and must stay alive, unmoved and on its owning context until the task completes.
/// Exceptions from write_all propagate. Counts as one write operation on the stream.
template <class Stream>
task<void> write(Stream& stream, std::string bytes)
{
    co_await stream.write_all(bytes);
}

/// Fills exactly `bytes` from a TCP, TLS or QUIC stream without consuming the following bytes.
/// Throws std::system_error(connection_reset) on EOF before the span is full; a failed read may have modified a
/// prefix. The buffer is borrowed until completion. Do not overlap another read (or any TLS operation).
template <class Stream>
task<void> read_exact(Stream& stream, std::span<char> bytes)
{
    while (!bytes.empty())
    {
        const auto size = co_await stream.read_some(bytes);
        if (!size)
        {
            throw std::system_error(
                std::make_error_code(std::errc::connection_reset),
                "EOF before read_exact completed"
            );
        }
        bytes = bytes.subspan(size);
    }
}

/// Byte-span form of read_exact(Stream&, std::span<char>).
template <class Stream>
task<void> read_exact(Stream& stream, std::span<std::byte> bytes)
{
    co_await read_exact(stream, std::span<char>(reinterpret_cast<char*>(bytes.data()), bytes.size()));
}

/// Returns an owned string of exactly `size` bytes read from `stream`.
/// Throws std::length_error before allocating or reading when `size > maximum`, and
/// std::system_error(connection_reset) on early EOF.
template <class Stream>
task<std::string> read_exact(Stream& stream, std::size_t size, std::size_t maximum = std::size_t{1024} * 1024)
{
    if (size > maximum)
    {
        throw std::length_error("read_exact exceeds maximum");
    }
    std::string bytes(size, '\0');
    co_await read_exact(stream, std::span<char>(bytes));
    co_return bytes;
}
} // namespace pace
