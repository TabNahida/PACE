// ABI-preserving facade: declarations belong to the global module, just as they do for #include
// consumers. Keep this export list in sync with the module's public headers.
module;
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 16
#error PACE_named_modules_require_Clang_or_GCC_16_with_coroutine_module_fixes
#endif
#include <pace/dns.hpp>

export module pace.dns;
export import pace.net;

#ifdef PACE_HAS_DNS
export namespace pace::dns
{
using ::pace::dns::address;
using ::pace::dns::error_category;
using ::pace::dns::family;
using ::pace::dns::options;
using ::pace::dns::resolve;
using ::pace::dns::resolver;
using ::pace::dns::result;
} // namespace pace::dns
#endif
