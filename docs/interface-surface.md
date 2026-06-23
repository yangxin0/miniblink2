# Minimal embedding surface for modern Blink (M150) — static render

**Derived from** the donor tree's own no-`//content` harness:
`third_party/blink/renderer/core/frame/frame_test_helpers.cc` (+ `core/testing/sim/`).
All citations are `file:line` in `/Users/yangxin/dennis/chrome/chromium-150.0.7871.24`.

## Headline result
To create a `WebView` + main `LocalFrame` and **render a page to pixels with no browser
process**, the browser-side Mojo surface is almost entirely **null / no-op**. The work
is renderer-side client code we write, not 364 Mojo hosts.

## The two embedding calls (what the host must replicate)

### 1. `WebView::Create(...)` — `frame_test_helpers.cc:778`
16 args, minimal values:
| arg | minimal value | host needs |
|---|---|---|
| `WebViewClient*` | a real (minimal) client | **[implement]** small WebViewClient |
| is_hidden | false | — |
| prerender_param | null | — |
| fenced_frame_mode | nullopt | — |
| compositing_enabled | true | — |
| widgets_never_composited | false | — |
| opener | nullptr | — |
| **PageBroadcast receiver** | **`mojo::NullAssociatedReceiver()`** | **[skip] — no browser host** |
| `WebAgentGroupScheduler&` | from Blink scheduler | **[wire]** see below |
| session_storage_namespace_id | `std::string()` (empty) | — |
| page_base_background_color | nullopt | — |
| browsing_context_group_token | `UnguessableToken::Create()` | **[trivial]** |
| color_provider_colors | nullptr (then `UpdateColorProvidersForTest()`) | **[wire]** |
| history_index / length | -1 / 0 | — |

### 2. `WebLocalFrame::CreateMainFrame(...)` — `frame_test_helpers.cc:489`
- `web_frame_client` = our `WebLocalFrameClient` — **[implement]** (renderer-side, substantial but it's our code, not Mojo).
- **BrowserInterfaceBroker remote = `mojo::NullRemote()`** → **[skip]**. Static render tolerates a null broker.
- `policy_container = nullptr` → default empty → **[skip]**.
- tokens: `LocalFrameToken()`, `DocumentToken()` → **[trivial]** create fresh.
- then `web_view_->DidAttachLocalMainFrame()` (`:506`).

## Components to implement (the actual P1 work), by cost

| Component | Model in donor tree | Status |
|---|---|---|
| **`blink::Platform`** | `TestingPlatformSupport : public Platform` (`platform/testing/testing_platform_support.h:58`) | **[implement]** the substrate: threads, fonts(skia), clipboard, etc. Biggest single piece. |
| **`WebViewClient`** | `frame_test_helpers` test client | **[implement]** small |
| **`WebLocalFrameClient`** | `TestWebFrameClient` | **[implement]** medium |
| **`WebFrameWidget` + compositing** | `TestWebFrameWidget` → `AllocateNewLayerTreeFrameSink` (`:1112`); `SimCompositor::BeginFrame` (`sim_compositor.cc:50,61`) does `LayerTreeHost::CompositeForTest(...)` then **`frame_view->GetPaintRecord().Playback(&canvas)`** | **[implement]** DEEPEST RISK NOW RESOLVED: real pixels come from replaying Blink's `cc::PaintRecord` into an `SkBitmap`-backed `SkCanvas` — **no viz `SoftwareRenderer`**. The cc `LayerTreeFrameSink` is just a STUB to satisfy `CompositeForTest`, not the pixel source. |
| **`WebAgentGroupScheduler`** | `std::make_unique<blink::scheduler::WebAgentGroupScheduler>(...)` from `MainThreadScheduler` (`:426`) | **[wire]** Blink provides it; just construct. |
| **URL loading → libcurl** | PRODUCTION path = `LocalFrameClientImpl::GetURLLoaderFactory()` → `network::SharedURLLoaderFactory` (Mojo). `class URLLoader` = `renderer/platform/loader/fetch/url_loader/url_loader.h` (non-public). `CreateURLLoaderForTesting()` is a TEST-ONLY hook (`URLLoaderMockFactory`). | **[implement]** a MINIMAL libcurl-backed `network::mojom::URLLoaderFactory` (factory + per-request job streaming to `URLLoaderClient`) handed to the frame via our client. CORRECTION: earlier "no mojom factory" was wrong — production needs it. Still bounded: ONE Mojo interface pair, no network service. mbLoadHTML needs no network at all. |

## Browser-side Mojo that is **bound but no-op** (widget channels)
Only the widget/compositor channels get bound at all, to trivial test impls:
`mojom::blink::WidgetHost`, `mojom::blink::FrameWidgetHost`,
`mojom::blink::WidgetInputHandlerHost`, plus receives
`viz::mojom::blink::CompositorFrameSink`,
`cc::mojom::blink::RenderFrameMetadataObserverClient`
(`frame_test_helpers.cc:217-264, 319-323, 465-469`). → **[stub]** no-op for static render;
real impls land in P3 (input) / P4 (GPU compositing).

## NOT needed for static render (defer)
`LocalFrameHost`, `LocalMainFrameHost`, `PageBroadcast`, `BrowserInterfaceBroker`,
storage/cookies hosts. These appear only when we add navigation commit, JS calling
browser services, etc. → grow on demand in P2/P3.
NOTE: `network::mojom::URLLoaderFactory` IS needed as soon as we fetch a URL (mbLoadURL) —
implemented minimally over libcurl (see loading row). Only the in-memory mbLoadHTML path
needs zero networking.

## Consequence for Phase 1 scope
The minimal host = **Platform + WebViewClient + WebLocalFrameClient + software
WebFrameWidget/compositor + libcurl URLLoader hook**, calling the two embedding functions
above with mostly-null browser handles. That is a bounded, enumerable target — not "the
browser process." This is the empirical justification for Path A being feasible.
