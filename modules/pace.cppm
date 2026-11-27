// Every PACE module. `import pace;` is `#include <pace/pace.hpp>`; import a single module, such as
// `import pace.net;`, to depend on less. Modules whose library is disabled export nothing.
module;
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 16
#error PACE_named_modules_require_Clang_or_GCC_16_with_coroutine_module_fixes
#endif

export module pace;

export import pace.runtime;
export import pace.net;
export import pace.http;
export import pace.http1;
export import pace.gateway;
export import pace.tls;
export import pace.dns;
export import pace.quic;
export import pace.http2;
export import pace.http3;
export import pace.websocket;
