# Releases

Release notes for miniblink2. Each release is an annotated git tag
(`v*`). `PROGRESS.md` has the day-to-day development journal.

---

## v0.1 — 2026-07-02 (`v0.1`)

**First baseline release.** A standalone, single-process embedder of modern
Blink (Chromium M150 / V8 15): a hand-written tiny content layer boots the
real engine in-process — no CEF, no separate browser process, no cross-process
Mojo IPC — behind a small C ABI.

- **Self-contained SDK** in `dist/<release|debug>/`: the whole engine as one
  `libminiblink2.dylib` or one complete `libminiblink2.a`, the two public C
  headers (`wke.h` classic miniblink API + `mb_capi.h` native ABI), and the
  runtime data (resource paks, `icudtl.dat`, V8 context snapshot, ANGLE).
- **GPU-accelerated WebGL on Metal** via ANGLE: `map.baidu.com` and MapLibre
  GL render their full vector maps on the GPU; SwiftShader ships as the
  software fallback. Dawn/WebGPU is compiled out unless `--webgpu`.
- **Network aligned with miniblink49**: curl_multi reactor; real sites load
  and render, including youtube.com.
- **Video playback**: the real `WebMediaPlayerImpl` pipeline plays YouTube
  video (VP9/MSE); audio output is the next step.
- **Sample apps** built against the SDK: `minibrowser_dyn` (89 KB app linking
  the dylib), `minibrowser_static` (fully self-contained), `mb_shot`.
- **Size-optimized builds** (`--size-optimized`, ThinLTO): 186 → 97 MB,
  roughly at parity with miniblink49 as shipped.
