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
#include "third_party/blink/public/platform/web_policy_container.h"
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
#include "third_party/blink/public/web/web_ax_context.h"
#include "third_party/blink/public/web/web_ax_object.h"
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
#include "third_party/blink/public/mojom/page/page_visibility_state.mojom-shared.h"
#include "third_party/blink/public/web/web_view.h"
#include "third_party/blink/renderer/platform/network/network_state_notifier.h"
#include "third_party/icu/source/common/unicode/unistr.h"
#include "third_party/icu/source/common/unicode/uscript.h"
#include "third_party/icu/source/i18n/unicode/timezone.h"
#include "ui/accessibility/ax_enum_util.h"
#include "ui/accessibility/ax_mode.h"
#include "third_party/blink/renderer/core/css/media_value_change.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/exported/web_view_impl.h"
#include "third_party/blink/renderer/core/fileapi/file.h"
#include "third_party/blink/renderer/core/fileapi/file_list.h"
#include "third_party/blink/renderer/core/html/forms/html_input_element.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/public/mojom/frame/find_in_page.mojom-blink.h"
#include "third_party/blink/renderer/core/editing/finder/text_finder.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/paint/paint_flags.h"
#include "third_party/blink/renderer/platform/graphics/paint/cull_rect.h"
#include "third_party/blink/renderer/platform/graphics/paint/paint_record_builder.h"
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
#include "third_party/blink/renderer/platform/blob/blob_data.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "ui/gfx/codec/jpeg_codec.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_f.h"
#include "url/gurl.h"
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

// Canonical JS for the fill / get-text selector ops, shared by the main-frame
// methods and their per-frame variants so the (React-compatible) fill semantics
// and the no-match sentinel ('0'/'' = no element) stay identical across both.
std::string BuildFillJs(const char* css_selector, const char* text) {
  return "(function(){var e=document.querySelector(\"" + JsEscape(css_selector) +
         "\");if(!e)return '0';e.focus();var t=\"" + JsEscape(text ? text : "") +
         "\";var proto=(e instanceof HTMLTextAreaElement)?HTMLTextAreaElement."
         "prototype:(e instanceof HTMLInputElement)?HTMLInputElement.prototype:"
         "null;var d=proto&&Object.getOwnPropertyDescriptor(proto,'value');"
         "if(d&&d.set){d.set.call(e,t);}else{try{e.value=t;}catch(ex){return "
         "'0';}}e.dispatchEvent(new Event('input',{bubbles:true}));"
         "e.dispatchEvent(new Event('change',{bubbles:true}));return '1';})()";
}
std::string BuildGetTextJs(const char* css_selector) {
  return "(function(){var e=document.querySelector(\"" + JsEscape(css_selector) +
         "\");if(!e)return '';return '1'+(e.innerText||'');})()";
}

// Pick the download filename. blink leaves suggested_name EMPTY when the <a
// download> attribute has no value, or strips it for a cross-origin link — in
// which case the browser is expected to derive a name from the URL. We were
// passing the empty string straight through; now we fall back to the URL's last
// path segment (percent-decoded by GURL::ExtractFileName), then a generic name.
std::string DownloadFilenameFor(const std::string& url,
                                const std::string& suggested) {
  if (!suggested.empty())
    return suggested;
  std::string from_url = GURL(url).ExtractFileName();  // "" for data:/blob:/no path
  return from_url.empty() ? "download" : from_url;
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
      MakeFrameInterfaceBroker(v->frame_client_->frame_key()),
      blink::LocalFrameToken(), blink::DocumentToken(),
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

  // Initialize the process-wide online state (default true). This marks it
  // "initialized" so a later MbSetOnline(false) actually fires the offline event
  // (NetworkStateNotifier suppresses the event for the very first transition).
  blink::GetNetworkStateNotifier().SetOnLine(true);

  // 4. Attach a session-storage namespace to the page so window.sessionStorage
  //    resolves (without it StorageNamespace::From(page) is null -> TypeError).
  //    The id is normally a browser-assigned 36-char token; we mint a UNIQUE one
  //    per view so each view's sessionStorage is isolated (the DOM Storage backend
  //    keys session areas by this id), while same-view frames share it. localStorage
  //    needs no namespace (it goes through the StorageController directly).
  if (blink::Page* page = v->web_view_->GetPage()) {
    static uint64_t session_ns_counter = 0;
    char id[blink::kSessionStorageNamespaceIdLength + 1];
    snprintf(id, sizeof(id), "%0*llu",
             static_cast<int>(blink::kSessionStorageNamespaceIdLength),
             static_cast<unsigned long long>(++session_ns_counter));
    blink::StorageNamespace::ProvideSessionStorageNamespaceTo(*page,
                                                              std::string(id));
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
  load_finished_ = false;  // a new navigation starts; DidFinishLoad will set it true
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
  // Give the new document a FRESH, empty policy container so a prior page's <meta> CSP
  // does not leak into this one in a reused view. The remote must be BOUND (a null
  // remote CHECK-fails at commit) — MbPolicyContainerHost provides a dedicated bound
  // receiver. policy_host is a stack local that outlives CommitNavigation below (same
  // as DoCommit for child/navigation commits).
  MbPolicyContainerHost policy_host;
  params->policy_container = std::make_unique<blink::WebPolicyContainer>(
      blink::WebPolicyContainerPolicies(), policy_host.BindRemote());
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
  last_error_.clear();  // an in-memory document always loads successfully
  const char* html = utf8_html ? utf8_html : "";
  CommitHtml(html, std::strlen(html), base_url);
}

namespace {
// Decide whether a fetched top-level response is a DOWNLOAD (save) rather than a page to
// render: a Content-Disposition: attachment is the explicit signal; otherwise a MIME the
// engine doesn't render inline (not text/* / image/* / the structured app types). Also
// extracts a suggested filename (Content-Disposition filename=, else the URL basename).
bool IsDownloadResponse(const std::string& url, const std::string& mime,
                        const std::string& headers, std::string* filename) {
  std::string lc_headers = headers;
  for (char& c : lc_headers)
    c = static_cast<char>(std::tolower((unsigned char)c));
  bool is_download = lc_headers.find("content-disposition: attachment") !=
                     std::string::npos;
  std::string m = mime;
  for (char& c : m)
    c = static_cast<char>(std::tolower((unsigned char)c));
  if (!is_download && !m.empty()) {
    const bool renderable =
        m.rfind("text/", 0) == 0 || m.rfind("image/", 0) == 0 ||
        m == "application/json" || m == "application/xml" ||
        m == "application/xhtml+xml" || m == "application/javascript" ||
        m == "application/ecmascript";
    is_download = !renderable;
  }
  if (is_download && filename) {
    // Content-Disposition filename="..." wins; else the URL's last path segment.
    std::string::size_type fp = lc_headers.find("filename=");
    if (fp != std::string::npos) {
      std::string fn = headers.substr(fp + 9);
      if (!fn.empty() && fn.front() == '"')
        fn = fn.substr(1, fn.find('"', 1) - 1);
      else
        fn = fn.substr(0, fn.find_first_of("\r\n;"));
      *filename = fn;
    }
    if (filename->empty()) {
      std::string::size_type slash = url.find_last_of('/');
      std::string base = slash == std::string::npos ? url : url.substr(slash + 1);
      base = base.substr(0, base.find_first_of("?#"));
      *filename = base.empty() ? "download" : base;
    }
  }
  return is_download;
}
}  // namespace

void MbWebView::LoadURL(const char* utf8_url) {
  std::string url(utf8_url ? utf8_url : "");
  http_status_ = 0;  // reset; only an http(s) load sets a real status
  response_headers_.clear();
  last_error_.clear();  // cleared up front; set only if this load fails
  constexpr char kFile[] = "file://";
  if (url.rfind(kFile, 0) == 0) {
    // Top-level file load: read it and commit. (Self-contained docs + data: URIs
    // need no URLLoader; external subresources await the libcurl factory in P2-net.)
    std::string contents;
    if (base::ReadFileToString(base::FilePath(url.substr(sizeof(kFile) - 1)),
                               &contents)) {
      CommitHtml(contents.data(), contents.size(), url.c_str());
    } else {
      last_error_ = "file not found or unreadable";
      NotifyLoadFailed();  // a failed load still "finishes" (signal waiters)
    }
    return;
  }
  if (url.rfind("http", 0) == 0 || url.rfind("data:", 0) == 0) {
    // Top-level http(s) (or a data: URI): fetch via the loader, commit with base = the
    // URL so relative subresources resolve and load through MbURLLoader.
    std::string body, content_type, final_url;
    const bool ok = MbFetchUrl(
        url, &body, &content_type,
        frame_client_ ? frame_client_->user_agent() : std::string(),
        frame_client_ ? frame_client_->extra_headers() : std::string(),
        /*post_body=*/std::string(), /*post_content_type=*/std::string(),
        /*http_method=*/std::string(), &final_url, &http_status_,
        &response_headers_, &last_error_);
    // If the server redirected us, commit with the FINAL URL as the document's
    // base so location.href and relative subresources reflect where we landed.
    const std::string& doc_url = final_url.empty() ? url : final_url;
    if (std::getenv("MB_VERBOSE")) {
      std::fprintf(stderr, "[mb_webview] main-doc %s ok=%d bytes=%zu ct='%s'\n",
                   url.c_str(), ok, body.size(), content_type.c_str());
    }
    // A download (Content-Disposition: attachment / non-renderable MIME) is handed to
    // the embedder via the download callback INSTEAD of committed as a document — but
    // only if a callback is registered, so default behavior is unchanged.
    std::string mime = content_type.substr(0, content_type.find(';'));
    while (!mime.empty() && mime.back() == ' ')
      mime.pop_back();
    std::string dl_name;
    if (ok && on_download_ &&
        IsDownloadResponse(doc_url, mime, response_headers_, &dl_name)) {
      on_download_(doc_url, mime, dl_name, body);
      return;
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
    } else {
      NotifyLoadFailed();  // fetch failed -> still finish, so a waiter isn't stuck
    }
  }
}

void MbWebView::NotifyLoadFailed() {
  // A top-level load that never commits (file read / fetch failure) still ENDED — mark
  // it finished and fire the load-finish callback, so mbIsLoadFinished is true and a
  // caller awaiting completion isn't stuck. (Success runs through DidFinishLoad instead.)
  load_finished_ = true;
  if (on_load_finish_)
    on_load_finish_();
}

bool MbWebView::FetchDownloadBody(const std::string& orig,
                                  std::string* body,
                                  std::string* content_type) {
  // Fetch a URL through the engine's network stack WITHOUT committing it as a
  // document — the shared core of a download. Honors the same interception layer
  // as page loads: rewrite the URL, let a block rule / the request hook veto it,
  // serve a mock with no fetch, and let the response hook inspect/rewrite the
  // bytes. http(s) also carries the view's UA + extra headers + per-URL headers
  // + cookies + proxy; data: is decoded inline by the loader.
  const std::string url = MbApplyUrlRewrites(orig);
  if (MbIsUrlBlocked(url) || MbRequestHookBlocks(orig))
    return false;
  std::string final_url, headers;
  int status = 0;
  bool ok = false;
  std::string mock_body, mock_ct;
  int mock_status = 0;
  if (MbFindMock(url, &mock_body, &mock_ct, &mock_status)) {
    *body = std::move(mock_body);
    *content_type = std::move(mock_ct);
    status = mock_status > 0 ? mock_status : 200;
    ok = true;
  } else {
    ok = MbFetchUrl(url, body, content_type,
                    frame_client_ ? frame_client_->user_agent() : std::string(),
                    frame_client_ ? frame_client_->extra_headers() : std::string(),
                    /*post_body=*/std::string(), /*post_content_type=*/std::string(),
                    /*http_method=*/std::string(), &final_url, &status, &headers);
  }
  if (!ok)
    return false;
  MbInvokeResponseHook(orig, status, body);  // inspect/rewrite before delivery
  return true;
}

bool MbWebView::DownloadURL(const char* url_in, const char* dest_path) {
  if (!url_in || !dest_path)
    return false;
  // A host-initiated download: fetch the URL and write the body to disk.
  std::string body, content_type;
  if (!FetchDownloadBody(url_in, &body, &content_type))
    return false;
  return base::WriteFile(base::FilePath(dest_path), body);
}

void MbWebView::PostURL(const char* utf8_url, const char* utf8_body,
                        size_t body_len, const char* content_type) {
  std::string url(utf8_url ? utf8_url : "");
  http_status_ = 0;
  response_headers_.clear();
  if (url.rfind("http", 0) != 0)
    return;  // POST navigation is only meaningful for http(s)
  // Construct from (ptr, len) — NOT NUL-terminated — so binary bodies with embedded
  // NULs (protobuf, multipart, raw bytes) post whole; the loader uses post_body.size().
  std::string post_body(utf8_body ? std::string(utf8_body, body_len) : std::string());
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

void MbWebView::SendMouseClickEx(int x, int y, int button, int modifiers) {
  if (widget_)
    widget_->SendMouseClickEx(x, y, button, modifiers);
}

void MbWebView::SendMouseDown(int x, int y) {
  if (widget_)
    widget_->SendMouseDown(x, y);
}

void MbWebView::SendTouchTap(int x, int y) {
  // A single-finger tap fires BOTH: (1) TRUSTED pointer events via a real WebPointerEvent
  // (pointerdown/up, isTrusted=true — what modern Pointer-Events mobile UIs use), and
  // (2) JS-synthesized TouchEvents (touchstart/touchend) for Touch-Events UIs (a raw
  // WebTouchEvent that would carry both DCHECKs in this offscreen widget). The pointer
  // events dispatch async (the touch queue) — callers pump (mbWait) before reading.
  if (widget_)
    widget_->SendTouchTap(x, y);
  const std::string sx = std::to_string(x), sy = std::to_string(y);
  std::string js =
      "(function(){try{var e=document.elementFromPoint(" + sx + "," + sy +
      ");if(!e)return;var t=new Touch({identifier:0,target:e,clientX:" + sx +
      ",clientY:" + sy + ",pageX:" + sx + ",pageY:" + sy +
      ",screenX:" + sx + ",screenY:" + sy + "});"
      "e.dispatchEvent(new TouchEvent('touchstart',{bubbles:true,cancelable:true,"
      "touches:[t],targetTouches:[t],changedTouches:[t]}));"
      "e.dispatchEvent(new TouchEvent('touchend',{bubbles:true,cancelable:true,"
      "touches:[],targetTouches:[],changedTouches:[t]}));}catch(err){}})()";
  EvalToString(js.c_str());
}

void MbWebView::SendTouchSwipe(int x1, int y1, int x2, int y2) {
  // A one-finger swipe fires BOTH trusted pointer events (real WebPointerEvent down ->
  // moves -> up, isTrusted) for Pointer-Events drag UIs AND JS-synthesized touchstart/
  // touchmove/touchend for Touch-Events UIs (a raw WebTouchEvent DCHECKs here). Pointer
  // events dispatch async (the touch queue) — callers pump before reading.
  if (widget_)
    widget_->SendTouchSwipe(x1, y1, x2, y2);
  const std::string sx1 = std::to_string(x1), sy1 = std::to_string(y1);
  const std::string sx2 = std::to_string(x2), sy2 = std::to_string(y2);
  std::string js =
      "(function(){try{var e=document.elementFromPoint(" + sx1 + "," + sy1 +
      ");if(!e)return;"
      "function T(cx,cy){return new Touch({identifier:0,target:e,clientX:cx,"
      "clientY:cy,pageX:cx,pageY:cy,screenX:cx,screenY:cy});}"
      "function D(type,cx,cy,active){var t=T(cx,cy);e.dispatchEvent(new TouchEvent("
      "type,{bubbles:true,cancelable:true,touches:active?[t]:[],"
      "targetTouches:active?[t]:[],changedTouches:[t]}));}"
      "var x1=" + sx1 + ",y1=" + sy1 + ",x2=" + sx2 + ",y2=" + sy2 + ";"
      "D('touchstart',x1,y1,true);"
      "for(var i=1;i<=6;i++)D('touchmove',Math.round(x1+(x2-x1)*i/6),"
      "Math.round(y1+(y2-y1)*i/6),true);"
      "D('touchend',x2,y2,false);}catch(err){}})()";
  EvalToString(js.c_str());
}

void MbWebView::SendMouseUp(int x, int y) {
  if (widget_)
    widget_->SendMouseUp(x, y);
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

bool MbWebView::DispatchEvent(const char* css_selector, const char* type) {
  if (!css_selector || !type)
    return false;
  // Dispatch a bubbling, cancelable Event of `type` on the first match — trigger
  // handlers that click/fill don't (mouseover/mouseenter hover menus, focus/blur,
  // submit, custom framework events). Synchronous DOM dispatch (no compositor).
  // Returns true if an element matched.
  std::string js =
      "(function(){var e=document.querySelector(\"" + JsEscape(css_selector) +
      "\");if(!e)return '0';e.dispatchEvent(new Event(\"" + JsEscape(type) +
      "\",{bubbles:true,cancelable:true}));return '1';})()";
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

bool MbWebView::DragSelector(const char* from_selector, const char* to_selector) {
  if (!from_selector || !to_selector || !widget_)
    return false;
  // Mouse-drag the center of `from` to the center of `to` (Puppeteer dragAndDrop):
  // press on from, glide through interpolated points (each move carries the held
  // button so e.buttons==1, and dragover/mousemove handlers see motion), release
  // on to. Drives mouse-based drag widgets (sliders, sortable lists, map panning);
  // it does NOT trigger HTML5 native drag-and-drop (dragstart/drop need a
  // DataTransfer). Both elements must be in the viewport. Returns true if both
  // matched and had a box.
  auto center = [&](const char* sel, int* cx, int* cy) -> bool {
    std::string js =
        "(function(){var e=document.querySelector(\"" + JsEscape(sel) +
        "\");if(!e)return '';var r=e.getBoundingClientRect();"
        "if(r.width<=0&&r.height<=0)return '';"
        "return Math.round(r.left+r.width/2)+','+Math.round(r.top+r.height/2);})()";
    std::string c = EvalToString(js.c_str());
    std::string::size_type comma = c.find(',');
    if (comma == std::string::npos)
      return false;
    *cx = std::atoi(c.substr(0, comma).c_str());
    *cy = std::atoi(c.substr(comma + 1).c_str());
    return true;
  };
  int fx = 0, fy = 0, tx = 0, ty = 0;
  if (!center(from_selector, &fx, &fy) || !center(to_selector, &tx, &ty))
    return false;
  SendMouseDown(fx, fy);
  constexpr int kSteps = 6;  // interpolate so drag handlers track the motion
  for (int i = 1; i <= kSteps; ++i) {
    SendMouseMove(fx + (tx - fx) * i / kSteps, fy + (ty - fy) * i / kSteps);
  }
  SendMouseUp(tx, ty);
  return true;
}

bool MbWebView::DragDropSelector(const char* from_selector,
                                 const char* to_selector) {
  if (!from_selector || !to_selector || !main_frame_)
    return false;
  // HTML5 NATIVE drag-and-drop (vs DragSelector's mouse-based drag): synthesize
  // the standard DragEvent sequence with ONE shared DataTransfer, so handlers
  // that setData() on dragstart and getData() on drop round-trip — the contract
  // for drag-to-upload, sortable lists, kanban boards, etc. that listen on
  // drag*/drop rather than mouse moves. dragover is dispatched (cancelable) so a
  // target that preventDefault()s it accepts the drop. Returns true if both
  // selectors matched. (Synthetic events are isTrusted=false; native gesture
  // synthesis would need the drag controller — this drives app-level handlers.)
  std::string js =
      "(function(){var s=document.querySelector(\"" + JsEscape(from_selector) +
      "\");var t=document.querySelector(\"" + JsEscape(to_selector) +
      "\");if(!s||!t)return '0';var dt=new DataTransfer();"
      "function f(el,ty){el.dispatchEvent(new DragEvent(ty,{bubbles:true,"
      "cancelable:true,dataTransfer:dt}));}"
      "f(s,'dragstart');f(t,'dragenter');f(t,'dragover');f(t,'drop');"
      "f(s,'dragend');return '1';})()";
  return EvalToString(js.c_str()) == "1";
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

bool MbWebView::GetViewSize(int* w, int* h) {
  // The current viewport size in logical (CSS) px — window.innerWidth/Height,
  // i.e. what was last set via mbCreateView/mbResize (DPR-independent). The
  // read-back peer of mbResize; distinct from GetContentSize (the full scrollable
  // document). Needs a committed document. Returns false if unavailable.
  std::string s = EvalToString(
      "(function(){return window.innerWidth+','+window.innerHeight;})()");
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
  return EvalToString(BuildFillJs(css_selector, text).c_str()) == "1";
}

bool MbWebView::FillSelectorInFrame(int frame_index,
                                    const char* css_selector,
                                    const char* text) {
  // Per-frame FillSelector: same React-compatible value-set + input/change
  // dispatch, but run host-privileged in the frame_index-th child frame's own
  // world (so it fills a form inside a cross-origin iframe the parent can't
  // reach). DOM-only, so no cross-frame coordinate mapping is needed. -1 = main.
  if (!css_selector)
    return false;
  return EvalInFrame(frame_index, BuildFillJs(css_selector, text).c_str()) ==
         "1";
}

bool MbWebView::SetFileForSelector(const char* css_selector,
                                   const char* paths_newline) {
  if (!css_selector || !main_frame_)
    return false;
  // A page CANNOT set an <input type=file>.files from script (security); only a
  // privileged host can. Reach the core HTMLInputElement and set its FileList.
  auto* impl = blink::To<blink::WebLocalFrameImpl>(main_frame_);
  blink::LocalFrame* frame = impl ? impl->GetFrame() : nullptr;
  if (!frame || !frame->GetDocument())
    return false;
  blink::Element* el =
      frame->GetDocument()->QuerySelector(blink::AtomicString(css_selector));
  auto* input = blink::DynamicTo<blink::HTMLInputElement>(el);
  if (!input ||
      input->FormControlType() != blink::mojom::blink::FormControlType::kInputFile)
    return false;
  // Build the FileList from newline-separated paths (a `multiple` input takes several).
  // Each file's bytes are read from disk and wrapped in an in-memory blob registered
  // with our BlobRegistry (the same path `new Blob([bytes])` uses, verified readable) —
  // a plain path-backed File would need a file-reading blob backend we don't have, so
  // its size/reads would be empty. This way .size, FileReader and form upload all work.
  blink::FileList* list = blink::MakeGarbageCollected<blink::FileList>();
  const std::string buf(paths_newline ? paths_newline : "");
  std::string::size_type start = 0;
  while (start <= buf.size()) {
    std::string::size_type nl = buf.find('\n', start);
    std::string one = buf.substr(
        start, nl == std::string::npos ? std::string::npos : nl - start);
    if (!one.empty() && one.back() == '\r')
      one.pop_back();
    if (!one.empty()) {
      std::string bytes;
      if (base::ReadFileToString(base::FilePath(one), &bytes)) {
        const std::string::size_type slash = one.find_last_of('/');
        const std::string base_name =
            slash == std::string::npos ? one : one.substr(slash + 1);
        auto blob_data = std::make_unique<blink::BlobData>();
        blob_data->AppendBytes(base::as_byte_span(bytes));
        blob_data->SetContentType("application/octet-stream");
        scoped_refptr<blink::BlobDataHandle> handle =
            blink::BlobDataHandle::Create(std::move(blob_data), bytes.size());
        list->Append(blink::MakeGarbageCollected<blink::File>(
            blink::String::FromUtf8(base_name), /*modification_time=*/std::nullopt,
            std::move(handle)));
      }
    }
    if (nl == std::string::npos)
      break;
    start = nl + 1;
  }
  if (list->length() == 0u)
    return false;
  input->setFiles(list);
  // setFiles updates .files but doesn't fire change (that's the chooser's job); fire it
  // so a page's change handler runs, matching a real file selection.
  EvalToString((std::string("(function(){var e=document.querySelector(\"") +
                JsEscape(css_selector) +
                "\");if(e)e.dispatchEvent(new Event('change',{bubbles:true}));})()")
                   .c_str());
  return true;
}

void MbWebView::SendMouseMove(int x, int y) {
  if (widget_)
    widget_->SendMouseMove(x, y);
}

void MbWebView::SendWheel(int x, int y, int delta_x, int delta_y, int modifiers) {
  if (!widget_)
    return;
  // 1. Deliver the trusted `wheel` event. 2. Unless a blocking listener consumed it
  // (preventDefault), apply the default scroll. The native compositor wheel->scroll
  // path is absent in this non-compositing widget, so we scroll the viewport
  // programmatically (same path as SendScroll) with the DOM-convention deltas
  // (deltaY>0 = down) — so mbSendWheel both fires the event AND scrolls, matching a
  // real browser (a passive/absent listener scrolls; preventDefault suppresses it).
  const bool consumed = widget_->SendWheel(x, y, delta_x, delta_y, modifiers);
  if (consumed || !main_frame_)
    return;
  auto* impl = blink::To<blink::WebLocalFrameImpl>(main_frame_);
  blink::LocalFrame* frame = impl->GetFrame();
  if (frame && frame->DomWindow())
    frame->DomWindow()->scrollByForTesting(static_cast<double>(delta_x),
                                           static_cast<double>(delta_y));
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

void MbWebView::EmulateMedia(const char* feature, const char* value) {
  // Generic media-feature emulation (the DevTools Emulation.setEmulatedMedia
  // path): override any media feature — prefers-reduced-motion, prefers-contrast,
  // forced-colors, color-gamut, prefers-reduced-data, etc. — so matchMedia() and
  // @media rules evaluate to the requested value live (Page::SetMediaFeatureOverride
  // re-runs the media queries). An empty `feature` clears ALL overrides; a set
  // `feature` with an empty `value` clears just that one.
  if (!web_view_ || !web_view_->GetPage())
    return;
  blink::Page* page = web_view_->GetPage();
  if (!feature || !*feature) {
    page->ClearMediaFeatureOverrides();
    return;
  }
  page->SetMediaFeatureOverride(blink::AtomicString(feature),
                                blink::String(value ? value : ""));
}

void MbWebView::EmulateMediaType(const char* media_type) {
  // Override the media TYPE (the DevTools Emulation.setEmulatedMedia `media`
  // param) — distinct from the media FEATURES above. "print" makes @media print
  // rules and matchMedia('print') apply while the page is still rendered to the
  // screen, so a screenshot/PDF reflects print styles; "screen" forces screen;
  // ""/NULL clears the override (back to the natural screen media). The
  // mediaTypeOverride setting is annotated invalidate:["MediaQuery"], so blink
  // re-evaluates all media queries on the change — no manual recalc needed.
  if (!web_view_ || !web_view_->GetPage())
    return;
  web_view_->GetPage()->GetSettings().SetMediaTypeOverride(
      blink::String(media_type ? media_type : ""));
}

std::string MbWebView::DrainConsole() {
  return frame_client_ ? frame_client_->DrainConsole() : std::string();
}

std::string MbWebView::GetCookies(const char* url) {
  return url ? MbGetCookiesForUrl(url) : std::string();
}

bool MbWebView::GetCookieValue(const char* url, const char* name,
                               std::string* out) {
  if (!url || !name || !*name)
    return false;
  // Pull one cookie's value out of the "n1=v1; n2=v2" jar string for `url` (the
  // common "read the session/auth cookie" check, without caller-side parsing).
  // Returns false if `name` isn't present; an empty value (name=) returns true.
  const std::string jar = MbGetCookiesForUrl(url);
  const std::string key(name);
  for (size_t i = 0; i < jar.size();) {
    size_t semi = jar.find("; ", i);
    const std::string tok =
        jar.substr(i, semi == std::string::npos ? std::string::npos : semi - i);
    const size_t eq = tok.find('=');
    if (eq != std::string::npos && tok.substr(0, eq) == key) {
      if (out)
        *out = tok.substr(eq + 1);
      return true;
    }
    if (semi == std::string::npos)
      break;
    i = semi + 2;
  }
  return false;
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

namespace {

// Append `s` to `out` as a JSON string body (no surrounding quotes), escaping the
// characters JSON requires. Keeps the snapshot machine-parseable.
void AppendJsonEscaped(const std::string& s, std::string* out) {
  for (char c : s) {
    switch (c) {
      case '"': *out += "\\\""; break;
      case '\\': *out += "\\\\"; break;
      case '\n': *out += "\\n"; break;
      case '\r': *out += "\\r"; break;
      case '\t': *out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          *out += buf;
        } else {
          *out += c;
        }
    }
  }
}

void SerializeAXNode(const blink::WebAXObject& obj, std::string* out);

// Append the included descendants of `obj` as comma-separated JSON objects. Nodes the
// AX tree marks "not included" (presentational/ignored wrappers) are flattened: we skip
// them but pull their included children up to this level, matching the platform tree.
void AppendIncludedChildren(const blink::WebAXObject& obj, std::string* out) {
  unsigned n = obj.ChildCount();
  for (unsigned i = 0; i < n; ++i) {
    blink::WebAXObject c = obj.ChildAt(i);
    if (c.IsNull() || c.IsDetached())
      continue;
    if (c.IsIncludedInTree()) {
      if (!out->empty() && out->back() != '[')
        *out += ',';
      SerializeAXNode(c, out);
    } else {
      AppendIncludedChildren(c, out);
    }
  }
}

// Serialize one included node: {"role","name"[,"value"][,"children":[...]]}.
void SerializeAXNode(const blink::WebAXObject& obj, std::string* out) {
  *out += "{\"role\":\"";
  AppendJsonEscaped(ui::ToString(obj.Role()), out);
  *out += "\",\"name\":\"";
  AppendJsonEscaped(obj.GetName().Utf8(), out);
  *out += '"';
  std::string value = obj.GetValueForControl().Utf8();
  if (!value.empty()) {
    *out += ",\"value\":\"";
    AppendJsonEscaped(value, out);
    *out += '"';
  }
  // Interactive STATE that automation needs but raw text can't convey: a checkbox/radio's
  // checked state, and which node holds focus. Emitted only when meaningful (a checkable
  // node / the focused node), so non-control nodes stay compact.
  switch (obj.CheckedState()) {
    case ax::mojom::CheckedState::kTrue:
      *out += ",\"checked\":true";
      break;
    case ax::mojom::CheckedState::kFalse:
      *out += ",\"checked\":false";
      break;
    case ax::mojom::CheckedState::kMixed:
      *out += ",\"checked\":\"mixed\"";
      break;
    case ax::mojom::CheckedState::kNone:
      break;  // not a checkable control — omit
  }
  if (obj.IsFocused())
    *out += ",\"focused\":true";
  // Frame-relative bounds (x,y,w,h), emitted only when the node has a non-empty box.
  // These are widget/page coordinates for the main frame, so an automation caller can
  // click a node's center via mbSendMouseClick — turning the snapshot into actions.
  gfx::Rect r = obj.GetBoundsInFrameCoordinates();
  if (r.width() > 0 && r.height() > 0) {
    char buf[80];
    std::snprintf(buf, sizeof(buf), ",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d", r.x(),
                  r.y(), r.width(), r.height());
    *out += buf;
  }
  // Recurse. Open the array first so AppendIncludedChildren's comma logic (which checks
  // for a trailing '[') works, then drop it again if no child was actually emitted.
  size_t before = out->size();
  *out += ",\"children\":[";
  size_t array_open = out->size();
  AppendIncludedChildren(obj, out);
  if (out->size() == array_open)
    out->resize(before);  // no children emitted — remove the empty "children" key
  else
    *out += ']';
  *out += '}';
}

}  // namespace

std::string MbWebView::GetAXTree() {
  if (!main_frame_)
    return std::string();
  blink::WebDocument doc = main_frame_->GetDocument();
  if (doc.IsNull())
    return std::string();
  // A live WebAXContext enables the AXObjectCache for `doc` for as long as it exists;
  // kWebContents builds the web-content accessibility tree (roles + names + values).
  blink::WebAXContext ax_context(doc, ui::AXMode(ui::AXMode::kWebContents));
  if (!ax_context.HasActiveDocument() || !ax_context.HasAXObjectCache())
    return std::string();
  ax_context.UpdateAXForAllDocuments();  // bring the tree up to date before walking
  blink::WebAXObject root = blink::WebAXObject::FromWebDocument(doc);
  if (root.IsNull() || root.IsDetached())
    return std::string();
  std::string out;
  SerializeAXNode(root, &out);
  return out;
}

int MbWebView::FindText(const char* text, bool match_case, bool forward,
                        bool* has_active) {
  if (has_active)
    *has_active = false;
  if (!text || !*text || !main_frame_)
    return 0;
  auto* impl = blink::To<blink::WebLocalFrameImpl>(main_frame_);
  if (!impl || !impl->GetFrame())
    return 0;
  // Bring style/layout current — scoping walks the laid-out text (the find tests do the
  // same before counting).
  if (widget_ && widget_->widget())
    widget_->widget()->UpdateAllLifecyclePhases(blink::DocumentUpdateReason::kFindInPage);
  blink::WebString needle = blink::WebString::FromUtf8(text);
  blink::TextFinder& finder = impl->EnsureTextFinder();
  auto options = blink::mojom::blink::FindOptions::New();
  options->match_case = match_case;
  options->forward = forward;
  options->new_session = true;
  options->run_synchronously_for_testing = true;  // count + select in this call
  const int id = ++find_id_;
  // Find() selects + scrolls to (and highlights) the active match.
  bool active = impl->GetFrame()->GetDocument()
                    ? finder.Find(id, needle, *options, /*wrap_within_frame=*/false,
                                  /*active_now=*/nullptr)
                    : false;
  // Scope the whole document to count every match (synchronous with the testing flag);
  // this also lays down the find-match markers (visible in a screenshot).
  finder.ResetMatchCount();
  finder.StartScopingStringMatches(id, needle, *options);
  // Remember the session so FindNext can step through these matches.
  find_text_ = text;
  find_match_case_ = match_case;
  if (has_active)
    *has_active = active;
  return finder.TotalMatchCount();
}

bool MbWebView::FindNext(bool forward) {
  if (find_text_.empty() || !main_frame_)
    return false;
  auto* impl = blink::To<blink::WebLocalFrameImpl>(main_frame_);
  if (!impl || !impl->GetFrame() || !impl->GetTextFinder())
    return false;
  if (widget_ && widget_->widget())
    widget_->widget()->UpdateAllLifecyclePhases(blink::DocumentUpdateReason::kFindInPage);
  auto options = blink::mojom::blink::FindOptions::New();
  options->match_case = find_match_case_;
  options->forward = forward;
  options->new_session = false;  // continue the session: advance from the active match
  options->run_synchronously_for_testing = true;
  // wrap_within_frame=true so stepping past the last/first match loops around.
  return impl->GetTextFinder()->Find(find_id_, blink::WebString::FromUtf8(find_text_),
                                     *options, /*wrap_within_frame=*/true,
                                     /*active_now=*/nullptr);
}

void MbWebView::StopFind() {
  find_text_.clear();  // end the session: a later FindNext returns false
  if (!main_frame_)
    return;
  auto* impl = blink::To<blink::WebLocalFrameImpl>(main_frame_);
  if (impl && impl->GetTextFinder())
    impl->GetTextFinder()->StopFindingAndClearSelection();  // clears matches + markers
}

bool MbWebView::GetTextForSelector(const char* css_selector, std::string* out) {
  if (!css_selector)
    return false;
  // innerText of the first match. We prefix a '1' flag on success so an element
  // whose text is genuinely "" is distinguishable from "no element matched" (JS
  // returns "" only in the no-match case). Strip the flag before returning.
  std::string s = EvalToString(BuildGetTextJs(css_selector).c_str());
  if (s.empty())
    return false;  // no element matched
  if (out)
    *out = s.substr(1);
  return true;
}

bool MbWebView::GetTextForSelectorInFrame(int frame_index,
                                          const char* css_selector,
                                          std::string* out) {
  // Per-frame GetTextForSelector: innerText of the first match in the
  // frame_index-th child frame's own world (reads a cross-origin iframe's text).
  // Same '1'-flag sentinel so an element whose text is genuinely "" is
  // distinguishable from no-match. -1 = main frame.
  if (!css_selector)
    return false;
  std::string s = EvalInFrame(frame_index, BuildGetTextJs(css_selector).c_str());
  if (s.empty())
    return false;  // no element matched (or out-of-range / remote frame)
  if (out)
    *out = s.substr(1);
  return true;
}

bool MbWebView::GetAllTextForSelector(const char* css_selector,
                                      std::string* out) {
  if (!css_selector)
    return false;
  // innerText of EVERY match, as a JSON array string (so embedded commas /
  // newlines / quotes survive intact). One call replaces the count-then-
  // :nth-of-type loop for list scraping. An invalid selector throws -> "" ->
  // false; zero matches is the valid "[]". JSON.stringify handles the escaping.
  std::string js =
      "(function(){try{var ns=document.querySelectorAll(\"" +
      JsEscape(css_selector) +
      "\");var a=[];for(var i=0;i<ns.length;i++)a.push(ns[i].innerText);"
      "return JSON.stringify(a);}catch(e){return '';}})()";
  std::string s = EvalToString(js.c_str());
  if (s.empty())
    return false;  // invalid selector (querySelectorAll threw)
  if (out)
    *out = s;
  return true;
}

bool MbWebView::GetAllValueForSelector(const char* css_selector,
                                       std::string* out) {
  if (!css_selector)
    return false;
  // The live .value of EVERY match, as a JSON array string — serialize a whole
  // form's current state in one call (vs GetAllAttribute's static "value"
  // attribute). A match with no value property contributes JSON null. Invalid
  // selector throws -> "" -> false; zero matches is the valid "[]".
  std::string js =
      "(function(){try{var ns=document.querySelectorAll(\"" +
      JsEscape(css_selector) +
      "\");var a=[];for(var i=0;i<ns.length;i++){var v=ns[i].value;"
      "a.push(v===undefined?null:v);}return JSON.stringify(a);}"
      "catch(e){return '';}})()";
  std::string s = EvalToString(js.c_str());
  if (s.empty())
    return false;  // invalid selector
  if (out)
    *out = s;
  return true;
}

bool MbWebView::GetAllAttributeForSelector(const char* css_selector,
                                           const char* attr, std::string* out) {
  if (!css_selector || !attr)
    return false;
  // getAttribute(attr) of EVERY match, as a JSON array string. An element whose
  // attribute is absent contributes JSON null (preserving index alignment with
  // GetAllTextForSelector). Raw attribute value (getAttribute), not the resolved
  // property — so href stays "/path", not the absolutized URL. Invalid selector
  // throws -> "" -> false; zero matches is the valid "[]".
  std::string js =
      "(function(){try{var ns=document.querySelectorAll(\"" +
      JsEscape(css_selector) + "\");var a=[];for(var i=0;i<ns.length;i++)"
      "a.push(ns[i].getAttribute(\"" + JsEscape(attr) +
      "\"));return JSON.stringify(a);}catch(e){return '';}})()";
  std::string s = EvalToString(js.c_str());
  if (s.empty())
    return false;  // invalid selector
  if (out)
    *out = s;
  return true;
}

bool MbWebView::SetHtmlForSelector(const char* css_selector, const char* html) {
  if (!css_selector)
    return false;
  // Set the first match's innerHTML (replace its contents) — template or redact a
  // fragment before a capture. The write side of GetHtmlForSelector. A normal
  // page-context property assignment (eval), not a C++ v8 [[Set]] on the global,
  // so it's safe in this build. Returns true if an element matched.
  std::string js =
      "(function(){var e=document.querySelector(\"" + JsEscape(css_selector) +
      "\");if(!e)return '0';e.innerHTML=\"" + JsEscape(html ? html : "") +
      "\";return '1';})()";
  return EvalToString(js.c_str()) == "1";
}

bool MbWebView::GetHtmlForSelector(const char* css_selector, std::string* out) {
  if (!css_selector)
    return false;
  // outerHTML of the first match (the element + its markup) — extract a fragment
  // (article body, table, card) to re-parse, vs GetTextForSelector's plain text or
  // GetHTML's whole document. Same '1'-flag trick so an empty element is distinct
  // from no-match (though outerHTML always includes the tag, so it's never empty).
  std::string js =
      "(function(){var e=document.querySelector(\"" + JsEscape(css_selector) +
      "\");if(!e)return '';return '1'+e.outerHTML;})()";
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

bool MbWebView::InsertCSS(const char* css) {
  if (!css)
    return false;
  // Append a <style> with `css` to <head> (Puppeteer's addStyleTag) — restyle or
  // hide noise (cookie banners, ads) before a screenshot. Returns true on
  // success. A plain DOM append, no v8 [[Set]] trap.
  std::string js =
      "(function(){try{var s=document.createElement('style');s.textContent=\"" +
      JsEscape(css) +
      "\";(document.head||document.documentElement).appendChild(s);"
      "return '1';}catch(e){return '0';}})()";
  return EvalToString(js.c_str()) == "1";
}

bool MbWebView::GetLocalStorage(const char* key, std::string* out) {
  if (!key)
    return false;
  // localStorage.getItem(key) for the document's origin. The '1' flag separates a
  // genuinely empty stored value from absent/no-storage (getItem -> null, or a
  // SecurityError on an opaque origin like about:blank -> caught). Storage needs
  // a real origin: commit with an http(s) base URL.
  std::string js = "(function(){try{var x=localStorage.getItem(\"" +
                   JsEscape(key) + "\");if(x==null)return '';return '1'+x;}"
                   "catch(e){return '';}})()";
  std::string s = EvalToString(js.c_str());
  if (s.empty())
    return false;  // absent, or storage unavailable on this origin
  if (out)
    *out = s.substr(1);
  return true;
}

bool MbWebView::SetLocalStorage(const char* key, const char* value) {
  if (!key)
    return false;
  // localStorage.setItem(key, value) for the document's origin; false on a
  // SecurityError (opaque origin) or quota failure. Needs a real origin.
  std::string js = "(function(){try{localStorage.setItem(\"" + JsEscape(key) +
                   "\",\"" + JsEscape(value ? value : "") +
                   "\");return '1';}catch(e){return '0';}})()";
  return EvalToString(js.c_str()) == "1";
}

bool MbWebView::GetSessionStorage(const char* key, std::string* out) {
  if (!key)
    return false;
  // sessionStorage.getItem(key) — same '1'-flag and origin caveats as
  // GetLocalStorage, but the store is per-session (not persisted to disk).
  std::string js = "(function(){try{var x=sessionStorage.getItem(\"" +
                   JsEscape(key) + "\");if(x==null)return '';return '1'+x;}"
                   "catch(e){return '';}})()";
  std::string s = EvalToString(js.c_str());
  if (s.empty())
    return false;
  if (out)
    *out = s.substr(1);
  return true;
}

bool MbWebView::SetSessionStorage(const char* key, const char* value) {
  if (!key)
    return false;
  std::string js = "(function(){try{sessionStorage.setItem(\"" + JsEscape(key) +
                   "\",\"" + JsEscape(value ? value : "") +
                   "\");return '1';}catch(e){return '0';}})()";
  return EvalToString(js.c_str()) == "1";
}

void MbWebView::ClearStorage() {
  // Empty both Web Storage areas for the document's origin (localStorage +
  // sessionStorage) — reset state between scrapes, or a logout. Best-effort per
  // store (an opaque origin throws and is ignored). The cookie-jar peer is
  // mbClearCookies; together they reset a login session.
  EvalToString(
      "(function(){try{localStorage.clear();}catch(e){}"
      "try{sessionStorage.clear();}catch(e){}})()");
}

std::string MbWebView::SaveLocalStorage() {
  // Snapshot the WHOLE localStorage for the document's origin as a JSON object
  // {key:value,...} — the embedder writes it to disk and reloads it next run, so a
  // session (auth token, app state) survives a process restart (the cookie-jar peer is
  // mbSaveCookies). "{}" on an opaque origin / no storage.
  std::string s = EvalToString(
      "(function(){try{var o={};for(var i=0;i<localStorage.length;i++){"
      "var k=localStorage.key(i);o[k]=localStorage.getItem(k);}"
      "return JSON.stringify(o);}catch(e){return '{}';}})()");
  return s.empty() ? std::string("{}") : s;
}

void MbWebView::LoadLocalStorage(const char* json) {
  // Restore a SaveLocalStorage() snapshot: JSON.parse it (passed as a JS string literal
  // so arbitrary content is safe) and setItem each key. Merges into the current store.
  if (!json || !*json)
    return;
  std::string js =
      "(function(){try{var o=JSON.parse(\"" + JsEscape(json) +
      "\");for(var k in o){if(Object.prototype.hasOwnProperty.call(o,k))"
      "localStorage.setItem(k,String(o[k]));}}catch(e){}})()";
  EvalToString(js.c_str());
}

bool MbWebView::SetAttribute(const char* css_selector, const char* attr,
                             const char* value) {
  if (!css_selector || !attr)
    return false;
  // setAttribute(attr, value) on the first match; the flag string reports whether
  // an element matched (1) or not (0). value defaults to "" when null (a bare
  // boolean attribute like 'disabled'). The write is a plain method call — no v8
  // [[Set]] trap — so it's safe in this build.
  std::string js =
      "(function(){var e=document.querySelector(\"" + JsEscape(css_selector) +
      "\");if(!e)return '0';e.setAttribute(\"" + JsEscape(attr) + "\",\"" +
      JsEscape(value ? value : "") + "\");return '1';})()";
  return EvalToString(js.c_str()) == "1";
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
  // Notify on every main-frame commit (host load, page navigation, redirect, reload) —
  // the "URL changed" signal, before the history bookkeeping below.
  if (on_url_changed_ && !url.empty())
    on_url_changed_(url);
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

void MbWebView::OnDidFinishLoad() {
  load_finished_ = true;
  if (on_load_finish_)
    on_load_finish_();
}

void MbWebView::SetLoadFinishCallback(std::function<void()> cb) {
  on_load_finish_ = std::move(cb);
}

void MbWebView::OnConsoleMessage(const std::string& level,
                                 const std::string& message) {
  if (on_console_)
    on_console_(level, message);
}

void MbWebView::SetConsoleCallback(ConsoleFn cb) {
  on_console_ = std::move(cb);
}

bool MbWebView::OnBeginNavigation(const std::string& url) {
  return on_navigation_ ? on_navigation_(url) != 0 : true;
}

void MbWebView::SetNavigationCallback(NavigationFn cb) {
  on_navigation_ = std::move(cb);
}

void MbWebView::SetUrlChangedCallback(UrlChangedFn cb) {
  on_url_changed_ = std::move(cb);
}

void MbWebView::SetTitleChangedCallback(TitleChangedFn cb) {
  on_title_changed_ = std::move(cb);
}

void MbWebView::OnTitleChanged(const std::string& title) {
  if (on_title_changed_)
    on_title_changed_(title);
}

void MbWebView::SetFaviconChangedCallback(FaviconChangedFn cb) {
  on_favicon_changed_ = std::move(cb);
}

void MbWebView::OnFaviconChanged(const std::string& favicon_urls) {
  if (on_favicon_changed_)
    on_favicon_changed_(favicon_urls);
}

void MbWebView::SetDownloadCallback(DownloadFn cb) {
  on_download_ = std::move(cb);
}

void MbWebView::OnPageDownload(const std::string& url,
                               const std::string& suggested_name,
                               const std::string& body) {
  // A page-initiated blob download (createObjectURL + <a download>) resolved by
  // MbLocalFrameHost. Surface it through the same callback as a server-driven
  // download. We don't carry the blob's MIME (DownloadURLParams omits it), so
  // report a generic type — the suggested filename's extension is the real hint.
  if (on_download_)
    on_download_(url, "application/octet-stream",
                 DownloadFilenameFor(url, suggested_name), body);
}

void MbWebView::OnPageDownloadFetch(const std::string& url,
                                    const std::string& suggested_name) {
  // A page-initiated download of a data: or http(s) URL (a download-attributed
  // link: <a download href="...">, or saving a data: URL). Unlike a blob: URL,
  // the bytes aren't in the in-process blob store — fetch them through the engine
  // (which decodes data: and fetches http(s), honoring the interception layer and
  // the view's cookies/headers) and surface them through the same callback.
  if (!on_download_)
    return;
  std::string body, content_type;
  if (!FetchDownloadBody(url, &body, &content_type))
    return;
  on_download_(url, content_type, DownloadFilenameFor(url, suggested_name),
               body);
}

void MbWebView::OnCreateNewWindow(const std::string& url,
                                  const std::string& name) {
  if (on_new_window_)
    on_new_window_(url, name);
}

void MbWebView::SetNewWindowCallback(NewWindowFn cb) {
  on_new_window_ = std::move(cb);
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

void MbWebView::EmulateDevice(int width, int height, float device_scale_factor,
                             bool mobile) {
  if (!web_view_)
    return;
  if (blink::WebSettings* s = web_view_->GetSettings()) {
    namespace m = blink::mojom;
    s->SetPrimaryPointerType(mobile ? m::PointerType::kPointerCoarseType
                                    : m::PointerType::kPointerFineType);
    s->SetAvailablePointerTypes(static_cast<int>(
        mobile ? m::PointerType::kPointerCoarseType
               : m::PointerType::kPointerFineType));
    s->SetPrimaryHoverType(mobile ? m::HoverType::kHoverNone
                                  : m::HoverType::kHoverHoverType);
    s->SetAvailableHoverTypes(static_cast<int>(
        mobile ? m::HoverType::kHoverNone : m::HoverType::kHoverHoverType));
    s->SetViewportEnabled(mobile);
    s->SetViewportMetaEnabled(mobile);
    s->SetViewportStyle(mobile ? m::ViewportStyle::kMobile
                               : m::ViewportStyle::kDefault);
    s->SetShrinksViewportContentToFit(mobile);
    s->SetMainFrameResizesAreOrientationChanges(mobile);
  }
  if (width > 0 && height > 0)
    Resize(width, height);
  // SetDeviceScaleFactor also nudges media queries (covers the pointer/hover change).
  SetDeviceScaleFactor(device_scale_factor > 0.0f ? device_scale_factor : dsf_);
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

void MbWebView::SendKeyUp(int windows_key_code) {
  if (widget_)
    widget_->SendKeyUp(windows_key_code);
}

void MbWebView::SendIme(const char* composing, const char* committed) {
  if (widget_)
    widget_->SendIme(composing, committed);
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

int MbWebView::ScrollToBottom(int max_steps) {
  if (!main_frame_)
    return 0;
  if (max_steps <= 0)
    max_steps = 20;  // sane default cap for unbounded infinite-scroll pages
  // Repeatedly scroll to the current bottom and settle, so IntersectionObserver /
  // lazy-load handlers append more content; stop when the page stops growing (all
  // content loaded) or max_steps is hit. WaitMs drives the lifecycle including
  // ForceUpdateViewportIntersections, so observers fire between scrolls. Returns
  // the number of steps that grew the page (0 = a static page that never grew).
  std::string last_h = EvalToString("''+document.body.scrollHeight");
  int grew = 0;
  for (int i = 0; i < max_steps; ++i) {
    EvalToString("(window.scrollTo(0,document.body.scrollHeight),'')");
    WaitMs(80);  // let lazy content append + lay out (same settle as the IO test)
    std::string h = EvalToString("''+document.body.scrollHeight");
    if (h == last_h)
      break;  // no new content appeared -> reached the end
    last_h = h;
    ++grew;
  }
  return grew;
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

void MbSetOnline(bool online) {
  // Process-global: flips navigator.onLine and dispatches the window online/
  // offline events on every frame observing the NetworkStateNotifier.
  blink::GetNetworkStateNotifier().SetOnLine(online);
}

void MbWebView::SetVisible(bool visible) {
  // Drive the page's visibility so an app can simulate tab backgrounding:
  // toggles document.visibilityState / document.hidden and fires the
  // visibilitychange event, which lets pages pause timers, video, polling, rAF,
  // etc. is_initial_state=false so blink dispatches the event (true would set the
  // state silently, for the very first commit).
  if (web_view_) {
    web_view_->SetVisibilityState(
        visible ? blink::mojom::PageVisibilityState::kVisible
                : blink::mojom::PageVisibilityState::kHidden,
        /*is_initial_state=*/false);
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

// The __mbDlg native bridge: the injected window.alert/confirm/prompt override calls
// this with (typeName, message, default). It routes to the view's dialog handler and
// returns a typed JS value — undefined (alert), boolean (confirm), or string|null
// (prompt) — via *out_type. userdata is the owning MbWebView*.
const char* MbDialogBridge(void* userdata, int argc, const char** argv,
                           const int* /*argtypes*/, int* out_type) {
  thread_local std::string* result = new std::string();  // -Wexit-time-destructors
  result->clear();
  auto* view = static_cast<MbWebView*>(userdata);
  if (!view || argc < 2 || !argv) {
    *out_type = 4;  // undefined
    return "";
  }
  int kind = 0;  // 0 alert, 1 confirm, 2 prompt
  if (std::strcmp(argv[0], "confirm") == 0)
    kind = 1;
  else if (std::strcmp(argv[0], "prompt") == 0)
    kind = 2;
  const char* message = argv[1] ? argv[1] : "";
  const char* def = (argc >= 3 && argv[2]) ? argv[2] : "";
  char buf[8192] = {0};
  const int accept = view->HandleJsDialog(kind, message, def, buf, sizeof(buf));
  if (kind == 0) {            // alert -> undefined
    *out_type = 4;
    return "";
  }
  if (kind == 1) {           // confirm -> boolean
    *out_type = 2;
    return accept ? "true" : "false";
  }
  if (!accept) {             // prompt cancel -> null
    *out_type = 3;
    return "";
  }
  *out_type = 0;             // prompt accept -> the entered text
  result->assign(buf);
  return result->c_str();
}
}  // namespace

int MbWebView::HandleJsDialog(int type, const char* message,
                              const char* default_value, char* out, int out_cap) {
  if (dialog_cb_) {
    return dialog_cb_(type, message ? message : "",
                      default_value ? default_value : "", out, out_cap,
                      dialog_userdata_);
  }
  // No callback: headless-safe defaults — alert "shows" (accept), confirm/prompt
  // dismiss (false / null). Matches the suppressed-dialog behavior.
  return type == 0 ? 1 : 0;
}

void MbWebView::SetJsDialogCallback(JsDialogFn fn, void* userdata) {
  dialog_cb_ = fn;
  dialog_userdata_ = userdata;
}

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
  // Called at document-element-available (before the page's own scripts). Install
  // bound native functions (incl. the internal __mbDlg dialog bridge), override the
  // JS dialog functions to route through it, then run the host init script — all
  // before the page's own scripts.
  if (!main_frame_)
    return;
  // Ensure the internal dialog bridge is registered (once) so it installs with the
  // other bindings. Reserved name; userdata is this view.
  if (!dialog_registered_) {
    auto b = std::make_unique<NativeBinding>();
    b->name = "__mbDlg";
    b->fn = &MbDialogBridge;
    b->userdata = this;
    js_bindings_.push_back(std::move(b));
    dialog_registered_ = true;
  }
  InstallJsBindings();
  // Replace window.alert/confirm/prompt with shims that call the bridge. The bridge
  // returns a properly typed value (undefined / boolean / string|null), so confirm()
  // and prompt() behave correctly. No browser, no modal, no main-thread block.
  main_frame_->ExecuteScript(blink::WebScriptSource(blink::WebString::FromUtf8(
      "(function(){var B=window.__mbDlg;if(!B)return;"
      "window.alert=function(m){B('alert',m===undefined?'':String(m),'');};"
      "window.confirm=function(m){return B('confirm',m===undefined?'':String(m),'');};"
      "window.prompt=function(m,d){return B('prompt',m===undefined?'':String(m),"
      "d===undefined||d===null?'':String(d));};})()")));
  if (!init_script_.empty()) {
    main_frame_->ExecuteScript(
        blink::WebScriptSource(blink::WebString::FromUtf8(init_script_)));
  }
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

// Number of direct child frames (top-level iframes) of the main document.
int MbWebView::GetFrameCount() {
  if (!main_frame_)
    return 0;
  int n = 0;
  for (blink::WebFrame* c = main_frame_->FirstChild(); c; c = c->NextSibling())
    ++n;
  return n;
}

// Eval in the frame_index-th direct child frame (0-based, document order); -1 = the
// main frame. Runs in that frame's main world host-privileged, so it reads even a
// CROSS-ORIGIN iframe's content (the same-origin policy that blocks the parent's
// iframe.contentDocument does not bind the host's own eval). Returns "" if the
// index is out of range or the child is a remote frame (not used single-process).
std::string MbWebView::EvalInFrame(int frame_index, const char* utf8_script) {
  if (!main_frame_ || !utf8_script)
    return {};
  blink::WebLocalFrame* frame = main_frame_;
  if (frame_index >= 0) {
    blink::WebFrame* child = main_frame_->FirstChild();
    for (int i = 0; i < frame_index && child; ++i)
      child = child->NextSibling();
    if (!child || !child->IsWebLocalFrame())
      return {};
    frame = child->ToWebLocalFrame();
  }
  std::string result;
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

bool MbWebView::WaitForSelectorHidden(const char* css, int timeout_ms) {
  if (!main_frame_ || !css)
    return false;
  // The inverse of WaitForVisibleSelector: succeed once the first match is NOT
  // visible — either it never matches / was removed, OR it exists but is hidden
  // (display:none / visibility:hidden / opacity:0). The canonical "wait for the
  // loading spinner to disappear" before scraping. "1" = gone-or-hidden.
  const std::string probe =
      "(function(){var e=document.querySelector(\"" + JsEscape(css) +
      "\");if(!e)return '1';return (typeof e.checkVisibility==='function'?"
      "e.checkVisibility({checkOpacity:true,checkVisibilityCSS:true}):"
      "!!(e.offsetWidth||e.offsetHeight||e.getClientRects().length))?'0':'1';})()";
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

bool MbWebView::WaitForNetworkIdle(int idle_ms, int timeout_ms) {
  if (!main_frame_)
    return false;
  // Pump until no NEW subresource request has been recorded for `idle_ms`
  // (Puppeteer's networkidle) — for SPAs that lazy-fetch after the initial load.
  // Reads the process-wide request log's count; each new fetch resets the idle
  // window. Returns true once idle, false if `timeout_ms` elapses first. Clear the
  // log (mbClearRequestLog) before the navigation to scope it to this page.
  if (idle_ms <= 0)
    idle_ms = 500;
  size_t last = mb::MbRequestCount();
  const base::TimeTicks hard_deadline =
      base::TimeTicks::Now() + base::Milliseconds(timeout_ms > 0 ? timeout_ms : 0);
  base::TimeTicks idle_deadline =
      base::TimeTicks::Now() + base::Milliseconds(idle_ms);
  for (;;) {
    base::RunLoop().RunUntilIdle();
    ServiceAnimations();
    const base::TimeTicks now = base::TimeTicks::Now();
    const size_t cur = mb::MbRequestCount();
    if (cur != last) {  // activity -> restart the idle window
      last = cur;
      idle_deadline = now + base::Milliseconds(idle_ms);
    }
    if (now >= idle_deadline)
      return true;  // quiet long enough
    if (now >= hard_deadline)
      return false;  // still busy at the hard timeout
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
  // Capture with kOmitCompositingInfo (the screenshot/print path): this FLATTENS
  // composited layers (<video>, and any other cc_layer content) into the software paint
  // so their frames rasterize into the bitmap. The plain GetPaintRecord() omits them (a
  // composited <video> records only a foreign-layer placeholder we can't draw -> blank).
  blink::PaintRecordBuilder builder;
  frame->View()->PaintOutsideOfLifecycle(builder.Context(),
                                         blink::PaintFlag::kOmitCompositingInfo,
                                         blink::CullRect::Infinite());
  builder.EndRecording().Playback(&canvas);
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
  // US Letter, portrait, 100%, no margin (back-compat default).
  return SavePdfEx(path, /*width_pt=*/612.0, /*height_pt=*/792.0,
                   /*landscape=*/false, /*scale=*/1.0, /*margin_pt=*/0.0);
}

bool MbWebView::SavePdfEx(const char* path, double width_pt, double height_pt,
                          bool landscape, double scale, double margin_pt) {
  // !path: defense-in-depth — base::FilePath(nullptr) would be UB. The C-ABI rejects
  // null too; this guards direct host-side callers (matching EncodeBitmapToPath).
  if (!path || !main_frame_ || !widget_ || !widget_->widget())
    return false;
  // Sanitize the geometry (points; PDF user space is 72/in). Default to Letter.
  if (!(width_pt > 0))
    width_pt = 612.0;
  if (!(height_pt > 0))
    height_pt = 792.0;
  if (landscape)
    std::swap(width_pt, height_pt);
  if (!(scale > 0))
    scale = 1.0;
  scale = std::clamp(scale, 0.1, 5.0);
  if (!(margin_pt >= 0))
    margin_pt = 0.0;
  // Keep a positive content area after margins.
  margin_pt = std::min(margin_pt, std::min(width_pt, height_pt) / 2.0 - 1.0);
  if (!(margin_pt >= 0))
    margin_pt = 0.0;
  const double content_w_pt = width_pt - 2.0 * margin_pt;
  const double content_h_pt = height_pt - 2.0 * margin_pt;

  // Settle the document, then drive Blink's print path into a Skia PDF document.
  for (int round = 0; round < 5; ++round) {
    widget_->widget()->UpdateAllLifecyclePhases(blink::DocumentUpdateReason::kTest);
    base::RunLoop().RunUntilIdle();
  }
  // Content lays out in CSS px (96/in); paint scale CSS px -> points is (72/96)*scale.
  // So the printable content area in CSS px is content_pt / paint_scale.
  const float paint_scale = static_cast<float>(72.0 / 96.0 * scale);
  const float css_w = static_cast<float>(content_w_pt) / paint_scale;
  const float css_h = static_cast<float>(content_h_pt) / paint_scale;
  // This ctor sets printable area + page description AND print_scaling_option =
  // kSourceSize, which the paginated layout requires (a DCHECK enforces it).
  blink::WebPrintParams params{gfx::SizeF(css_w, css_h)};
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
    SkCanvas* canvas = doc->beginPage(static_cast<float>(width_pt),
                                      static_cast<float>(height_pt));
    canvas->translate(static_cast<float>(margin_pt),
                      static_cast<float>(margin_pt));  // honor the margin
    canvas->scale(paint_scale, paint_scale);  // CSS-px content -> points
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

bool MbWebView::SaveElementPng(const char* css_selector, const char* path) {
  if (!css_selector || !path)
    return false;
  // Screenshot just the first match (Puppeteer's elementHandle.screenshot). Scroll
  // it into view, read its viewport-relative box, and clip that region — no
  // destructive resize, so the view is left as-is (bar the scroll). An element
  // taller/wider than the viewport is captured to the visible extent. Returns
  // false on no match or no box (display:none / zero-size).
  ScrollIntoView(css_selector);
  std::string js =
      "(function(){var e=document.querySelector(\"" + JsEscape(css_selector) +
      "\");if(!e)return '';var r=e.getBoundingClientRect();"
      "if(r.width<=0||r.height<=0)return '';"
      "return Math.round(Math.max(0,r.left))+','+Math.round(Math.max(0,r.top))+"
      "','+Math.round(r.width)+','+Math.round(r.height);})()";
  std::string box = EvalToString(js.c_str());
  int x = 0, y = 0, w = 0, h = 0;
  if (std::sscanf(box.c_str(), "%d,%d,%d,%d", &x, &y, &w, &h) != 4 || w <= 0 ||
      h <= 0)
    return false;
  return SavePngRect(path, x, y, w, h);
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
