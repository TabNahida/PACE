#include <pace/cancellation.hpp>
#include <atomic>
#include <condition_variable>
#include <list>
#include <mutex>
#include <thread>
#include <vector>

namespace pace::detail {
struct cancellation_registration::implementation {
    std::weak_ptr<cancellation_state> owner;
    using list = std::list<std::shared_ptr<implementation>>;
    list::iterator position;
    bool registered = false; // Protected by owner->mutex.
    std::function<void()> callback;
    std::mutex mutex;
    std::condition_variable finished;
    std::thread::id runner;
    bool disabled = false;
    bool running = false;
    void invoke() noexcept {
        {
            std::lock_guard lock(mutex);
            if (disabled) return;
            running = true;
            runner = std::this_thread::get_id();
        }
        // Internal notifications only mark state/wake a reactor. Like a stop
        // callback, they must not throw; user callbacks run later on the loop.
        callback();
        {
            std::lock_guard lock(mutex);
            running = false;
        }
        finished.notify_all();
    }
    void disable() noexcept {
        std::unique_lock lock(mutex);
        disabled = true;
        if (running && runner != std::this_thread::get_id())
            finished.wait(lock, [&] { return !running; });
    }
};
struct cancellation_state {
    std::atomic<int> reason{0};
    pace::deadline expires;
    std::mutex mutex;
    std::list<std::shared_ptr<cancellation_registration::implementation>> callbacks;
    std::vector<std::unique_ptr<cancellation_registration>> parents;
    explicit cancellation_state(pace::deadline when) : expires(when) {}
    bool request(std::errc value) noexcept {
        int expected = 0;
        if (!reason.compare_exchange_strong(expected, static_cast<int>(value))) return false;
        std::list<std::shared_ptr<cancellation_registration::implementation>> pending;
        {
            std::lock_guard lock(mutex);
            for (auto& callback : callbacks) callback->registered = false;
            pending.splice(pending.end(), callbacks);
        }
        for (auto& callback : pending) callback->invoke();
        return true;
    }
};
cancellation_registration::cancellation_registration(cancellation_token token, std::function<void()> callback) {
    if (!token.state_) return;
    impl_ = std::make_shared<implementation>();
    impl_->owner = token.state_;
    impl_->callback = std::move(callback);
    {
        std::lock_guard lock(token.state_->mutex);
        if (!token.state_->reason.load()) {
            impl_->position = token.state_->callbacks.insert(token.state_->callbacks.end(), impl_);
            impl_->registered = true;
            return;
        }
    }
    impl_->invoke(); // An already-canceled source notifies before registration returns.
}
cancellation_registration::~cancellation_registration() {
    if (!impl_) return;
    if (auto state = impl_->owner.lock()) {
        std::lock_guard lock(state->mutex);
        if (impl_->registered) {
            state->callbacks.erase(impl_->position);
            impl_->registered = false;
        }
    }
    impl_->disable();
}
}
namespace pace {
std::error_code cancellation_token::error() const noexcept {
    if (!state_) return {};
    if (auto reason = state_->reason.load()) return std::make_error_code(static_cast<std::errc>(reason));
    return state_->expires.expired() ? std::make_error_code(std::errc::timed_out) : std::error_code{};
}
deadline cancellation_token::expires() const noexcept { return state_ ? state_->expires : deadline{}; }
cancellation_source::cancellation_source(pace::deadline expires)
    : state_(std::make_shared<detail::cancellation_state>(expires)) {}
bool cancellation_source::request_cancellation() const noexcept { return state_->request(std::errc::operation_canceled); }
bool cancellation_source::request_timeout() const noexcept { return state_->request(std::errc::timed_out); }
void cancellation_source::link(cancellation_token parent) {
    if (!parent.stop_possible()) return;
    if (parent.state_ == state_) throw std::invalid_argument("cannot link a cancellation source to itself");
    std::weak_ptr<detail::cancellation_state> child = state_;
    auto forward = [child, parent = std::weak_ptr<detail::cancellation_state>(parent.state_)] {
        auto owner = parent.lock();
        if (auto state = child.lock()) state->request(owner && cancellation_token(owner).error() == std::errc::timed_out
            ? std::errc::timed_out : std::errc::operation_canceled);
    };
    state_->parents.push_back(std::make_unique<detail::cancellation_registration>(parent, std::move(forward)));
    if (auto error = parent.error()) state_->request(error == std::errc::timed_out ? std::errc::timed_out : std::errc::operation_canceled);
}
}
