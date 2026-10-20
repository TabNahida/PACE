set_project("pace")
set_version("0.1.0")
set_xmakever("2.9.8")
add_rules("mode.debug", "mode.release", "mode.releasedbg")
set_warnings("allextra")

option("cxxstandard")
    set_default("cxx20")
    set_values("cxx20", "cxx23", "cxxlatest")
    set_description("C++ language standard (C++20 or newer)")
option_end()
set_languages(get_config("cxxstandard") or "cxx20")

option("tests")
    set_default(true)
    set_showmenu(true)
option_end()
option("examples")
    set_default(true)
    set_showmenu(true)
option_end()
option("benchmarks")
    set_default(false)
    set_showmenu(true)
    set_description("Build fair TCP benchmarks against standalone Asio and libuv")
option_end()
option("sanitizers")
    set_default(false)
    set_description("Enable AddressSanitizer and UndefinedBehaviorSanitizer")
option_end()

if has_config("benchmarks") then
    add_requires("asio 1.34.2", "libuv 1.52.1")
end
if has_config("sanitizers") and not is_plat("windows") then
    add_cxxflags("-fsanitize=address,undefined", "-fno-omit-frame-pointer")
    add_ldflags("-fsanitize=address,undefined")
end

target("pace")
    set_kind("static")
    add_files("src/*.cpp")
    add_headerfiles("include/(pace/**.hpp)")
    add_includedirs("include", {public = true})
    if is_plat("windows", "mingw") then
        add_syslinks("ws2_32", {public = true})
        add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX", "_WIN32_WINNT=0x0A00", {public = true})
    else
        add_syslinks("pthread", {public = true})
    end
    if is_plat("windows") then add_cxxflags("/utf-8") end

target_end()

if has_config("tests") then
    for _, name in ipairs({"runtime", "network"}) do
        target("pace_" .. name .. "_tests")
            set_kind("binary")
            add_files("tests/" .. name .. "_tests.cpp")
            add_deps("pace")
            add_tests("default")
        target_end()
    end
end
if has_config("examples") then
    for _, name in ipairs({"echo_server", "http_server"}) do
        target(name)
            set_kind("binary")
            add_files("examples/" .. name .. ".cpp")
            add_deps("pace")
        target_end()
    end
end
if has_config("benchmarks") then
    target("pace_bench")
        set_kind("binary")
        add_files("benchmarks/echo_bench.cpp")
        add_deps("pace")
        add_packages("asio", "libuv")
        add_defines("PACE_HAS_ASIO", "PACE_HAS_LIBUV", "ASIO_STANDALONE")
    target_end()
end
