// ABI-preserving facade: declarations belong to the global module, just as they do for #include
// consumers. Keep this export list in sync with the module's public headers.
module;
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 16
#error PACE_named_modules_require_Clang_or_GCC_16_with_coroutine_module_fixes
#endif
#include <pace/http.hpp>

export module pace.http;
export import pace.runtime;

export namespace pace::http
{
using ::pace::http::handler;
using ::pace::http::header_field;
using ::pace::http::html;
using ::pace::http::json;
using ::pace::http::make_handler;
using ::pace::http::protocol_error;
using ::pace::http::request;
using ::pace::http::response;
using ::pace::http::router;
using ::pace::http::text;
} // namespace pace::http

export namespace pace
{
namespace http1 = http;
} // namespace pace
