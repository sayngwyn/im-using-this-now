-- I'm Using This Now: one SKSE DLL for SE 1.5.97, AE 1.6.x and 1.7.x
-- (CommonLibSSE-NG, Address Library). Built by build.bat.
includes("lib/commonlibsse-ng")

set_project("ImUsingThisNow")
set_version("1.0.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

add_rules("mode.debug", "mode.releasedbg")

-- Test builds (build.bat test) add the in-game self-test; never ship them.
option("selftest", function()
    set_default(false)
    add_defines("IUTN_SELFTEST")
end)

target("ImUsingThisNow")
    add_rules("commonlibsse-ng.plugin", {
        name = "ImUsingThisNow",
        author = "sayngwyn",
        description = "I'm Using This Now: NPCs step away from crafting stations you activate"
    })

    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
    add_options("selftest")
    -- Record only the .pdb's file name in the DLL, not the build folder.
    add_shflags("/PDBALTPATH:%_PDB%", {force = true})
