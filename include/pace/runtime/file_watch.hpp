#pragma once

#include <pace/runtime/task.hpp>

#include <filesystem>
#include <memory>

namespace pace
{
class io_context;

/// Watches one filename in an existing directory, including creation, writes, removal and replacement.
/// Events can coalesce. Arm the watch before reading the file, then re-read it after each notification.
/// The parent directory must remain in place. The file itself may be absent at construction.
class file_watch
{
public:
    file_watch(io_context& io, const std::filesystem::path& file);
    ~file_watch();
    file_watch(const file_watch&) = delete;
    file_watch& operator=(const file_watch&) = delete;

    /// One wait at a time on the owning context. Canceling a wait leaves the subscription active.
    task<void> wait();
    /// Idempotent. Wakes a pending wait with operation_canceled. Close/destroy on the owner or while idle.
    void close() noexcept;

private:
    struct implementation;
    std::shared_ptr<implementation> impl_;
};
} // namespace pace
