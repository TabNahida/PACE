// ABI-preserving facade: declarations belong to the global module, just as they do for #include
// consumers. Keep this export list in sync with the module's public headers.
module;
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 16
#error PACE_named_modules_require_Clang_or_GCC_16_with_coroutine_module_fixes
#endif
#include <pace/gateway.hpp>

export module pace.gateway;
export import pace.http1;

#ifdef PACE_HAS_HTTP1
export namespace pace::http
{
using ::pace::http::cache_options;
using ::pace::http::cache_statistics;
using ::pace::http::load_balance;
using ::pace::http::proxy_options;
using ::pace::http::response_cache;
using ::pace::http::reverse_proxy;
using ::pace::http::static_files;
using ::pace::http::static_options;
} // namespace pace::http
#endif
