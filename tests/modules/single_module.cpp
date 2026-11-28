// Importing single modules: each exports only its own names.
#include <type_traits>
#include <utility>
import pace.runtime;
import pace.net;

static_assert(!std::is_copy_constructible_v<pace::task<int>>, "pace.runtime exports task");

pace::tcp::endpoint single_module_endpoint()
{
    return {"127.0.0.1", 0};
}

pace::task<pace::result<pace::tcp::stream>> single_module_accept(pace::tcp::listener& listener)
{
    return listener.accept(pace::as_result);
}

pace::task<pace::tcp::stream> single_module_connect(pace::tcp::endpoint peer, pace::client_options options)
{
    return pace::tcp::connect(std::move(peer), std::move(options));
}
