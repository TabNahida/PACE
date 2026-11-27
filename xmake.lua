set_project("pace")
set_version("0.6.1")
set_xmakever("2.9.8")
add_rules("mode.debug", "mode.release", "mode.releasedbg")
set_warnings("allextra")

includes("xmake/options.lua")
set_languages(get_config("cxxstandard") or "cxx20")

if (has_config("quic") or has_config("http3")) and is_plat("windows", "mingw") then
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
includes("xmake/packages.lua")

-- Flags in this file apply to every target in the included files below.
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
-- They are never installed.
add_includedirs("src")

-- Library targets, one per module.
includes("xmake/targets.lua")
-- Everything else keeps its options and dependencies in its own file.
includes("xmake/modules.lua", "xmake/tests.lua", "xmake/examples.lua", "xmake/benchmarks.lua", "xmake/fuzz.lua",
         "xmake/checks.lua")
