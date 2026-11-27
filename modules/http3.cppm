// ABI-preserving facade: declarations belong to the global module, just as they do for #include
// consumers. Keep this export list in sync with the module's public headers.
module;
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 16
#error PACE_named_modules_require_Clang_or_GCC_16_with_coroutine_module_fixes
#endif
#include <pace/http3.hpp>

export module pace.http3;
export import pace.net;
export import pace.http;

#ifdef PACE_HAS_HTTP3
export namespace pace::http3
{
using ::pace::http3::async_exchange;
using ::pace::http3::async_fetch;
using ::pace::http3::async_get;
using ::pace::http3::async_post;
using ::pace::http3::fetch;
using ::pace::http3::get;
using ::pace::http3::limits;
using ::pace::http3::post;
using ::pace::http3::serve;
using ::pace::http3::serve_connection;
} // namespace pace::http3
#endif
