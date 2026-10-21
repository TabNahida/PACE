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
option("components")
    set_default(false)
    set_showmenu(true)
    set_description("Build PACE component microbenchmarks without external dependencies")
option_end()
option("scaling")
    set_default(false)
    set_showmenu(true)
    set_description("Build PACE worker/execution scaling benchmark without external dependencies")
option_end()
option("profiling")
    set_default(false)
    set_description("Enable intrusive per-thread stage timings (diagnostics only)")
option_end()
option("tls")
    set_default(false)
    set_showmenu(true)
    set_description("Enable TLS/HTTPS with OpenSSL")
option_end()
option("http2")
    set_default(false)
    set_showmenu(true)
    set_description("Enable HTTP/2 with nghttp2 (enable tls for HTTPS)")
option_end()

if has_config("tls") then add_requires("openssl3 3.5.7") end
if has_config("http2") then add_requires("nghttp2 1.70.0") end

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
    if has_config("tls") then
        add_packages("openssl3", {public = true})
        add_defines("PACE_HAS_TLS", {public = true})
    end
    if has_config("http2") then
        add_packages("nghttp2", {public = true})
        add_defines("PACE_HAS_HTTP2", {public = true})
    end
    if has_config("profiling") then add_defines("PACE_ENABLE_PROFILING", {public = true}) end
    if is_plat("windows", "mingw") then
        add_syslinks("ws2_32", "bcrypt", {public = true})
        add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX", "_WIN32_WINNT=0x0A00", {public = true})
    else
        add_syslinks("pthread", {public = true})
    end
    if is_plat("windows") then add_cxxflags("/utf-8") end

target_end()

if has_config("tests") then
    local test_names = {"runtime", "network", "udp", "http_extended", "websocket"}
    if has_config("tls") then table.insert(test_names, "tls") end
    if has_config("http2") then table.insert(test_names, "http2") end
    for _, name in ipairs(test_names) do
        target("pace_" .. name .. "_tests")
            set_kind("binary")
            add_files("tests/" .. name .. "_tests.cpp")
            add_deps("pace")
            set_rundir(os.projectdir())
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
    local tutorials = {"tutorial_tasks", "tutorial_tcp", "tutorial_udp", "tutorial_http", "tutorial_http_client", "tutorial_websocket", "tutorial_workers", "tutorial_shutdown"}
    if has_config("tls") then table.insert(tutorials, "tutorial_tls") end
    if has_config("http2") then table.insert(tutorials, "tutorial_http2") end
    for _, name in ipairs(tutorials) do
        target(name)
            set_kind("binary")
            add_files("examples/" .. name .. ".cpp")
            add_deps("pace")
            set_rundir(os.projectdir())
            if has_config("tests") then add_tests("default") end
        target_end()
    end
end
if has_config("scaling") then
    target("pace_scaling")
        set_kind("binary")
        add_files("benchmarks/scaling_bench.cpp")
        add_deps("pace")
    target_end()
end
if has_config("benchmarks") then
    target("pace_bench")
        set_kind("binary")
        add_files("benchmarks/echo_bench.cpp")
        add_deps("pace")
        add_packages("asio", "libuv")
        if is_plat("windows", "mingw") then add_syslinks("psapi") end
        add_defines("PACE_HAS_ASIO", "PACE_HAS_LIBUV", "ASIO_STANDALONE")
    target_end()
end
if has_config("components") then
    target("pace_components")
        set_kind("binary")
        add_files("benchmarks/components_bench.cpp")
        add_deps("pace")
    target_end()
end
