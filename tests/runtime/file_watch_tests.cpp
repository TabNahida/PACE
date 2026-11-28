#include <pace/runtime.hpp>
#include "support/pace_test.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>

using namespace std::chrono_literals;

namespace
{
namespace fs = std::filesystem;
using pace::test::require;

struct scratch_directory
{
    fs::path path;

    scratch_directory()
    {
        std::random_device random;
        do
        {
            path = fs::temp_directory_path() / ("pace-watch-" + std::to_string(random()));
        } while (!fs::create_directory(path));
    }

    ~scratch_directory()
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

void write_file(const fs::path& path, std::string_view contents)
{
    std::ofstream output;
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output.open(path, std::ios::binary | std::ios::trunc);
    output << contents;
    output.close();
}

pace::task<void> expect_change(pace::file_watch& watch)
{
    co_await pace::with_deadline(pace::deadline::after(3s), watch.wait());
}
} // namespace

TEST_CASE("file watch follows a filename through replacement, removal and recreation")
{
    scratch_directory temp;
    const auto file = temp.path / "config";
    pace::run(
        [&](pace::io_context& io) -> pace::task<void>
        {
            pace::file_watch watch(io, file);
            write_file(file, "first");
            co_await expect_change(watch);
            const auto replacement = temp.path / "new-config";
            write_file(replacement, "second");
            fs::rename(replacement, file);
            co_await expect_change(watch);
            write_file(file, "changed in place");
            co_await expect_change(watch);
            fs::remove(file);
            co_await expect_change(watch);
            write_file(file, "recreated");
            co_await expect_change(watch);
        }
    );
}

TEST_CASE("file watch filters sibling writes and can wait again after timeout")
{
    scratch_directory temp;
    const auto file = temp.path / "config";
    write_file(file, "initial");
    pace::run(
        [&](pace::io_context& io) -> pace::task<void>
        {
            pace::file_watch watch(io, file);
            write_file(temp.path / "sibling", "unrelated");
            auto result = co_await pace::as_result(pace::with_deadline(pace::deadline::after(50ms), watch.wait()));
            require(result.error() == std::errc::timed_out, "sibling event does not complete wait");
            co_await pace::with_task_group(
                [&](pace::task_group& group) -> pace::task<void>
                {
                    bool started = false;
                    auto waiter = group.spawn(
                        [&]() -> pace::task<void>
                        {
                            started = true;
                            co_await expect_change(watch);
                        }
                    );
                    while (!started)
                    {
                        co_await io.yield();
                    }
                    bool rejected = false;
                    try
                    {
                        co_await watch.wait();
                    }
                    catch (const std::logic_error&)
                    {
                        rejected = true;
                    }
                    require(rejected, "concurrent wait rejected");
                    write_file(file, "updated");
                    co_await waiter;
                }
            );
        }
    );
}

TEST_CASE("closing and destroying a watch cancels pending and unstarted waits")
{
    scratch_directory temp;
    pace::run(
        [&](pace::io_context& io) -> pace::task<void>
        {
            auto watch = std::make_unique<pace::file_watch>(io, temp.path / "config");
            auto delayed = watch->wait();
            co_await pace::with_task_group(
                [&](pace::task_group& group) -> pace::task<void>
                {
                    bool started = false;
                    auto waiter = group.spawn(
                        [&]() -> pace::task<void>
                        {
                            started = true;
                            auto result = co_await pace::as_result(watch->wait());
                            require(result.error() == std::errc::operation_canceled, "pending wait canceled");
                        }
                    );
                    while (!started)
                    {
                        co_await io.yield();
                    }
                    watch->close();
                    watch->close();
                    watch.reset();
                    co_await waiter;
                }
            );
            auto result = co_await pace::as_result(std::move(delayed));
            require(result.error() == std::errc::operation_canceled, "unstarted wait survives destruction");
        }
    );
}
