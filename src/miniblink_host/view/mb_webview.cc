// mb_webview.cc — WebView + main LocalFrame + widget orchestration.
//
// Replicates WebViewHelper::Initialize (vendor/reference/frame_test_helpers.cc:778,489)
// with all browser-side handles null. Status: Phase 1 scaffold; exact blink call
// signatures pinned during compile.

#include "miniblink_host/view/mb_webview.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "miniblink_host/frame/mb_frame_broker.h"
#include "miniblink_host/frame/mb_frame_client.h"
#include "miniblink_host/frame/mb_view_client.h"
#include "miniblink_host/loader/mb_url_loader.h"
#include "miniblink_host/loader/mb_url_loader.h"
#include "miniblink_host/widget/mb_widget.h"

#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "base/unguessable_token.h"
#include "cc/paint/paint_record.h"
#include "cc/paint/skia_paint_canvas.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "third_party/blink/public/common/metrics/document_update_reason.h"
#include "third_party/blink/public/mojom/page/prerender_page_param.mojom.h"
#include "services/network/public/mojom/web_sandbox_flags.mojom-shared.h"
#include "third_party/blink/public/common/tokens/tokens.h"
#include "third_party/blink/public/platform/scheduler/web_agent_group_scheduler.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/mojom/css/preferred_color_scheme.mojom-shared.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_node.h"
#include "third_party/blink/public/web/web_print_page_description.h"
#include "third_party/blink/public/web/web_print_params.h"
#include "third_party/blink/public/web/web_navigation_params.h"
#include "third_party/blink/public/platform/web_url.h"
#include "third_party/blink/public/web/web_script_source.h"
#include "third_party/blink/public/web/web_settings.h"
#include "third_party/blink/public/web/web_view.h"
#include "third_party/icu/source/common/unicode/unistr.h"
#include "third_party/icu/source/common/unicode/uscript.h"
#include "third_party/icu/source/i18n/unicode/timezone.h"
#include "third_party/blink/renderer/core/css/media_value_change.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/exported/web_view_impl.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/frame/settings.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/public/common/dom_storage/session_storage_namespace_id.h"
#include "third_party/blink/renderer/core/page/page.h"
#include "third_party/blink/renderer/core/page/page_animator.h"
#include "third_party/blink/renderer/modules/storage/storage_namespace.h"
#include "third_party/blink/renderer/platform/scheduler/public/main_thread_scheduler.h"
#include "third_party/blink/renderer/platform/scheduler/public/thread_scheduler.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkStream.h"
#include "third_party/skia/include/docs/SkPDFDocument.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "ui/gfx/codec/jpeg_codec.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/size_f.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-value.h"

namespace mb {

namespace {
// Escape a C string so it can be embedded inside a double-quoted JS string
// literal (selectors, fill text). Handles backslash, quote, and newlines.
std::string JsEscape(const char* s) {
  std::string out;
  for (const char* p = s; p && *p; ++p) {
    switch (*p) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      default: out.push_back(*p);
    }
  }
  return out;
}
}  // namespace

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
  settings->SetLocalStorageEnabled(true);  // else window.localStorage is null (TypeError)
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
      MakeFrameInterfaceBroker(), blink::LocalFrameToken(), blink::DocumentToken(),
      /*policy_container=*/nullptr, /*opener=*/nullptr,
      /*name=*/blink::WebString(), network::mojom::WebSandboxFlags::kNone);
  // Let the frame client know its frame so it can parent child frames (iframes).
  v->frame_client_->SetFrame(v->main_frame_);

  // 3. Frame widget (non-compositing), attach, size, then inform the WebView.
  v->widget_ = std::make_unique<MbWidget>();
  v->widget_->Attach(v->main_frame_, width, height);
  v->web_view_->DidAttachLocalMainFrame();

  // 4. Attach a session-storage namespace to the page so window.sessionStorage
  //    resolves (without it StorageNamespace::From(page) is null -> TypeError).
  //    The id is normally a browser-assigned 36-char token; any non-empty one of
  //    that length works for our single in-process page. localStorage needs no
  //    such namespace (it goes through the StorageController directly).
  if (blink::Page* page = v->web_view_->GetPage()) {
    blink::StorageNamespace::ProvideSessionStorageNamespaceTo(
        *page, std::string(blink::kSessionStorageNamespaceIdLength, 'm'));
  }

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

void MbWebView::CommitHtml(const char* data, size_t len, const char* base_url,
                           const std::string& charset) {
  if (!main_frame_)
    return;
  // INSIDE_BLINK: WebURL is built from a KURL (the GURL ctor is non-INSIDE_BLINK only).
  blink::WebURL url{
      blink::KURL((base_url && *base_url) ? base_url : "about:blank")};
  // Build the response ourselves instead of CreateWithHTMLStringForTesting,
  // which hardcodes the encoding to "UTF-8" (authoritative), silently breaking
  // any non-UTF-8 page. An empty encoding is tentative: the HTML parser then
  // honors <meta charset>, a BOM, and UTF-8 auto-detection. A known charset
  // (from the HTTP Content-Type) is passed through as authoritative.
  auto params = std::make_unique<blink::WebNavigationParams>();
  params->url = url;
  blink::WebNavigationParams::FillStaticResponse(
      params.get(), "text/html", blink::WebString::FromUtf8(charset),
      base::span<const char>(data, len));
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
    std::string body, content_type, final_url;
    const bool ok = MbFetchUrl(
        url, &body, &content_type,
        frame_client_ ? frame_client_->user_agent() : std::string(),
        frame_client_ ? frame_client_->extra_headers() : std::string(),
        /*post_body=*/std::string(), /*post_content_type=*/std::string(),
        /*http_method=*/std::string(), &final_url);
    // If the server redirected us, commit with the FINAL URL as the document's
    // base so location.href and relative subresources reflect where we landed.
    const std::string& doc_url = final_url.empty() ? url : final_url;
    if (std::getenv("MB_VERBOSE")) {
      std::fprintf(stderr, "[mb_webview] main-doc %s ok=%d bytes=%zu ct='%s'\n",
                   url.c_str(), ok, body.size(), content_type.c_str());
    }
    if (ok) {
      // Pass the server's charset (authoritative) when present; else empty so the
      // parser detects <meta charset>/BOM.
      std::string charset;
      std::string lc = content_type;
      for (char& c : lc) c = static_cast<char>(std::tolower((unsigned char)c));
      if (auto p = lc.find("charset="); p != std::string::npos) {
        p += 8;
        std::string::size_type end = content_type.find_first_of("; \t", p);
        charset = content_type.substr(p, end == std::string::npos ? end : end - p);
      }
      CommitHtml(body.data(), body.size(), doc_url.c_str(), charset);
    }
  }
}

void MbWebView::SendMouseClick(int x, int y) {
  if (widget_)
    widget_->SendMouseClick(x, y);
}

bool MbWebView::ClickSelector(const char* css_selector) {
  if (!css_selector || !widget_)
    return false;
  // Embed the selector as a JS string literal, ask the page for the element's
  // center, then click there. Returns "" if there is no match or no box.
  std::string js =
      "(function(){var e=document.querySelector(\"" + JsEscape(css_selector) +
      "\");if(!e)return '';var r=e.getBoundingClientRect();"
      "if(r.width<=0&&r.height<=0)return '';"
      "return Math.round(r.left+r.width/2)+','+Math.round(r.top+r.height/2);})()";
  std::string center = EvalToString(js.c_str());
  std::string::size_type comma = center.find(',');
  if (comma == std::string::npos)
    return false;
  int x = std::atoi(center.substr(0, comma).c_str());
  int y = std::atoi(center.substr(comma + 1).c_str());
  SendMouseClick(x, y);
  return true;
}

bool MbWebView::FillSelector(const char* css_selector, const char* text) {
  if (!css_selector || !main_frame_)
    return false;
  // Focus the field and set its value through the prototype's native value
  // setter, so frameworks that wrap the setter (React's value tracker) observe
  // the change; then fire input + change (bubbling) like real typing. Falls back
  // to a direct assignment for non-input/textarea elements. Returns "1" on
  // success, "0" if the selector matches nothing.
  std::string js =
      "(function(){var e=document.querySelector(\"" + JsEscape(css_selector) +
      "\");if(!e)return '0';e.focus();var t=\"" + JsEscape(text ? text : "") +
      "\";var proto=(e instanceof HTMLTextAreaElement)?HTMLTextAreaElement.prototype"
      ":(e instanceof HTMLInputElement)?HTMLInputElement.prototype:null;"
      "var d=proto&&Object.getOwnPropertyDescriptor(proto,'value');"
      "if(d&&d.set){d.set.call(e,t);}else{try{e.value=t;}catch(ex){return '0';}}"
      "e.dispatchEvent(new Event('input',{bubbles:true}));"
      "e.dispatchEvent(new Event('change',{bubbles:true}));return '1';})()";
  return EvalToString(js.c_str()) == "1";
}

void MbWebView::SendMouseMove(int x, int y) {
  if (widget_)
    widget_->SendMouseMove(x, y);
}

void MbWebView::SetUserAgent(const char* utf8_ua) {
  if (frame_client_)
    frame_client_->SetUserAgent(utf8_ua ? utf8_ua : "");
}

void MbWebView::SetLoadImages(bool enabled) {
  // Disabling image auto-loading speeds up text/HTML scraping (no image fetch or
  // decode). Set before navigating to apply to that load.
  if (web_view_ && web_view_->GetSettings())
    web_view_->GetSettings()->SetLoadsImagesAutomatically(enabled);
}

void MbWebView::SetLocale(const char* langs) {
  // Drive navigator.language / navigator.languages (a comma-separated list like
  // "fr-FR,fr,en"), so JS i18n that branches on the user's languages sees it.
  if (!langs || !web_view_ || !web_view_->GetPage())
    return;
  // Set before navigating: the new document's navigator reads this fresh on first
  // access, so no explicit languages-changed notification is needed.
  web_view_->GetPage()->GetSettings().SetAcceptLanguages(
      blink::String::FromUtf8(langs));
}

void MbWebView::SetTimezone(const char* tz) {
  // Override the timezone for Date / Intl so time-dependent UIs render
  // deterministically. Process-global (ICU default + a v8 redetect for the isolate).
  if (!tz || !*tz)
    return;
  icu::TimeZone::adoptDefault(
      icu::TimeZone::createTimeZone(icu::UnicodeString::fromUTF8(tz)));
  if (v8::Isolate* isolate = v8::Isolate::GetCurrent()) {
    // kSkip: invalidate v8's cached zone but keep the ICU default we just set
    // (kRedetect would re-read the host OS zone, clobbering our override).
    isolate->DateTimeConfigurationChangeNotification(
        v8::Isolate::TimeZoneDetection::kSkip);
  }
}

void MbWebView::SetDarkMode(bool dark) {
  // Drive the prefers-color-scheme media feature so a page renders its dark (or
  // light) theme. Set before navigating to apply to that load.
  if (web_view_ && web_view_->GetSettings()) {
    web_view_->GetSettings()->SetPreferredColorScheme(
        dark ? blink::mojom::PreferredColorScheme::kDark
             : blink::mojom::PreferredColorScheme::kLight);
  }
}

std::string MbWebView::DrainConsole() {
  return frame_client_ ? frame_client_->DrainConsole() : std::string();
}

std::string MbWebView::GetCookies(const char* url) {
  return url ? MbGetCookiesForUrl(url) : std::string();
}

void MbWebView::SetExtraHeaders(const char* utf8_headers) {
  if (frame_client_)
    frame_client_->SetExtraHeaders(utf8_headers ? utf8_headers : "");
}

void MbWebView::SetTransparentBackground(bool transparent) {
  transparent_bg_ = transparent;
  if (!web_view_)
    return;
  // The compositor path (SetBaseBackgroundColorOverrideTransparent) DCHECKs
  // does_composite_; the inspector base-color override has no such check and feeds
  // the same BaseBackgroundColor(), so use it. PaintInto also clears to transparent
  // so areas the document doesn't paint keep alpha 0.
  web_view_->SetBaseBackgroundColorOverrideForInspector(
      transparent ? std::optional<SkColor>(SK_ColorTRANSPARENT) : std::nullopt);
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

void MbWebView::RunInFrameTask(base::OnceClosure body, bool settle) {
  // Host-driven JS must execute within a scheduler task. Page scripts always do
  // (bracketed by WillProcessTask/DidProcessTask), and engine subsystems rely on
  // that bracketing: a canvas draw (CanvasRenderingContext::DidDraw) made outside
  // any task scope trips a FATAL NOTREACHED in CanvasPerformanceMonitor. A bare
  // synchronous ExecuteScript would draw outside a task, so host-injected canvas
  // drawing would crash. Posting `body` as a task fixes that; loop.Run() blocks
  // until it has run, so callers still observe DOM effects (and read back results)
  // synchronously.
  base::RunLoop loop(base::RunLoop::Type::kNestableTasksAllowed);
  auto runner = base::SingleThreadTaskRunner::GetCurrentDefault();
  runner->PostTask(FROM_HERE, std::move(body));
  if (settle) {
    // RunJS semantics: let async continuations (timers, microtasks) settle, but
    // never spin forever — quit when idle, or at a hard 250ms cap.
    runner->PostTask(FROM_HERE, loop.QuitWhenIdleClosure());
    runner->PostDelayedTask(FROM_HERE, loop.QuitClosure(),
                            base::Milliseconds(250));
  } else {
    // Eval semantics: run exactly the one task (so the script is task-bracketed),
    // then return — no extra async progress beyond the body's own microtasks.
    runner->PostTask(FROM_HERE, loop.QuitClosure());
  }
  loop.Run();
}

void MbWebView::RunJS(const char* utf8_script) {
  if (!main_frame_ || !utf8_script)
    return;
  blink::WebLocalFrame* frame = main_frame_;
  RunInFrameTask(
      base::BindOnce(
          [](blink::WebLocalFrame* f, std::string s) {
            f->ExecuteScript(
                blink::WebScriptSource(blink::WebString::FromUtf8(s)));
          },
          frame, std::string(utf8_script)),
      /*settle=*/true);
}

void MbWebView::SetInitScript(const char* utf8_script) {
  init_script_ = utf8_script ? utf8_script : "";
}

void MbWebView::RunDocumentStartScript() {
  // Called at document-element-available (before the page's own scripts). Run the
  // host init script so it can set globals / override APIs the page then observes.
  if (init_script_.empty() || !main_frame_)
    return;
  main_frame_->ExecuteScript(
      blink::WebScriptSource(blink::WebString::FromUtf8(init_script_)));
}

std::string MbWebView::EvalToString(const char* utf8_script) {
  if (!main_frame_ || !utf8_script)
    return {};
  // Run inside a task (see RunInFrameTask) so an eval that draws to a canvas does
  // not trip CanvasPerformanceMonitor. loop.Run() blocks until the body has run,
  // so writing the result into this stack-local is safe.
  std::string result;
  blink::WebLocalFrame* frame = main_frame_;
  RunInFrameTask(
      base::BindOnce(
          [](blink::WebLocalFrame* f, std::string s, std::string* out) {
            v8::Isolate* isolate = v8::Isolate::GetCurrent();
            if (!isolate)
              return;
            v8::HandleScope handle_scope(isolate);
            v8::Local<v8::Context> context = f->MainWorldScriptContext();
            if (context.IsEmpty())
              return;
            v8::Context::Scope context_scope(context);
            v8::Local<v8::Value> value = f->ExecuteScriptAndReturnValue(
                blink::WebScriptSource(blink::WebString::FromUtf8(s)));
            if (value.IsEmpty())
              return;
            v8::Local<v8::String> str;
            if (!value->ToString(context).ToLocal(&str))
              return;
            v8::String::Utf8Value utf8(isolate, str);
            if (*utf8)
              out->assign(*utf8, utf8.length());
          },
          frame, std::string(utf8_script), &result),
      /*settle=*/false);
  return result;
}

std::string MbWebView::EvalIsolated(const char* utf8_script) {
  if (!main_frame_ || !utf8_script)
    return {};
  std::string result;
  blink::WebLocalFrame* frame = main_frame_;
  RunInFrameTask(
      base::BindOnce(
          [](blink::WebLocalFrame* f, std::string s, std::string* out) {
            v8::Isolate* isolate = v8::Isolate::GetCurrent();
            if (!isolate)
              return;
            v8::HandleScope handle_scope(isolate);
            // A dedicated isolated world: JS globals separate from the page (and
            // each run), but the SAME DOM — the content-script execution model.
            constexpr int32_t kIsolatedWorldId = 1;  // > main-world (0)
            v8::Local<v8::Value> value =
                f->ExecuteScriptInIsolatedWorldAndReturnValue(
                    kIsolatedWorldId,
                    blink::WebScriptSource(blink::WebString::FromUtf8(s)),
                    blink::BackForwardCacheAware::kAllow);
            if (value.IsEmpty())
              return;
            // Stringify the (primitive) result; the main-world context suffices.
            v8::Local<v8::Context> context = f->MainWorldScriptContext();
            if (context.IsEmpty())
              return;
            v8::Local<v8::String> str;
            if (!value->ToString(context).ToLocal(&str))
              return;
            v8::String::Utf8Value utf8(isolate, str);
            if (*utf8)
              out->assign(*utf8, utf8.length());
          },
          frame, std::string(utf8_script), &result),
      /*settle=*/false);
  return result;
}

void MbWebView::WaitMs(int ms) {
  // Drive the engine for ~ms of real time so delayed timers (setTimeout) and async
  // work (fetch microtasks) run. RunUntilIdle alone won't advance to a not-yet-due
  // delayed task, so interleave short real sleeps with task draining + lifecycle.
  if (!widget_ || !widget_->widget())
    return;
  const base::TimeTicks deadline =
      base::TimeTicks::Now() + base::Milliseconds(ms > 0 ? ms : 0);
  do {
    ServiceAnimations();
    widget_->widget()->UpdateAllLifecyclePhases(blink::DocumentUpdateReason::kTest);
    base::RunLoop().RunUntilIdle();
    // Run idle-scheduled work. Idle tasks only execute inside an idle period,
    // which the compositor normally starts; with no compositor they would only
    // fire via each feature's ~1s fallback timeout (e.g. canvas.toBlob encodes
    // on an idle task, requestIdleCallback). Start an idle period explicitly so
    // they run promptly, then drain.
    if (auto* mts = blink::ThreadScheduler::Current()->ToMainThreadScheduler()) {
      mts->StartIdlePeriodForTesting();
      base::RunLoop().RunUntilIdle();
    }
    if (base::TimeTicks::Now() >= deadline)
      break;
    base::PlatformThread::Sleep(base::Milliseconds(10));
  } while (true);
}

bool MbWebView::WaitForSelector(const char* css, int timeout_ms) {
  if (!main_frame_ || !css)
    return false;
  // Poll document.querySelector(css) while pumping the loop until it matches or the
  // timeout elapses. Lets a capture wait for JS-rendered / delayed content (the way
  // Puppeteer's waitForSelector does) instead of shooting the page prematurely.
  const std::string probe =
      std::string("(document.querySelector(\"") + css + "\")?1:0)";
  const base::TimeTicks deadline =
      base::TimeTicks::Now() + base::Milliseconds(timeout_ms > 0 ? timeout_ms : 0);
  for (;;) {
    base::RunLoop().RunUntilIdle();
    if (EvalToString(probe.c_str()) == "1")
      return true;
    if (base::TimeTicks::Now() >= deadline)
      return false;
    base::PlatformThread::Sleep(base::Milliseconds(10));
  }
}

bool MbWebView::WaitForFunction(const char* js_expr, int timeout_ms) {
  if (!main_frame_ || !js_expr)
    return false;
  // Wrap the caller's expression so any truthy value -> "1" and falsey/throw ->
  // "0", then poll while pumping the loop (same cadence as WaitForSelector).
  const std::string probe = std::string("(function(){try{return ((") + js_expr +
                            ")?1:0);}catch(e){return 0;}})()";
  const base::TimeTicks deadline =
      base::TimeTicks::Now() + base::Milliseconds(timeout_ms > 0 ? timeout_ms : 0);
  for (;;) {
    base::RunLoop().RunUntilIdle();
    if (EvalToString(probe.c_str()) == "1")
      return true;
    if (base::TimeTicks::Now() >= deadline)
      return false;
    base::PlatformThread::Sleep(base::Milliseconds(10));
  }
}

void MbWebView::ServiceAnimations() {
  // Run rAF callbacks. The compositor normally drives this via BeginMainFrame; with
  // no compositor we call the page animator directly so requestAnimationFrame fires
  // (animation libraries, framework schedulers, lazy renderers all depend on it).
  if (web_view_ && web_view_->GetPage()) {
    web_view_->GetPage()->Animator().ServiceScriptedAnimations(
        base::TimeTicks::Now());
  }
  // Force IntersectionObserver computation. The normal lifecycle step skips it for
  // a throttled frame, and our offscreen widget reads as throttled — so observers
  // (lazy-load, infinite scroll, viewability) would never fire. This bypasses the
  // throttle gate; the queued notifications are then delivered by the loop pump.
  if (main_frame_) {
    auto* impl = blink::To<blink::WebLocalFrameImpl>(main_frame_);
    if (impl->GetFrame() && impl->GetFrame()->View())
      impl->GetFrame()->View()->ForceUpdateViewportIntersections();
  }
}

bool MbWebView::PaintInto(SkCanvas& canvas, int origin_x, int origin_y) {
  if (!widget_ || !widget_->widget() || !main_frame_)
    return false;
  // Interleave lifecycle + task draining: layout issues subresource requests (images
  // load lazily during layout), the loads complete async, then a later lifecycle applies
  // them. A few rounds settle CSS + images before the final paint. Service rAF each
  // round so animation-driven DOM changes are reflected.
  for (int round = 0; round < 5; ++round) {
    ServiceAnimations();
    widget_->widget()->UpdateAllLifecyclePhases(blink::DocumentUpdateReason::kTest);
    base::RunLoop().RunUntilIdle();
  }
  widget_->widget()->UpdateAllLifecyclePhases(blink::DocumentUpdateReason::kTest);

  auto* impl = blink::To<blink::WebLocalFrameImpl>(main_frame_);
  blink::LocalFrame* frame = impl->GetFrame();
  if (!frame || !frame->View())
    return false;

  // Replay Blink's recorded paint ops straight into the canvas (no compositor).
  // Transparent capture (omitBackground): clear to 0 alpha so unpainted areas
  // stay transparent; otherwise an opaque white base like a normal screenshot.
  canvas.clear(transparent_bg_ ? SK_ColorTRANSPARENT : SK_ColorWHITE);
  // HiDPI: the paint record is in CSS px; scaling the canvas makes skia re-raster
  // glyphs/vectors crisply at the device pixel ratio into the (logical*dsf) bitmap.
  if (dsf_ != 1.0f)
    canvas.scale(dsf_, dsf_);
  // Clip capture: shift the document so logical (origin_x, origin_y) lands at the
  // canvas origin; the (smaller) bitmap then holds just that region. The translate
  // is in CSS px because it's applied after the dsf scale.
  if (origin_x != 0 || origin_y != 0)
    canvas.translate(static_cast<float>(-origin_x), static_cast<float>(-origin_y));
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

bool MbWebView::PaintRectToBitmap(void* out_bgra, int x, int y, int w, int h,
                                  int stride) {
  if (!out_bgra || w <= 0 || h <= 0)
    return false;
  SkBitmap bitmap;
  if (!bitmap.installPixels(
          SkImageInfo::Make(w, h, kBGRA_8888_SkColorType, kPremul_SkAlphaType),
          out_bgra, stride)) {
    return false;
  }
  SkCanvas canvas(bitmap);
  return PaintInto(canvas, x, y);
}

namespace {
bool EndsWith(const std::string& s, const char* suffix) {
  std::string suf(suffix);
  return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}
// Encode `bitmap` to disk, choosing the format by the path's extension: .jpg/.jpeg
// -> JPEG (quality 90; smaller, no alpha), anything else -> PNG (lossless, alpha).
bool EncodeBitmapToPath(const SkBitmap& bitmap, const char* path) {
  std::string p(path ? path : "");
  for (char& c : p)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  std::optional<std::vector<uint8_t>> data;
  if (EndsWith(p, ".jpg") || EndsWith(p, ".jpeg"))
    data = gfx::JPEGCodec::Encode(bitmap, /*quality=*/90);
  else
    data = gfx::PNGCodec::EncodeBGRASkBitmap(bitmap, /*discard_transparency=*/false);
  return data && base::WriteFile(base::FilePath(path), *data);
}
}  // namespace

bool MbWebView::SavePngRect(const char* path, int x, int y, int w, int h) {
  if (w <= 0 || h <= 0)
    return false;
  // The bitmap is in physical px (logical size * dsf); the clip origin (x,y) is in
  // logical px and applied inside PaintInto after the dsf scale.
  const int pw = static_cast<int>(w * dsf_);
  const int ph = static_cast<int>(h * dsf_);
  SkBitmap bitmap;
  if (!bitmap.tryAllocPixels(SkImageInfo::Make(pw, ph, kBGRA_8888_SkColorType,
                                               kPremul_SkAlphaType))) {
    return false;
  }
  SkCanvas canvas(bitmap);
  if (!PaintInto(canvas, x, y))
    return false;
  return EncodeBitmapToPath(bitmap, path);
}

bool MbWebView::SavePdf(const char* path) {
  if (!main_frame_ || !widget_ || !widget_->widget())
    return false;
  // Settle the document, then drive Blink's print path into a Skia PDF document.
  for (int round = 0; round < 5; ++round) {
    widget_->widget()->UpdateAllLifecyclePhases(blink::DocumentUpdateReason::kTest);
    base::RunLoop().RunUntilIdle();
  }
  // US Letter at 96 CSS dpi (816x1056 px); PDF user space is points (72/in).
  constexpr float kCssW = 816.f, kCssH = 1056.f;
  constexpr float kPtScale = 72.f / 96.f;  // CSS px -> points
  // This ctor sets printable area + page description AND print_scaling_option =
  // kSourceSize, which the paginated layout requires (a DCHECK enforces it).
  blink::WebPrintParams params{gfx::SizeF(kCssW, kCssH)};
  params.printer_dpi = 300;

  const uint32_t pages = main_frame_->PrintBegin(params, blink::WebNode());
  if (pages == 0) {
    main_frame_->PrintEnd();
    return false;
  }
  SkDynamicMemoryWStream stream;
  sk_sp<SkDocument> doc = SkPDF::MakeDocument(&stream);
  if (!doc) {
    main_frame_->PrintEnd();
    return false;
  }
  for (uint32_t i = 0; i < pages; ++i) {
    SkCanvas* canvas = doc->beginPage(kCssW * kPtScale, kCssH * kPtScale);
    canvas->scale(kPtScale, kPtScale);  // paint CSS-px content into the points page
    cc::SkiaPaintCanvas paint_canvas(canvas);
    main_frame_->PrintPage(i, &paint_canvas);
    doc->endPage();
  }
  main_frame_->PrintEnd();
  doc->close();

  std::vector<uint8_t> buf(stream.bytesWritten());
  stream.copyToAndReset(buf.data());
  return base::WriteFile(base::FilePath(path), buf);
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
  return EncodeBitmapToPath(bitmap, path);
}

}  // namespace mb
