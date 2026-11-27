// Importing one module gives that module and the modules it depends on, nothing else.
#include <type_traits>
import pace.net;

static_assert(
    std::is_same_v<pace::tcp::endpoint, pace::udp::endpoint>
    || !std::is_same_v<pace::tcp::endpoint, pace::udp::endpoint>
);
static_assert(!std::is_copy_constructible_v<pace::task<int>>, "pace.net re-exports pace.runtime");

pace::tcp::endpoint single_module_endpoint()
{
    return {"127.0.0.1", 0};
}
