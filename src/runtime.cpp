#include <pace/runtime.hpp>
#include <mutex>

namespace pace {
runtime::runtime(std::size_t workers) {
    if (workers == 0) throw std::invalid_argument("runtime needs at least one worker");
    contexts_.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) contexts_.push_back(std::make_unique<io_context>());
}
runtime::~runtime() = default;
void runtime::stop() noexcept { for (auto& context : contexts_) context->stop(); }
void runtime::run() {
    std::vector<std::thread> workers;
    std::mutex error_mutex;
    std::exception_ptr error;
    try {
        for (auto& context : contexts_) {
            workers.emplace_back([&, pointer = context.get()] {
                try { pointer->run(); }
                catch (...) {
                    { std::lock_guard lock(error_mutex); if (!error) error = std::current_exception(); }
                    stop();
                }
            });
        }
    } catch (...) {
        stop();
        for (auto& worker : workers) worker.join();
        throw;
    }
    for (auto& worker : workers) worker.join();
    if (error) std::rethrow_exception(error);
}
} // namespace pace
