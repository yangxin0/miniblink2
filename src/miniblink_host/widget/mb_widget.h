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
  void SendMouseMove(int x, int y);   // move pointer to (x,y): hover + mousemove
  void SendText(const char* utf8);    // type ASCII text into the focused element

  blink::WebFrameWidget* widget() { return widget_; }

 private:
  blink::WebFrameWidget* widget_ = nullptr;  // owned by Blink (the frame)
};

}  // namespace mb

#endif  // MINIBLINK_HOST_WIDGET_MB_WIDGET_H_
