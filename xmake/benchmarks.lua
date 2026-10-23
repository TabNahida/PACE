option("benchmarks", {default = false, showmenu = true, description = "Build fair TCP benchmarks against standalone Asio and libuv"})
option("components", {default = false, showmenu = true, description = "Build PACE component microbenchmarks without external dependencies"})
option("scaling", {default = false, showmenu = true, description = "Build PACE worker/execution scaling benchmark without external dependencies"})
option("protocol_benchmarks", {default = false, showmenu = true, description = "Build enabled HTTP/WebSocket protocol probes (local performance measurements)"})

if has_config("benchmarks") then add_requires("asio 1.34.2", "libuv 1.52.1") end

-- Paths stay anchored to PACE when included by another project.
local pace_root = path.directory(os.scriptdir())
if has_config("protocol_benchmarks") then
    if has_config("http2") or has_config("http3") then
        target("pace_protocol_multiplex")
            set_kind("binary")
            add_files(path.join(pace_root, "benchmarks/protocol_multiplex.cpp"))
            if has_config("http2") then add_deps("pace_http2") end
            if has_config("http3") then add_deps("pace_http3") end
            if has_config("tls") then add_deps("pace_tls") end
        target_end()
    end
    if has_config("http1") then
        target("pace_http_close_probe")
            set_kind("binary")
            add_files(path.join(pace_root, "benchmarks/http_close.cpp"))
            add_deps("pace_http1")
        target_end()
    end
    if has_config("websocket") then
        target("pace_websocket_bench")
            set_kind("binary")
            add_files(path.join(pace_root, "benchmarks/websocket_bench.cpp"))
            add_deps("pace_websocket")
        target_end()
    end
end
if has_config("scaling") then
    target("pace_scaling")
        set_kind("binary")
        add_files(path.join(pace_root, "benchmarks/scaling_bench.cpp"))
        add_deps("pace_net")
    target_end()
end
if has_config("benchmarks") then
    target("pace_bench")
        set_kind("binary")
        add_files(path.join(pace_root, "benchmarks/echo_bench.cpp"))
        add_deps("pace_net")
        add_packages("asio", "libuv")
        if is_plat("windows", "mingw") then add_syslinks("psapi") end
        add_defines("PACE_HAS_ASIO", "PACE_HAS_LIBUV", "ASIO_STANDALONE")
    target_end()
end
if has_config("components") then
    target("pace_components")
        set_kind("binary")
        add_files(path.join(pace_root, "benchmarks/components_bench.cpp"))
        add_deps("pace_net")
    target_end()
end
