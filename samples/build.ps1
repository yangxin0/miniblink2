# build.ps1 — build the mb-API samples against a dist\<mode> SDK (Windows x64).
#
#   powershell -File samples\build.ps1 [-Mode release|debug]
#
# PowerShell peer of scripts/build-samples.sh (macOS). Run from a VS x64
# developer environment (cl.exe on PATH). Build the SDK first:
#   powershell -File scripts\build-lib.ps1 --release
#
# The samples are OS-INDEPENDENT (samples\*.cc / *.c); windowed ones link the
# Win32 backend of the shared scaffold (samples\compat\win\mb_window.cc — the
# Cocoa peer is compat\mac\mb_window.mm). Binaries land IN dist\<mode>\ next to
# miniblink2.dll and the engine's runtime data (icudtl.dat, the paks, the V8
# snapshots), which the engine loads from beside the executable.
#
# Run from the dist dir:   cd dist\release; .\sample2_basic_app.exe
param([string]$Mode = 'release')
$ErrorActionPreference = 'Stop'

$Samples = $PSScriptRoot
$Root = Split-Path -Parent $Samples
$Dist = Join-Path $Root "dist\$Mode"
$ImpLib = Join-Path $Dist 'miniblink2.dll.lib'

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
  Write-Error 'cl.exe not on PATH — run from a "x64 Native Tools" VS prompt.'
}
if (-not (Test-Path $ImpLib)) {
  Write-Error "no $ImpLib — build the SDK first: powershell -File scripts\build-lib.ps1 --$Mode"
}

$Obj = Join-Path $Dist 'sample-obj'
New-Item -ItemType Directory -Force $Obj | Out-Null

# /utf-8: the sources carry UTF-8 comments/strings; without it cl decodes them
# in the system codepage (C4819 warning spam on CJK-locale machines).
$Common = @('/nologo', '/O2', '/EHsc', '/utf-8', '/DUNICODE', '/D_UNICODE',
            "/I$Root\src", "/I$Samples", "/Fo$Obj\")
$Libs = @($ImpLib, 'user32.lib', 'gdi32.lib', 'shell32.lib')
$Scaffold = Join-Path $Samples 'compat\win\mb_window.cc'

function Build-Sample([string]$Name, [string[]]$Sources, [string[]]$ExtraFlags) {
  Write-Host "==> $Name"
  & cl @Common @ExtraFlags @Sources /Fe:(Join-Path $Dist "$Name.exe") /link @Libs
  if ($LASTEXITCODE -ne 0) { throw "$Name failed" }
  Write-Host "    -> $Dist\$Name.exe"
}

# Headless (no scaffold).
Build-Sample 'sample1_render_to_png' @("$Samples\sample1_render_to_png\main.cc") @('/std:c++20')
# Sample 6 is PLAIN C — cl compiles .c as C; proves the mb headers are C-clean.
Build-Sample 'sample6_intro_c_api'   @("$Samples\sample6_intro_c_api\main.c")    @()

# Windowed (OS-independent sample + the Win32 scaffold backend).
foreach ($s in 'sample2_basic_app', 'sample3_resizable_app', 'sample4_javascript',
               'sample5_file_loading', 'sample9_multi_window') {
  Build-Sample $s @("$Samples\$s\main.cc", $Scaffold) @('/std:c++20')
}

# Sample 8 — minibrowser: OS-independent app + Win32 scaffold + CDP bridge
# (winsock + common controls for the tracking tooltip).
$OldLibs = $Libs
$Libs = $OldLibs + @('ws2_32.lib', 'comctl32.lib')
Build-Sample 'minibrowser' @("$Samples\sample8_minibrowser\main.cc",
                             "$Samples\sample8_minibrowser\cdp_bridge.cc",
                             $Scaffold) @('/std:c++20')
$Libs = $OldLibs

Write-Host "==> done. run FROM the dist dir so runtime data is found:"
Write-Host "    cd $Dist; .\sample2_basic_app.exe"
