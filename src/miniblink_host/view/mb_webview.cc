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
#include "v8/include/v8-external.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-function-callback.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-json.h"
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

  // Mark the page active + focused so focus-dependent behavior works headlessly:
  // Tab advancing focus (FocusController::AdvanceFocus is gated on page focus),
  // <input autofocus>, and :focus styles. A real browser sets this when its
  // window gains focus; we always want it for an automation view. Done after
  // DidAttachLocalMainFrame so the FocusController is on a live main frame.
  v->web_view_->SetIsActive(true);
  v->web_view_->SetPageFocus(true);

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
  http_status_ = 0;  // in-memory doc; no HTTP status
  response_headers_.clear();
  const char* html = utf8_html ? utf8_html : "";
  CommitHtml(html, std::strlen(html), base_url);
}

void MbWebView::LoadURL(const char* utf8_url) {
  std::string url(utf8_url ? utf8_url : "");
  http_status_ = 0;  // reset; only an http(s) load sets a real status
  response_headers_.clear();
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
        /*http_method=*/std::string(), &final_url, &http_status_,
        &response_headers_);
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

void MbWebView::PostURL(const char* utf8_url, const char* utf8_body,
                        const char* content_type) {
  std::string url(utf8_url ? utf8_url : "");
  http_status_ = 0;
  response_headers_.clear();
  if (url.rfind("http", 0) != 0)
    return;  // POST navigation is only meaningful for http(s)
  std::string post_body(utf8_body ? utf8_body : "");
  std::string post_ct(content_type ? content_type : "");
  std::string body, resp_ct, final_url;
  const bool ok = MbFetchUrl(
      url, &body, &resp_ct,
      frame_client_ ? frame_client_->user_agent() : std::string(),
      frame_client_ ? frame_client_->extra_headers() : std::string(), post_body,
      post_ct, "POST", &final_url, &http_status_, &response_headers_);
  const std::string& doc_url = final_url.empty() ? url : final_url;
  if (ok) {
    std::string charset;  // server charset (authoritative), as in LoadURL
    std::string lc = resp_ct;
    for (char& c : lc) c = static_cast<char>(std::tolower((unsigned char)c));
    if (auto p = lc.find("charset="); p != std::string::npos) {
      p += 8;
      std::string::size_type end = resp_ct.find_first_of("; \t", p);
      charset = resp_ct.substr(p, end == std::string::npos ? end : end - p);
    }
    CommitHtml(body.data(), body.size(), doc_url.c_str(), charset);
  }
}

void MbWebView::SendMouseClick(int x, int y) {
  if (widget_)
    widget_->SendMouseClick(x, y);
}

bool MbWebView::ScrollIntoView(const char* css_selector) {
  // Scroll the first match to the viewport center. scrollIntoView forces layout
  // and updates the scroll offset synchronously, so a getBoundingClientRect that
  // runs right after sees the element's new (in-viewport) position. False if no
  // element matches. Used by the click/hover paths and exposed via the C API.
  if (!css_selector)
    return false;
  std::string js =
      "(function(){var e=document.querySelector(\"" + JsEscape(css_selector) +
      "\");if(!e||!e.scrollIntoView)return '0';"
      "e.scrollIntoView({block:'center',inline:'center'});return '1';})()";
  return EvalToString(js.c_str()) == "1";
}

bool MbWebView::ClickSelector(const char* css_selector) {
  if (!css_selector || !widget_)
    return false;
  // Bring the element on-screen first so a below-the-fold target has a box
  // inside the viewport for the coordinate-based click to land on.
  ScrollIntoView(css_selector);
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

bool MbWebView::GetContentSize(int* w, int* h) {
  // The full scrollable document size (logical px), >= the viewport. For
  // full-page screenshots: mbResize to this height, then paint — content below
  // the original fold gets laid out and rendered.
  std::string s = EvalToString(
      "(function(){var d=document.documentElement,b=document.body;"
      "return Math.max(d.scrollWidth,d.clientWidth,b?b.scrollWidth:0)+','+"
      "Math.max(d.scrollHeight,d.clientHeight,b?b.scrollHeight:0);})()");
  std::string::size_type comma = s.find(',');
  if (comma == std::string::npos)
    return false;
  if (w) *w = std::atoi(s.substr(0, comma).c_str());
  if (h) *h = std::atoi(s.substr(comma + 1).c_str());
  return true;
}

bool MbWebView::SelectOption(const char* css_selector, const char* value) {
  // Select a <select>'s option whose value OR visible text equals `value`, then
  // fire input+change (so framework-bound selects react) — like Puppeteer's
  // page.select. Returns false if no <select> matches or no option matches.
  if (!css_selector)
    return false;
  std::string js =
      "(function(){var e=document.querySelector(\"" + JsEscape(css_selector) +
      "\");if(!e||e.tagName!=='SELECT')return '0';var v=\"" +
      JsEscape(value ? value : "") +
      "\";var hit=-1;for(var i=0;i<e.options.length;i++){"
      "if(e.options[i].value===v||e.options[i].text===v){hit=i;break;}}"
      "if(hit<0)return '0';e.selectedIndex=hit;"
      "e.dispatchEvent(new Event('input',{bubbles:true}));"
      "e.dispatchEvent(new Event('change',{bubbles:true}));return '1';})()";
  return EvalToString(js.c_str()) == "1";
}

bool MbWebView::DoubleClickSelector(const char* css_selector) {
  // Double-click the first match's center (fires dblclick — text selection,
  // expand/collapse, inline edit). Returns false if no match / no box.
  ScrollIntoView(css_selector);  // bring a below-fold target on-screen
  int x = 0, y = 0, w = 0, h = 0;
  if (!widget_ || !GetElementRect(css_selector, &x, &y, &w, &h) ||
      (w <= 0 && h <= 0))
    return false;
  widget_->SendDoubleClick(x + w / 2, y + h / 2);
  return true;
}

bool MbWebView::RightClickSelector(const char* css_selector) {
  // Right-click the first match's center (fires contextmenu — right-click menus).
  ScrollIntoView(css_selector);  // bring a below-fold target on-screen
  int x = 0, y = 0, w = 0, h = 0;
  if (!widget_ || !GetElementRect(css_selector, &x, &y, &w, &h) ||
      (w <= 0 && h <= 0))
    return false;
  widget_->SendRightClick(x + w / 2, y + h / 2);
  return true;
}

bool MbWebView::HoverSelector(const char* css_selector) {
  // Move the pointer to the first match's center, generating mousemove +
  // mouseover/mouseenter and applying :hover — for dropdown menus, tooltips, and
  // hover-revealed controls. Returns false if there's no match or no box.
  ScrollIntoView(css_selector);  // bring a below-fold target on-screen
  int x = 0, y = 0, w = 0, h = 0;
  if (!widget_ || !GetElementRect(css_selector, &x, &y, &w, &h) ||
      (w <= 0 && h <= 0))
    return false;
  SendMouseMove(x + w / 2, y + h / 2);
  return true;
}

bool MbWebView::FocusSelector(const char* css_selector) {
  // Focus the first match (HTMLElement.focus()), firing focus/focusin — for
  // focusing non-clickable focusables before key input. False if no match.
  if (!css_selector)
    return false;
  std::string js = "(function(){var e=document.querySelector(\"" +
                   JsEscape(css_selector) +
                   "\");if(!e||!e.focus)return '0';e.focus();return '1';})()";
  return EvalToString(js.c_str()) == "1";
}

bool MbWebView::BlurSelector(const char* css_selector) {
  // Blur the first match (HTMLElement.blur()), firing blur/focusout — commonly
  // what triggers form-field validation. False if no match.
  if (!css_selector)
    return false;
  std::string js = "(function(){var e=document.querySelector(\"" +
                   JsEscape(css_selector) +
                   "\");if(!e||!e.blur)return '0';e.blur();return '1';})()";
  return EvalToString(js.c_str()) == "1";
}

bool MbWebView::GetElementRect(const char* css_selector, int* x, int* y, int* w,
                               int* h) {
  if (!css_selector)
    return false;
  // The first match's viewport-relative bounding box (logical px), as
  // "left,top,width,height". Composes with PaintRectToBitmap for an element
  // screenshot, or with SendMouseClick for a precise click. "" if no match.
  std::string js =
      "(function(){var e=document.querySelector(\"" + JsEscape(css_selector) +
      "\");if(!e)return '';var r=e.getBoundingClientRect();"
      "return Math.round(r.left)+','+Math.round(r.top)+','+"
      "Math.round(r.width)+','+Math.round(r.height);})()";
  std::string s = EvalToString(js.c_str());
  if (s.empty())
    return false;
  int vals[4] = {0, 0, 0, 0};
  int n = 0;
  std::string::size_type start = 0;
  for (std::string::size_type i = 0; i <= s.size() && n < 4; ++i) {
    if (i == s.size() || s[i] == ',') {
      vals[n++] = std::atoi(s.substr(start, i - start).c_str());
      start = i + 1;
    }
  }
  if (n < 4)
    return false;
  if (x) *x = vals[0];
  if (y) *y = vals[1];
  if (w) *w = vals[2];
  if (h) *h = vals[3];
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

std::string MbWebView::GetUserAgent() {
  return frame_client_ ? frame_client_->EffectiveUserAgent() : std::string();
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

std::string MbWebView::GetAllCookies() {
  return MbGetAllCookies();  // whole jar as a Netscape cookie file (in memory)
}

void MbWebView::SetCookie(const char* url, const char* cookie) {
  // Inject a cookie (a "name=value[; attrs]" string) into the shared HTTP jar for
  // `url`'s origin — the inverse of GetCookies, for restoring a saved session.
  if (url && cookie)
    MbAddCookieToJar(url, cookie);
}

void MbWebView::ClearCookies() {
  MbClearCookieJar();
}

std::string MbWebView::GetURL() {
  // The committed main document's URL — this is the FINAL URL after any
  // server/navigation redirects (see LoadURL), read straight from the frame so
  // it is correct even with JS disabled or an unusual document.
  if (!main_frame_)
    return std::string();
  return main_frame_->GetDocument().Url().GetString().Utf8();
}

std::string MbWebView::GetTitle() {
  if (!main_frame_)
    return std::string();
  return main_frame_->GetDocument().Title().Utf8();
}

std::string MbWebView::GetText() {
  // The page's visible text (document.body.innerText) — the common scraping read.
  return EvalToString("document.body ? document.body.innerText : ''");
}

std::string MbWebView::GetHTML() {
  // The rendered, post-JS DOM serialized to HTML (document.documentElement
  // .outerHTML) — for scraping the live document, not the original source.
  return EvalToString(
      "document.documentElement ? document.documentElement.outerHTML : ''");
}

bool MbWebView::GetTextForSelector(const char* css_selector, std::string* out) {
  if (!css_selector)
    return false;
  // innerText of the first match. We prefix a '1' flag on success so an element
  // whose text is genuinely "" is distinguishable from "no element matched" (JS
  // returns "" only in the no-match case). Strip the flag before returning.
  std::string js =
      "(function(){var e=document.querySelector(\"" + JsEscape(css_selector) +
      "\");if(!e)return '';return '1'+(e.innerText||'');})()";
  std::string s = EvalToString(js.c_str());
  if (s.empty())
    return false;  // no element matched
  if (out)
    *out = s.substr(1);
  return true;
}

bool MbWebView::GetAttribute(const char* css_selector, const char* attr,
                             std::string* out) {
  if (!css_selector || !attr)
    return false;
  // getAttribute on the first match. Same '1'-flag trick; "" (→ false) covers
  // both "no element" and "attribute absent" (getAttribute returned null), which
  // is the natural "no value" semantics for the caller.
  std::string js =
      "(function(){var e=document.querySelector(\"" + JsEscape(css_selector) +
      "\");if(!e)return '';var a=e.getAttribute(\"" + JsEscape(attr) +
      "\");if(a==null)return '';return '1'+a;})()";
  std::string s = EvalToString(js.c_str());
  if (s.empty())
    return false;  // no element, or attribute absent
  if (out)
    *out = s.substr(1);
  return true;
}

bool MbWebView::GetValueForSelector(const char* css_selector, std::string* out) {
  if (!css_selector)
    return false;
  // The live .value property of the first match. Same '1'-flag trick; an element
  // with no value property (e.g. a <div>) yields undefined -> "" -> false, the
  // same "no value" path as no-match. A control whose value is genuinely "" is
  // preserved by the flag.
  std::string js =
      "(function(){var e=document.querySelector(\"" + JsEscape(css_selector) +
      "\");if(!e)return '';var v=e.value;if(v==null)return '';return '1'+v;})()";
  std::string s = EvalToString(js.c_str());
  if (s.empty())
    return false;  // no element, or no value property
  if (out)
    *out = s.substr(1);
  return true;
}

int MbWebView::GetCheckedForSelector(const char* css_selector) {
  if (!css_selector)
    return -1;
  // The first match's .checked state: 1 checked, 0 unchecked, -1 if no element
  // matches or it isn't a checkable control (.checked not a boolean). The flag
  // string maps directly so there's no empty-vs-no-match ambiguity.
  std::string js =
      "(function(){var e=document.querySelector(\"" + JsEscape(css_selector) +
      "\");if(!e||typeof e.checked!=='boolean')return '-1';"
      "return e.checked?'1':'0';})()";
  std::string s = EvalToString(js.c_str());
  if (s.empty())
    return -1;
  return std::atoi(s.c_str());
}

int MbWebView::IsVisibleForSelector(const char* css_selector) {
  if (!css_selector)
    return -1;
  // The first match's actual visibility: 1 visible, 0 hidden, -1 if no element
  // matches. Uses Element.checkVisibility (M150) with checkOpacity +
  // checkVisibilityCSS so display:none, visibility:hidden, content-visibility,
  // and opacity:0 all count as hidden. Falls back to layout-box presence on the
  // (unexpected) chance the API is absent.
  std::string js =
      "(function(){var e=document.querySelector(\"" + JsEscape(css_selector) +
      "\");if(!e)return '-1';var v=(typeof e.checkVisibility==='function')?"
      "e.checkVisibility({checkOpacity:true,checkVisibilityCSS:true}):"
      "!!(e.offsetWidth||e.offsetHeight||e.getClientRects().length);"
      "return v?'1':'0';})()";
  std::string s = EvalToString(js.c_str());
  if (s.empty())
    return -1;
  return std::atoi(s.c_str());
}

int MbWebView::CountSelector(const char* css_selector) {
  if (!css_selector)
    return -1;
  // querySelectorAll(selector).length. The try/catch maps an invalid selector
  // (querySelectorAll throws SyntaxError) to -1, distinct from a valid 0 matches.
  std::string js =
      "(function(){try{return ''+document.querySelectorAll(\"" +
      JsEscape(css_selector) + "\").length;}catch(e){return '-1';}})()";
  std::string s = EvalToString(js.c_str());
  if (s.empty())
    return -1;
  return std::atoi(s.c_str());
}

bool MbWebView::GetComputedStyle(const char* css_selector, const char* property,
                                 std::string* out) {
  if (!css_selector || !property)
    return false;
  // getComputedStyle(el).getPropertyValue(prop) for the first match. The '1'
  // flag (as in GetAttribute) distinguishes an empty computed value from
  // no-match (JS returns "" only when nothing matches).
  std::string js =
      "(function(){var e=document.querySelector(\"" + JsEscape(css_selector) +
      "\");if(!e)return '';var v=getComputedStyle(e).getPropertyValue(\"" +
      JsEscape(property) + "\");return '1'+(v==null?'':v);})()";
  std::string s = EvalToString(js.c_str());
  if (s.empty())
    return false;  // no element matched
  if (out)
    *out = s.substr(1);
  return true;
}

void MbWebView::Reload() {
  // Re-navigate to the committed document's URL, re-fetching it. Only meaningful
  // for real (file/http) URLs; in-memory docs (about:blank, data:) are left as-is.
  std::string u = GetURL();
  if (u.rfind("file://", 0) == 0 || u.rfind("http", 0) == 0)
    LoadURL(u.c_str());
}

void MbWebView::OnDidCommitMainFrame(const std::string& url, bool standard) {
  // Maintain the back/forward stack. Only standard commits append; reloads and
  // the initial empty document (inert commits) do not. A Go{Back,Forward}
  // re-navigates via LoadURL (also a standard commit) — the in_history_nav_ flag
  // distinguishes it so we move within the list instead of appending.
  if (!standard || url.empty())
    return;
  if (in_history_nav_) {
    in_history_nav_ = false;
    return;  // position already set by GoBack/GoForward
  }
  if (history_index_ >= 0 &&
      history_index_ < static_cast<int>(history_.size()) &&
      history_[history_index_] == url) {
    return;  // same URL (e.g. a reload committed as standard) — no new entry
  }
  history_.resize(history_index_ + 1);  // a new navigation truncates forward
  history_.push_back(url);
  history_index_ = static_cast<int>(history_.size()) - 1;
}

bool MbWebView::GoBack() {
  if (!CanGoBack())
    return false;
  in_history_nav_ = true;
  --history_index_;
  LoadURL(history_[history_index_].c_str());
  return true;
}

bool MbWebView::GoForward() {
  if (!CanGoForward())
    return false;
  in_history_nav_ = true;
  ++history_index_;
  LoadURL(history_[history_index_].c_str());
  return true;
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

void MbWebView::SendKey(const char* key_name) {
  if (widget_)
    widget_->SendKey(key_name);
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

void MbWebView::ScrollTo(int x, int y) {
  // Absolute scroll to a known offset (vs SendScroll's relative gesture) — for
  // capturing the real viewport at a position, where fixed/sticky elements render
  // correctly (a full-page resize would not). window.scrollTo via the eval path.
  std::string js = "window.scrollTo(" + std::to_string(x) + "," +
                   std::to_string(y) + ")";
  EvalToString(js.c_str());
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

void MbWebView::SetFocus(bool focused) {
  // Mirror the creation-time focus setup (SetIsActive + SetPageFocus), so an app
  // can simulate the view's window gaining/losing focus — toggling
  // document.hasFocus(), :focus-within, and blur/focus events.
  if (web_view_) {
    web_view_->SetIsActive(focused);
    web_view_->SetPageFocus(focused);
  }
}

void MbWebView::SetInitScript(const char* utf8_script) {
  init_script_ = utf8_script ? utf8_script : "";
}

namespace {
// v8 callback for a bound native function: coerce args to UTF-8 strings, call the
// C function, return its UTF-8 string result (or undefined). The NativeBinding*
// rides in the function's External data slot.
void MbNativeTrampoline(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  auto* b = static_cast<MbWebView::NativeBinding*>(
      info.Data().As<v8::External>()->Value(v8::kExternalPointerTypeTagDefault));
  if (!b || !b->fn)
    return;
  std::vector<std::string> args;
  std::vector<int> types;  // per-arg JS type (see MbJsNativeFn doc)
  args.reserve(info.Length());
  types.reserve(info.Length());
  for (int i = 0; i < info.Length(); ++i) {
    v8::Local<v8::Value> v = info[i];
    int t = 0;  // string
    if (v->IsNumber())
      t = 1;
    else if (v->IsBoolean())
      t = 2;
    else if (v->IsNull())
      t = 3;
    else if (v->IsUndefined())
      t = 4;
    else if (v->IsArray())
      t = 6;
    else if (v->IsFunction())
      t = 7;
    else if (v->IsString())
      t = 0;
    else if (v->IsObject())
      t = 5;
    types.push_back(t);
    v8::String::Utf8Value s(isolate, v);
    args.emplace_back(*s ? *s : "");
  }
  std::vector<const char*> argv;
  argv.reserve(args.size());
  for (const std::string& a : args)
    argv.push_back(a.c_str());
  int out_type = 0;  // 0 string, 1 number, 2 boolean, 3 null, 4 undefined
  const char* result = b->fn(b->userdata, static_cast<int>(args.size()),
                             argv.empty() ? nullptr : argv.data(),
                             types.empty() ? nullptr : types.data(), &out_type);
  switch (out_type) {
    case 1:  // number
      info.GetReturnValue().Set(
          v8::Number::New(isolate, result ? std::atof(result) : 0.0));
      return;
    case 2:  // boolean
      info.GetReturnValue().Set(
          v8::Boolean::New(isolate, result && std::strcmp(result, "true") == 0));
      return;
    case 3:  // null
      info.GetReturnValue().SetNull();
      return;
    case 4:  // undefined
      return;
    case 5: {  // JSON -> a parsed object/array/value (structured-data return)
      v8::Local<v8::String> js;
      if (result &&
          v8::String::NewFromUtf8(isolate, result).ToLocal(&js)) {
        v8::Local<v8::Value> parsed;
        if (v8::JSON::Parse(isolate->GetCurrentContext(), js).ToLocal(&parsed))
          info.GetReturnValue().Set(parsed);
      }
      return;
    }
    default:  // string
      if (result) {
        v8::Local<v8::String> rs;
        if (v8::String::NewFromUtf8(isolate, result).ToLocal(&rs))
          info.GetReturnValue().Set(rs);
      }
      return;
  }
}
}  // namespace

void MbWebView::InstallJsBindings() {
  // Install each bound native function onto the main world's window. Runs at
  // document-element-available. NB: uses CreateDataProperty, not Object::Set —
  // the public v8 [[Set]] API traps in this hardened build (verified by probe),
  // while the define-own-property path works.
  if (js_bindings_.empty() || !main_frame_)
    return;
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  if (!isolate)
    return;
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = main_frame_->MainWorldScriptContext();
  if (context.IsEmpty())
    return;
  v8::Context::Scope context_scope(context);
  v8::Local<v8::Object> global = context->Global();
  for (const std::unique_ptr<NativeBinding>& b : js_bindings_) {
    v8::Local<v8::Function> fn;
    if (!v8::Function::New(
             context, &MbNativeTrampoline,
             v8::External::New(isolate, b.get(),
                               v8::kExternalPointerTypeTagDefault))
             .ToLocal(&fn)) {
      continue;
    }
    v8::Local<v8::String> name;
    if (!v8::String::NewFromUtf8(isolate, b->name.c_str()).ToLocal(&name))
      continue;
    v8::Maybe<bool> ok = global->CreateDataProperty(context, name, fn);
    (void)ok;  // best-effort
  }
}

void MbWebView::BindJsFunction(const char* name, MbJsNativeFn fn,
                               void* userdata) {
  if (!name || !fn)
    return;
  auto b = std::make_unique<NativeBinding>();
  b->name = name;
  b->fn = fn;
  b->userdata = userdata;
  js_bindings_.push_back(std::move(b));
}

void MbWebView::RunDocumentStartScript() {
  // Called at document-element-available (before the page's own scripts). Run the
  // host init script, then install bound native functions, so both are present
  // before the page's own scripts run.
  if (!main_frame_)
    return;
  if (!init_script_.empty()) {
    main_frame_->ExecuteScript(
        blink::WebScriptSource(blink::WebString::FromUtf8(init_script_)));
  }
  InstallJsBindings();
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

std::string MbWebView::EvalWithType(const char* utf8_script,
                                    std::string* out_type) {
  if (out_type)
    out_type->clear();
  if (!main_frame_ || !utf8_script)
    return {};
  std::string result;
  std::string type;
  blink::WebLocalFrame* frame = main_frame_;
  RunInFrameTask(
      base::BindOnce(
          [](blink::WebLocalFrame* f, std::string s, std::string* out,
             std::string* out_t) {
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
            // Type: check the specific kinds before the generic object (an array
            // and a function are also objects).
            if (value->IsNull())
              *out_t = "null";
            else if (value->IsUndefined())
              *out_t = "undefined";
            else if (value->IsArray())
              *out_t = "array";
            else if (value->IsFunction())
              *out_t = "function";
            else if (value->IsBoolean())
              *out_t = "boolean";
            else if (value->IsNumber())
              *out_t = "number";
            else if (value->IsString())
              *out_t = "string";
            else if (value->IsObject())
              *out_t = "object";
            else
              *out_t = "undefined";
            v8::Local<v8::String> str;
            if (!value->ToString(context).ToLocal(&str))
              return;
            v8::String::Utf8Value utf8(isolate, str);
            if (*utf8)
              out->assign(*utf8, utf8.length());
          },
          frame, std::string(utf8_script), &result, &type),
      /*settle=*/false);
  if (out_type)
    *out_type = type;
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

bool MbWebView::WaitForVisibleSelector(const char* css, int timeout_ms) {
  if (!main_frame_ || !css)
    return false;
  // Like WaitForSelector, but the condition is real VISIBILITY (the same
  // checkVisibility test as IsVisibleForSelector), not mere existence — so it
  // waits out a fade-in / display:none toggle / lazy reveal, not just DOM
  // insertion. Same poll cadence; "0" while absent or hidden, "1" once shown.
  const std::string probe =
      "(function(){var e=document.querySelector(\"" + JsEscape(css) +
      "\");if(!e)return '0';return (typeof e.checkVisibility==='function'?"
      "e.checkVisibility({checkOpacity:true,checkVisibilityCSS:true}):"
      "!!(e.offsetWidth||e.offsetHeight||e.getClientRects().length))?'1':'0';})()";
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
// Encode `bitmap` to PNG bytes in memory (lossless, alpha kept).
bool EncodeBitmapToPng(const SkBitmap& bitmap, std::vector<uint8_t>* out) {
  std::optional<std::vector<uint8_t>> data =
      gfx::PNGCodec::EncodeBGRASkBitmap(bitmap, /*discard_transparency=*/false);
  if (!data)
    return false;
  *out = std::move(*data);
  return true;
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

bool MbWebView::EncodePng(int w, int h) {
  if (w <= 0 || h <= 0)
    return false;
  // Render the full view to a w×h BGRA bitmap and encode it to PNG bytes held in
  // encoded_png_ (so the C API can hand back a pointer without a temp file). Same
  // paint path as SavePng; the bytes live until the next EncodePng or teardown.
  SkBitmap bitmap;
  if (!bitmap.tryAllocPixels(
          SkImageInfo::Make(w, h, kBGRA_8888_SkColorType, kPremul_SkAlphaType))) {
    return false;
  }
  SkCanvas canvas(bitmap);
  if (!PaintInto(canvas))
    return false;
  return EncodeBitmapToPng(bitmap, &encoded_png_);
}

}  // namespace mb
