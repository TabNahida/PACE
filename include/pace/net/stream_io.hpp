#pragma once
#include <pace/net/buffer.hpp>
#include <pace/runtime/io_context.hpp>
#include <pace/runtime/task.hpp>

#include <concepts>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>

namespace pace
{
/// A connected, ordered, bidirectional byte stream on one io_context: TCP, TLS and QUIC streams satisfy it, and
/// so may application types. The contract every member keeps:
/// - `read_some(buffer)` completes with 1..buffer.size() bytes, or 0 only at end of stream;
/// - `write_all(bytes)` completes once every byte was accepted;
/// - `shutdown_write()` ends this direction (the peer reads end of stream) while reading stays possible;
/// - `close()` releases the stream at once; suspended operations resume with std::errc::operation_canceled;
/// - `context()` is the io_context that runs every operation.
/// One read and one write may be outstanding at a time unless the type documents a stricter limit (TLS allows one
/// operation in total). Buffers are borrowed until the operation completes.
template <class Stream>
concept byte_stream =
    requires(Stream& stream, const Stream& owner, std::span<std::byte> input, std::span<const std::byte> output) {
        { stream.read_some(input) } -> std::same_as<task<std::size_t>>;
        { stream.write_all(output) } -> std::same_as<task<void>>;
        { stream.shutdown_write() } -> std::same_as<task<void>>;
        { stream.close() } noexcept;
        { owner.context() } noexcept -> std::same_as<io_context&>;
    };

/// Writes every byte of `bytes` with `stream.write_all`; the task owns the string across suspension, so it may be
/// stored or spawned. The stream is borrowed and must stay alive, unmoved and on its owning context until the task
/// completes. Exceptions from write_all propagate. Counts as one write operation on the stream.
template <byte_stream Stream>
task<void> write(Stream& stream, std::string bytes)
{
    co_await stream.write_all(as_bytes(bytes));
}

/// Fills exactly `bytes` without consuming the following bytes. Throws std::system_error(connection_reset) on end
/// of stream before the span is full; a failed read may have modified a prefix. The buffer is borrowed until
/// completion. Counts as one read operation on the stream.
template <byte_stream Stream>
task<void> read_exact(Stream& stream, std::span<std::byte> bytes)
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

/// Returns an owned string of exactly `size` bytes read from `stream`.
/// Throws std::length_error before allocating or reading when `size > maximum`, and
/// std::system_error(connection_reset) on early EOF.
template <byte_stream Stream>
task<std::string> read_exact(Stream& stream, std::size_t size, std::size_t maximum = std::size_t{1024} * 1024)
{
    if (size > maximum)
    {
        throw std::length_error("read_exact exceeds maximum");
    }
    std::string bytes(size, '\0');
    co_await read_exact(stream, as_writable_bytes(bytes));
    co_return bytes;
}
} // namespace pace
