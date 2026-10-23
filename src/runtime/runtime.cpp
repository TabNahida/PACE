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
void runtime::run() { run_impl(false); }
void runtime::run_on_current_thread() { run_impl(true); }
void runtime::run_impl(bool use_caller) {
    // Validate before launching peers or consuming the single-use state: a
    // rejected nested call must leave an otherwise unused runtime runnable.
    if (use_caller && io_context::current())
        throw std::logic_error("runtime caller mode cannot nest inside io_context::run");
    if (started_.exchange(true)) throw std::logic_error("runtime is single-use");
    std::vector<std::thread> workers;
    std::mutex error_mutex;
    std::exception_ptr error;
    const auto run_context = [&](io_context& context) {
        try { context.run(); }
        catch (...) {
            { std::lock_guard lock(error_mutex); if (!error) error = std::current_exception(); }
            stop();
        }
    };
    try {
        const std::size_t first_worker = use_caller ? 1 : 0;
        workers.reserve(contexts_.size() - first_worker);
        for (std::size_t i = first_worker; i < contexts_.size(); ++i)
            workers.emplace_back([&, pointer = contexts_[i].get()] { run_context(*pointer); });
        if (use_caller) run_context(*contexts_[0]);
    } catch (...) {
        stop();
        for (auto& worker : workers) worker.join();
        throw;
    }
    for (auto& worker : workers) worker.join();
    if (error) std::rethrow_exception(error);
}
} // namespace pace
