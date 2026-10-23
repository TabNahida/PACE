set_project("pace")
set_version("0.1.1")
set_xmakever("2.9.8")
add_rules("mode.debug", "mode.release", "mode.releasedbg")
set_warnings("allextra")

option("cxxstandard", {default = "cxx20", values = {"cxx20", "cxx23", "cxxlatest"}, description = "C++ language standard (C++20 or newer)"})
option("sanitizers", {default = false, description = "Enable AddressSanitizer and UndefinedBehaviorSanitizer"})
option("profiling", {default = false, description = "Enable intrusive per-thread stage timings (diagnostics only)"})
option("http1", {default = true, showmenu = true, description = "Enable the HTTP/1.0 and HTTP/1.1 application module"})
option("websocket", {default = true, showmenu = true, description = "Enable the WebSocket application module"})
option("tls", {default = false, showmenu = true, description = "Enable TLS/HTTPS with OpenSSL"})
option("dns", {default = false, showmenu = true, description = "Enable asynchronous DNS with c-ares"})
option("quic", {default = false, showmenu = true, description = "Enable QUIC with OpenSSL 3.5+ (also enabled by http3)"})
option("http2", {default = false, showmenu = true, description = "Enable HTTP/2 with nghttp2 (enable tls for HTTPS)"})
option("http3", {default = false, showmenu = true, description = "Enable HTTP/3 with nghttp3 and QUIC"})
set_languages(get_config("cxxstandard") or "cxx20")

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
if has_config("http2") then add_requires("nghttp2 1.70.0") end
if has_config("dns") then add_requires("c-ares 1.34.8") end
if has_config("http3") then add_requires("nghttp3 v1.18.0") end

if has_config("sanitizers") and not is_plat("windows") then
    add_cxxflags("-fsanitize=address,undefined", "-fno-omit-frame-pointer")
    add_ldflags("-fsanitize=address,undefined")
end

-- Layer 0: no networking or optional protocol dependencies.
target("pace_runtime")
    set_kind("static")
    add_files("src/runtime/*.cpp")
    add_headerfiles("include/(pace/**.hpp)")
    add_includedirs("include", {public = true})
    if has_config("profiling") then add_defines("PACE_ENABLE_PROFILING", {public = true}) end
    if is_plat("windows", "mingw") then
        add_syslinks("ws2_32", "bcrypt", {public = true})
        add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX", "_WIN32_WINNT=0x0A00", {public = true})
    else
        add_syslinks("pthread", {public = true})
    end
    if is_plat("windows") then add_cxxflags("/utf-8") end

target_end()

-- Layer 1: core sockets. Consumers can stop here with add_deps("pace_net").
target("pace_net")
    set_kind("static")
    add_files("src/networking/*.cpp")
    add_deps("pace_runtime", {public = true})
target_end()

-- Layers 2 and 3: each protocol is separately compiled and independently linked.
local extension_targets = {}
local application_targets = {}
local function protocol_target(name, layer, package_name, feature, dependencies)
    target("pace_" .. name)
        set_kind("static")
        add_files("src/" .. layer .. "/" .. name .. ".cpp")
        add_deps("pace_net", {public = true})
        for _, dep in ipairs(dependencies or {}) do add_deps(dep, {public = true}) end
        if package_name then add_packages(package_name, {public = true}) end
        add_defines("PACE_HAS_" .. feature, {public = true})
        if is_plat("windows") then add_cxxflags("/utf-8") end
    target_end()
    table.insert(layer == "extensions" and extension_targets or application_targets, "pace_" .. name)
end
if has_config("tls") then protocol_target("tls", "extensions", "openssl3", "TLS") end
if has_config("dns") then protocol_target("dns", "extensions", "c-ares", "DNS") end
if enable_quic then protocol_target("quic", "extensions", "openssl3", "QUIC") end
local tls_dependency = has_config("tls") and {"pace_tls"} or {}
if has_config("http1") then
    target("pace_http1")
        set_kind("static")
        add_files("src/application/http.cpp")
        add_deps("pace_net", {public = true})
        for _, dep in ipairs(tls_dependency) do add_deps(dep, {public = true}) end
        add_defines("PACE_HAS_HTTP1", {public = true})
    target_end()
    table.insert(application_targets, "pace_http1")
end
if has_config("http2") then protocol_target("http2", "application", "nghttp2", "HTTP2", tls_dependency) end
if has_config("http3") then protocol_target("http3", "application", "nghttp3", "HTTP3", {"pace_quic"}) end
if has_config("websocket") then protocol_target("websocket", "application", nil, "WEBSOCKET", tls_dependency) end
target("pace_extensions")
    set_kind("phony")
    add_deps("pace_net", {public = true})
    for _, dep in ipairs(extension_targets) do add_deps(dep, {public = true}) end
target_end()
target("pace_application")
    set_kind("phony")
    add_deps("pace_net", {public = true})
    for _, dep in ipairs(application_targets) do add_deps(dep, {public = true}) end
target_end()
-- Source-compatible aggregate: existing applications still add_deps("pace").
target("pace")
    set_kind("phony")
    add_deps("pace_extensions", "pace_application", {public = true})
target_end()

-- Auxiliary targets keep their options and dependencies in separate modules.
includes("xmake/tests.lua", "xmake/examples.lua", "xmake/benchmarks.lua")
