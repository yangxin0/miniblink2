// mb_widget — frame widget for miniblink2 (non-compositing, offscreen).
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

#include <functional>
#include <memory>
#include <string>

#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "third_party/blink/public/mojom/widget/platform_widget.mojom-blink.h"
#include "third_party/blink/public/web/web_non_composited_widget_client.h"

namespace blink {
class WebLocalFrame;
class WebFrameWidget;
}  // namespace blink

namespace mb {

class SoftwareCompositor;

class MbWidget : public blink::WebNonCompositedWidgetClient,
                 public blink::mojom::blink::WidgetHost {
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
  // Typed-event entries (the mbMouseEvent/mbWheelEvent C structs route here).
  // General mouse event: type 0=move 1=down 2=up; button 0=left 1=middle
  // 2=right; click_count for down/up (2 = double-click); modifiers bitmask
  // 1=ctrl 2=shift 4=alt 8=meta.
  void SendMouseEvent(int type, int x, int y, int button, int click_count,
                      int modifiers);
  // Precise wheel: float pixel deltas with DOM sign; `precise` marks
  // trackpad-style deltas (kScrollByPrecisePixel). Returns true if a blocking
  // listener consumed it (preventDefault).
  bool SendWheelEx(int x, int y, float delta_x, float delta_y, bool precise,
                   int modifiers);
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
  // separately (a standalone keyup dispatch).
  void SendKeyUp(int windows_key_code);
  // Typed keyboard event (mbSendKeyEvent): dispatch ONE WebKeyboardEvent built
  // from explicit fields — type 0=RawKeyDown 1=KeyDown 2=KeyUp 3=Char;
  // modifiers bitmask 1=ctrl 2=shift 4=alt 8=meta; text/unmodified_text UTF-8
  // (kChar inserts `text`). Lossless forwarding of a real host key event,
  // including auto-repeat / keypad / system-key flags the shorthands can't say.
  void SendKeyEvent(int type, int modifiers, int windows_key_code,
                    int native_key_code, const std::string& text,
                    const std::string& unmodified_text, bool is_keypad,
                    bool is_auto_repeat, bool is_system_key);
  // Drive the focused editable through an IME sequence: `composing` shows the in-progress
  // reading (compositionstart/update), `committed` inserts the final text + fires
  // compositionend + input. Either may be empty/null.
  void SendIme(const char* composing, const char* committed);

  // Frame-request flag for hosts that poll (mbViewIsDirty): blink calls
  // ScheduleNonCompositedAnimation() whenever the non-composited widget wants a
  // new frame (style/layout invalidation, animations, rAF). Painting SNAPSHOTS
  // and clears it up front, so a request that lands mid-paint counts toward the
  // NEXT frame instead of being lost.
  void ScheduleNonCompositedAnimation() override { needs_frame_ = true; }
  bool needs_frame() const { return needs_frame_; }
  void set_needs_frame(bool on) { needs_frame_ = on; }

  // ---- WidgetHost (blink -> host UI state) ----------------------------------
  // We bind the browser end of the widget's WidgetHost channel (previously
  // dropped) so blink's cursor / tooltip / text-input-state reports reach the
  // embedder instead of the void.
  void SetCursor(const ui::Cursor& cursor) override;
  void UpdateTooltipUnderCursor(
      const blink::String& tooltip_text,
      base::i18n::TextDirection text_direction_hint) override;
  void UpdateTooltipFromKeyboard(const blink::String& tooltip_text,
                                 base::i18n::TextDirection text_direction_hint,
                                 const gfx::Rect& bounds) override;
  void ClearKeyboardTriggeredTooltip() override;
  void TextInputStateChanged(ui::mojom::blink::TextInputStatePtr state) override;
  void SelectionBoundsChanged(const gfx::Rect& anchor_rect,
                              base::i18n::TextDirection anchor_dir,
                              const gfx::Rect& focus_rect,
                              base::i18n::TextDirection focus_dir,
                              const gfx::Rect& bounding_box_rect,
                              bool is_anchor_first) override {}
  void CreateFrameSink(
      mojo::PendingReceiver<viz::mojom::blink::CompositorFrameSink>,
      mojo::PendingRemote<viz::mojom::blink::CompositorFrameSinkClient>,
      mojo::PendingRemote<blink::mojom::blink::RenderInputRouterClient>)
      override {}
  void RegisterRenderFrameMetadataObserver(
      mojo::PendingReceiver<cc::mojom::blink::RenderFrameMetadataObserverClient>,
      mojo::PendingRemote<cc::mojom::blink::RenderFrameMetadataObserver>)
      override {}

  // Cursor code (the ui::mojom::CursorType value; custom -> kPointer), fired on
  // change only. Tooltip text (UTF-8; "" = hide), deduped. {} clears.
  void SetCursorChangedCallback(std::function<void(int)> cb) {
    on_cursor_changed_ = std::move(cb);
  }
  void SetTooltipChangedCallback(std::function<void(const std::string&)> cb) {
    on_tooltip_changed_ = std::move(cb);
  }
  // True when the focused element accepts text input (an editable with a
  // caret). Queried synchronously from the active input-method controller —
  // blink only PUSHES TextInputStateChanged for browser-focused widgets, which
  // this headless widget never is; the pushed state is kept as a fast path.
  bool HasInputFocus() const;

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
  // Browser end of the widget's WidgetHost channel (bound in Attach).
  mojo::AssociatedReceiver<blink::mojom::blink::WidgetHost>
      widget_host_receiver_{this};
  std::function<void(int)> on_cursor_changed_;
  std::function<void(const std::string&)> on_tooltip_changed_;
  int last_cursor_ = 0;               // ui::mojom::CursorType::kPointer
  std::string last_tooltip_;
  bool has_input_focus_ = false;      // last reported text-input state != NONE
  bool needs_frame_ = true;  // start dirty: the first paint is always wanted
  bool mouse_pressed_ = false;  // left button held (drag): moves carry the mask
  bool composited_ = false;
  std::unique_ptr<SoftwareCompositor> compositor_;  // non-null iff composited_
};

}  // namespace mb

#endif  // MINIBLINK_HOST_WIDGET_MB_WIDGET_H_
