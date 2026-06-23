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
  void SendScroll(int x, int y, int dx, int dy);
  std::string EvalToString(const char* utf8_script);  // eval JS -> string result
  bool PaintToBitmap(void* out_bgra, int w, int h, int stride);
  bool SavePng(const char* path, int w, int h);  // render + encode PNG to disk

 private:
  MbWebView();
  // Settle async loads, run lifecycle, and play the frame's paint record into `canvas`.
  bool PaintInto(SkCanvas& canvas);

  std::unique_ptr<MbViewClient> view_client_;
  std::unique_ptr<MbFrameClient> frame_client_;
  std::unique_ptr<MbWidget> widget_;
  std::unique_ptr<blink::scheduler::WebAgentGroupScheduler> agent_group_scheduler_;
  // [[maybe_unused]] until the handshake bodies (currently scaffolded) use them.
  [[maybe_unused]] blink::WebViewImpl* web_view_ = nullptr;     // owned by blink; Close() in dtor
  [[maybe_unused]] blink::WebLocalFrame* main_frame_ = nullptr; // owned by blink
};

}  // namespace mb

#endif  // MINIBLINK_HOST_VIEW_MB_WEBVIEW_H_
