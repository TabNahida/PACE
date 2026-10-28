option("examples", {default = true, showmenu = true, description = "Build runnable examples and tutorials"})

-- Paths stay anchored to PACE when included by another project.
local pace_root = path.directory(os.scriptdir())
if has_config("examples") then
    local servers = {"echo_server"}
    if has_config("http1") then table.insert(servers, "http_server") end
    for _, name in ipairs(servers) do
        target(name)
            set_kind("binary")
            add_files(path.join(pace_root, "examples", name .. ".cpp"))
            add_deps("pace")
        target_end()
    end
    local tutorials = {"tutorial_tasks", "tutorial_structured", "tutorial_tcp", "tutorial_udp", "tutorial_workers", "tutorial_shutdown"}
    if has_config("http1") then
        table.insert(tutorials, "tutorial_http")
        table.insert(tutorials, "tutorial_http_client")
    end
    if has_config("websocket") then table.insert(tutorials, "tutorial_websocket") end
    if has_config("tls") then table.insert(tutorials, "tutorial_tls") end
    if has_config("http2") then table.insert(tutorials, "tutorial_http2") end
    if has_config("dns") then table.insert(tutorials, "tutorial_dns") end
    if has_config("quic") or has_config("http3") then table.insert(tutorials, "tutorial_quic") end
    if has_config("http3") then table.insert(tutorials, "tutorial_http3") end
    for _, name in ipairs(tutorials) do
        target(name)
            set_kind("binary")
            add_files(path.join(pace_root, "examples", name .. ".cpp"))
            add_deps("pace")
            set_rundir(pace_root)
            if has_config("tests") then add_tests("default") end
        target_end()
    end
end
