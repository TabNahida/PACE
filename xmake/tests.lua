option("tests", {default = true, showmenu = true, description = "Build correctness tests"})

-- Paths stay anchored to PACE when included by another project.
local pace_root = path.directory(os.scriptdir())
if has_config("tests") then
    local test_names = {"runtime", "network", "udp", "direct_client"}
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
    -- Force default trust loading to fail without a production test hook. The
    -- application trust path must still perform real verified handshakes.
    if has_config("tls") and is_plat("linux") then
        target("pace_tls_trust_failure_tests")
            set_kind("binary")
            add_files(path.join(pace_root, "tests", "tls_trust_failure_tests.cpp"))
            add_deps("pace_tls")
            add_ldflags("-Wl,--wrap=SSL_CTX_set_default_verify_paths", {force = true})
            set_rundir(pace_root)
            add_tests("default")
        target_end()
    end
    if has_config("tls") then
        -- Independent dependency probe: no PACE transport or coroutine code.
        target("pace_openssl_diagnostic")
            set_kind("binary")
            add_files(path.join(pace_root, "tests", "openssl_diagnostic.cpp"))
            add_packages("openssl3")
            if is_plat("windows") then
                add_cxxflags("/utf-8")
                set_symbols("debug")
                add_syslinks("dbghelp")
                add_ldflags("/DEBUG", "/MAP", {force = true})
            end
            set_rundir(pace_root)
            add_tests("default")
        target_end()
    end
end
