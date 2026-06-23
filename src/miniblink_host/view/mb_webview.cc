// mb_webview.cc — WebView + main LocalFrame + widget orchestration.
//
// Replicates WebViewHelper::Initialize (vendor/reference/frame_test_helpers.cc:778,489)
// with all browser-side handles null. Status: Phase 1 scaffold; exact blink call
// signatures pinned during compile.

#include "miniblink_host/view/mb_webview.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "miniblink_host/frame/mb_frame_client.h"
#include "miniblink_host/frame/mb_view_client.h"
#include "miniblink_host/loader/mb_url_loader.h"
#include "miniblink_host/loader/mb_url_loader.h"
#include "miniblink_host/widget/mb_widget.h"

#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/run_loop.h"
#include "base/unguessable_token.h"
#include "cc/paint/paint_record.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "third_party/blink/public/common/metrics/document_update_reason.h"
#include "third_party/blink/public/mojom/page/prerender_page_param.mojom.h"
#include "services/network/public/mojom/web_sandbox_flags.mojom-shared.h"
#include "third_party/blink/public/common/tokens/tokens.h"
#include "third_party/blink/public/platform/scheduler/web_agent_group_scheduler.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_navigation_params.h"
#include "third_party/blink/public/platform/web_url.h"
#include "third_party/blink/public/web/web_script_source.h"
#include "third_party/blink/public/web/web_settings.h"
#include "third_party/blink/public/web/web_view.h"
#include "third_party/icu/source/common/unicode/uscript.h"
#include "third_party/blink/renderer/core/css/media_value_change.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/exported/web_view_impl.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/renderer/platform/scheduler/public/thread_scheduler.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/rect.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-value.h"

namespace mb {

// static
std::unique_ptr<MbWebView> MbWebView::Create(int width, int height) {
  auto v = std::unique_ptr<MbWebView>(new MbWebView());
  v->view_client_ = std::make_unique<MbViewClient>();
  v->frame_client_ = std::make_unique<MbFrameClient>(v.get());

  v->agent_group_scheduler_ =
      std::make_unique<blink::scheduler::WebAgentGroupScheduler>(
          blink::ThreadScheduler::Current()
              ->ToMainThreadScheduler()
              ->CreateAgentGroupScheduler());

  // 1. WebView::Create — all browser-side handles null (frame_test_helpers.cc:778).
  v->web_view_ = blink::To<blink::WebViewImpl>(blink::WebView::Create(
      v->view_client_.get(),
      /*is_hidden=*/false,
      /*prerender_param=*/nullptr,
      /*fenced_frame_mode=*/std::nullopt,
      // Non-compositing offscreen path (InitializeNonCompositing requires this);
      // pixels come from the paint record, not a compositor.
      /*compositing_enabled=*/false,
      /*widgets_never_composited=*/true,
      /*opener=*/nullptr, mojo::NullAssociatedReceiver(),
      *v->agent_group_scheduler_,
      /*session_storage_namespace_id=*/std::string(),
      /*page_base_background_color=*/std::nullopt,
      base::UnguessableToken::Create(),
      /*color_provider_colors=*/nullptr,
      /*history_index=*/-1,
      /*history_length=*/0));

  // Default fonts: generic_font_family_settings_ is empty by default, so without
  // this the FontCache resolves no font and no glyphs paint. Use stock macOS fonts.
  blink::WebSettings* settings = v->web_view_->GetSettings();
  settings->SetJavaScriptEnabled(true);          // off by default
  settings->SetLoadsImagesAutomatically(true);   // off by default — else no <img> fetch
  // Let file:// docs load file:// subresources (off by default).
  settings->SetAllowFileAccessFromFileURLs(true);
  settings->SetAllowUniversalAccessFromFileURLs(true);
  settings->SetStandardFontFamily(blink::WebString::FromUtf8("Times"),
                                  USCRIPT_COMMON);
  settings->SetSerifFontFamily(blink::WebString::FromUtf8("Times"),
                               USCRIPT_COMMON);
  settings->SetSansSerifFontFamily(blink::WebString::FromUtf8("Helvetica"),
                                   USCRIPT_COMMON);
  settings->SetFixedFontFamily(blink::WebString::FromUtf8("Courier"),
                               USCRIPT_COMMON);
  settings->SetDefaultFontSize(16);
  settings->SetDefaultFixedFontSize(13);

  // 2. main frame — broker = NullRemote, policy_container = nullptr
  //    (frame_test_helpers.cc:489).
  v->main_frame_ = blink::WebLocalFrame::CreateMainFrame(
      v->web_view_, v->frame_client_.get(), /*previous_sibling=*/nullptr,
      mojo::NullRemote(), blink::LocalFrameToken(), blink::DocumentToken(),
      /*policy_container=*/nullptr, /*opener=*/nullptr,
      /*name=*/blink::WebString(), network::mojom::WebSandboxFlags::kNone);

  // 3. Frame widget (non-compositing), attach, size, then inform the WebView.
  v->widget_ = std::make_unique<MbWidget>();
  v->widget_->Attach(v->main_frame_, width, height);
  v->web_view_->DidAttachLocalMainFrame();

  return v;
}

MbWebView::MbWebView() = default;

MbWebView::~MbWebView() {
  // if (web_view_) web_view_->Close();  // closes frame + widget
}

void MbWebView::Resize(int width, int height) {
  if (widget_)
    widget_->Resize(width, height);
}

void MbWebView::CommitHtml(const char* data, size_t len, const char* base_url) {
  if (!main_frame_)
    return;
  // INSIDE_BLINK: WebURL is built from a KURL (the GURL ctor is non-INSIDE_BLINK only).
  blink::WebURL url{
      blink::KURL((base_url && *base_url) ? base_url : "about:blank")};
  auto params = blink::WebNavigationParams::CreateWithHTMLStringForTesting(
      base::span<const char>(data, len), url);
  auto* impl = blink::To<blink::WebLocalFrameImpl>(main_frame_);
  impl->CommitNavigation(std::move(params), /*extra_data=*/nullptr);
  // Drive parsing + parse-time subresource loads (render-blocking <link> CSS, scripts)
  // to completion. Several rounds: the parser blocks on the load, our loader delivers,
  // the parser resumes — repeat until quiescent.
  for (int i = 0; i < 20; ++i)
    base::RunLoop().RunUntilIdle();
}

void MbWebView::LoadHTML(const char* utf8_html, const char* base_url) {
  const char* html = utf8_html ? utf8_html : "";
  CommitHtml(html, std::strlen(html), base_url);
}

void MbWebView::LoadURL(const char* utf8_url) {
  std::string url(utf8_url ? utf8_url : "");
  constexpr char kFile[] = "file://";
  if (url.rfind(kFile, 0) == 0) {
    // Top-level file load: read it and commit. (Self-contained docs + data: URIs
    // need no URLLoader; external subresources await the libcurl factory in P2-net.)
    std::string contents;
    if (base::ReadFileToString(base::FilePath(url.substr(sizeof(kFile) - 1)),
                               &contents)) {
      CommitHtml(contents.data(), contents.size(), url.c_str());
    }
    return;
  }
  if (url.rfind("http", 0) == 0) {
    // Top-level http(s): fetch via libcurl, commit with base = the URL so relative
    // subresources resolve and load through MbURLLoader.
    std::string body, content_type;
    const bool ok = MbFetchUrl(url, &body, &content_type);
    if (std::getenv("MB_VERBOSE")) {
      std::fprintf(stderr, "[mb_webview] main-doc %s ok=%d bytes=%zu ct='%s'\n",
                   url.c_str(), ok, body.size(), content_type.c_str());
    }
    if (ok)
      CommitHtml(body.data(), body.size(), url.c_str());
  }
}

void MbWebView::SendMouseClick(int x, int y) {
  if (widget_)
    widget_->SendMouseClick(x, y);
}

void MbWebView::SendMouseMove(int x, int y) {
  if (widget_)
    widget_->SendMouseMove(x, y);
}

void MbWebView::SetDeviceScaleFactor(float scale) {
  dsf_ = scale > 0.0f ? scale : 1.0f;
  if (!web_view_ || !web_view_->GetPage())
    return;
  // DevicePixelRatio = InspectorDeviceScaleFactorOverride * LayoutZoomFactor, so
  // this makes window.devicePixelRatio report `scale` without zooming layout. The
  // compositor-based SetZoomFactorForDeviceScaleFactor path DCHECKs does_composite_,
  // which is false for our non-compositing widget — this override route avoids it.
  web_view_->GetPage()->SetInspectorDeviceScaleFactorOverride(dsf_);
  // The setter only stores the value; nudge media queries so min-resolution /
  // -webkit-device-pixel-ratio (and srcset selection) re-evaluate on next lifecycle.
  if (main_frame_) {
    auto* impl = blink::To<blink::WebLocalFrameImpl>(main_frame_);
    if (blink::LocalFrame* frame = impl->GetFrame()) {
      if (frame->GetDocument()) {
        frame->GetDocument()->MediaQueryAffectingValueChanged(
            blink::MediaValueChange::kOther);
      }
    }
  }
}

void MbWebView::SendText(const char* utf8) {
  if (widget_)
    widget_->SendText(utf8);
}

void MbWebView::SendScroll(int x, int y, int dx, int dy) {
  // Modern Blink routes gesture/wheel scrolls through the compositor input
  // pipeline; the main-thread WebFrameWidgetImpl::HandleGestureEvent CHECKs that
  // scroll events never reach it. A non-compositing offscreen widget has no such
  // pipeline, so we scroll the layout viewport programmatically on the main
  // thread. This moves the viewport, updates window.scrollY, and fires the
  // 'scroll' event — what a headless capture/automation host needs. (x,y) is
  // accepted for API symmetry with input events but unused: the document
  // viewport is the scroll target. Positive dy scrolls the page downward.
  (void)x;
  (void)y;
  if (!main_frame_)
    return;
  auto* impl = blink::To<blink::WebLocalFrameImpl>(main_frame_);
  blink::LocalFrame* frame = impl->GetFrame();
  if (!frame || !frame->DomWindow())
    return;
  frame->DomWindow()->scrollByForTesting(static_cast<double>(dx),
                                         static_cast<double>(dy));
}

void MbWebView::RunJS(const char* utf8_script) {
  if (!main_frame_ || !utf8_script)
    return;
  main_frame_->ExecuteScript(
      blink::WebScriptSource(blink::WebString::FromUtf8(utf8_script)));
  base::RunLoop().RunUntilIdle();
}

std::string MbWebView::EvalToString(const char* utf8_script) {
  if (!main_frame_ || !utf8_script)
    return {};
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  if (!isolate)
    return {};
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = main_frame_->MainWorldScriptContext();
  if (context.IsEmpty())
    return {};
  v8::Context::Scope context_scope(context);
  v8::Local<v8::Value> value = main_frame_->ExecuteScriptAndReturnValue(
      blink::WebScriptSource(blink::WebString::FromUtf8(utf8_script)));
  if (value.IsEmpty())
    return {};
  v8::Local<v8::String> str;
  if (!value->ToString(context).ToLocal(&str))
    return {};
  v8::String::Utf8Value utf8(isolate, str);
  return *utf8 ? std::string(*utf8, utf8.length()) : std::string();
}

bool MbWebView::PaintInto(SkCanvas& canvas) {
  if (!widget_ || !widget_->widget() || !main_frame_)
    return false;
  // Interleave lifecycle + task draining: layout issues subresource requests (images
  // load lazily during layout), the loads complete async, then a later lifecycle applies
  // them. A few rounds settle CSS + images before the final paint.
  for (int round = 0; round < 5; ++round) {
    widget_->widget()->UpdateAllLifecyclePhases(blink::DocumentUpdateReason::kTest);
    base::RunLoop().RunUntilIdle();
  }
  widget_->widget()->UpdateAllLifecyclePhases(blink::DocumentUpdateReason::kTest);

  auto* impl = blink::To<blink::WebLocalFrameImpl>(main_frame_);
  blink::LocalFrame* frame = impl->GetFrame();
  if (!frame || !frame->View())
    return false;

  // Replay Blink's recorded paint ops straight into the canvas (no compositor).
  canvas.clear(SK_ColorWHITE);
  // HiDPI: the paint record is in CSS px; scaling the canvas makes skia re-raster
  // glyphs/vectors crisply at the device pixel ratio into the (logical*dsf) bitmap.
  if (dsf_ != 1.0f)
    canvas.scale(dsf_, dsf_);
  frame->View()->GetPaintRecord().Playback(&canvas);
  return true;
}

bool MbWebView::PaintToBitmap(void* out_bgra, int w, int h, int stride) {
  if (!out_bgra)
    return false;
  SkBitmap bitmap;
  if (!bitmap.installPixels(
          SkImageInfo::Make(w, h, kBGRA_8888_SkColorType, kPremul_SkAlphaType),
          out_bgra, stride)) {
    return false;
  }
  SkCanvas canvas(bitmap);
  return PaintInto(canvas);
}

bool MbWebView::SavePng(const char* path, int w, int h) {
  SkBitmap bitmap;
  if (!bitmap.tryAllocPixels(
          SkImageInfo::Make(w, h, kBGRA_8888_SkColorType, kPremul_SkAlphaType))) {
    return false;
  }
  SkCanvas canvas(bitmap);
  if (!PaintInto(canvas))
    return false;
  std::optional<std::vector<uint8_t>> png =
      gfx::PNGCodec::EncodeBGRASkBitmap(bitmap, /*discard_transparency=*/false);
  return png && base::WriteFile(base::FilePath(path), *png);
}

}  // namespace mb
