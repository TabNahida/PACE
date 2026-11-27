// ABI-preserving facade: declarations belong to the global module, just as they do for #include
// consumers. Keep this export list in sync with the module's public headers.
module;
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 16
#error PACE_named_modules_require_Clang_or_GCC_16_with_coroutine_module_fixes
#endif
#include <pace/websocket.hpp>

export module pace.websocket;
export import pace.net;

#ifdef PACE_HAS_WEBSOCKET
export namespace pace::websocket
{
using ::pace::websocket::accept;
using ::pace::websocket::basic_stream;
using ::pace::websocket::connect;
using ::pace::websocket::limits;
using ::pace::websocket::message;
using ::pace::websocket::message_type;
using ::pace::websocket::protocol_error;
using ::pace::websocket::role;
using ::pace::websocket::stream;
using ::pace::websocket::upgrade;
using ::pace::websocket::upgrade_request;
} // namespace pace::websocket
#ifdef PACE_HAS_TLS
export namespace pace::websocket
{
using ::pace::websocket::secure_stream;
} // namespace pace::websocket
#endif
#endif
