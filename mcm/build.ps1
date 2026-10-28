# MCM edition: compiles the menu scripts and writes the plugin into mcm\build.
# Needs Skyrim SE's Papyrus Compiler (Creation Kit) and, in mcm\deps (ignored):
#   skyui\   SKI_ConfigBase.psc, SKI_ConfigManager.psc, SKI_QuestBase.psc
#            (github.com/schlangster/skyui, dist/Data/Scripts/Source)
#   vanilla\ the game's Data\Scripts.zip, unpacked
# plus SKSE's script sources (any SKSE64 install's Scripts\Source).
# Usage: build.ps1 -Game <Skyrim SE folder> -SkseSource <SKSE's Scripts\Source>
param(
    [Parameter(Mandatory)][string]$Game,
    [Parameter(Mandatory)][string]$SkseSource
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$out = "$here\build"
New-Item -ItemType Directory -Force "$out\Scripts" | Out-Null
$imports = "$here\Source\Scripts;$here\deps\skyui;$SkseSource;$here\deps\vanilla\Source\Scripts"
& "$Game\Papyrus Compiler\PapyrusCompiler.exe" "$here\Source\Scripts" -all -optimize `
    -f="TESV_Papyrus_Flags.flg" -i="$imports" -o="$out\Scripts"
if ($LASTEXITCODE -ne 0) { throw "Papyrus compile failed" }
python "$here\scrub_pex.py" "$out\Scripts" sayngwyn
if ($LASTEXITCODE -ne 0) { throw "scrubbing the scripts failed" }
python "$here\make_plugin.py" "$out\ImUsingThisNow.esp"
if ($LASTEXITCODE -ne 0) { throw "plugin build failed" }
Get-ChildItem $out -Recurse -File | ForEach-Object { "$($_.FullName.Substring($out.Length + 1))  $($_.Length) bytes" }
