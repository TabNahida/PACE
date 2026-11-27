// Importing single modules: each exports only its own names.
#include <type_traits>
import pace.runtime;
import pace.net;

static_assert(!std::is_copy_constructible_v<pace::task<int>>, "pace.runtime exports task");

pace::tcp::endpoint single_module_endpoint()
{
    return {"127.0.0.1", 0};
}
