# build-lib.ps1 — build the self-contained miniblink2 SDK on Windows.
# PowerShell peer of scripts/build-lib.sh: SAME flags, same profiles, same dist
# layout (see the bash header for full docs).
#
#   scripts\build-lib.ps1 [--release|--debug] [--ship]
#                         [--webgpu] [--video] [--ml] [--wasm] [--webrtc] [--av1-encode]
#                         [--turbofan] [--maglev]
#                         [--tracing] [--swiftshader] [--icu-full]
#                         [--chromium DIR] [--depot DIR] [--no-stage] [--print-only]
#
# Profiles (one out dir per profile, exactly like the bash script):
#   --release        dev release: /O2, DCHECKs ON  -> dist\release\miniblink2.dll
#                    + miniblink2.dll.lib + merged miniblink2_static.lib
#                    (out\mono-release)
#   --release --ship publishable: size-optimized, DCHECKs OUT
#                    dll pass (ThinLTO): out\mono-release-dynamic
#                    static pass (native): out\mono-release-static
#   --debug          no optimization, full symbols -> dist\debug\miniblink2.dll
#                    (out\mono-debug)
#
# MB_JOBS caps ninja parallelism (env var, like the bash script).
#
# Windows notes vs the mac script:
#   - artifacts are miniblink2.dll / miniblink2.dll.lib / miniblink2.dll.pdb and a
#     merged COFF archive miniblink2_static.lib (llvm-ar MRI, same recipe).
#   - the vendored curl is the Schannel DLL: staged from third_party\curl\win\lib
#     (libcurl_imp.lib + libcurl.dll); libcurl.dll ships in dist.
#   - GL runtime: SwiftShader-ANGLE is the engine DEFAULT on Windows (no Metal), so
#     libEGL.dll / libGLESv2.dll / vk_swiftshader.dll / vk_swiftshader_icd.json
#     always ship in dist; --swiftshader is accepted for parity but is a no-op.
#   - --depot is accepted for parity; gn/ninja come from buildtools\win + PATH.
#   - no strip step: symbol_level in args.gn already controls PE/PDB size.
$ErrorActionPreference = 'Stop'

# --- GNU-style args, identical to build-lib.sh -----------------------------------
$Mode = 'release'; $Ship = $false
$WebGpu = $false; $Video = $false; $Ml = $false; $Wasm = $false; $WebRtc = $false
$Av1Encode = $false; $Turbofan = $false; $Maglev = $false; $Tracing = $false
$SwiftShader = $false; $IcuFull = $false
$Chromium = ''; $Depot = ''; $NoStage = $false; $PrintOnly = $false
for ($i = 0; $i -lt $args.Count; $i++) {
  switch ($args[$i]) {
    '--release'     { $Mode = 'release' }
    '--debug'       { $Mode = 'debug' }
    '--ship'        { $Ship = $true }
    '--chromium'    { $i++; $Chromium = $args[$i] }
    '--depot'       { $i++; $Depot = $args[$i] }   # parity; unused on Windows
    '--no-stage'    { $NoStage = $true }
    '--print-only'  { $PrintOnly = $true }
    '--webgpu'      { $WebGpu = $true }
    '--video'       { $Video = $true }
    '--ml'          { $Ml = $true }
    '--wasm'        { $Wasm = $true }
    '--webrtc'      { $WebRtc = $true }
    '--turbofan'    { $Turbofan = $true }
    '--maglev'      { $Maglev = $true }
    '--av1-encode'  { $Av1Encode = $true }
    '--tracing'     { $Tracing = $true }
    '--swiftshader' { $SwiftShader = $true }       # no-op: always shipped on Windows
    '--icu-full'    { $IcuFull = $true }
    { $_ -in '-h','--help' } {
      Get-Content $PSCommandPath | Select-Object -Skip 1 -First 33 |
        ForEach-Object { $_ -replace '^# ?','' }
      exit 0
    }
    default { Write-Error "build-lib.ps1: unknown arg '$($args[$i])'"; exit 2 }
  }
}
if ($env:CHROMIUM -and -not $Chromium) { $Chromium = $env:CHROMIUM }

$Here = Split-Path -Parent $PSScriptRoot                  # repo root (script lives in scripts\)
if (-not $Chromium) {
  $parent = Split-Path -Parent $Here
  $Chromium = Join-Path $parent 'chromium-150.0.7871.24'
}
if (-not (Test-Path (Join-Path $Chromium 'third_party\blink\renderer'))) {
  throw "not a Chromium checkout: $Chromium (pass -Chromium DIR)"
}
if ($Ship -and $Mode -eq 'debug') { throw '-Ship applies to -Mode release only' }

# --- toolchain -----------------------------------------------------------------
$Gn = Join-Path $Chromium 'buildtools\win\gn.exe'
if (-not (Test-Path $Gn)) { throw "gn not found at $Gn (cipd ensure gn/gn/windows-amd64 — see BUILD.md 'Windows')" }
$Ninja = (Get-Command ninja -ErrorAction SilentlyContinue).Source
if (-not $Ninja) { throw 'ninja not found on PATH' }
$env:DEPOT_TOOLS_WIN_TOOLCHAIN = '0'
if (-not $env:vs2022_install) {
  $bt = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
  if (Test-Path $bt) { $env:vs2022_install = $bt }
}
# gn's scripts exec `python3`: make sure a real one wins over the Store stub.
$py3 = (Get-Command python3 -ErrorAction SilentlyContinue).Source
if (-not $py3 -or $py3 -like '*WindowsApps*') {
  $cand = Get-ChildItem "$env:LOCALAPPDATA\Programs\Python\Python3*\python3.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($cand) { $env:PATH = "$($cand.DirectoryName);$env:PATH" }
  else { throw 'no real python3.exe found (see BUILD.md "Windows" step 1)' }
}

$GnPath = 'third_party/blink/renderer/miniblink_host'
$SharedTarget = "$GnPath`:miniblink2"
$Dist = Join-Path $Here "dist\$Mode"

# --- locks (one build per out dir; mkdir is atomic) ------------------------------
$script:Locks = @()
function Acquire-Lock([string]$OutDir) {
  $lock = Join-Path $Chromium "$OutDir.build-lib.lock"
  try { [System.IO.Directory]::CreateDirectory((Split-Path $lock)) | Out-Null
        New-Item -ItemType Directory -Path $lock -ErrorAction Stop | Out-Null }
  catch { throw "another build-lib is already building $OutDir (stale? rmdir '$lock')" }
  $script:Locks += $lock
}
function Release-Locks { foreach ($l in $script:Locks) { Remove-Item $l -Force -ErrorAction SilentlyContinue } }

try {

# --- 1. stage sources into the donor tree ---------------------------------------
if (-not $NoStage) {
  $dest    = Join-Path $Chromium 'third_party\blink\renderer\miniblink_host'
  $mb2dest = Join-Path $Chromium 'third_party\blink\renderer\miniblink2'
  $curldst = Join-Path $Chromium 'third_party\blink\renderer\miniblink2_curl'
  Write-Host "==> staging host + miniblink2 API sources -> $dest"
  # robocopy /MIR = rsync -a --delete (mtimes preserved -> ninja stays incremental).
  # Exit codes 0-7 are success.
  & robocopy (Join-Path $Here 'src\miniblink_host') $dest /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
  if ($LASTEXITCODE -ge 8) { throw "robocopy miniblink_host failed ($LASTEXITCODE)" }
  & robocopy (Join-Path $Here 'src\miniblink2') $mb2dest /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
  if ($LASTEXITCODE -ge 8) { throw "robocopy miniblink2 failed ($LASTEXITCODE)" }
  # Vendored curl: shared headers + the WINDOWS lib dir (Schannel DLL + import lib).
  $curlwin = Join-Path $Here 'third_party\curl\win\lib'
  if (-not (Test-Path (Join-Path $curlwin 'libcurl_imp.lib'))) {
    throw "Windows curl not vendored at $curlwin (build it per BUILD.md 'Windows' step 3)"
  }
  & robocopy (Join-Path $Here 'third_party\curl\include') (Join-Path $curldst 'include') /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
  if ($LASTEXITCODE -ge 8) { throw 'robocopy curl include failed' }
  & robocopy $curlwin (Join-Path $curldst 'lib') /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
  if ($LASTEXITCODE -ge 8) { throw 'robocopy curl lib failed' }

  Write-Host '==> applying blink compatibility patches'
  # 0028 is mac-only (.mm file never compiled on Windows) and does not apply here.
  $skip = @('0028-fonts-mac-keep-color-emoji-in-substitution-path.patch')
  Get-ChildItem (Join-Path $Here 'patches\*.patch') | Sort-Object Name | ForEach-Object {
    if ($skip -contains $_.Name) { Write-Host "  (skipped mac-only: $($_.Name))"; return }
    # cmd /c so git's stderr can't become a PS terminating error; reverse-check
    # failing is the NORMAL "not applied yet" case.
    cmd /c "git -C `"$Chromium`" apply --reverse --check `"$($_.FullName)`" 2>nul" | Out-Null
    if ($LASTEXITCODE -eq 0) { return }   # already applied
    cmd /c "git -C `"$Chromium`" apply `"$($_.FullName)`" 2>nul" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "could not apply $($_.Name) — do NOT build without it" }
    Write-Host "  applied $($_.Name)"
  }
}

# --- feature-flag GN values ------------------------------------------------------
function GnBool([bool]$b) { if ($b) { 'true' } else { 'false' } }
if ($Maglev) { $Turbofan = $true }
if ($Wasm)   { $Turbofan = $true }   # wasm requires TurboFan
$USE_DAWN = GnBool $WebGpu
$VID   = GnBool $Video
$MLV   = GnBool $Ml
$WASMV = GnBool $Wasm
$RTCV  = GnBool $WebRtc
$TFV   = GnBool $Turbofan
$MGV   = GnBool $Maglev
$LITEV = GnBool (-not $Turbofan)
$AOMV  = GnBool $Av1Encode
$TRACEV = GnBool $Tracing

function Write-Args([string]$OutDir, [bool]$ThinLto) {
  if ($Mode -eq 'debug') {
    $isDebug = 'true'; $sym = 2; $bsym = 1
    $profile = '# debug profile: assertions + full symbols'
  } elseif ($Ship) {
    $isDebug = 'false'; $sym = 0; $bsym = 0
    $profile = @'
optimize_for_size = true          # -Oz/-Os instead of -O2/-O3
dcheck_always_on = false          # compile out every DCHECK
exclude_unwind_tables = true
'@
    if ($ThinLto) { $profile += "`nuse_thin_lto = true`nthin_lto_enable_optimizations = true" }
  } else {
    $isDebug = 'false'; $sym = 1; $bsym = 0
    $profile = '# dev release: /O2, DCHECKs compiled IN (Chromium developer default)'
  }
  $dir = Join-Path $Chromium $OutDir
  New-Item -ItemType Directory -Force $dir | Out-Null
  @"
is_debug = $isDebug
is_component_build = false        # single-file: statically link the whole engine
use_siso = false
use_dawn = $USE_DAWN
skia_use_dawn = $USE_DAWN
enable_ffmpeg_video_decoders = $VID
webnn_use_tflite = $MLV
webnn_use_litert = $MLV
build_tflite_with_xnnpack = $MLV
v8_enable_webassembly = $WASMV
v8_enable_lite_mode = $LITEV
v8_enable_turbofan = $TFV
v8_enable_maglev = $MGV
mb_enable_webrtc = $RTCV
enable_libaom = $AOMV
optional_trace_events_enabled = $TRACEV
$profile
symbol_level = $sym
blink_symbol_level = $bsym
clang_use_chrome_plugins = false
use_clang_modules = false
enable_precompiled_headers = false
# Windows: <select> popups surface to the HOST (mbOnSelectPopup) — patch 0033.
use_external_popup_menu = true
"@ | Set-Content -Encoding ascii (Join-Path $dir 'args.gn')
}

# --- merged static lib (llvm-ar MRI over the solink edge's inputs) ---------------
function Merge-Static([string]$OutDir) {
  Write-Host '==> merging link inputs -> miniblink2_static.lib'
  $nj = Join-Path $Chromium "$OutDir\obj\$($GnPath -replace '/','\')\miniblink2.ninja"
  $txt = Get-Content $nj -Raw
  $m = [regex]::Match($txt, '(?m)^build [^:]*: solink (?<in>.*?)(?: \|.*)?$')
  if (-not $m.Success) { throw "no solink edge in $nj" }
  $inputs = $m.Groups['in'].Value -split ' ' | Where-Object { $_ -match '\.(lib|obj|o)$' }
  $rl = [regex]::Match($txt, '(?m)^  rlibs = (?<r>.*)$')
  if ($rl.Success) { $inputs += ($rl.Groups['r'].Value -split ' ' | Where-Object { $_ -match '\.rlib$' }) }
  $inputs += "obj/$GnPath/miniblink_host.lib"          # /WHOLEARCHIVE'd host archive
  $inputs = $inputs | Where-Object { $_ } | Sort-Object -Unique |
            Where-Object { Test-Path (Join-Path $Chromium "$OutDir\$_") }
  Write-Host "  merging $($inputs.Count) inputs"
  $llvmAr = Join-Path $Chromium 'third_party\llvm-build\Release+Asserts\bin\llvm-ar.exe'
  if (-not (Test-Path $llvmAr)) { throw "llvm-ar not found at $llvmAr" }
  $out = Join-Path $Dist 'miniblink2_static.lib'
  Remove-Item $out -Force -ErrorAction SilentlyContinue
  $mri = New-TemporaryFile
  $lines = @("create $($out -replace '\\','/')")
  foreach ($i in $inputs) {
    if ($i -match '\.(lib|a|rlib)$') { $lines += "addlib $i" } else { $lines += "addmod $i" }
  }
  $lines += 'save'; $lines += 'end'
  $lines | Set-Content -Encoding ascii $mri
  Push-Location (Join-Path $Chromium $OutDir)
  try { Get-Content $mri | & $llvmAr -M; if ($LASTEXITCODE) { throw "llvm-ar merge failed" } }
  finally { Pop-Location; Remove-Item $mri -Force }
}

# --- one provision+build pass -----------------------------------------------------
function Build-Pass([string]$OutDir, [bool]$ThinLto, [bool]$WantDll, [bool]$WantLib, [bool]$WantSnapshot) {
  Acquire-Lock $OutDir
  Write-Args $OutDir $ThinLto
  Write-Host "==> gn gen $OutDir"
  Push-Location $Chromium
  try { & $Gn gen $OutDir | Out-Null; if ($LASTEXITCODE) { throw "gn gen $OutDir failed" } }
  finally { Pop-Location }
  if ($PrintOnly) {
    Write-Host "==> dry-run: ninja -n $SharedTarget ($OutDir)"
    & $Ninja -C (Join-Path $Chromium $OutDir) -n $SharedTarget | Out-Null
    if ($LASTEXITCODE) { throw 'ninja dry-run failed' }
    Write-Host '  graph OK'
    return
  }
  $targets = @($SharedTarget)
  if ($WantSnapshot) { $targets += 'tools/v8_context_snapshot' }
  $jobArgs = @(); if ($env:MB_JOBS) { $jobArgs = @('-j', $env:MB_JOBS) }
  Write-Host "==> ninja -C $OutDir $($targets -join ' ')   (first build of a profile is a full compile — hours)"
  & $Ninja -C (Join-Path $Chromium $OutDir) @jobArgs @targets
  if ($LASTEXITCODE) { throw "ninja failed in $OutDir" }
  if ($WantDll) {
    foreach ($f in 'miniblink2.dll','miniblink2.dll.lib') {
      Copy-Item (Join-Path $Chromium "$OutDir\$f") $Dist -Force
    }
    $pdb = Join-Path $Chromium "$OutDir\miniblink2.dll.pdb"
    if (($Mode -eq 'debug') -and (Test-Path $pdb)) { Copy-Item $pdb $Dist -Force }
  }
  if ($WantLib) { Merge-Static $OutDir }
}

# --- run the profile's pass(es) ----------------------------------------------------
New-Item -ItemType Directory -Force (Join-Path $Dist 'include\miniblink2') | Out-Null
Copy-Item (Join-Path $Here 'src\miniblink2\*.h') (Join-Path $Dist 'include\miniblink2') -Force

if ($Mode -eq 'debug') {
  Build-Pass 'out/mono-debug' $false $true $false $true
  $DataOut = 'out/mono-debug'
} elseif ($Ship) {
  Build-Pass 'out/mono-release-dynamic' $true  $true  $false $true
  Build-Pass 'out/mono-release-static'  $false $false $true  $false
  $DataOut = 'out/mono-release-dynamic'
} else {
  Build-Pass 'out/mono-release' $false $true $true $true
  $DataOut = 'out/mono-release'
}
if ($PrintOnly) { exit 0 }

# --- runtime data -------------------------------------------------------------------
# Flag-neutral paks come from a reference component build (REF); V8 snapshots are
# flag-dependent and MUST come from this pass's out dir.
$Ref = if ($env:REF) { $env:REF } elseif (Test-Path (Join-Path $Chromium 'out\Release')) {
  Join-Path $Chromium 'out\Release' } else { Join-Path $Chromium 'out\win' }
foreach ($f in 'blink_resources.pak','icudtl.dat','media_controls_resources_100_percent.pak') {
  $p = Join-Path $Ref $f
  if (-not (Test-Path $p)) {
    $alt = @{ 'blink_resources.pak'='gen\third_party\blink\public\resources\blink_resources.pak'
              'media_controls_resources_100_percent.pak'='gen\third_party\blink\renderer\modules\media_controls\resources\media_controls_resources_100_percent.pak' }[$f]
    if ($alt) { $p = Join-Path $Ref $alt }
  }
  if (Test-Path $p) { Copy-Item $p (Join-Path $Dist $f) -Force }
}
foreach ($f in 'snapshot_blob.bin','v8_context_snapshot.bin') {
  $p = Join-Path $Chromium "$DataOut\$f"
  if (Test-Path $p) { Copy-Item $p $Dist -Force }
}
if (-not $IcuFull) {
  $keep = if ($env:MB_ICU_KEEP) { $env:MB_ICU_KEEP } else { 'en,zh' }
  & python3 (Join-Path $Here 'scripts\trim_icu.py') --keep $keep `
      (Join-Path $Ref 'icudtl.dat') (Join-Path $Dist 'icudtl.dat')
}
# GL runtime: SwiftShader-ANGLE is the Windows default GL — always ship it.
foreach ($f in 'libEGL.dll','libGLESv2.dll','vk_swiftshader.dll','vk_swiftshader_icd.json') {
  $p = Join-Path $Chromium "$DataOut\$f"
  if (Test-Path $p) { Copy-Item $p $Dist -Force }
}
# The networking DLL every consumer needs next to their exe.
Copy-Item (Join-Path $Here 'third_party\curl\win\lib\libcurl.dll') $Dist -Force

Write-Host "==> done. dist tree ($Dist):"
Get-ChildItem $Dist -Recurse -File | ForEach-Object {
  '   {0,9:N0}  {1}' -f $_.Length, $_.FullName.Substring($Dist.Length + 1)
}

} finally { Release-Locks }
