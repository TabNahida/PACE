#pragma once

// Deprecated 0.x names, removed in P3 of the 1.0 plan. New code uses pace::tcp.
#include <pace/net/tcp.hpp>

namespace pace
{
/// Deprecated: use tcp::stream.
using tcp_stream = tcp::stream;
/// Deprecated: use tcp::listener.
using tcp_listener = tcp::listener;
/// Deprecated: use tcp::endpoint (which also accepts IPv6).
using ipv4_endpoint = tcp::endpoint;
/// Deprecated: use tcp::listen_options.
using listen_options = tcp::listen_options;
} // namespace pace
