#pragma once

// Shared helpers for PACE correctness tests. Tests use doctest's TEST_CASE / SUBCASE / CHECK / REQUIRE.
// tests/support/main.cpp provides main() and a watchdog that ends a hung test case.

#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>

#include <doctest/doctest.h>

namespace pace::test
{
// Thrown by require(). It derives from std::runtime_error so code under test that catches
// std::exception (for example a coroutine body) still propagates it like any other failure.
class failure : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// Usable from coroutines and from threads other than the test thread, where doctest's REQUIRE
// cannot be used safely. Throws with the caller's location; doctest reports the message when the
// exception leaves the test case.
inline void require(bool value, std::string_view what, std::source_location where = std::source_location::current())
{
    if (!value)
    {
        throw failure(std::string(where.file_name()) + ":" + std::to_string(where.line()) + ": " + std::string(what));
    }
}

// A test executable can also act as an interop peer (for example `pace_quic_tests --interop-server`).
// Register the mode at namespace scope; when argv[1] equals `flag`, main() calls `run` with the full
// argv instead of running doctest, and exits with its return value.
//
//     const pace::test::command interop_server{"--interop-server", run_interop_peer};
using command_handler = int (*)(int argc, char** argv);

struct command
{
    command(const char* flag, command_handler run);
};
} // namespace pace::test
