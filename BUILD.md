# Building miniblink2

miniblink2 builds against a **Chromium M150 component checkout** that lives as a
sibling of this repo, together with depot_tools:

```
<parent>/
  miniblink2/                  this repo
  chromium-150.0.7871.24/      Chromium checkout (from tarball or gclient)
  depot_tools/                 optional — see "depot_tools" below
```

No absolute path is hardcoded; override the defaults with `CHROMIUM=…` /
`DEPOT=…` env vars or the `--chromium DIR` / `--depot DIR` flags.

## TL;DR

```sh
# dev release SDK (-O2, DCHECKs on) -> dist/release/libminiblink2.{dylib,a}
scripts/build-lib.sh --release

# publishable SDK (-Oz, DCHECKs out, stripped; ThinLTO dylib + native .a)
scripts/build-lib.sh --release --ship

# debugger build (no optimization, full symbols) -> dist/debug/libminiblink2.dylib
scripts/build-lib.sh --debug
```

Each profile uses its own out dir under the Chromium tree (`out/mono-release`,
`out/mono-release-{dynamic,static}` for ship, `out/mono-debug`). The first build
of a profile is a full from-scratch compile (~30k ninja steps — hours); re-runs
are incremental.

Each `dist/<mode>/` is a self-contained SDK:

- `libminiblink2.{dylib,a}` — the library (exposes the `mb*` C API)
- `include/miniblink2/{webview,automation}.h` — public headers
- `blink_resources.pak`, `icudtl.dat`, `snapshot_blob.bin`, `v8_context_snapshot.bin`
  — runtime data the engine loads at startup

## Feature flags

Features are include-only and default **OFF** (to trim toward miniblink49's
footprint); add what you need:

| Flag | Adds |
|---|---|
| `--webgpu` | WebGPU/Dawn (~97 MB) |
| `--video` | `<video>` decode (audio is always on) |
| `--ml` | WebNN on-device ML (TFLite/LiteRT/XNNPACK) |
| `--wasm` | WebAssembly (V8 wasm engine) |
| `--av1-encode` | AV1 encoding via libaom (decode stays regardless) |
| `--tracing` | `OPTIONAL_TRACE_EVENT` instrumentation |
| `--swiftshader` | ship SwiftShader software Vulkan in dist (headless/CI fallback) |
| `--icu-full` | untrimmed `icudtl.dat` (all ~90 locales, 10.4 MB; default trims to root+en+zh, 6.3 MB — customize with `MB_ICU_KEEP=en,zh,ja`) |

Other useful flags: `--no-stage` (skip re-staging sources into the tree),
`--print-only` (gn gen + ninja dry-run, no compile).

## One-time machine setup (macOS arm64)

A fresh machine / fresh Chromium tarball is missing several toolchain pieces
that a `gclient sync` checkout would normally provide. All of these were needed
in practice; each is one-time.

### 1. depot_tools (gn + ninja)

If there is no sibling `depot_tools/`, the Chromium tree bundles one — point
`DEPOT` at it:

```sh
DEPOT="$CHROMIUM/third_party/depot_tools" scripts/build-lib.sh --release
```

The bundled depot_tools ships wrapper *scripts* for gn/ninja, not binaries. If
`gn --version` fails, install the DEPS-pinned gn via cipd (find the revision in
the tree's `DEPS` under `gn_version`):

```sh
cd "$CHROMIUM"
third_party/depot_tools/cipd ensure -root buildtools/mac -ensure-file /dev/stdin \
  <<< "gn/gn/mac-arm64 git_revision:<gn_version from DEPS>"
```

### 2. Pinned clang

```sh
cd "$CHROMIUM" && python3 tools/clang/scripts/update.py
python3 tools/clang/scripts/update.py --package=objdump   # llvm-otool + llvm-nm
```

(gn gen fails with `clang_revision="" but update.py expected …` until the first
runs; without the second, every dylib link fails in `linker_driver.py` because
the TOC step can't find `llvm-otool`/`llvm-nm`.)

### 3. Linux binaries in the tarball — check architectures

The 150 tarball ships several **Linux x86-64** tool binaries that fail
mid-build with `Exec format error`. Known offenders and their mac-arm64 fixes
(versions come from the pins in `DEPS` / devtools-frontend `DEPS`):

```sh
cd "$CHROMIUM"

# rust toolchain (rustc)
python3 tools/rust/update_rust.py

# esbuild (devtools-frontend grd step)
third_party/depot_tools/cipd ensure \
  -root third_party/devtools-frontend/src/third_party/esbuild \
  -ensure-file /dev/stdin \
  <<< "infra/3pp/tools/esbuild/mac-arm64 version:3@0.25.1.chromium.2"

# third_party/ninja
third_party/depot_tools/cipd ensure -root third_party/ninja \
  -ensure-file /dev/stdin \
  <<< "infra/3pp/tools/ninja/mac-arm64 version:3@1.12.1.chromium.4"

# rollup's native binding (devtools-frontend node_modules only has the
# linux-x64 one; version = node_modules/rollup/package.json)
cd third_party/devtools-frontend/src/node_modules/@rollup
curl -sL "https://registry.npmjs.org/@rollup/rollup-darwin-arm64/-/rollup-darwin-arm64-4.60.4.tgz" -o r.tgz
mkdir -p rollup-darwin-arm64 && tar xzf r.tgz -C rollup-darwin-arm64 --strip-components=1 && rm r.tgz
```

If a later build step dies with `Exec format error: <tool>`, run
`file <tool>` — an `ELF … x86-64` result means another one; find its cipd/gcs
pin in the nearest `DEPS` and fetch the `mac-arm64` variant the same way.

### 4. Xcode Metal toolchain

gn gen probes `metal`; on a fresh Xcode it fails with
`cannot execute tool 'metal' due to missing Metal Toolchain`:

```sh
xcodebuild -runFirstLaunch                      # repairs Xcode components first if needed
xcodebuild -downloadComponent MetalToolchain    # ~700 MB
```

### 5. Node (pinned)

Ninja needs `third_party/node/mac_arm64/node-darwin-arm64/bin/node`. Find the
`src/third_party/node/mac_arm64` entry in `DEPS` (gcs bucket `chromium-nodejs`)
and fetch it:

```sh
cd "$CHROMIUM/third_party/node"
curl -sL "https://storage.googleapis.com/chromium-nodejs/<object_name>" -o node.tar.gz
shasum -a 256 node.tar.gz          # verify against the sha256sum in DEPS
mkdir -p mac_arm64 && tar xzf node.tar.gz -C mac_arm64 && rm node.tar.gz
```

### 6. Wire the target into the root build graph

One-time edit to `$CHROMIUM/BUILD.gn` — add to `group("gn_all")` deps:

```gn
deps = [
  "//third_party/blink/renderer/miniblink_host",
  ...
]
```

Without this, ninja reports `unknown target
'third_party/blink/renderer/miniblink_host:miniblink2'`.

## Gotchas

- **Cap build parallelism** with `MB_JOBS=N scripts/build-lib.sh …` (passes
  `ninja -j N`; unset = ninja's cores+2 default).
- **Missing paks in dist**: `blink_resources.pak` / `icudtl.dat` /
  `media_controls_resources_100_percent.pak` are copied from a reference
  component build (`REF`, default `$CHROMIUM/out/Release`); if none exists the
  copy is silently skipped. Either run `./build.sh` once to create it, or take
  the paks from the mono out dir
  (`out/mono-release/gen/third_party/blink/public/resources/blink_resources.pak`,
  `…/media_controls/resources/media_controls_resources_100_percent.pak`) and
  trim ICU from the source tree:
  `python3 scripts/trim_icu.py --keep en,zh $CHROMIUM/third_party/icu/common/icudtl.dat dist/release/icudtl.dat`.

- **"another build-lib.sh is already building"** with no build running usually
  means `$CHROMIUM/out/` doesn't exist yet (the lock `mkdir` fails on the
  missing parent). `mkdir "$CHROMIUM/out"` — or remove the stale lock dir the
  message names if one really exists.
- Patches under `patches/` are applied to the Chromium tree at stage time and
  are **fatal if they don't apply** — on a Chromium uprev, re-derive the failing
  patch; never build without it (several guard runtime deadlocks/crashes).
- The build uses plain `ninja`, not autoninja/siso, and one build per out dir
  at a time (two ninjas corrupt `.ninja_deps`).
- V8 snapshots are build-flag-dependent; dist data always comes from the same
  out dir as the library, so don't mix artifacts across profiles.

## Windows (x64)

The engine, all smoke suites (mb_smoke 207, platform 46, render 141, mb_shot 66),
the GPU/compositor probes and real-site HTTPS mb_shot runs work on Windows
(first ported 2026-07-08 on Windows 11 + VS 2022 Build Tools). One-time setup on
a fresh machine / fresh tarball:

1. **Tools**: VS 2022 Build Tools with MSVC x64 + Windows 11 SDK + **ATL**
   (`Microsoft.VisualStudio.Component.VC.ATL`) + the SDK's **Debugging Tools**
   feature (build needs `dbghelp.dll`; the port used
   `winsdksetup.exe /features OptionId.WindowsDesktopDebuggers`), Git for
   Windows (its bash runs `build`/smoke scripts), CMake, ninja, Python 3.12
   (copy `python.exe` to `python3.exe` in the install dir — gn invokes
   `python3`).
2. **Toolchain pins** (the tarball ships mac/linux binaries only):
   - gn: `cipd ensure -root buildtools\win` with `gn/gn/windows-amd64` at the
     DEPS `gn_version`.
   - clang: `python3 tools\clang\scripts\update.py`; rust:
     `python3 tools\rust\update_rust.py`.
   - node.exe (GCS pin in DEPS `src/third_party/node/win`), esbuild
     (`infra/3pp/tools/esbuild/windows-amd64`, then copy `esbuild.exe` over the
     extensionless `esbuild` the scripts exec), rollup native binding
     (`@rollup/rollup-win32-x64-msvc` at the vendored rollup version into
     devtools-frontend `node_modules/@rollup/`), `rc.exe` (GCS
     `chromium-browser-clang/rc/<sha1 from build/toolchain/win/rc/win/rc.exe.sha1>`).
   - `checkout_win` git DEPS: gperf, microsoft_dxheaders, microsoft_webauthn,
     perl (clone at the DEPS revisions).
3. **Vendored curl**: build curl 8.21 with cmake
   (`-DBUILD_SHARED_LIBS=ON -DCURL_USE_SCHANNEL=ON -DCURL_USE_LIBPSL=OFF`
   `-DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF`), stage `include/` +
   `libcurl_imp.lib` + `libcurl.dll` into `miniblink2_curl/{include,lib}`.
4. **Environment for gn/ninja**: `DEPOT_TOOLS_WIN_TOOLCHAIN=0` and
   `vs2022_install=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`
   (the detector only scans non-x86 `Program Files` for 2022).
5. **args.gn extras vs mac**: `use_external_popup_menu = true` (host `<select>`
   popups via patch 0032; patches 0031/0033 cover WebGPU-on-SwiftShader and the
   Windows font-fallback hook). `mb_enable_webrtc = false` (the dev-default ON
   trips gmock-include errors in blink platform test files on win; OFF is the
   ship default anyway).
6. **Runtime files** next to the binaries: `blink_resources.pak`,
   `media_controls_resources_100_percent.pak`, `icudtl.dat`,
   `snapshot_blob.bin`, `v8_context_snapshot.bin` (ninja target
   `tools/v8_context_snapshot`), `libcurl.dll`.
7. Defender materially slows the build; consider
   `Add-MpPreference -ExclusionPath <checkout>` (elevated PowerShell).

Windows behavior notes: audio output is the silent sink (decode+clock, no
speaker); ANGLE defaults to SwiftShader (`--use-angle=d3d11` opts into
hardware); WebGPU runs on Vulkan/SwiftShader; system-font metrics + text AA are
seeded at init in `mb_runtime.cc` (no browser process to deliver renderer
prefs).

## Other build scripts

- `build.sh /path/to/chromium` — the component (dev) build: stages sources,
  builds `miniblink_host` plus all smoke/probe binaries, and runs the smoke
  tests. Use this for day-to-day development; `scripts/build-lib.sh` for SDKs.
- `scripts/build-samples.sh` — builds `samples/` against a built SDK.
- `scripts/package.sh` — zips a dist SDK for release.
- `scripts/build-curl-macos.sh` — rebuilds the vendored libcurl.
