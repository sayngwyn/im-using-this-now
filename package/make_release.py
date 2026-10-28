"""Assemble the FOMOD release zip from the release DLL and the MCM build.

Layout: fomod/, "00 Core" (SKSE/Plugins: DLL and ini), "10 MCM" (the
plugin and its scripts). Entries are sorted with a fixed timestamp, so the
same inputs give the same zip. Run dll\\build.bat and mcm\\build.ps1 first.
Usage: python make_release.py <repo> <out dir>
"""
import re
import sys
import zipfile
from pathlib import Path

STAMP = (2026, 1, 1, 0, 0, 0)


def main(repo, out_dir):
    repo, out_dir = Path(repo), Path(out_dir)
    found = re.search(r"<Version>(.+?)</Version>", (repo / "package/fomod/info.xml").read_text("utf-8"))
    if not found:
        sys.exit("no <Version> in package/fomod/info.xml")
    version = found.group(1)
    entries = {f"fomod/{f.name}": f for f in (repo / "package/fomod").iterdir()}
    plugins = "00 Core/SKSE/Plugins"
    entries[f"{plugins}/ImUsingThisNow.dll"] = repo / "dll/build/windows/x64/releasedbg/ImUsingThisNow.dll"
    entries[f"{plugins}/ImUsingThisNow.ini"] = repo / "dll/ImUsingThisNow.ini"
    mcm = repo / "mcm/build"
    entries["10 MCM/ImUsingThisNow.esp"] = mcm / "ImUsingThisNow.esp"
    for pex in sorted((mcm / "Scripts").glob("*.pex")):
        entries[f"10 MCM/Scripts/{pex.name}"] = pex
    entries["10 MCM/Scripts/Source/ImUsingThisNow.psc"] = repo / "mcm/Source/Scripts/ImUsingThisNow.psc"
    entries["10 MCM/Scripts/Source/ImUsingThisNow_MCM.psc"] = repo / "mcm/Source/Scripts/ImUsingThisNow_MCM.psc"
    for name, src in entries.items():
        if not src.is_file():
            sys.exit(f"missing: {src} (for {name})")
    out_dir.mkdir(parents=True, exist_ok=True)
    out = out_dir / f"I'm Using This Now {version}.zip"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for name in sorted(entries):
            info = zipfile.ZipInfo(name, STAMP)
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, entries[name].read_bytes())
    print(f"{out}: version {version}, {len(entries)} files")


if __name__ == "__main__":
    main(*sys.argv[1:3])
