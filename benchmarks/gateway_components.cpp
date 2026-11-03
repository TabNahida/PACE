#include <pace/http_services.hpp>
#include <pace/detail/content_hash.hpp>
#include <pace/run.hpp>
#include <barrier>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include <ctime>

// Isolate content hashing and hot-key contention from sockets/client scheduling.
// shared/local perform the same owned 4 KiB response copy and validate every copy.
int main(int argc, char** argv) {
    try {
        if (argc != 4) throw std::invalid_argument("usage: pace_gateway_components shared|local|fnv|xxh WORKERS ITERATIONS");
        const std::string mode = argv[1];
        const auto workers = std::stoul(argv[2]), iterations = std::stoul(argv[3]);
        if (!workers || workers > 64 || !iterations || iterations > 10000000 ||
            (mode != "shared" && mode != "local" && mode != "fnv" && mode != "xxh")) throw std::invalid_argument("invalid workload");
        pace::http::response_cache cache({.max_bytes=65536, .max_entries=8, .max_object_bytes=4096, .ttl=std::chrono::seconds(3600), .shards=1});
        cache.put("hot", pace::http::text(std::string(4096, 'x')), std::chrono::seconds(3600));
        struct alignas(128) result { std::uint64_t checksum=0, failures=0; };
        std::vector<result> results(workers);
        std::barrier ready(static_cast<std::ptrdiff_t>(workers+1)), start(static_cast<std::ptrdiff_t>(workers+1)), finish(static_cast<std::ptrdiff_t>(workers+1));
        std::vector<std::thread> threads;
        for (std::size_t n=0; n<workers; ++n) threads.emplace_back([&, n] {
            pace::run([&]() -> pace::task<void> {
                // Warm the reactor memo before measuring either path.
                (void)cache.get_copy("hot");
                std::string input(4096, 'x');
                ready.arrive_and_wait(); start.arrive_and_wait();
                for (std::size_t i=0; i<iterations; ++i) {
                    if (mode == "shared" || mode == "local") {
                        std::optional<pace::http::response> value;
                        if (mode == "local") value=cache.get_copy("hot");
                        else if (auto snapshot=cache.get("hot")) value=*snapshot;
                        if (!value || value->body != input) ++results[n].failures;
                        else results[n].checksum += value->body.size();
                    } else {
                        input[0] = static_cast<char>(i); // Prevent loop-invariant hashing.
                        std::uint64_t hash=14695981039346656037ULL;
                        if (mode == "xxh") hash=pace::detail::content_hash(input);
                        else for (unsigned char c : input) { hash^=c; hash*=1099511628211ULL; }
                        results[n].checksum ^= hash;
                    }
                }
                finish.arrive_and_wait();
                co_return;
            });
        });
        ready.arrive_and_wait();
        const auto cpu=std::clock();
        const auto begin=std::chrono::steady_clock::now();
        start.arrive_and_wait(); finish.arrive_and_wait();
        const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
        const auto cpu_seconds=double(std::clock()-cpu)/CLOCKS_PER_SEC;
        for (auto& thread : threads) thread.join();
        std::uint64_t checksum=0, failures=0;
        for (const auto& r : results) { checksum+=r.checksum; failures+=r.failures; }
        if (failures) throw std::runtime_error("response validation failed");
        std::cout << "{\"mode\":\"" << mode << "\",\"workers\":" << workers << ",\"iterations_per_worker\":" << iterations
                  << ",\"operations_per_second\":" << workers*iterations/elapsed << ",\"elapsed_seconds\":" << elapsed
                  << ",\"cpu_seconds\":" << cpu_seconds << ",\"checksum\":" << checksum << ",\"validated\":true}\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
