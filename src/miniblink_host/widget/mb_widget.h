// mb_widget — frame widget for miniblink-modern (non-compositing, offscreen).
//
// We use the NON-compositing widget path: InitializeFrameWidget (with no-op Mojo
// channels) + InitializeNonCompositing(this). This avoids LayerTreeHost / frame-sink /
// LayerTreeSettings entirely. Pixels come from the paint record after a lifecycle
// update (see mb_webview PaintToBitmap), not from a compositor.
//
// WebNonCompositedWidgetClient has a single optional no-op virtual, so MbWidget can
// be its own client.
//
// Status: Phase 1.

#ifndef MINIBLINK_HOST_WIDGET_MB_WIDGET_H_
#define MINIBLINK_HOST_WIDGET_MB_WIDGET_H_

#include "third_party/blink/public/web/web_non_composited_widget_client.h"

namespace blink {
class WebLocalFrame;
class WebFrameWidget;
}  // namespace blink

namespace mb {

class MbWidget : public blink::WebNonCompositedWidgetClient {
 public:
  MbWidget();
  ~MbWidget() override;

  // Creates the frame widget on `main_frame` with no-op browser channels, inits it
  // non-compositing, and sizes it. Must be followed by web_view->DidAttachLocalMainFrame().
  void Attach(blink::WebLocalFrame* main_frame, int width, int height);
  void Resize(int width, int height);
  void SendMouseClick(int x, int y);  // synthesize mousedown+mouseup at (x,y)
  void SendMouseDown(int x, int y);   // press the left button at (x,y) (drag start)
  void SendMouseUp(int x, int y);     // release the left button at (x,y) (drag end)
  void SendDoubleClick(int x, int y); // two clicks (count 1 then 2) -> dblclick
  void SendRightClick(int x, int y);  // right mousedown+up -> contextmenu
  // General click: button 0=left/1=middle/2=right, modifiers bitmask 1=ctrl 2=shift
  // 4=alt 8=meta — covers ctrl/shift-click, middle-click (auxclick), etc.
  void SendMouseClickEx(int x, int y, int button, int modifiers);
  void SendMouseMove(int x, int y);   // move pointer to (x,y): hover + mousemove
  // Dispatch a TRUSTED mouse-wheel at (x,y): delta_x/delta_y are pixel deltas with
  // DOM `wheel` sign (positive deltaY = scroll content DOWN). Fires the page's
  // `wheel` handlers (isTrusted=true) and scrolls the document (main-thread scroll).
  void SendWheel(int x, int y, int delta_x, int delta_y, int modifiers);
  void SendText(const char* utf8);    // type ASCII text into the focused element
  // Press a named non-text key ("Enter", "Tab", "Escape", "Backspace", "Delete",
  // "Arrow{Left,Up,Right,Down}", "Home", "End", "PageUp", "PageDown") as a real
  // trusted key event, so default actions fire (Enter submits a form, Tab moves
  // focus) — unlike a JS-dispatched (untrusted) event. No-op for unknown names.
  void SendKey(const char* key_name);
  // Dispatch a standalone key RELEASE (kKeyUp) for a Win32 VK code, so page `keyup`
  // handlers fire on release. Pairs with SendKey for callers that drive down/up
  // separately (e.g. wke's wkeFireKeyUpEvent).
  void SendKeyUp(int windows_key_code);
  // Drive the focused editable through an IME sequence: `composing` shows the in-progress
  // reading (compositionstart/update), `committed` inserts the final text + fires
  // compositionend + input. Either may be empty/null.
  void SendIme(const char* composing, const char* committed);

  blink::WebFrameWidget* widget() { return widget_; }

 private:
  blink::WebFrameWidget* widget_ = nullptr;  // owned by Blink (the frame)
  bool mouse_pressed_ = false;  // left button held (drag): moves carry the mask
};

}  // namespace mb

#endif  // MINIBLINK_HOST_WIDGET_MB_WIDGET_H_
