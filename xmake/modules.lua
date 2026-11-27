-- Keep the header-based libraries and their ABI; consumers opt into import pace
-- by depending on pace_module. Disabled builds never invoke a module scanner.
if has_config("modules") then
    local pace_root = path.directory(os.scriptdir())
    target("pace_module")
        set_kind("static")
        add_files(path.join(pace_root, "modules/*.cppm"), {public = true})
        add_deps("pace", {public = true})
    target_end()
end
