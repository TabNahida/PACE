set_project("pace")
set_version("0.6.1")
set_xmakever("2.9.8")
add_rules("mode.debug", "mode.release", "mode.releasedbg")
set_warnings("allextra")

option("cxxstandard", {default = "cxx20", values = {"cxx20", "cxx23", "cxxlatest"}, description = "C++ language standard (C++20 or newer)"})
option("modules", {default = false, showmenu = true, description = "Build the optional C++20 named module (import pace)"})
option("sanitizers", {default = false, description = "Enable AddressSanitizer and UndefinedBehaviorSanitizer"})
option("uring", {default = false, showmenu = true, description = "Build the optional Linux io_uring readiness backend (liburing)"})
option("profiling", {default = false, description = "Enable intrusive per-thread stage timings (diagnostics only)"})
option("fuzz", {default = "none", values = {"none", "libfuzzer", "replay"}, showmenu = true, description = "Build fuzz targets with libFuzzer (Clang) or a corpus replay driver"})
option("http1", {default = true, showmenu = true, description = "Enable the HTTP/1.0 and HTTP/1.1 application module"})
option("websocket", {default = true, showmenu = true, description = "Enable the WebSocket application module"})
option("tls", {default = false, showmenu = true, description = "Enable TLS/HTTPS with OpenSSL"})
option("dns", {default = false, showmenu = true, description = "Enable asynchronous DNS with c-ares"})
option("quic", {default = false, showmenu = true, description = "Enable QUIC (also enabled by http3)"})
option("quic_backend", {default = "ngtcp2", values = {"ngtcp2", "openssl"}, showmenu = true, description = "QUIC transport implementation"})
option("http2", {default = false, showmenu = true, description = "Enable HTTP/2 with nghttp2 (enable tls for HTTPS)"})
option("http3", {default = false, showmenu = true, description = "Enable HTTP/3 with nghttp3 and QUIC"})
set_languages(get_config("cxxstandard") or "cxx20")

if has_config("uring") and is_plat("linux") then add_requires("liburing 2.12") end

local enable_quic = has_config("quic") or has_config("http3")
if enable_quic and is_plat("windows", "mingw") then
    -- Error helpers are available in sandbox callbacks, not the top-level DSL.
    -- Stop before optional dependencies are resolved on an unsupported platform.
    target("pace_unsupported_protocol")
        set_kind("phony")
        on_load(function ()
            raise("QUIC/HTTP3 currently require a POSIX platform; Windows QUIC transport is not implemented")
        end)
    target_end()
    return
end
if has_config("tls") or enable_quic then
    if is_plat("windows") and is_arch("arm64") then
        includes("xmake/openssl_arm64.lua")
        add_requires("pace-openssl3-arm64 3.5.7", {alias = "openssl3", system = false})
    else
        add_requires("openssl3 3.5.7")
    end
end
if enable_quic and get_config("quic_backend") == "ngtcp2" then
    if is_plat("bsd") then
        includes("xmake/ngtcp2_bsd.lua")
        add_requires("pace-ngtcp2-bsd 1.26.0", {alias = "ngtcp2", system = false, configs = {openssl = "openssl3"}})
        add_requireconfs("pace-ngtcp2-bsd.openssl3", {version = "3.5.7", override = true})
    else
        add_requires("ngtcp2 1.26.0", {configs = {openssl = "openssl3"}})
        add_requireconfs("ngtcp2.openssl3", {version = "3.5.7", override = true})
    end
end
if has_config("http2") then add_requires("nghttp2 1.70.0") end
if has_config("dns") then add_requires("c-ares 1.34.8") end
if has_config("http3") then add_requires("nghttp3 v1.18.0") end

-- libFuzzer needs coverage instrumentation in the library code it exercises, not only in the targets.
if get_config("fuzz") == "libfuzzer" then
    add_cxflags("-fsanitize=fuzzer-no-link,address,undefined", "-fno-omit-frame-pointer", {force = true})
    add_ldflags("-fsanitize=address,undefined", {force = true})
end

if has_config("sanitizers") and not is_plat("windows") then
    add_cxxflags("-fsanitize=address,undefined", "-fno-omit-frame-pointer")
    add_ldflags("-fsanitize=address,undefined")
end

-- Internal headers live beside the sources in src/<module>/ and are included as "<module>/name.hpp".
-- They are never installed; tests and benchmarks that need them add src/ themselves.
add_includedirs("src")

-- One library target per module. A module compiles only src/<module>/, installs only its own headers
-- (include/pace/<module>/ and the umbrella pace/<module>.hpp) and links only the modules it needs, so
-- `xmake build pace_tls` builds runtime, net and tls and nothing else.
local modules = {}

local function module_headers(name)
    add_headerfiles("include/(pace/" .. name .. "/**.hpp)", "include/(pace/" .. name .. ".hpp)")
end

target("pace_runtime")
    set_kind("static")
    add_files("src/runtime/*.cpp")
    module_headers("runtime")
    -- The aggregate header includes whichever modules the consumer links.
    add_headerfiles("include/(pace/pace.hpp)")
    add_includedirs("include", {public = true})
    if has_config("uring") and is_plat("linux") then
        add_packages("liburing", {public = true})
        add_defines("PACE_HAS_IO_URING", {public = true})
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
table.insert(modules, "pace_runtime")

-- spec: files, deps (module names), packages, feature (defines PACE_HAS_<feature>), headers (extra
-- header directories this target installs), kind (default static).
local function module(name, spec)
    target("pace_" .. name)
        set_kind(spec.kind or "static")
        if spec.files then add_files(spec.files) end
        module_headers(name)
        for _, extra in ipairs(spec.headers or {}) do module_headers(extra) end
        for _, dependency in ipairs(spec.deps or {}) do add_deps("pace_" .. dependency, {public = true}) end
        for _, package_name in ipairs(spec.packages or {}) do add_packages(package_name, {public = true}) end
        for _, define in ipairs(spec.defines or {}) do add_defines(define, {public = true}) end
        if spec.feature then add_defines("PACE_HAS_" .. spec.feature, {public = true}) end
        if is_plat("windows") and spec.kind ~= "headeronly" then add_cxxflags("/utf-8") end
    target_end()
    table.insert(modules, "pace_" .. name)
end

-- Optional dependencies a module uses only when they were enabled.
local function when(enabled, ...)
    return enabled and {...} or {}
end

local function join(...)
    local result = {}
    for _, list in ipairs({...}) do
        for _, value in ipairs(list) do table.insert(result, value) end
    end
    return result
end

module("net", {files = "src/net/*.cpp", deps = {"runtime"}})
-- Protocol-independent HTTP messages and router; header-only for now.
module("http", {kind = "headeronly", deps = {"runtime"}})
if has_config("tls") then
    module("tls", {files = "src/tls/*.cpp", deps = {"net"}, packages = {"openssl3"}, feature = "TLS"})
end
if has_config("dns") then
    module("dns", {files = "src/dns/*.cpp", deps = {"net"}, packages = {"c-ares"}, feature = "DNS"})
end
if enable_quic then
    local ngtcp2 = get_config("quic_backend") == "ngtcp2"
    module("quic", {
        files = ngtcp2 and "src/quic/quic_ngtcp2.cpp" or "src/quic/quic_openssl.cpp",
        deps = {"net"},
        packages = ngtcp2 and {"openssl3", "ngtcp2"} or {"openssl3"},
        defines = when(ngtcp2, "PACE_QUIC_NGTCP2"),
        feature = "QUIC",
    })
end
local tls = when(has_config("tls"), "tls")
local dns = when(has_config("dns"), "dns")
if has_config("http1") then
    -- The gateway services stay in this target until P7: http::app's mount()/proxy() call them and
    -- the reverse proxy uses the HTTP/1 client, so a separate pace_gateway target would be circular.
    module("http1", {
        files = {"src/http1/*.cpp", "src/gateway/*.cpp"},
        headers = {"gateway"},
        deps = join({"net", "http"}, tls, dns),
        feature = "HTTP1",
    })
end
if has_config("http2") then
    module("http2", {files = "src/http2/*.cpp", deps = join({"net", "http"}, tls, dns), packages = {"nghttp2"},
                     feature = "HTTP2"})
end
if has_config("http3") then
    module("http3", {files = "src/http3/*.cpp", deps = join({"net", "http", "quic"}, dns), packages = {"nghttp3"},
                     feature = "HTTP3"})
end
if has_config("websocket") then
    module("websocket", {files = "src/websocket/*.cpp", deps = join({"net"}, tls), feature = "WEBSOCKET"})
end

-- Every enabled module: add_deps("pace").
target("pace")
    set_kind("phony")
    for _, name in ipairs(modules) do add_deps(name, {public = true}) end
target_end()

-- Auxiliary targets keep their options and dependencies in separate modules.
includes("xmake/modules.lua")
includes("xmake/tests.lua", "xmake/examples.lua", "xmake/benchmarks.lua", "xmake/fuzz.lua", "xmake/checks.lua")
