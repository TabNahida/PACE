#include <pace/http/router.hpp>
#include <pace/runtime/run.hpp>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

// Isolates route lookup and handler ownership from network and parser costs.
// Run with both the old and new headers, using identical compiler flags.
int main(int argc, char** argv)
{
    const auto routes_count = argc > 1 ? std::stoul(argv[1]) : 1024;
    const auto iterations = argc > 2 ? std::stoul(argv[2]) : 200000;
    if (!routes_count || routes_count > 100000 || !iterations || iterations > 100000000)
    {
        return 2;
    }
    // Activate the standard library's multithreaded reference-counting path.
    std::thread([] {}).join();
    pace::http::router routes;
    for (std::size_t i = 0; i < routes_count; ++i)
    {
        routes.get("/route/" + std::to_string(i), [](pace::http::request) { return pace::http::text("ok"); });
    }
    const auto target = "/route/" + std::to_string(routes_count - 1);
    pace::run(
        [&]() -> pace::task<void>
        {
            for (std::size_t i = 0; i < 5000; ++i)
            {
                (void)co_await routes({.method = "GET", .target = target});
            }
            std::size_t completed = 0;
            const auto start = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < iterations; ++i)
            {
                const auto response = co_await routes({.method = "GET", .target = target});
                if (response.status != 200 || response.body != "ok")
                {
                    throw std::runtime_error("wrong route result");
                }
                ++completed;
            }
            const auto ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
            std::cout << "{\"routes\":" << routes_count << ",\"iterations\":" << completed
                      << ",\"ns_per_operation\":" << static_cast<double>(ns) / completed << "}\n";
        }
    );
}
