-- Paths stay anchored to PACE when included by another project.
local pace_root = path.directory(os.scriptdir())

-- One library target per module. A module compiles only src/<module>/, installs only its own headers
-- (include/pace/<module>/ and the umbrella pace/<module>.hpp) and links only the modules it needs, so
-- `xmake build pace_tls` builds runtime, net and tls and nothing else.
local modules = {}

local function module_headers(name)
    add_headerfiles(path.join(pace_root, "include/(pace/" .. name .. "/**.hpp)"),
                    path.join(pace_root, "include/(pace/" .. name .. ".hpp)"))
end

target("pace_runtime")
    set_kind("static")
    add_files(path.join(pace_root, "src/runtime/*.cpp"))
    module_headers("runtime")
    add_includedirs(path.join(pace_root, "include"), {public = true})
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
        for _, pattern in ipairs(type(spec.files) == "table" and spec.files or {spec.files}) do
            add_files(path.join(pace_root, pattern))
        end
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
if has_config("quic") or has_config("http3") then
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
    set_kind("headeronly")
    for _, name in ipairs(modules) do add_deps(name, {public = true}) end
    -- The aggregate header includes every enabled module, so it belongs to the target that links them all.
    add_headerfiles(path.join(pace_root, "include/(pace/pace.hpp)"))
target_end()
