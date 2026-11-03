#include "poller_backend.hpp"
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#include <array>
#include <unordered_map>
#include <algorithm>
#include <limits>
#include <string>
#include <cstddef>

namespace pace::detail {
namespace {
// ABI of SOCK_NOTIFY_REGISTRATION; kept local so older SDK/MinGW headers can
// compile the dynamically loaded Windows build 20348+ API.
struct registration_info {
    SOCKET socket;
    void* completionKey;
    UINT16 eventFilter;
    UINT8 operation;
    UINT8 triggerFlags;
    DWORD registrationResult;
};
#ifdef SOCK_NOTIFY_REGISTER_EVENT_IN
static_assert(sizeof(registration_info) == sizeof(SOCK_NOTIFY_REGISTRATION));
static_assert(offsetof(registration_info, eventFilter) == offsetof(SOCK_NOTIFY_REGISTRATION, eventFilter));
#endif
using process_notifications = DWORD (WSAAPI*)(HANDLE, UINT32, registration_info*, UINT32, ULONG, OVERLAPPED_ENTRY*, UINT32*);
constexpr UINT8 enable = 1, remove_registration = 4, oneshot_level = 1 | 4;
constexpr UINT16 input = 1, output = 2, hangup = 4, error_event = 0x40, removed = 0x80;
class iocp_poller final : public poller_backend {
    HANDLE port_ = nullptr;
    process_notifications process_ = nullptr;
    struct subscription { SOCKET fd; std::uint64_t token; };
    std::uintptr_t next_ = 0;
    std::unordered_map<SOCKET, std::uintptr_t> current_;
    std::unordered_map<std::uintptr_t, subscription> live_;
    void check(DWORD error, const char* message) {
        if (error) throw std::system_error(static_cast<int>(error), std::system_category(), message);
    }
public:
    iocp_poller() {
        initialize_sockets();
        const auto module = GetModuleHandleW(L"ws2_32.dll");
        process_ = reinterpret_cast<process_notifications>(GetProcAddress(module, "ProcessSocketNotifications"));
        if (!process_) throw std::system_error(std::make_error_code(std::errc::not_supported), "IOCP socket notifications require Windows build 20348+");
        port_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
        if (!port_) check(GetLastError(), "CreateIoCompletionPort");
    }
    ~iocp_poller() override {
        // The port owns only integer/pointer keys, never coroutine/buffer state.
        // Closing it prevents future delivery before registration storage dies.
        CloseHandle(port_);
    }
    bool persistent() const noexcept override { return false; }
    io_backend backend() const noexcept override { return io_backend::iocp; }
    void update(native_socket_type fd, bool read, bool write, std::uint64_t token) override {
        if (token == std::numeric_limits<std::uint64_t>::max()) throw std::invalid_argument("reserved poll token");
        if (!read && !write) { remove(fd); return; }
        std::uintptr_t id;
        bool inserted = false;
        const auto it = current_.find(fd);
        if (it == current_.end()) {
            if (next_ == std::numeric_limits<std::uintptr_t>::max()) throw std::overflow_error("IOCP key identity exhausted");
            id = ++next_;
            live_.emplace(id, subscription{fd, token});
            try { current_.emplace(fd, id); } catch (...) { live_.erase(id); throw; }
            inserted = true;
        } else id = it->second;
        live_.at(id).token = token;
        registration_info info{fd, reinterpret_cast<void*>(id), static_cast<UINT16>((read ? input : 0) | (write ? output : 0) | hangup), enable, oneshot_level, 0};
        const auto result = process_(port_, 1, &info, 0, 0, nullptr, nullptr);
        if (result || info.registrationResult) {
            if (inserted) { current_.erase(fd); live_.erase(id); }
            const auto message = "ProcessSocketNotifications register (call=" + std::to_string(result)
                + ", registration=" + std::to_string(info.registrationResult) + ", size=" + std::to_string(sizeof(info))
                + ", filter=" + std::to_string(info.eventFilter) + ", new=" + std::to_string(inserted) + ")";
            check(result ? result : info.registrationResult, message.c_str());
        }
    }
    void remove(native_socket_type fd) noexcept override {
        const auto it = current_.find(fd);
        if (it == current_.end()) return;
        const auto id = it->second; current_.erase(it); live_.erase(id);
        registration_info info{fd, reinterpret_cast<void*>(id), 0, remove_registration, 0, 0};
        process_(port_, 1, &info, 0, 0, nullptr, nullptr);
        // Keys are never dereferenced or reused. Late packets (including REMOVE)
        // can be ignored without retaining per-connection heap state.
    }
    std::size_t wait(std::span<poll_event> events, int timeout) override {
        if (events.empty()) return 0;
        std::array<OVERLAPPED_ENTRY, 256> entries{};
        ULONG received = 0;
        if (!GetQueuedCompletionStatusEx(port_, entries.data(), static_cast<ULONG>(std::min(entries.size(), events.size())),
                &received, timeout < 0 ? INFINITE : static_cast<DWORD>(timeout), FALSE)) {
            if (GetLastError() == WAIT_TIMEOUT) return 0;
            check(GetLastError(), "GetQueuedCompletionStatusEx");
        }
        std::size_t count = 0;
        for (ULONG i = 0; i < received; ++i) {
            const auto& entry = entries[i];
            if (!entry.lpCompletionKey) continue; // PostQueuedCompletionStatus wakeup.
            const auto it = live_.find(entry.lpCompletionKey);
            if (it == live_.end()) continue;
            const auto flags = entry.dwNumberOfBytesTransferred;
            if (flags & removed) continue;
            events[count++] = {it->second.token, bool(flags & (input | hangup)), bool(flags & output), bool(flags & (error_event | hangup))};
        }
        return count;
    }
    void wake() noexcept override { PostQueuedCompletionStatus(port_, 0, 0, nullptr); }
};
}
std::unique_ptr<poller_backend> make_iocp_poller() { return std::make_unique<iocp_poller>(); }
}
#else
namespace pace::detail {
std::unique_ptr<poller_backend> make_iocp_poller() {
    throw std::system_error(std::make_error_code(std::errc::not_supported), "IOCP requires Windows");
}
}
#endif
