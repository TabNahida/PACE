#pragma once

#include <pace/net/socket_options.hpp>
#include <pace/runtime/cancellation.hpp>
#include <pace/runtime/platform.hpp>
#include <pace/runtime/unique_function.hpp>

#include <chrono>
#include <cstddef>
#include <optional>

namespace pace
{
class io_context;

namespace tls
{
class context;
} // namespace tls

namespace tcp
{
/// Applied after selecting the destination family and before the first native connect.
struct connect_options
{
    socket_options socket{};
    /// Called synchronously with the borrowed final handle. Throw to abort the attempt.
    unique_function<void(native_socket_type)> pre_connect{};
};
} // namespace tcp

/// Options owned by a single direct client operation. Move a named options object to transfer its TCP hook.
struct client_options
{
    /// Async calls run on this context (the running context by default). Blocking calls own a fresh context.
    io_context* context = nullptr;
    /// Positive total duration from the start of execution. Default: inherited for async, 30 s for blocking.
    std::optional<std::chrono::milliseconds> timeout{};
    cancellation_token cancellation{};
    pace::deadline deadline{};
    /// Exchange limit: defaults to 1 MiB for TCP/TLS and 65536 bytes for UDP.
    std::optional<std::size_t> max_reply_size{};
    /// Borrowed until TLS connect/exchange finishes. Null selects verified system trust.
    const tls::context* tls = nullptr;
    /// Native connect settings for TCP and TLS.
    tcp::connect_options transport{};
};
} // namespace pace
