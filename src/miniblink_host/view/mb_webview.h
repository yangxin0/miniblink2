// mb_webview — orchestrator that owns one WebView + main LocalFrame + widget.
//
// Replicates the WebViewHelper handshake (vendor/reference/frame_test_helpers.cc):
//   1. WebView::Create(view_client, ... PageBroadcast=NullAssociatedReceiver,
//                      agent_group_scheduler, browsing_context_group_token, ...) (:778)
//   2. WebLocalFrame::CreateMainFrame(web_view, frame_client, /*broker=*/NullRemote,
//                      tokens, /*policy_container=*/nullptr, ...) (:489)
//   3. create MbWidget, InitializeCompositing
//   4. web_view->DidAttachLocalMainFrame()
// All browser-side handles are null/default — no browser process (see interface-surface.md).
//
// This is what the C ABI mbView wraps 1:1.
// Status: Phase 1 scaffold.

#ifndef MINIBLINK_HOST_VIEW_MB_WEBVIEW_H_
#define MINIBLINK_HOST_VIEW_MB_WEBVIEW_H_

#include <memory>
#include <string>

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
  void RunJS(const char* utf8_script);  // execute JS in the main frame
  void SendMouseClick(int x, int y);
  void SendMouseMove(int x, int y);
  void SendText(const char* utf8);
  // Set the device pixel ratio (HiDPI). The page lays out in CSS px but reports
  // window.devicePixelRatio == scale and rasterizes at `scale`x in PaintInto, so
  // captures are retina-crisp. Caller sizes the output bitmap to logical*scale.
  void SetDeviceScaleFactor(float scale);
  // Override the User-Agent for navigator.userAgent and outgoing requests. Set
  // before LoadURL/LoadHTML to take effect for that navigation.
  void SetUserAgent(const char* utf8_ua);
  // Return captured console output ("level: text" per line) and clear the buffer.
  std::string DrainConsole();
  // Capture with a transparent base background (omitBackground): unpainted areas
  // keep alpha 0 instead of being filled white.
  void SetTransparentBackground(bool transparent);
  void SendScroll(int x, int y, int dx, int dy);
  std::string EvalToString(const char* utf8_script);  // eval JS -> string result
  // Drive the engine for ~ms of real time (lets setTimeout / async work run).
  void WaitMs(int ms);
  // Pump until document.querySelector(css) matches or timeout_ms elapses; true if found.
  bool WaitForSelector(const char* css, int timeout_ms);
  bool PaintToBitmap(void* out_bgra, int w, int h, int stride);
  bool SavePng(const char* path, int w, int h);  // render + encode PNG to disk
  // Render just the logical rect (x,y,w,h) to a PNG (output is w*dsf x h*dsf px).
  bool SavePngRect(const char* path, int x, int y, int w, int h);
  // Same clip, but into a caller-provided BGRA buffer (w x h px; dsf not applied).
  bool PaintRectToBitmap(void* out_bgra, int x, int y, int w, int h, int stride);

 private:
  MbWebView();
  // Commit an in-memory document (any bytes, including embedded NULs) as the main
  // frame's content and drive parsing to quiescence. Both LoadHTML and the network
  // LoadURL funnel through here so neither truncates the body at a NUL.
  void CommitHtml(const char* data, size_t len, const char* base_url);
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
  bool transparent_bg_ = false;  // omitBackground: clear to alpha 0
};

}  // namespace mb

#endif  // MINIBLINK_HOST_VIEW_MB_WEBVIEW_H_
