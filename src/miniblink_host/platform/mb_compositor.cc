// mb_compositor.cc — see mb_compositor.h. MbDirectLayerTreeFrameSink is a port of
// ui::DirectLayerTreeFrameSink (testonly upstream); SoftwareCompositor assembles the
// production software viz::Display the frame sink draws into.

#include "miniblink_host/platform/mb_compositor.h"

#include <optional>
#include <utility>

#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/notreached.h"
#include "base/time/time.h"
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
#include "gpu/command_buffer/service/scheduler.h"
#include "gpu/command_buffer/service/shared_image/shared_image_manager.h"
#include "gpu/command_buffer/service/sync_point_manager.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkSurface.h"

namespace mb {

// ---- A SoftwareOutputDevice that snapshots the composited surface each EndPaint ----
class MbCapturingSoftwareOutputDevice : public viz::SoftwareOutputDevice {
 public:
  void EndPaint() override {
    viz::SoftwareOutputDevice::EndPaint();
    if (surface_)
      surface_->makeImageSnapshot()->asLegacyBitmap(&captured_);
  }
  const SkBitmap& captured() const { return captured_; }

 private:
  SkBitmap captured_;
};

// ===================== MbDirectLayerTreeFrameSink =====================
// Port of ui::DirectLayerTreeFrameSink, software-only (no context provider, no widget /
// CALayer path).

MbDirectLayerTreeFrameSink::MbDirectLayerTreeFrameSink(
    const viz::FrameSinkId& frame_sink_id,
    viz::FrameSinkManagerImpl* frame_sink_manager,
    viz::Display* display,
    scoped_refptr<base::SingleThreadTaskRunner> compositor_task_runner)
    : LayerTreeFrameSink(/*context_provider=*/nullptr,
                         /*worker_context_provider=*/nullptr,
                         std::move(compositor_task_runner),
                         /*shared_image_interface=*/nullptr),
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
      shared_image_manager_(
          std::make_unique<gpu::SharedImageManager>(/*thread_safe=*/false)),
      frame_sink_manager_(std::make_unique<viz::FrameSinkManagerImpl>(
          viz::FrameSinkManagerImpl::InitParams())),
      frame_sink_id_(1, 1),
      size_(size) {
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
      shared_image_manager_.get(), gpu_scheduler_.get(), settings,
      &debug_settings_, frame_sink_id_, /*gpu_dependency=*/nullptr,
      std::move(output_surface), std::move(overlay), std::move(scheduler),
      task_runner_);
}

SoftwareCompositor::~SoftwareCompositor() {
  display_.reset();
}

std::unique_ptr<cc::LayerTreeFrameSink> SoftwareCompositor::CreateFrameSink() {
  return std::make_unique<MbDirectLayerTreeFrameSink>(
      frame_sink_id_, frame_sink_manager_.get(), display_.get(), task_runner_);
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

}  // namespace mb
