// mb_sw_frame_sink — minimal cc::LayerTreeFrameSink stub for offscreen compositing.
//
// IMPORTANT (resolved 2026-06-23): this is NOT the pixel source. SimCompositor shows
// the real readback path is:
//     LayerTreeHost()->CompositeForTest(...);                       // run cc lifecycle
//     frame_view->GetPaintRecord().Playback(canvas_over_SkBitmap);  // <-- real pixels
// i.e. Blink's recorded cc::PaintRecord is played straight into an SkBitmap-backed
// SkCanvas. No viz SoftwareRenderer, no quad rasterization needed for P1.
//
// So this frame sink only has to satisfy cc (CompositeForTest requires a bound
// frame sink). A software/no-op sink that accepts SubmitCompositorFrame and reports
// itself bound is enough. Model on cc/test software sinks if convenient.
//
// Status: Phase 1 scaffold.

#ifndef MINIBLINK_HOST_WIDGET_MB_SW_FRAME_SINK_H_
#define MINIBLINK_HOST_WIDGET_MB_SW_FRAME_SINK_H_

// TODO(mb): "cc/trees/layer_tree_frame_sink.h"

namespace mb {

// Minimal frame sink so cc can run a synchronous composite. Pixels are read back
// separately via the paint-record playback path (see mb_widget.h).
class MbSoftwareFrameSink /* : public cc::LayerTreeFrameSink */ {
 public:
  MbSoftwareFrameSink();
  ~MbSoftwareFrameSink();
  // bool BindToClient(cc::LayerTreeFrameSinkClient*) override;  // accept + report bound
  // void SubmitCompositorFrame(viz::CompositorFrame, ...) override;  // ack, discard
};

}  // namespace mb

#endif  // MINIBLINK_HOST_WIDGET_MB_SW_FRAME_SINK_H_
