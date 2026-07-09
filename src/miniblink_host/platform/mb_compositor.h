// mb_compositor.h — a self-contained, PRODUCTION (non-testonly) software-compositing
// backend for the host's blink widget.
//
// Blink's WebFrameWidgetImpl::AllocateNewLayerTreeFrameSink() must return a
// cc::LayerTreeFrameSink for the widget to composite through cc instead of the current
// non-compositing software-paint path. This component supplies one:
//
//   SoftwareCompositor owns standalone gpu primitives (SyncPointManager / Scheduler /
//   SharedImageManager — a software viz::Display needs no GPU service or GL context), a
//   viz::FrameSinkManagerImpl, and a viz::Display over a viz::SoftwareOutputSurface whose
//   SoftwareOutputDevice captures the composited SkBitmap. CreateFrameSink() hands out a
//   MbDirectLayerTreeFrameSink bound to that Display.
//
// MbDirectLayerTreeFrameSink is a verbatim copy of ui::DirectLayerTreeFrameSink (which is
// testonly upstream, in //ui/compositor:test_support) so this non-testonly library can own
// it: a cc::LayerTreeFrameSink that submits its client's frame as the ROOT surface of the
// in-process Display. The whole arc (mb_compositor_probe milestones A–D1) verified this
// path composites a yellow quad to pixels with production components.

#ifndef MINIBLINK_HOST_PLATFORM_MB_COMPOSITOR_H_
#define MINIBLINK_HOST_PLATFORM_MB_COMPOSITOR_H_

#include <memory>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/thread_checker.h"
#include "cc/trees/layer_tree_frame_sink.h"
#include "components/viz/common/display/renderer_settings.h"
#include "components/viz/common/frame_sinks/begin_frame_source.h"
#include "components/viz/common/surfaces/frame_sink_id.h"
#include "components/viz/common/surfaces/parent_local_surface_id_allocator.h"
#include "components/viz/service/display/display_client.h"
#include "components/viz/service/frame_sinks/compositor_frame_sink_support.h"
#include "services/viz/public/mojom/compositing/compositor_frame_sink.mojom.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/geometry/size.h"

namespace gpu {
class GLInProcessContext;
class Scheduler;
class SharedImageInterface;
class SharedImageManager;
class SyncPointManager;
}  // namespace gpu

namespace viz {
class Display;
class FrameSinkManagerImpl;
class OverlayProcessorInterface;
}  // namespace viz

namespace mb {

class MbCapturingSoftwareOutputDevice;

// A cc::LayerTreeFrameSink that submits the client's frame as the root surface of an
// in-process viz::Display. Verbatim port of ui::DirectLayerTreeFrameSink (testonly
// upstream) so this production library can own it. |frame_sink_manager| and |display|
// must outlive this object.
class MbDirectLayerTreeFrameSink : public cc::LayerTreeFrameSink,
                                   public viz::mojom::CompositorFrameSinkClient,
                                   public viz::ExternalBeginFrameSourceClient,
                                   public viz::DisplayClient {
 public:
  MbDirectLayerTreeFrameSink(
      const viz::FrameSinkId& frame_sink_id,
      viz::FrameSinkManagerImpl* frame_sink_manager,
      viz::Display* display,
      scoped_refptr<base::SingleThreadTaskRunner> compositor_task_runner,
      scoped_refptr<gpu::SharedImageInterface> shared_image_interface);
  MbDirectLayerTreeFrameSink(const MbDirectLayerTreeFrameSink&) = delete;
  MbDirectLayerTreeFrameSink& operator=(const MbDirectLayerTreeFrameSink&) =
      delete;
  ~MbDirectLayerTreeFrameSink() override;

  // cc::LayerTreeFrameSink:
  bool BindToClient(cc::LayerTreeFrameSinkClient* client) override;
  void DetachFromClient() override;
  void SubmitCompositorFrame(viz::CompositorFrame frame,
                             bool hit_test_data_changed) override;
  void DidNotProduceFrame(const viz::BeginFrameAck& ack,
                          cc::FrameSkippedReason reason) override;
  void NotifyNewLocalSurfaceIdExpectedWhilePaused() override;

  // viz::DisplayClient:
  void DisplayOutputSurfaceLost() override;
  void DisplayWillDrawAndSwap(
      bool will_draw_and_swap,
      viz::AggregatedRenderPassList* render_passes) override;
  void DisplayDidDrawAndSwap() override {}
  void DisplayDidReceiveCALayerParams(
      gfx::CALayerParams ca_layer_params) override {}
  void DisplayDidCompleteSwapWithSize(const gfx::Size& pixel_size) override {}
  void DisplayAddChildWindowToBrowser(
      gpu::SurfaceHandle child_window) override {}
  void SetWideColorEnabled(bool enabled) override {}

 private:
  // viz::mojom::CompositorFrameSinkClient:
  void DidReceiveCompositorFrameAck(
      std::vector<viz::ReturnedResource> resources) override;
  void OnBeginFrame(const viz::BeginFrameArgs& args,
                    const viz::FrameTimingDetailsMap& timing_details,
                    std::vector<viz::ReturnedResource> resources) override;
  void ReclaimResources(std::vector<viz::ReturnedResource> resources) override;
  void OnBeginFramePausedChanged(bool paused) override;
  void OnCompositorFrameTransitionDirectiveProcessed(
      uint32_t sequence_id) override {}
  void OnSurfaceEvicted(const viz::LocalSurfaceId& local_surface_id) override {}

  // viz::ExternalBeginFrameSourceClient:
  void OnNeedsBeginFrames(bool needs_begin_frames) override;

  void DidReceiveCompositorFrameAckInternal(
      std::vector<viz::ReturnedResource> resources);

  THREAD_CHECKER(thread_checker_);

  std::unique_ptr<viz::CompositorFrameSinkSupport> support_;
  bool needs_begin_frames_ = false;
  const viz::FrameSinkId frame_sink_id_;
  raw_ptr<viz::FrameSinkManagerImpl> frame_sink_manager_;
  viz::ParentLocalSurfaceIdAllocator parent_local_surface_id_allocator_;
  raw_ptr<viz::Display> display_;
  gfx::Size last_swap_frame_size_;
  float device_scale_factor_ = 1.f;
  bool is_lost_ = false;
  std::unique_ptr<viz::ExternalBeginFrameSource> begin_frame_source_;
  base::WeakPtrFactory<MbDirectLayerTreeFrameSink> weak_factory_{this};
};

// Owns the production software-compositing backend (gpu primitives + viz::Display over a
// capturing SoftwareOutputSurface) and hands out frame sinks for blink's widget. Single
// thread; one Display.
class SoftwareCompositor {
 public:
  explicit SoftwareCompositor(const gfx::Size& size);
  SoftwareCompositor(const SoftwareCompositor&) = delete;
  SoftwareCompositor& operator=(const SoftwareCompositor&) = delete;
  ~SoftwareCompositor();

  // Creates the frame sink blink's WebFrameWidgetImpl returns from
  // AllocateNewLayerTreeFrameSink(). The sink references this object's Display +
  // FrameSinkManager, which must outlive it. Intended to be called once.
  std::unique_ptr<cc::LayerTreeFrameSink> CreateFrameSink();

  viz::Display* display() { return display_.get(); }
  viz::FrameSinkManagerImpl* frame_sink_manager() {
    return frame_sink_manager_.get();
  }
  const viz::FrameSinkId& frame_sink_id() const { return frame_sink_id_; }

  // Number of times CreateFrameSink() has been called — i.e. how many times
  // blink's compositor has requested a frame sink through the host hook. >0 after
  // a compositing widget is shown and the loop pumped (used to verify the live
  // integration).
  int frame_sink_count() const { return frame_sink_count_; }

  void Resize(const gfx::Size& size);

  // Drives one Display draw+swap (the in-process scheduler is begin-frame-driven; for
  // headless we force a draw) and returns the composited bitmap captured off the
  // SoftwareOutputDevice. Empty if nothing has composited yet.
  const SkBitmap& DrawAndCapture();
  const SkBitmap& captured_bitmap() const;
  // macOS: the IOSurfaceRef the Display renders into (the shared-texture
  // output path — bind as CALayer.contents, zero CPU readback). Null on other
  // platforms, before the first composited frame, or if IOSurface allocation
  // fell back to heap memory. Owned by the output device: valid until the
  // next Resize or teardown.
  void* io_surface() const;

 private:
  scoped_refptr<base::SingleThreadTaskRunner> task_runner_;
  std::unique_ptr<gpu::SyncPointManager> sync_point_manager_;
  std::unique_ptr<gpu::Scheduler> gpu_scheduler_;
  // In-process GL context (holder-backed) sources the SharedImageInterface cc rasters tiles
  // through; the GPU service's SharedImageManager (which the Display also reads) backs both.
  // gl_context_ is declared before shared_image_interface_ so the SII wrapper tears down first.
  std::unique_ptr<gpu::GLInProcessContext> gl_context_;
  scoped_refptr<gpu::SharedImageInterface> shared_image_interface_;
  std::unique_ptr<viz::FrameSinkManagerImpl> frame_sink_manager_;
  const viz::FrameSinkId frame_sink_id_;
  // viz::Display stores &debug_settings_ by pointer and the begin-frame source via its
  // scheduler; both must outlive the Display (declared before display_).
  viz::DebugRendererSettings debug_settings_;
  std::unique_ptr<viz::StubBeginFrameSource> begin_frame_source_keepalive_;
  raw_ptr<MbCapturingSoftwareOutputDevice> output_device_ = nullptr;
  std::unique_ptr<viz::Display> display_;
  gfx::Size size_;
  int frame_sink_count_ = 0;
};

}  // namespace mb

#endif  // MINIBLINK_HOST_PLATFORM_MB_COMPOSITOR_H_
