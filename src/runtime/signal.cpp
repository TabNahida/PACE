// Awaitable process signals.
//
// Ownership model: dispositions are process-wide and reference-counted per signal. The first signal_set that
// subscribes to a signal installs PACE's handler (POSIX sigaction, Windows console control handler) and saves the
// previous disposition; the last one to go restores it. Deliveries are counted process-wide; each signal_set
// remembers how many it has reported, so every set sees every delivery after its construction (broadcast), and a
// delivery that arrives while no wait is pending is reported by the next wait.
//
// Wakeup: each io_context that waits for signals owns one notifier (POSIX: a nonblocking pipe; Windows: an
// auto-reset event) in a fixed process-wide slot table. The handler bumps the counter and pokes every notifier;
// one pump coroutine per context waits on its notifier (wait_readable / wait_handle, so every backend including
// io_uring works unchanged) and resumes the waits whose signals arrived. The handler only touches lock-free atomics
// and write(), so it is async-signal-safe. signalfd is not used: it needs the signal blocked in every thread, and
// one read consumes the signal for all contexts. kqueue's EVFILT_SIGNAL would work, but a handler is needed anyway
// to keep the default action from running, and one path on every POSIX system is the path that is tested.
#include <pace/runtime/signal.hpp>
#include <pace/runtime/this_context.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>

#include <fcntl.h>
#include <unistd.h>
#endif

namespace pace
{
namespace
{
constexpr std::size_t signal_count = 5;
constexpr std::size_t notifier_capacity = 256;

static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free);
static_assert(std::atomic<std::size_t>::is_always_lock_free);

// Touched by signal handlers: lock-free atomics with constant initialization only.
constinit std::array<std::atomic<std::uint32_t>, signal_count> deliveries{};
constinit std::array<std::atomic<std::uint32_t>, signal_count> subscribers{};
// A notifier: pipe write end + 1 (POSIX) or event HANDLE (Windows); 0 is a free slot.
constinit std::array<std::atomic<std::uintptr_t>, notifier_capacity> notifiers{};
constinit std::atomic<std::size_t> notifier_end{0};
// Handlers and deliver_signal calls between loading a notifier and using it.
constinit std::atomic<std::uint32_t> notifying{0};

std::size_t index_of(process_signal value) noexcept
{
    return static_cast<std::size_t>(value);
}

void notify_all(std::size_t index) noexcept
{
    // Sequentially consistent on purpose: release_notifier clears a slot and then waits for `notifying` to reach
    // zero, so a notifier that loaded the old value is always counted.
    notifying.fetch_add(1);
    deliveries[index].fetch_add(1);
    const auto end = notifier_end.load();
    for (std::size_t slot = 0; slot < end; ++slot)
    {
        const auto value = notifiers[slot].load();
        if (!value)
        {
            continue;
        }
#ifdef _WIN32
        // NOLINTNEXTLINE(performance-no-int-to-ptr): the slot stores the event handle's bits
        SetEvent(reinterpret_cast<HANDLE>(value));
#else
        const char byte = 1;
        // A full pipe already holds a wakeup; the counter carries the delivery.
        (void)::write(static_cast<int>(value - 1), &byte, 1);
#endif
    }
    notifying.fetch_sub(1);
}

// Process-wide state that is never touched by a handler. Leaked on purpose: a signal_set destroyed during static
// destruction still finds it.
struct registry
{
    std::mutex mutex;
    std::array<bool, notifier_capacity> used{};
#ifndef _WIN32
    std::array<struct sigaction, signal_count> previous{};
#endif
};

// Not noexcept: only the first call allocates, and it comes from a subscription, which may throw.
registry& global()
{
    static auto* const instance = new registry; // NOLINT(cppcoreguidelines-owning-memory): never destroyed
    return *instance;
}

std::size_t acquire_notifier(std::uintptr_t value)
{
    auto& state = global();
    std::scoped_lock lock(state.mutex);
    for (std::size_t slot = 0; slot < notifier_capacity; ++slot)
    {
        if (!state.used[slot])
        {
            state.used[slot] = true;
            notifiers[slot].store(value);
            if (notifier_end.load() <= slot)
            {
                notifier_end.store(slot + 1);
            }
            return slot;
        }
    }
    throw std::system_error(
        std::make_error_code(std::errc::resource_unavailable_try_again),
        "every signal notifier slot is in use"
    );
}

void release_notifier(std::size_t slot) noexcept
{
    notifiers[slot].store(0);
    // A handler on another thread may have loaded the old value; wait until it is done with it. A handler on this
    // thread cannot be in flight: it ran to completion before this code resumed.
    while (notifying.load() != 0)
    {
        std::this_thread::yield();
    }
    auto& state = global();
    std::scoped_lock lock(state.mutex);
    state.used[slot] = false;
}

#ifdef _WIN32
BOOL WINAPI console_handler(DWORD type) noexcept
{
    std::size_t index;
    switch (type)
    {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
            index = index_of(process_signal::interrupt);
            break;
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            index = index_of(process_signal::terminate);
            break;
        default:
            return FALSE;
    }
    if (subscribers[index].load() == 0)
    {
        return FALSE; // Nobody listens: let the next handler or the default action run.
    }
    notify_all(index);
    if (index == index_of(process_signal::terminate))
    {
        // Windows ends the process as soon as this handler returns. Block this system-created thread so the
        // program can shut down gracefully until it exits or the system's timeout ends it.
        Sleep(INFINITE);
    }
    return TRUE;
}

bool console_event(std::size_t index) noexcept
{
    return index == index_of(process_signal::interrupt) || index == index_of(process_signal::terminate);
}

void subscribe(std::size_t index)
{
    auto& state = global();
    std::scoped_lock lock(state.mutex);
    const bool console_before = subscribers[index_of(process_signal::interrupt)].load() != 0
                                || subscribers[index_of(process_signal::terminate)].load() != 0;
    if (console_event(index) && !console_before && !SetConsoleCtrlHandler(&console_handler, TRUE))
    {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "SetConsoleCtrlHandler");
    }
    subscribers[index].fetch_add(1);
}

void unsubscribe(std::size_t index) noexcept
{
    auto& state = global();
    std::scoped_lock lock(state.mutex);
    subscribers[index].fetch_sub(1);
    const bool console_after = subscribers[index_of(process_signal::interrupt)].load() != 0
                               || subscribers[index_of(process_signal::terminate)].load() != 0;
    if (console_event(index) && !console_after)
    {
        SetConsoleCtrlHandler(&console_handler, FALSE);
    }
}
#else
constexpr std::array<int, signal_count> signal_numbers{SIGHUP, SIGINT, SIGTERM, SIGUSR1, SIGUSR2};

void handle_signal(int number) noexcept
{
    const int saved = errno;
    for (std::size_t index = 0; index < signal_count; ++index)
    {
        if (signal_numbers[index] == number)
        {
            notify_all(index);
        }
    }
    errno = saved;
}

void subscribe(std::size_t index)
{
    auto& state = global();
    std::scoped_lock lock(state.mutex);
    if (subscribers[index].load() == 0)
    {
        struct sigaction action{};
        action.sa_handler = &handle_signal;
        sigemptyset(&action.sa_mask);
        // Other threads' blocking calls keep running; PACE's pollers treat EINTR as an empty wait anyway.
        action.sa_flags = SA_RESTART;
        if (::sigaction(signal_numbers[index], &action, &state.previous[index]) != 0)
        {
            throw std::system_error(errno, std::generic_category(), "sigaction");
        }
    }
    subscribers[index].fetch_add(1);
}

void unsubscribe(std::size_t index) noexcept
{
    auto& state = global();
    std::scoped_lock lock(state.mutex);
    if (subscribers[index].fetch_sub(1) == 1)
    {
        ::sigaction(signal_numbers[index], &state.previous[index], nullptr);
    }
}
#endif

std::error_code canceled() noexcept
{
    return std::make_error_code(std::errc::operation_canceled);
}
} // namespace

// One per io_context that has waited for a signal, stored as a context-local service.
struct signal_access
{
    struct service
    {
        io_context& io;
        std::size_t slot = 0;
#ifdef _WIN32
        HANDLE event = nullptr;
#else
        int read_end = -1;
        int write_end = -1;
#endif
        signal_set::awaiter* first = nullptr;
        bool pumping = false;

        explicit service(io_context& context)
            : io(context)
        {
#ifdef _WIN32
            event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!event)
            {
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "CreateEventW");
            }
            try
            {
                slot = acquire_notifier(reinterpret_cast<std::uintptr_t>(event));
            }
            catch (...)
            {
                CloseHandle(event);
                throw;
            }
#else
            std::array<int, 2> ends{};
            if (::pipe(ends.data()) != 0)
            {
                throw std::system_error(errno, std::generic_category(), "pipe");
            }
            read_end = ends[0];
            write_end = ends[1];
            try
            {
                for (const int fd : ends)
                {
                    if (::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK) < 0
                        || ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
                    {
                        throw std::system_error(errno, std::generic_category(), "fcntl signal pipe");
                    }
                }
                slot = acquire_notifier(static_cast<std::uintptr_t>(write_end) + 1);
            }
            catch (...)
            {
                ::close(read_end);
                ::close(write_end);
                throw;
            }
#endif
        }

        service(const service&) = delete;
        service& operator=(const service&) = delete;

        // Runs when the context destroys its services (its waits are already cancelled and unregistered) or later,
        // when the last signal_set holding it goes. It must not touch the context.
        ~service()
        {
            release_notifier(slot);
#ifdef _WIN32
            CloseHandle(event);
#else
            ::close(read_end);
            ::close(write_end);
#endif
        }

        void link(signal_set::awaiter& operation) noexcept
        {
            operation.previous = nullptr;
            operation.next = first;
            if (first)
            {
                first->previous = &operation;
            }
            first = &operation;
            operation.linked = true;
            operation.set.waiter_ = &operation;
        }

        void unlink(signal_set::awaiter& operation) noexcept
        {
            if (!operation.linked)
            {
                return;
            }
            if (operation.previous)
            {
                operation.previous->next = operation.next;
            }
            else
            {
                first = operation.next;
            }
            if (operation.next)
            {
                operation.next->previous = operation.previous;
            }
            operation.previous = nullptr;
            operation.next = nullptr;
            operation.linked = false;
            operation.set.waiter_ = nullptr;
        }

        void resume(signal_set::awaiter& operation, std::error_code error)
        {
            unlink(operation);
            operation.error = error;
            scheduler_access::schedule(io, operation.continuation);
        }

#ifndef _WIN32
        void drain() noexcept
        {
            std::array<char, 64> bytes{};
            for (;;)
            {
                const auto count = ::read(read_end, bytes.data(), bytes.size());
                if (count > 0)
                {
                    continue;
                }
                if (count < 0 && errno == EINTR)
                {
                    continue;
                }
                return;
            }
        }
#endif

        void dispatch()
        {
            for (auto* operation = first; operation;)
            {
                auto* following = operation->next;
                if (operation->set.take(operation->value))
                {
                    resume(*operation, {});
                }
                operation = following;
            }
        }

        void fail_all(std::error_code error)
        {
            while (first)
            {
                resume(*first, error);
            }
        }
    };

    // One per context while waits are pending. A delivery between a wait's check and this pump's first poll
    // leaves a byte in the pipe (or the event set), so it is never lost.
    static task<void> pump(service& owner)
    {
        std::error_code failure;
        try
        {
            while (owner.first)
            {
#ifdef _WIN32
                co_await owner.io.wait_handle(owner.event);
#else
                co_await owner.io.wait_readable(owner.read_end);
                owner.drain();
#endif
                owner.dispatch();
            }
        }
        catch (const std::system_error& error)
        {
            failure = error.code();
        }
        catch (...)
        {
            failure = std::make_error_code(std::errc::io_error);
        }
        owner.pumping = false;
        if (failure)
        {
            owner.fail_all(failure);
        }
    }

    static service& service_of(signal_set& set)
    {
        if (!set.service_)
        {
            static const auto key = std::make_shared<const int>(0);
            auto& io = set.io_;
            set.service_ = io.local_state(key, [&io] { return std::make_shared<service>(io); });
        }
        return *static_cast<service*>(set.service_.get());
    }
};

signal_set::awaiter::~awaiter()
{
    if (linked)
    {
        static_cast<signal_access::service*>(set.service_.get())->unlink(*this);
    }
}

bool signal_set::awaiter::await_ready()
{
    if (set.io_.stopped())
    {
        error = canceled();
        return true;
    }
    return set.take(value);
}

process_signal signal_set::awaiter::await_resume()
{
    if (token.stop_possible())
    {
        registration.cancel();
        token.throw_if_cancelled();
    }
    if (error)
    {
        throw std::system_error(error);
    }
    if (set.io_.stopped())
    {
        throw std::system_error(canceled());
    }
    return value;
}

signal_set::signal_set(io_context& io, std::initializer_list<process_signal> signals)
    : io_(io)
{
    if (signals.size() == 0)
    {
        throw std::invalid_argument("empty signal set");
    }
    unsigned mask = 0;
    for (const auto value : signals)
    {
        const auto index = index_of(value);
        if (index >= signal_count)
        {
            throw std::invalid_argument("unknown process_signal");
        }
        mask |= 1U << index;
    }
    for (std::size_t index = 0; index < signal_count; ++index)
    {
        if (!(mask & (1U << index)))
        {
            continue;
        }
        // Snapshot first: a delivery between the snapshot and the handler's installation can only come from an
        // earlier subscriber's handler, and is reported to this set too.
        seen_[index] = deliveries[index].load();
        try
        {
            subscribe(index);
        }
        catch (...)
        {
            for (std::size_t done = 0; done < signal_count; ++done)
            {
                if (mask_ & (1U << done))
                {
                    unsubscribe(done);
                }
            }
            throw;
        }
        mask_ |= 1U << index;
    }
}

signal_set::~signal_set()
{
    if (waiter_)
    {
        static_cast<signal_access::service*>(service_.get())->unlink(*waiter_);
    }
    for (std::size_t index = 0; index < signal_count; ++index)
    {
        if (mask_ & (1U << index))
        {
            unsubscribe(index);
        }
    }
}

bool signal_set::contains(process_signal value) const noexcept
{
    const auto index = index_of(value);
    return index < signal_count && (mask_ & (1U << index));
}

bool signal_set::take(process_signal& value) noexcept
{
    for (std::size_t index = 0; index < signal_count; ++index)
    {
        if ((mask_ & (1U << index)) && deliveries[index].load(std::memory_order_acquire) != seen_[index])
        {
            ++seen_[index];
            value = static_cast<process_signal>(index);
            return true;
        }
    }
    return false;
}

bool signal_set::arm(awaiter& operation, std::coroutine_handle<> handle)
{
    if (io_.stopped())
    {
        operation.error = canceled();
        return false;
    }
    if (io_context::current() != &io_)
    {
        throw std::logic_error("signal waits must run on their io_context");
    }
    if (waiter_)
    {
        throw std::logic_error("only one pending wait per signal_set is allowed");
    }
    auto& owner = signal_access::service_of(*this);
    // A delivery since await_ready that reached no notifier yet (this context had none) is only in the counter.
    if (take(operation.value))
    {
        return false;
    }
    operation.continuation = handle;
    owner.link(operation);
    if (!owner.pumping)
    {
        try
        {
            io_.spawn(signal_access::pump(owner));
        }
        catch (...)
        {
            owner.unlink(operation);
            throw;
        }
        owner.pumping = true;
    }
    return true;
}

void signal_set::cancel_wait(awaiter& operation, std::error_code error) noexcept
{
    if (!operation.linked)
    {
        return;
    }
    // Runs on the context: on_cancel callbacks are dispatched there.
    static_cast<signal_access::service*>(service_.get())->resume(operation, error);
}

const char* signal_name(process_signal value) noexcept
{
    switch (value)
    {
        case process_signal::hangup:
            return "SIGHUP";
        case process_signal::interrupt:
            return "SIGINT";
        case process_signal::terminate:
            return "SIGTERM";
        case process_signal::user1:
            return "SIGUSR1";
        case process_signal::user2:
            return "SIGUSR2";
    }
    return "unknown";
}

namespace
{
task<process_signal> wait_once(std::unique_ptr<signal_set> signals)
{
    co_return co_await signals->wait();
}
} // namespace

task<process_signal> wait_for_signal(io_context& io, std::initializer_list<process_signal> signals)
{
    return wait_once(std::make_unique<signal_set>(io, signals));
}

task<process_signal> wait_for_signal(std::initializer_list<process_signal> signals)
{
    return wait_for_signal(this_context(), signals);
}

void deliver_signal(process_signal value) noexcept
{
    const auto index = index_of(value);
    if (index < signal_count)
    {
        notify_all(index);
    }
}
} // namespace pace
