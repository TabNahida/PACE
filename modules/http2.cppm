// ABI-preserving facade: declarations belong to the global module, just as they do for #include
// consumers. Keep this export list in sync with the module's public headers.
module;
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 16
#error PACE_named_modules_require_Clang_or_GCC_16_with_coroutine_module_fixes
#endif
#include <pace/http2.hpp>

export module pace.http2;
export import pace.net;
export import pace.http;

#ifdef PACE_HAS_HTTP2
export namespace pace::http2
{
using ::pace::http2::async_exchange;
using ::pace::http2::async_fetch;
using ::pace::http2::async_get;
using ::pace::http2::async_post;
using ::pace::http2::fetch;
using ::pace::http2::get;
using ::pace::http2::limits;
using ::pace::http2::post;
using ::pace::http2::serve;
using ::pace::http2::serve_connection;
} // namespace pace::http2
#endif
