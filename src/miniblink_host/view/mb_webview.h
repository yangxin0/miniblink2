// mb_webview — orchestrator that owns one WebView + main LocalFrame + widget.
//
// Replicates the WebViewHelper handshake (vendor/reference/frame_test_helpers.cc):
//   1. WebView::Create(view_client, ... PageBroadcast=NullAssociatedReceiver,
//                      agent_group_scheduler, browsing_context_group_token, ...) (:778)
//   2. WebLocalFrame::CreateMainFrame(web_view, frame_client, /*broker=*/NullRemote,
//                      tokens, /*policy_container=*/nullptr, ...) (:489)
//   3. create MbWidget, InitializeCompositing
//   4. web_view->DidAttachLocalMainFrame()
// All browser-side handles are null/default — no browser process.
//
// This is what the C ABI mbView wraps 1:1.
// Status: Phase 1 scaffold.

#ifndef MINIBLINK_HOST_VIEW_MB_WEBVIEW_H_
#define MINIBLINK_HOST_VIEW_MB_WEBVIEW_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"

class SkCanvas;  // global scope (skia is not namespaced)

namespace blink {
class WebViewImpl;
class WebLocalFrame;
namespace scheduler {
class WebAgentGroupScheduler;
}
}  // namespace blink

namespace mb {

class MbViewClient;     // blink::WebViewClient (minimal)
class MbFrameClient;    // blink::WebLocalFrameClient
class MbWidget;

class MbWebView {
 public:
  static std::unique_ptr<MbWebView> Create(int width, int height);
  ~MbWebView();

  void Resize(int width, int height);
  void LoadHTML(const char* utf8_html, const char* base_url);  // no network
  void LoadURL(const char* utf8_url);                          // via libcurl factory
  // Fetch `url` through the engine network stack and write the body to `dest_path`
  // (a real download — not committed as a document). Honors the interception layer
  // (rewrite / block / mock / request+response hooks) and, for http(s), the view's
  // UA + headers + cookies + proxy. Returns false on fetch failure or a write error.
  bool DownloadURL(const char* url, const char* dest_path);
  // POST `body` (with `content_type`, default form-urlencoded) to an http(s) URL
  // and commit the response as the document — host-driven POST navigation.
  // `body_len` is the exact byte length, so binary bodies with embedded NULs post
  // intact (don't strlen-truncate utf8_body).
  void PostURL(const char* utf8_url, const char* utf8_body, size_t body_len,
               const char* content_type);
  void RunJS(const char* utf8_script);  // execute JS in the main frame
  // Set a script to run on every new document BEFORE its own scripts (init/inject).
  void SetInitScript(const char* utf8_script);
  // Bind a native C function callable from JS as window[name](...). JS args are
  // coerced to UTF-8 strings (argv[i]); argtypes[i] reports each arg's JS type
  // (0=string,1=number,2=boolean,3=null,4=undefined,5=object,6=array,7=function).
  // The function returns a UTF-8 string and may set *out_type to choose the JS
  // type of the return value (0=string default, 1=number, 2=boolean, 3=null,
  // 4=undefined, 5=json — the string is JSON.parse'd into an object/array/value).
  // Synchronous (JS gets the return value inline). Installed into each new
  // document's main world. `userdata` is passed to the callback.
  using MbJsNativeFn = const char* (*)(void* userdata, int argc,
                                       const char** argv, const int* argtypes,
                                       int* out_type);
  void BindJsFunction(const char* name, MbJsNativeFn fn, void* userdata);
  // A bound native function (public so the install trampoline can read it).
  struct NativeBinding {
    std::string name;
    MbJsNativeFn fn = nullptr;
    void* userdata = nullptr;
  };
  // Called by the frame client at document-element-available; runs the init
  // script and installs bound native functions.
  void RunDocumentStartScript();

  // JS dialog handling (alert/confirm/prompt). Without a callback the defaults are
  // headless-safe (alert no-op, confirm=false, prompt=null). A registered callback is
  // invoked synchronously for each dialog with the type (0=alert,1=confirm,2=prompt),
  // message and prompt default; it returns accept(1)/dismiss(0) and, for prompt-accept,
  // writes the entered text into out_value. Handled via a JS-level override of
  // window.alert/confirm/prompt installed before page scripts (no browser, no modal).
  using JsDialogFn = int (*)(int type, const char* message,
                             const char* default_value, char* out_value,
                             int out_cap, void* userdata);
  void SetJsDialogCallback(JsDialogFn fn, void* userdata);
  // Bridge entry (called by the injected override via the __mbDlg native binding):
  // returns accept(1)/dismiss(0); fills `out` with the prompt text when accepting.
  int HandleJsDialog(int type, const char* message, const char* default_value,
                     char* out, int out_cap);

  void SendMouseClick(int x, int y);
  // Press / release the left button at (x,y). Down then move(s) then up performs a
  // drag (intermediate SendMouseMove calls carry the held button). Down+Up at one
  // point equals a click.
  void SendMouseDown(int x, int y);
  void SendMouseUp(int x, int y);
  // Single-finger touch tap (touchstart+touchend) at (x,y) — fires touch-only
  // handlers (mobile menus, tap targets) with a populated touches[0].
  void SendTouchTap(int x, int y);
  // One-finger swipe from (x1,y1) to (x2,y2): touchstart + interpolated touchmoves
  // + touchend — touch scroll / swipe gestures (carousels, pull-to-refresh).
  void SendTouchSwipe(int x1, int y1, int x2, int y2);
  // Click the center of the first element matching `css_selector`. Resolves the
  // element's bounding box in the page, then synthesizes a click there. Returns
  // false if the selector matches nothing or the element has no box (display:none
  // / zero-size). Puppeteer-style page.click(selector) — saves callers computing
  // coordinates by hand.
  bool ClickSelector(const char* css_selector);
  // Mouse-drag the center of `from_selector` to the center of `to_selector`
  // (press, interpolated moves carrying the held button, release) — slider/
  // sortable/map-pan widgets. Not HTML5 native DnD. Both must be in view. Returns
  // true if both matched.
  bool DragSelector(const char* from_selector, const char* to_selector);
  // Dispatch a bubbling, cancelable DOM Event of `type` on the first match —
  // trigger handlers click/fill don't (mouseover/focus/blur/submit/custom events).
  // Returns true if an element matched.
  bool DispatchEvent(const char* css_selector, const char* type);
  // Move the pointer to the first match's center (mousemove + mouseover + :hover).
  bool HoverSelector(const char* css_selector);
  // Double-click the first match's center (fires dblclick).
  bool DoubleClickSelector(const char* css_selector);
  // Right-click the first match's center (fires contextmenu).
  bool RightClickSelector(const char* css_selector);
  // Focus / blur the first match (HTMLElement.focus()/blur()). Blur commonly
  // triggers form-field validation.
  bool FocusSelector(const char* css_selector);
  bool BlurSelector(const char* css_selector);
  // Scroll the first match to the viewport center (Element.scrollIntoView). The
  // coordinate-based interactions below (click/hover) call this first so an
  // element below the fold gets an on-screen box to target; also independently
  // useful to trigger lazy-loading or to frame an element for a screenshot.
  bool ScrollIntoView(const char* css_selector);
  // Full scrollable document size (logical px), >= viewport — for full-page
  // capture (resize to this height, then paint).
  bool GetContentSize(int* w, int* h);
  // Current viewport size in logical px (window.innerWidth/Height) — the read-back
  // peer of Resize; distinct from GetContentSize (full scrollable document).
  bool GetViewSize(int* w, int* h);
  // First match's viewport-relative bounding box (logical px) via out-params;
  // false if no element matches. Composes with PaintRectToBitmap (element shot).
  bool GetElementRect(const char* css_selector, int* x, int* y, int* w, int* h);
  // Fill the first <input>/<textarea> matching `css_selector` with `text`:
  // focuses it, sets the value through the native value setter (so frameworks
  // like React observe it), and fires input+change. Playwright-style fill().
  // Returns false if the selector matches nothing.
  bool FillSelector(const char* css_selector, const char* text);
  // Set an <input type=file>'s selected files from disk paths (newline-separated for a
  // `multiple` input) — the privileged host op a page's script can't do — and fire
  // change, so a form submit then uploads the file(s). The bytes are read from disk into
  // an in-memory blob so size/FileReader/upload work. False if the selector doesn't match
  // a file input or no valid path was given.
  bool SetFileForSelector(const char* css_selector, const char* paths_newline);
  // Select a <select> option by value or visible text, firing input+change.
  bool SelectOption(const char* css_selector, const char* value);
  void SendMouseMove(int x, int y);
  void SendText(const char* utf8);
  void SendKey(const char* key_name);  // press a named non-text key (Enter, Tab, ...)
  void SendKeyUp(int windows_key_code);  // standalone key release (fires `keyup`)
  // Set the device pixel ratio (HiDPI). The page lays out in CSS px but reports
  // window.devicePixelRatio == scale and rasterizes at `scale`x in PaintInto, so
  // captures are retina-crisp. Caller sizes the output bitmap to logical*scale.
  void SetDeviceScaleFactor(float scale);
  // Override the User-Agent for navigator.userAgent and outgoing requests. Set
  // before LoadURL/LoadHTML to take effect for that navigation.
  void SetUserAgent(const char* utf8_ua);
  // The effective User-Agent (the override if set, else the built-in default) —
  // exactly what navigator.userAgent and outgoing requests carry.
  std::string GetUserAgent();
  // Enable/disable automatic image loading (off = faster text/HTML scraping).
  void SetLoadImages(bool enabled);
  // Emulate prefers-color-scheme: dark (true) or light (false). Set before loading.
  void SetDarkMode(bool dark);
  // Set navigator.language(s) (comma-separated, e.g. "fr-FR,fr,en"). Before loading.
  void SetLocale(const char* langs);
  // Override the timezone for Date/Intl (e.g. "America/New_York"). Process-global.
  void SetTimezone(const char* tz);
  // Set the view's window-focus state (page active + focused). The view starts
  // focused; clearing it makes document.hasFocus() false and blurs the active
  // element, like a real window losing focus.
  void SetFocus(bool focused);
  // Return captured console output ("level: text" per line) and clear the buffer.
  std::string DrainConsole();
  // Return the HTTP cookie jar's cookies for `url` ("name=value; ..."), for session export.
  std::string GetCookies(const char* url);
  // The value of a single cookie `name` for `url` (vs GetCookies' whole jar
  // string) — e.g. read the session/auth cookie. False if `name` is absent.
  bool GetCookieValue(const char* url, const char* name, std::string* out);
  // Return the WHOLE jar (every host, session + persistent) as a Netscape cookie
  // file, in memory — for full session export without a temp file.
  std::string GetAllCookies();
  // Inject a cookie ("name=value[; attrs]") into the jar for `url`'s origin
  // (session restore), and erase the whole jar (session reset).
  void SetCookie(const char* url, const char* cookie);
  void ClearCookies();
  // The committed main document's URL (final URL after redirects) and title,
  // read from the frame — no JS needed. Empty before the first load.
  std::string GetURL();
  std::string GetTitle();
  // Scraping reads: visible text (body.innerText) and the post-JS serialized DOM.
  std::string GetText();
  std::string GetHTML();
  // Per-element scraping by selector. Fill *out with the first match's innerText
  // / the named attribute's value and return true; return false if no element
  // matches (GetAttribute also returns false if the attribute is absent). *out is
  // only written on success.
  bool GetTextForSelector(const char* css_selector, std::string* out);
  // innerText of EVERY element matching `css_selector`, as a JSON array string
  // (one call for list scraping, vs count + :nth-of-type loop). Returns false on
  // an invalid selector; zero matches yields the valid "[]".
  bool GetAllTextForSelector(const char* css_selector, std::string* out);
  // outerHTML of the first match (element + its markup) — extract a fragment to
  // re-parse, vs GetTextForSelector (plain text) or GetHTML (whole document).
  // Returns false if no element matches.
  bool GetHtmlForSelector(const char* css_selector, std::string* out);
  // Set the first match's innerHTML (replace contents) — template or redact a
  // fragment before a capture. The write side of GetHtmlForSelector. Returns true
  // if an element matched.
  bool SetHtmlForSelector(const char* css_selector, const char* html);
  // getAttribute(attr) of EVERY match as a JSON array string (absent attr ->
  // null, preserving index alignment); raw value, not the resolved property.
  // Returns false on an invalid selector; "[]" for zero matches.
  bool GetAllAttributeForSelector(const char* css_selector, const char* attr,
                                  std::string* out);
  // The live .value of EVERY match as a JSON array string (serialize a form's
  // current state; absent value -> null). False on an invalid selector.
  bool GetAllValueForSelector(const char* css_selector, std::string* out);
  bool GetAttribute(const char* css_selector, const char* attr, std::string* out);
  // setAttribute(attr, value) on the first match (value="" for a bare boolean
  // attribute like 'disabled'). Returns true if an element matched. The write
  // side of GetAttribute — sets the static HTML attribute, not the .value
  // property (use FillSelector for a control's value).
  bool SetAttribute(const char* css_selector, const char* attr,
                    const char* value);
  // Append a <style> with `css` to the document head (Puppeteer addStyleTag) —
  // restyle or hide elements (cookie banners, ads) before a capture. Returns
  // true on success.
  bool InsertCSS(const char* css);
  // localStorage access for the document's origin (inject an auth token, read
  // SPA state). Get returns false if the key is absent or storage is unavailable
  // (opaque origin); Set returns false on a SecurityError/quota failure. Needs a
  // real origin — commit with an http(s) base URL, not about:blank.
  bool GetLocalStorage(const char* key, std::string* out);
  bool SetLocalStorage(const char* key, const char* value);
  // sessionStorage peer of the above — same semantics, but per-session (not
  // persisted). Get false if absent/unavailable; Set false on error.
  bool GetSessionStorage(const char* key, std::string* out);
  bool SetSessionStorage(const char* key, const char* value);
  // Empty both Web Storage areas (localStorage + sessionStorage) for the
  // document's origin — reset state between scrapes / a logout. Best-effort.
  void ClearStorage();
  // The first match's LIVE .value property (what an <input>/<textarea>/<select>
  // currently holds after typing or selection) — distinct from GetAttribute,
  // which reads the static "value" HTML attribute (the initial value). Returns
  // false if no element matches or the element has no value property.
  bool GetValueForSelector(const char* css_selector, std::string* out);
  // The first match's .checked state: 1 checked, 0 unchecked, -1 if no element
  // matches or it isn't a checkable control (checkbox/radio).
  int GetCheckedForSelector(const char* css_selector);
  // The first match's actual visibility: 1 visible, 0 hidden (display:none,
  // visibility:hidden, content-visibility, or opacity:0), -1 if no element
  // matches. Existence (querySelector) is not visibility — use this for "is it
  // really shown", e.g. after a CSS transition or a toggle.
  int IsVisibleForSelector(const char* css_selector);
  // Number of elements matching `css_selector` (querySelectorAll length). 0 is a
  // valid result; returns -1 on a null or syntactically invalid selector. Paired
  // with :nth-of-type(n) selectors on the per-element accessors above, this drives
  // list scraping (count, then read each index).
  int CountSelector(const char* css_selector);
  // Computed value of CSS `property` for the first match (getComputedStyle ->
  // getPropertyValue, so values are resolved/normalized: color -> "rgb(r, g, b)",
  // bold -> "700", display:none -> "none"). Fill *out + return true; false if no
  // element matches. Drives visibility checks (display/visibility/opacity) and
  // style assertions without writing JS. *out only written on success.
  bool GetComputedStyle(const char* css_selector, const char* property,
                        std::string* out);
  // HTTP status of the last top-level http(s) navigation (200, 404, 500…), or 0
  // when the last load was non-http (file/data/in-memory) or the network request
  // failed before a response. Lets a caller tell success from a 404/error page.
  int GetHttpStatus() const { return http_status_; }
  // Raw response headers of the last top-level http(s) navigation (CRLF-separated
  // "Name: Value" lines, as the server sent them, including the status line).
  // Empty for non-http loads or a failed fetch.
  const std::string& GetResponseHeaders() const { return response_headers_; }
  // Re-navigate to the current document URL, re-fetching it (file/http only).
  void Reload();

  // Host-driven session history over the main frame's navigations (host-initiated
  // LoadURL *and* page-initiated link/location/form commits — all captured via
  // OnDidCommitMainFrame). Go{Back,Forward} re-navigate to the adjacent entry.
  // (This is the embedder's history; page-driven history.back() is separate and
  // still routes to the absent browser.)
  bool CanGoBack() const { return history_index_ > 0; }
  bool CanGoForward() const {
    return history_index_ + 1 < static_cast<int>(history_.size());
  }
  bool GoBack();
  bool GoForward();
  // Called by MbFrameClient when the MAIN frame commits a document. `standard`
  // is true for a normal (history-appending) commit, false for reload/initial.
  void OnDidCommitMainFrame(const std::string& url, bool standard);
  // Called by MbFrameClient when the MAIN frame's load finishes (the `load` event).
  // Sets the load-finished flag and invokes the registered finish callback — the
  // real engine push signal (vs. polling / a fixed settle timer).
  void OnDidFinishLoad();
  // Register a callback fired on each main-frame load finish. Pass {} to clear.
  void SetLoadFinishCallback(std::function<void()> cb);
  // True once the current navigation's load has finished; reset when a new load
  // is started (see ResetLoadFinished). Lets the load primitives wait for the real
  // finish instead of a fixed delay.
  bool load_finished() const { return load_finished_; }
  void ResetLoadFinished() { load_finished_ = false; }
  // Set extra request headers (newline-separated "Name: Value") for navigation +
  // subresources. Set before LoadURL to apply to that navigation.
  void SetExtraHeaders(const char* utf8_headers);
  // Capture with a transparent base background (omitBackground): unpainted areas
  // keep alpha 0 instead of being filled white.
  void SetTransparentBackground(bool transparent);
  void SendScroll(int x, int y, int dx, int dy);
  // Absolute scroll: move the layout viewport to (x, y) in CSS px (window.scrollTo).
  void ScrollTo(int x, int y);
  // Repeatedly scroll to the bottom and settle until the page stops growing (or
  // max_steps, default 20), triggering IntersectionObserver/lazy-load so deferred
  // content materializes before a --full capture or scrape. Returns the number of
  // steps that grew the page (0 = static).
  int ScrollToBottom(int max_steps);
  std::string EvalToString(const char* utf8_script);  // eval JS -> string result
  int GetFrameCount();  // number of direct child frames (top-level iframes)
  // Eval in the frame_index-th child frame (0-based; -1 = main frame), host-
  // privileged so it reads even a cross-origin iframe. "" if out of range.
  std::string EvalInFrame(int frame_index, const char* utf8_script);
  // Like EvalToString, but also reports the JS typeof the result (one of
  // "number"/"string"/"boolean"/"object"/"function"/"undefined"/"array"/"null")
  // via *out_type — captured from the SAME single eval (no re-run / side effects).
  std::string EvalWithType(const char* utf8_script, std::string* out_type);
  // Eval JS in a dedicated isolated world: separate globals from the page, shared DOM.
  std::string EvalIsolated(const char* utf8_script);
  // Drive the engine for ~ms of real time (lets setTimeout / async work run).
  void WaitMs(int ms);
  // Pump until document.querySelector(css) matches or timeout_ms elapses; true if found.
  bool WaitForSelector(const char* css, int timeout_ms);
  // Poll a JS expression while pumping the loop until it evaluates truthy or the
  // timeout elapses (Puppeteer's waitForFunction). Exceptions count as falsey.
  // Generalizes WaitForSelector — wait on any condition (window.appReady,
  // results.length>0, ...). Returns true if it became truthy, false on timeout.
  bool WaitForFunction(const char* js_expr, int timeout_ms);
  // Like WaitForSelector but waits for real VISIBILITY (checkVisibility), not
  // mere existence — waits out a fade-in / display toggle / lazy reveal. Returns
  // true once the first match is shown, false on timeout.
  bool WaitForVisibleSelector(const char* css, int timeout_ms);
  // The inverse: wait until the first match is NOT visible — gone from the DOM
  // or hidden (display:none / visibility:hidden / opacity:0). The "wait for the
  // loading spinner to disappear" primitive. True once gone/hidden, else timeout.
  bool WaitForSelectorHidden(const char* css, int timeout_ms);
  // Wait until no new subresource request has been recorded for idle_ms
  // (Puppeteer networkidle) — for SPAs that fetch after the initial load. True
  // once idle, false at timeout_ms. Reads the process-wide request log.
  bool WaitForNetworkIdle(int idle_ms, int timeout_ms);
  bool PaintToBitmap(void* out_bgra, int w, int h, int stride);
  bool SavePng(const char* path, int w, int h);  // render + encode PNG to disk
  // Render the full view to a w×h PNG held in memory (encoded_png_) — for
  // embedders that want the bytes (serve over HTTP, store in a DB) without a temp
  // file. Returns true on success; read the bytes via EncodedData(). The buffer is
  // valid until the next EncodePng or the view's destruction.
  bool EncodePng(int w, int h);
  const std::vector<uint8_t>& EncodedData() const { return encoded_png_; }
  // Print the document to a multi-page PDF (US Letter) at `path` via Blink's print path.
  bool SavePdf(const char* path);
  // Render just the logical rect (x,y,w,h) to a PNG (output is w*dsf x h*dsf px).
  bool SavePngRect(const char* path, int x, int y, int w, int h);
  // Screenshot just the first element matching `css_selector` (scroll into view +
  // clip its viewport box) — Puppeteer's elementHandle.screenshot. False on no
  // match / no box. Captures the visible extent for an oversized element.
  bool SaveElementPng(const char* css_selector, const char* path);
  // Same clip, but into a caller-provided BGRA buffer (w x h px; dsf not applied).
  bool PaintRectToBitmap(void* out_bgra, int x, int y, int w, int h, int stride);

 private:
  MbWebView();
  // Commit an in-memory document (any bytes, including embedded NULs) as the main
  // frame's content and drive parsing to quiescence. Both LoadHTML and the network
  // LoadURL funnel through here so neither truncates the body at a NUL.
  // `charset` is the authoritative text encoding (e.g. an HTTP Content-Type
  // charset). Empty => tentative, so the HTML parser honors <meta charset>/BOM.
  void CommitHtml(const char* data, size_t len, const char* base_url,
                  const std::string& charset = "");
  // Run `body` inside a scheduler task (so subsystems that require task bracketing
  // — e.g. CanvasPerformanceMonitor around canvas draws — are satisfied), blocking
  // until it has executed. Host-driven JS must run here, not via a bare synchronous
  // ExecuteScript, or a canvas draw outside any task scope trips a FATAL NOTREACHED.
  // settle=true also drains async continuations (timers/microtasks) until idle,
  // capped at 250ms (RunJS semantics); settle=false runs just the one task then
  // returns (Eval's synchronous-read semantics).
  void RunInFrameTask(base::OnceClosure body, bool settle);
  // Run requestAnimationFrame callbacks (no compositor drives them otherwise).
  void ServiceAnimations();
  // Settle async loads, run lifecycle, and play the frame's paint record into `canvas`.
  // (origin_x, origin_y) shifts the document so that logical point lands at the canvas
  // origin — used for clip/region capture; (0,0) renders from the top-left as usual.
  bool PaintInto(SkCanvas& canvas, int origin_x = 0, int origin_y = 0);

  std::unique_ptr<MbViewClient> view_client_;
  std::unique_ptr<MbFrameClient> frame_client_;
  std::unique_ptr<MbWidget> widget_;
  std::unique_ptr<blink::scheduler::WebAgentGroupScheduler> agent_group_scheduler_;
  // [[maybe_unused]] until the handshake bodies (currently scaffolded) use them.
  [[maybe_unused]] blink::WebViewImpl* web_view_ = nullptr;     // owned by blink; Close() in dtor
  [[maybe_unused]] blink::WebLocalFrame* main_frame_ = nullptr; // owned by blink
  float dsf_ = 1.0f;  // device pixel ratio; PaintInto scales the canvas by it
  std::string init_script_;  // runs before each new document's own scripts
  std::vector<std::unique_ptr<NativeBinding>> js_bindings_;  // BindJsFunction
  JsDialogFn dialog_cb_ = nullptr;       // alert/confirm/prompt handler (optional)
  void* dialog_userdata_ = nullptr;
  bool dialog_registered_ = false;       // __mbDlg bridge pushed into js_bindings_ once
  void InstallJsBindings();  // install all bindings into the current main world
  bool transparent_bg_ = false;  // omitBackground: clear to alpha 0

  std::vector<std::string> history_;  // main-frame navigation stack (URLs)
  int history_index_ = -1;            // current position; -1 before first load
  bool in_history_nav_ = false;       // a Go{Back,Forward} is in flight

  bool load_finished_ = false;        // main-frame load event has fired (DidFinishLoad)
  std::function<void()> on_load_finish_;  // optional embedder finish callback

  std::vector<uint8_t> encoded_png_;  // retained bytes from the last EncodePng
  int http_status_ = 0;  // HTTP status of the last http(s) load; 0 if none/failed
  std::string response_headers_;  // raw response headers of the last http(s) load
};

}  // namespace mb

#endif  // MINIBLINK_HOST_VIEW_MB_WEBVIEW_H_
