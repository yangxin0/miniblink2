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

#include <memory>

#include "third_party/blink/public/web/web_non_composited_widget_client.h"

namespace blink {
class WebLocalFrame;
class WebFrameWidget;
}  // namespace blink

namespace mb {

class SoftwareCompositor;

class MbWidget : public blink::WebNonCompositedWidgetClient {
 public:
  MbWidget();
  ~MbWidget() override;

  // Creates the frame widget on `main_frame` with no-op browser channels and sizes it;
  // must be followed by web_view->DidAttachLocalMainFrame(). When `composited` is false
  // (default) it inits NON-compositing (pixels via the software-paint path). When true it
  // owns a mb::SoftwareCompositor, installs the blink frame-sink hook (patch 0012), and
  // inits COMPOSITING so blink drives cc -> our in-process software Display.
  void Attach(blink::WebLocalFrame* main_frame, int width, int height,
              bool composited = false);
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
  // `wheel` handlers (isTrusted=true). Returns true if a blocking listener consumed
  // it (called preventDefault) — the caller then suppresses the default scroll.
  bool SendWheel(int x, int y, int delta_x, int delta_y, int modifiers);
  // Trusted single-finger touch tap at (x,y): a real WebPointerEvent(kTouch) down+up,
  // so blink fires pointerdown/up + touchstart/end with isTrusted=true. Returns false if
  // no widget. (Dispatch may be async — the element's handlers run on the next pump.)
  bool SendTouchTap(int x, int y);
  // Trusted one-finger swipe (x1,y1)->(x2,y2): a WebPointerEvent(kTouch) down ->
  // interpolated moves -> up, so pointerdown/pointermove/pointerup fire isTrusted=true
  // (touch-drag UIs use Pointer Events). Returns false if no widget. Dispatch is async.
  bool SendTouchSwipe(int x1, int y1, int x2, int y2);
  void SendText(const char* utf8);    // type ASCII text into the focused element
  // Press a named non-text key ("Enter", "Tab", "Escape", "Backspace", "Delete",
  // "Arrow{Left,Up,Right,Down}", "Home", "End", "PageUp", "PageDown") as a real
  // trusted key event, so default actions fire (Enter submits a form, Tab moves
  // focus) — unlike a JS-dispatched (untrusted) event. No-op for unknown names.
  void SendKey(const char* key_name);
  // Like SendKey but WITH a modifier bitmask (1=ctrl 2=shift 4=alt 8=meta), and `key`
  // may be a named key ("ArrowRight", "Home", ...) OR a single character ("a", "1", "k").
  // Sends a real trusted key event so keyboard SHORTCUTS fire (Ctrl+A select-all, Ctrl+S,
  // app hotkeys) and Shift+arrow extends a selection. A kChar is only emitted when no
  // command modifier (ctrl/alt/meta) is held (a shortcut produces no typed character).
  void SendKeyEx(const char* key, int modifiers);
  // Dispatch a standalone key RELEASE (kKeyUp) for a Win32 VK code, so page `keyup`
  // handlers fire on release. Pairs with SendKey for callers that drive down/up
  // separately (e.g. wke's wkeFireKeyUpEvent).
  void SendKeyUp(int windows_key_code);
  // Drive the focused editable through an IME sequence: `composing` shows the in-progress
  // reading (compositionstart/update), `committed` inserts the final text + fires
  // compositionend + input. Either may be empty/null.
  void SendIme(const char* composing, const char* committed);

  blink::WebFrameWidget* widget() { return widget_; }
  // The software compositor backing this widget when attached compositing, else null.
  SoftwareCompositor* compositor() { return compositor_.get(); }
  bool composited() const { return composited_; }
  // Drive one synchronous compositor frame (BeginMainFrame + lifecycle + cc commit/draw
  // through the in-process Display). No-op unless attached compositing. This is how a
  // headless compositing widget produces a frame (no browser begin-frame source).
  void Composite();

 private:
  // Give a non-compositing widget a realistic desktop screen (window.screen.*) and
  // window rect (window.outer{Width,Height}) instead of the default 0x0 — the latter
  // is a glaring headless tell and breaks size-based site logic. view_w/view_h are
  // the inner viewport (logical px). Relies on patch 0015 (UpdateScreenInfo is
  // null-LayerTreeHost-safe for non-compositing widgets).
  void SetRealisticScreen(int view_w, int view_h);

  blink::WebFrameWidget* widget_ = nullptr;  // owned by Blink (the frame)
  bool mouse_pressed_ = false;  // left button held (drag): moves carry the mask
  bool composited_ = false;
  std::unique_ptr<SoftwareCompositor> compositor_;  // non-null iff composited_
};

}  // namespace mb

#endif  // MINIBLINK_HOST_WIDGET_MB_WIDGET_H_
