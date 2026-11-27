#pragma once

// Deprecated 0.x names, removed in P3 of the 1.0 plan. New code uses pace::tcp.
#include <pace/net/tcp.hpp>

namespace pace
{
using tcp_stream = tcp::stream;
using tcp_listener = tcp::listener;
using ipv4_endpoint = tcp::endpoint;
using listen_options = tcp::listen_options;
} // namespace pace
