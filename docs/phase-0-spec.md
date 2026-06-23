# Phase 0 — Build bring-up + "Blink renders" spike

**Status:** in progress (2026-06-23)
**Purpose:** convert the two biggest unknowns into measurements *before* designing the
host: (a) does modern Blink build+link+render on this Mac, and (b) what is the real
browser-side interface surface a single frame actually exercises.

## Why this phase exists
The later phases (host implementation) cannot be specced honestly until we know the
concrete list of Mojo/Platform interfaces a minimal frame needs. Blink already ships
that list implicitly as the mocks in `core/frame/frame_test_helpers.{h,cc}` and
`core/testing/sim/`. Phase 0 builds that harness, proves it renders, and reads the
surface off it.

## Deliverables
1. **A built `blink_unittests`** (carries SimTest) from the donor `out/Release`.
2. **A render proof:** at least one SimTest case (e.g. a `*Sim*` paint/layout test)
   passing, demonstrating real `WebViewImpl` + `LocalFrame` layout+paint with no
   `//content` and no browser process. Bonus: dump a bitmap of a trivial page.
3. **`docs/interface-surface.md`** — enumerated browser-side surface, grouped:
   - Mojo interfaces mocked/bound by `frame_test_helpers` + `sim_*` (LocalFrameHost,
     PageBroadcast, FrameWidgetHost/Widget, BrowserInterfaceBroker, URLLoaderFactory…)
   - `blink::Platform` methods that must be non-trivially implemented.
   - The `WebView::Create` / `WebLocalFrame::CreateMainFrame` / `BindToFrame` call args
     and where each handle originates.
4. **`docs/substrate-libs.md`** — the GN component libs `blink_unittests` links
   (the irreducible substrate the standalone project must link against).

## Method
- Build: `PATH=buildtools/mac:$PATH ninja -C out/Release blink_unittests`.
- Smoke: `out/Release/blink_unittests --gtest_filter=*Sim*` (and a layout/paint subset).
- Surface enumeration: read `frame_test_helpers.h/.cc`, `sim_test.{h,cc}`,
  `sim_network.*`, `sim_compositor.*`, `sim_page.*`; list every `mojom::*Host` bound and
  every `Bind*`/`GetBrowserInterfaceBroker` hook. Cross-check vs `render_frame_impl.cc`
  to see which the *real* `//content` provides (those are the ones we must reimplement).
- Substrate: `gn desc out/Release //third_party/blink/renderer/controller:blink_unittests deps`
  (or read the linked dylibs) to capture the link set.

## Exit criteria
- `blink_unittests` builds and a `*Sim*` case passes → substrate + render path proven.
- `interface-surface.md` lists the minimal browser-side surface with each interface
  marked **[implement] / [stub] / [skip]** for the first milestone (render a static page).
- Then: write Phase 1 spec (minimal host) against that concrete list.

## Risks being measured here
- Build may fail on macOS SDK 26 / system-Xcode quirks → triage args.gn.
- Substrate link set may be larger than `frame_test_helpers` implies (stickier deps).
- Software compositor path (SimCompositor) may not produce real pixels without GPU →
  determines how hard P4 (real compositing) is pulled forward.
