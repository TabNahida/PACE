#pragma once

// Compiler and language requirements, checked wherever PACE is included so that an unsupported toolchain fails with
// a clear message instead of an obscure error deep in a coroutine or a concept. The minimums are the oldest versions
// CI builds and tests (see docs/reference/platform-support.md).

#if defined(_MSVC_LANG)
#if _MSVC_LANG < 202002L
#error "PACE requires C++20: compile with /std:c++20 or newer"
#endif
#elif __cplusplus < 202002L
#error "PACE requires C++20: compile with -std=c++20 or newer"
#endif

#if defined(__clang__)
#if defined(__apple_build_version__)
#if __clang_major__ < 16
#error "PACE requires Apple Clang 16 (Xcode 16) or newer"
#endif
#elif __clang_major__ < 16
#error "PACE requires Clang 16 or newer"
#endif
#elif defined(__GNUC__)
#if __GNUC__ < 13
#error "PACE requires GCC 13 or newer"
#endif
#elif defined(_MSC_VER)
#if _MSC_VER < 1938
#error "PACE requires MSVC 19.38 (Visual Studio 2022 17.8) or newer"
#endif
#endif
