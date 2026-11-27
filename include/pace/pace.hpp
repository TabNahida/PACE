#pragma once

// Every module enabled in this build. Prefer a module's own header, such as <pace/net.hpp>.
#include <pace/http.hpp>
#include <pace/net.hpp>
#include <pace/runtime.hpp>
#ifdef PACE_HAS_TLS
#include <pace/tls.hpp>
#endif
#ifdef PACE_HAS_DNS
#include <pace/dns.hpp>
#endif
#ifdef PACE_HAS_QUIC
#include <pace/quic.hpp>
#endif
#ifdef PACE_HAS_HTTP1
#include <pace/gateway.hpp>
#include <pace/http1.hpp>
#endif
#ifdef PACE_HAS_HTTP2
#include <pace/http2.hpp>
#endif
#ifdef PACE_HAS_HTTP3
#include <pace/http3.hpp>
#endif
#ifdef PACE_HAS_WEBSOCKET
#include <pace/websocket.hpp>
#endif
