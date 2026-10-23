option("tests", {default = true, showmenu = true, description = "Build correctness tests"})

-- Paths stay anchored to PACE when included by another project.
local pace_root = path.directory(os.scriptdir())
if has_config("tests") then
    local test_names = {"runtime", "network", "udp"}
    if has_config("http1") then table.insert(test_names, "http_extended") end
    if has_config("websocket") then table.insert(test_names, "websocket") end
    if has_config("tls") then table.insert(test_names, "tls") end
    if has_config("http2") then table.insert(test_names, "http2") end
    if has_config("dns") then table.insert(test_names, "dns") end
    if has_config("quic") or has_config("http3") then table.insert(test_names, "quic") end
    if has_config("http3") then table.insert(test_names, "http3") end
    for _, name in ipairs(test_names) do
        target("pace_" .. name .. "_tests")
            set_kind("binary")
            add_files(path.join(pace_root, "tests", name .. "_tests.cpp"))
            add_deps("pace")
            if name == "network" and not has_config("http1") then add_defines("PACE_CORE_ONLY_TESTS") end
            set_rundir(pace_root)
            add_tests("default")
        target_end()
    end
end
