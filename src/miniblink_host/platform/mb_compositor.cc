// mb_compositor.cc — see mb_compositor.h. MbDirectLayerTreeFrameSink is a port of
// ui::DirectLayerTreeFrameSink (testonly upstream); SoftwareCompositor assembles the
// production software viz::Display the frame sink draws into.

#include "miniblink_host/platform/mb_compositor.h"

#include <optional>
#include <utility>

#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/notreached.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "components/viz/common/resources/shared_image_format.h"
#include "skia/ext/legacy_display_globals.h"

#if BUILDFLAG(IS_MAC)
#include <IOSurface/IOSurfaceRef.h>

#include "ui/gfx/mac/io_surface.h"
#endif
#include "cc/trees/layer_tree_frame_sink_client.h"
#include "components/viz/common/display/renderer_settings.h"
#include "components/viz/common/features.h"
#include "components/viz/common/frame_sinks/begin_frame_source.h"
#include "components/viz/common/hit_test/hit_test_region_list.h"
#include "components/viz/common/quads/compositor_frame.h"
#include "components/viz/service/display/display.h"
#include "components/viz/service/display/display_scheduler.h"
#include "components/viz/service/display/overlay_processor_stub.h"
#include "components/viz/service/display/software_output_device.h"
#include "components/viz/service/display_embedder/software_output_surface.h"
#include "components/viz/service/frame_sinks/frame_sink_manager_impl.h"
#include "components/viz/service/surfaces/surface_manager.h"
#include "gpu/command_buffer/client/shared_image_interface.h"
#include "gpu/command_buffer/service/command_buffer_task_executor.h"
#include "gpu/command_buffer/service/scheduler.h"
#include "gpu/command_buffer/service/shared_image/shared_image_manager.h"
#include "gpu/command_buffer/service/sync_point_manager.h"
#include "gpu/ipc/gl_in_process_context.h"
#include "gpu/ipc/in_process_gpu_thread_holder.h"
#include "miniblink_host/platform/mb_gpu_thread.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkSurface.h"

namespace mb {

// ---- The Display's software output device ----------------------------------
//
// macOS: the device renders into an IOSurface (skia draws straight into the
// surface's mapped memory), so a host can bind the composited frame as
// CALayer.contents with ZERO CPU readback — the shared-texture output path.
// CPU coherency is scoped per frame (IOSurfaceLock in BeginPaint, Unlock in
// EndPaint), which is also what makes the WindowServer/GPU see each frame's
// writes. Other platforms (and an IOSurface-allocation failure) keep the
// plain heap surface.
//
// The pixel CAPTURE (captured()) is now LAZY: pre-IOSurface this device copied
// the full frame into an SkBitmap on every EndPaint, which is exactly the
// readback the IOSurface path exists to eliminate. Only the bitmap consumers
// (PaintToBitmap's composited branch, CompositorPixel) pay it, on demand.
class MbCapturingSoftwareOutputDevice : public viz::SoftwareOutputDevice {
 public:
  void Resize(const gfx::Size& pixel_size, float scale_factor) override {
#if BUILDFLAG(IS_MAC)
    if (viewport_pixel_size_ == pixel_size)
      return;
    viewport_pixel_size_ = pixel_size;
    surface_.reset();
    io_surface_.reset();
    have_frame_ = false;
    captured_dirty_ = false;
    captured_.reset();
    if (pixel_size.IsEmpty())
      return;
    io_surface_ = gfx::CreateIOSurface(pixel_size,
                                       viz::SinglePlaneFormat::kBGRA_8888);
    if (io_surface_) {
      // MakeN32 is BGRA on mac — matches the IOSurface's pixel format, so the
      // canvas draws directly into the surface's memory.
      SkImageInfo info = SkImageInfo::MakeN32(
          pixel_size.width(), pixel_size.height(), kOpaque_SkAlphaType);
      SkSurfaceProps props = skia::LegacyDisplayGlobals::GetSkSurfaceProps();
      surface_ = SkSurfaces::WrapPixels(
          info, IOSurfaceGetBaseAddress(io_surface_.get()),
          IOSurfaceGetBytesPerRow(io_surface_.get()), &props);
    }
    if (!surface_) {
      // IOSurface unavailable: fall back to the base heap surface. The base
      // early-returns on an unchanged size, so reset it first.
      io_surface_.reset();
      viewport_pixel_size_ = gfx::Size();
      viz::SoftwareOutputDevice::Resize(pixel_size, scale_factor);
    }
#else
    viz::SoftwareOutputDevice::Resize(pixel_size, scale_factor);
#endif
  }

  SkCanvas* BeginPaint(const gfx::Rect& damage_rect) override {
#if BUILDFLAG(IS_MAC)
    if (io_surface_)
      IOSurfaceLock(io_surface_.get(), 0, nullptr);
#endif
    return viz::SoftwareOutputDevice::BeginPaint(damage_rect);
  }

  void EndPaint() override {
    viz::SoftwareOutputDevice::EndPaint();
#if BUILDFLAG(IS_MAC)
    if (io_surface_)
      IOSurfaceUnlock(io_surface_.get(), 0, nullptr);
#endif
    if (surface_) {
      have_frame_ = true;
      captured_dirty_ = true;
    }
  }

  // The composited pixels as a bitmap, copied on demand (empty until the
  // first frame). Cached until the next EndPaint.
  const SkBitmap& captured() const {
    if (captured_dirty_ && surface_) {
#if BUILDFLAG(IS_MAC)
      if (io_surface_)
        IOSurfaceLock(io_surface_.get(), kIOSurfaceLockReadOnly, nullptr);
#endif
      if (captured_.dimensions() != surface_->imageInfo().dimensions())
        captured_.allocPixels(surface_->imageInfo());
      surface_->readPixels(captured_, 0, 0);
#if BUILDFLAG(IS_MAC)
      if (io_surface_)
        IOSurfaceUnlock(io_surface_.get(), kIOSurfaceLockReadOnly, nullptr);
#endif
      captured_dirty_ = false;
    }
    return captured_;
  }

  // The IOSurface holding the last composited frame (macOS; null elsewhere,
  // before the first frame, or on allocation fallback). Owned by this device:
  // valid until the next Resize or teardown.
  void* io_surface() const {
#if BUILDFLAG(IS_MAC)
    return have_frame_ ? io_surface_.get() : nullptr;
#else
    return nullptr;
#endif
  }

 private:
  mutable SkBitmap captured_;
  mutable bool captured_dirty_ = false;
  bool have_frame_ = false;
#if BUILDFLAG(IS_MAC)
  gfx::ScopedIOSurface io_surface_;
#endif
};

// ===================== MbDirectLayerTreeFrameSink =====================
// Port of ui::DirectLayerTreeFrameSink, software-only (no context provider, no widget /
// CALayer path).

MbDirectLayerTreeFrameSink::MbDirectLayerTreeFrameSink(
    const viz::FrameSinkId& frame_sink_id,
    viz::FrameSinkManagerImpl* frame_sink_manager,
    viz::Display* display,
    scoped_refptr<base::SingleThreadTaskRunner> compositor_task_runner,
    scoped_refptr<gpu::SharedImageInterface> shared_image_interface)
    : LayerTreeFrameSink(/*context_provider=*/nullptr,
                         /*worker_context_provider=*/nullptr,
                         std::move(compositor_task_runner),
                         std::move(shared_image_interface)),
      frame_sink_id_(frame_sink_id),
      frame_sink_manager_(frame_sink_manager),
      display_(display) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
}

MbDirectLayerTreeFrameSink::~MbDirectLayerTreeFrameSink() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  // Break the circular Display<->client reference before we are destroyed.
  display_->ResetDisplayClientForTesting(/*old_client=*/this);
}

bool MbDirectLayerTreeFrameSink::BindToClient(
    cc::LayerTreeFrameSinkClient* client) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  if (!cc::LayerTreeFrameSink::BindToClient(client))
    return false;

  support_ = std::make_unique<viz::CompositorFrameSinkSupport>(
      this, frame_sink_manager_, frame_sink_id_, /*is_root=*/true);
  begin_frame_source_ = std::make_unique<viz::ExternalBeginFrameSource>(this);
  client_->SetBeginFrameSource(begin_frame_source_.get());

  // Shares the Display's context; do not initialize a GL context here.
  display_->Initialize(this, frame_sink_manager_->surface_manager());
  support_->SetUpHitTest(display_);
  return true;
}

void MbDirectLayerTreeFrameSink::DetachFromClient() {
  client_->SetBeginFrameSource(nullptr);
  begin_frame_source_.reset();
  support_.reset();
  cc::LayerTreeFrameSink::DetachFromClient();
}

void MbDirectLayerTreeFrameSink::SubmitCompositorFrame(
    viz::CompositorFrame frame,
    bool hit_test_data_changed) {
  DCHECK(frame.metadata.begin_frame_ack.has_damage);
  DCHECK(frame.metadata.begin_frame_ack.frame_id.IsSequenceValid());

  if (frame.size_in_pixels() != last_swap_frame_size_ ||
      frame.device_scale_factor() != device_scale_factor_ ||
      !parent_local_surface_id_allocator_.HasValidLocalSurfaceId()) {
    parent_local_surface_id_allocator_.GenerateId();
    last_swap_frame_size_ = frame.size_in_pixels();
    device_scale_factor_ = frame.device_scale_factor();
    display_->SetLocalSurfaceId(
        parent_local_surface_id_allocator_.GetCurrentLocalSurfaceId(),
        device_scale_factor_);
  }

  std::optional<viz::HitTestRegionList> hit_test_region_list =
      client_->BuildHitTestData();
  if (!hit_test_region_list)
    hit_test_region_list = viz::HitTestRegionList();

  support_->SubmitCompositorFrame(
      parent_local_surface_id_allocator_.GetCurrentLocalSurfaceId(),
      std::move(frame), std::move(hit_test_region_list));
}

void MbDirectLayerTreeFrameSink::DidNotProduceFrame(
    const viz::BeginFrameAck& ack,
    cc::FrameSkippedReason reason) {
  DCHECK(!ack.has_damage);
  DCHECK(ack.frame_id.IsSequenceValid());
  support_->DidNotProduceFrame(ack);
}

void MbDirectLayerTreeFrameSink::NotifyNewLocalSurfaceIdExpectedWhilePaused() {
  support_->NotifyNewLocalSurfaceIdExpectedWhilePaused();
}

void MbDirectLayerTreeFrameSink::DisplayOutputSurfaceLost() {
  is_lost_ = true;
  client_->DidLoseLayerTreeFrameSink();
}

void MbDirectLayerTreeFrameSink::DisplayWillDrawAndSwap(
    bool will_draw_and_swap,
    viz::AggregatedRenderPassList* render_passes) {
  if (support_->GetHitTestAggregator())
    support_->GetHitTestAggregator()->Aggregate(display_->CurrentSurfaceId());
}

void MbDirectLayerTreeFrameSink::DidReceiveCompositorFrameAck(
    std::vector<viz::ReturnedResource> resources) {
  // PostTask so the client is notified on a fresh stack frame.
  compositor_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          &MbDirectLayerTreeFrameSink::DidReceiveCompositorFrameAckInternal,
          weak_factory_.GetWeakPtr(), std::move(resources)));
}

void MbDirectLayerTreeFrameSink::DidReceiveCompositorFrameAckInternal(
    std::vector<viz::ReturnedResource> resources) {
  client_->ReclaimResources(std::move(resources));
  if (base::FeatureList::IsEnabled(features::kNoCompositorFrameAcks))
    return;
  client_->DidReceiveCompositorFrameAck();
}

void MbDirectLayerTreeFrameSink::OnBeginFrame(
    const viz::BeginFrameArgs& args,
    const viz::FrameTimingDetailsMap& timing_details,
    std::vector<viz::ReturnedResource> resources) {
  if (!resources.empty())
    ReclaimResources(std::move(resources));
  for (const auto& pair : timing_details)
    client_->DidPresentCompositorFrame(pair.first, pair.second);

  if (!needs_begin_frames_) {
    DidNotProduceFrame(viz::BeginFrameAck(args, false),
                       cc::FrameSkippedReason::kNoDamage);
    return;
  }
  begin_frame_source_->OnBeginFrame(args);
}

void MbDirectLayerTreeFrameSink::ReclaimResources(
    std::vector<viz::ReturnedResource> resources) {
  client_->ReclaimResources(std::move(resources));
}

void MbDirectLayerTreeFrameSink::OnBeginFramePausedChanged(bool paused) {
  begin_frame_source_->OnSetBeginFrameSourcePaused(paused);
}

void MbDirectLayerTreeFrameSink::OnNeedsBeginFrames(bool needs_begin_frames) {
  needs_begin_frames_ = needs_begin_frames;
  support_->SetNeedsBeginFrame(needs_begin_frames);
}

// ===================== SoftwareCompositor =====================

SoftwareCompositor::SoftwareCompositor(const gfx::Size& size)
    : task_runner_(base::SingleThreadTaskRunner::GetCurrentDefault()),
      sync_point_manager_(std::make_unique<gpu::SyncPointManager>()),
      gpu_scheduler_(
          std::make_unique<gpu::Scheduler>(sync_point_manager_.get())),
      frame_sink_manager_(std::make_unique<viz::FrameSinkManagerImpl>(
          viz::FrameSinkManagerImpl::InitParams())),
      frame_sink_id_(1, 1),
      size_(size) {
  // cc rasters tiles THROUGH a gpu::SharedImageInterface into a SharedImageManager, and the
  // Display's SoftwareRenderer reads from the SAME manager. Source both from the shared in-process
  // GPU service (the one mb_webgl uses): an in-process GL context gives the SII (wrapped so cc's
  // software path accepts it), and the service's SharedImageManager (via its task executor) backs
  // the Display. If the holder is unavailable, fall back to a Display with no shared manager (the
  // hand-built-frame path still works; cc raster won't).
  gpu::SharedImageManager* shared_image_manager = nullptr;
  if (gpu::InProcessGpuThreadHolder* holder = GetSharedGpuThreadHolder()) {
    gl_context_ = std::make_unique<gpu::GLInProcessContext>();
    if (gl_context_->Initialize(holder->GetTaskExecutor(),
                                gpu::CONTEXT_TYPE_OPENGLES2) ==
        gpu::ContextResult::kSuccess) {
      shared_image_interface_ =
          base::WrapRefCounted(gl_context_->GetSharedImageInterface());
      shared_image_manager =
          holder->GetTaskExecutor()->shared_image_manager();
    } else {
      gl_context_.reset();
    }
  }

  auto device = std::make_unique<MbCapturingSoftwareOutputDevice>();
  output_device_ = device.get();
  auto output_surface =
      std::make_unique<viz::SoftwareOutputSurface>(std::move(device));
  auto overlay = std::make_unique<viz::OverlayProcessorStub>();
  auto begin_frame_source = std::make_unique<viz::StubBeginFrameSource>();
  auto scheduler = std::make_unique<viz::DisplayScheduler>(
      begin_frame_source.get(), task_runner_.get(), viz::PendingSwapParams(1));

  // The Display stores &debug_settings_ by pointer and references begin_frame_source via
  // its scheduler; debug_settings_ is a member and begin_frame_source is pinned here, both
  // outliving display_. RendererSettings is copied by the Display.
  begin_frame_source_keepalive_ = std::move(begin_frame_source);
  viz::RendererSettings settings;

  display_ = std::make_unique<viz::Display>(
      shared_image_manager, gpu_scheduler_.get(), settings,
      &debug_settings_, frame_sink_id_, /*gpu_dependency=*/nullptr,
      std::move(output_surface), std::move(overlay), std::move(scheduler),
      task_runner_);
}

SoftwareCompositor::~SoftwareCompositor() {
  display_.reset();
}

std::unique_ptr<cc::LayerTreeFrameSink> SoftwareCompositor::CreateFrameSink() {
  ++frame_sink_count_;
  return std::make_unique<MbDirectLayerTreeFrameSink>(
      frame_sink_id_, frame_sink_manager_.get(), display_.get(), task_runner_,
      shared_image_interface_);
}

void SoftwareCompositor::Resize(const gfx::Size& size) {
  size_ = size;
  if (display_)
    display_->Resize(size_);
}

const SkBitmap& SoftwareCompositor::DrawAndCapture() {
  // The in-process scheduler is begin-frame-driven; for headless we force one draw.
  display_->SetVisible(true);
  display_->Resize(size_);
  viz::DrawAndSwapParams params;
  params.expected_display_time = base::TimeTicks::Now();
  display_->DrawAndSwap(params);
  return captured_bitmap();
}

const SkBitmap& SoftwareCompositor::captured_bitmap() const {
  return output_device_->captured();
}

void* SoftwareCompositor::io_surface() const {
  return output_device_ ? output_device_->io_surface() : nullptr;
}

}  // namespace mb
