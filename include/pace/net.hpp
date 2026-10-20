#pragma once

// Compatibility facade. New code should include <pace/tcp.hpp> and use pace::tcp.
#include <pace/tcp.hpp>

namespace pace {
using tcp_stream = tcp::stream;
using tcp_listener = tcp::listener;
using ipv4_endpoint = tcp::endpoint;
using listen_options = tcp::listen_options;
} // namespace pace
