-- Fuzz targets. `--fuzz=libfuzzer` (Clang with libFuzzer) instruments PACE and links libFuzzer
-- with AddressSanitizer and UndefinedBehaviorSanitizer; `--fuzz=replay` builds the same targets with
-- a small driver that replays files, so any toolchain can run the corpus as a regression test.
-- The option itself and the instrumentation flags live in xmake.lua, because they must apply to
-- the library targets too.

-- Paths stay anchored to PACE when included by another project.
local pace_root = path.directory(os.scriptdir())
local mode = get_config("fuzz")
if mode == "libfuzzer" or mode == "replay" then
    local fuzzers = {}
    if has_config("http1") then
        fuzzers.http1_server = "pace_http1"
        fuzzers.http1_client = "pace_http1"
    end
    if has_config("websocket") then
        fuzzers.websocket_server = "pace_websocket"
    end
    for name, dependency in pairs(fuzzers) do
        target("pace_fuzz_" .. name)
            set_kind("binary")
            set_default(true)
            add_files(path.join(pace_root, "tests/fuzz", name .. ".cpp"))
            add_includedirs(path.join(pace_root, "tests"))
            add_deps(dependency)
            if mode == "libfuzzer" then
                add_ldflags("-fsanitize=fuzzer", {force = true})
            else
                add_files(path.join(pace_root, "tests/fuzz/standalone_main.cpp"))
            end
            set_rundir(pace_root)
            -- `xmake test` replays the seed corpus in both modes.
            -- `-runs=0` makes libFuzzer execute the corpus once instead of fuzzing.
            add_tests("corpus", {runargs = {"-runs=0", path.join(pace_root, "tests/fuzz/corpus", name)}})
        target_end()
    end
end
