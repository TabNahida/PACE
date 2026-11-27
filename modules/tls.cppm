// ABI-preserving facade: declarations belong to the global module, just as they do for #include
// consumers. Keep this export list in sync with the module's public headers.
module;
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 16
#error PACE_named_modules_require_Clang_or_GCC_16_with_coroutine_module_fixes
#endif
#include <pace/tls.hpp>

export module pace.tls;
export import pace.net;

#ifdef PACE_HAS_TLS
export namespace pace::tls
{
using ::pace::tls::accept;
using ::pace::tls::client_hello;
using ::pace::tls::client_hello_action;
using ::pace::tls::client_hello_callback;
using ::pace::tls::client_hello_handoff;
using ::pace::tls::client_hello_key_share;
using ::pace::tls::connect;
using ::pace::tls::context;
using ::pace::tls::pin_type;
using ::pace::tls::role;
using ::pace::tls::sha256_digest;
using ::pace::tls::sni_policy;
using ::pace::tls::stream;
using ::pace::tls::trust_source;
using ::pace::tls::verification_callback;
} // namespace pace::tls
#endif
