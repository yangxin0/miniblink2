// mb_compositor4_probe — SOFTWARE COMPOSITOR bring-up, milestone D1: same end-to-end path as
// milestone C (cc frame sink -> viz::Display -> bitmap) but assembled entirely from PRODUCTION
// (non-test) viz/gpu components, to prove the viz::Display the host LIBRARY will build is viable
// without TestGpuServiceHolder. Differences vs C:
//   * gpu primitives: standalone gpu::SyncPointManager + gpu::Scheduler + gpu::SharedImageManager
//     (NOT viz::TestGpuServiceHolder::gpu_service()).
//   * output surface: real viz::SoftwareOutputSurface (NOT viz::FakeSoftwareOutputSurface).
// Only the frame sink (ui::DirectLayerTreeFrameSink) and the CompositorFrameBuilder stay testonly
// here — the probe itself is testonly; the host integration (D2) de-testonly's the sink. A STUB
// cc::LayerTreeFrameSinkClient stands in for cc::LayerTreeHostImpl (blink's LayerTreeView provides
// the real one in the integration).
//
// Binds the sink to the stub client (the sink creates its CompositorFrameSinkSupport + initializes
// the Display), submits a CompositorFrame (one yellow quad) THROUGH THE SINK, forces a Display
// draw, and checks the captured center pixel is yellow.

#include <cstdio>
#include <memory>
#include <optional>
#include <vector>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/single_thread_task_executor.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/test/null_task_runner.h"
#include "base/test/test_timeouts.h"
#include "base/time/time.h"
#include "cc/trees/layer_tree_frame_sink_client.h"
#include "cc/trees/managed_memory_policy.h"
#include "components/viz/common/display/renderer_settings.h"
#include "components/viz/common/frame_sinks/begin_frame_args.h"
#include "components/viz/common/frame_sinks/begin_frame_source.h"
#include "components/viz/common/quads/compositor_render_pass.h"
#include "components/viz/common/quads/solid_color_draw_quad.h"
#include "components/viz/common/surfaces/frame_sink_id.h"
#include "components/viz/service/display/display.h"
#include "components/viz/service/display/display_scheduler.h"
#include "components/viz/service/display/overlay_processor_stub.h"
#include "components/viz/service/display/software_output_device.h"
#include "components/viz/service/display_embedder/software_output_surface.h"
#include "components/viz/service/frame_sinks/frame_sink_manager_impl.h"
#include "components/viz/test/compositor_frame_helpers.h"
#include "gpu/command_buffer/service/scheduler.h"
#include "gpu/command_buffer/service/shared_image/shared_image_manager.h"
#include "gpu/command_buffer/service/sync_point_manager.h"
#include "mojo/core/embedder/embedder.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "ui/compositor/test/direct_layer_tree_frame_sink.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/geometry/transform.h"
#include "ui/gl/gl_switches.h"
#include "ui/gl/init/gl_factory.h"

namespace {

class MbSinkClient : public cc::LayerTreeFrameSinkClient {
 public:
  void SetBeginFrameSource(viz::BeginFrameSource*) override {}
  std::optional<viz::HitTestRegionList> BuildHitTestData() override {
    return std::nullopt;
  }
  void ReclaimResources(std::vector<viz::ReturnedResource>) override {}
  void SetTreeActivationCallback(base::RepeatingClosure) override {}
  void DidReceiveCompositorFrameAck() override {}
  void DidPresentCompositorFrame(uint32_t,
                                 const viz::FrameTimingDetails&) override {}
  void DidLoseLayerTreeFrameSink() override {}
  void OnDraw(const gfx::Transform&, const gfx::Rect&, bool, bool) override {}
  void SetMemoryPolicy(const cc::ManagedMemoryPolicy&) override {}
  void SetExternalTilePriorityConstraints(const gfx::Rect&,
                                          const gfx::Transform&) override {}
};

class MbCapturingSoftwareOutputDevice : public viz::SoftwareOutputDevice {
 public:
  void EndPaint() override {
    viz::SoftwareOutputDevice::EndPaint();
    if (surface_)
      surface_->makeImageSnapshot()->asLegacyBitmap(&captured);
  }
  SkBitmap captured;
};

}  // namespace

int main(int argc, char** argv) {
  base::AtExitManager at_exit;
  base::CommandLine::Init(argc, argv);
  TestTimeouts::Initialize();
  mojo::core::Init();
  base::SingleThreadTaskExecutor main_task_executor;
  base::ThreadPoolInstance::CreateAndStartWithDefaultParams("mb_compositor4_probe");

  base::CommandLine* cmd = base::CommandLine::ForCurrentProcess();
  cmd->AppendSwitchASCII(switches::kUseGL, gl::kGLImplementationANGLEName);
  cmd->AppendSwitchASCII(switches::kUseANGLE,
                         gl::kANGLEImplementationSwiftShaderName);
  cmd->AppendSwitch(switches::kEnableUnsafeSwiftShader);
  if (!gl::init::InitializeGLOneOff(gl::GpuPreference::kDefault)) {
    fprintf(stderr, "mb_compositor4_probe: InitializeGLOneOff FAILED\n");
    return 1;
  }

  // PRODUCTION gpu primitives for the software Display's DisplayResourceProviderSoftware — a
  // standalone SyncPointManager/Scheduler/SharedImageManager, no GPU service or GL context (a
  // software display owns no GPU-backed resources). This is exactly what the host library will own.
  auto sync_point_manager = std::make_unique<gpu::SyncPointManager>();
  auto gpu_scheduler =
      std::make_unique<gpu::Scheduler>(sync_point_manager.get());
  auto shared_image_manager =
      std::make_unique<gpu::SharedImageManager>(/*thread_safe=*/false);

  viz::FrameSinkManagerImpl manager{viz::FrameSinkManagerImpl::InitParams()};
  const viz::FrameSinkId frame_sink_id(1, 1);
  scoped_refptr<base::NullTaskRunner> task_runner =
      base::MakeRefCounted<base::NullTaskRunner>();

  // Display (the sink will Initialize it + create its own CompositorFrameSinkSupport).
  auto device = std::make_unique<MbCapturingSoftwareOutputDevice>();
  MbCapturingSoftwareOutputDevice* device_ptr = device.get();
  auto output_surface =
      std::make_unique<viz::SoftwareOutputSurface>(std::move(device));
  auto overlay = std::make_unique<viz::OverlayProcessorStub>();
  auto begin_frame_source = std::make_unique<viz::StubBeginFrameSource>();
  auto scheduler = std::make_unique<viz::DisplayScheduler>(
      begin_frame_source.get(), task_runner.get(), viz::PendingSwapParams(1));
  viz::RendererSettings settings;
  viz::DebugRendererSettings debug;
  auto display = std::make_unique<viz::Display>(
      shared_image_manager.get(), gpu_scheduler.get(),
      settings, &debug, frame_sink_id, /*gpu_dependency=*/nullptr,
      std::move(output_surface), std::move(overlay), std::move(scheduler),
      task_runner);

  // The REAL in-process frame sink.
  auto sink = std::make_unique<ui::DirectLayerTreeFrameSink>(
      frame_sink_id, &manager, display.get(),
      /*context_provider=*/nullptr, /*worker_context_provider=*/nullptr,
      task_runner);
  MbSinkClient client;
  if (!sink->BindToClient(&client)) {
    fprintf(stderr, "mb_compositor4_probe: BindToClient FAILED\n");
    return 1;
  }
  display->SetVisible(true);
  // cc normally drives the Display size via the frame-sink flow; we bypass that with a
  // direct DrawAndSwap, so size the Display ourselves.
  display->Resize(gfx::Size(100, 100));

  // One render pass, one yellow solid-color quad filling the viewport.
  const gfx::Rect rect(0, 0, 100, 100);
  viz::CompositorRenderPassList pass_list;
  auto pass = viz::CompositorRenderPass::Create();
  pass->SetNew(viz::CompositorRenderPassId{1}, rect, rect, gfx::Transform());
  viz::SharedQuadState* sqs = pass->CreateAndAppendSharedQuadState();
  sqs->SetAll(gfx::Transform(), rect, rect, gfx::MaskFilterInfo(),
              /*clip=*/std::nullopt, /*contents_opaque=*/true, /*opacity=*/1.0f,
              SkBlendMode::kSrcOver, /*sorting_context=*/0, /*layer_id=*/0u,
              /*fast_rounded_corner=*/false);
  auto* quad = pass->CreateAndAppendDrawQuad<viz::SolidColorDrawQuad>();
  quad->SetNew(sqs, rect, rect, SkColors::kYellow, false);
  pass_list.push_back(std::move(pass));

  viz::CompositorFrame frame =
      viz::CompositorFrameBuilder()
          .SetRenderPassList(std::move(pass_list))
          .SetBeginFrameAck(viz::BeginFrameAck::CreateManualAckWithDamage())
          .SetDeviceScaleFactor(1.0f)
          .Build();
  // Submit THROUGH the sink (it forwards to its support / the Display's root surface).
  sink->SubmitCompositorFrame(std::move(frame), /*hit_test_data_changed=*/false);

  // Force the Display to draw (we own it; the scheduler is begin-frame-driven).
  viz::DrawAndSwapParams params;
  params.expected_display_time = base::TimeTicks::Now();
  display->DrawAndSwap(params);

  const SkBitmap& b = device_ptr->captured;
  bool ok = false;
  if (!b.isNull() && b.width() >= 100 && b.height() >= 100) {
    const SkColor c = b.getColor(50, 50);
    ok = SkColorGetR(c) > 200 && SkColorGetG(c) > 200 && SkColorGetB(c) < 60;
    printf("mb_compositor4_probe: sink-routed center pixel = R%d G%d B%d\n",
           SkColorGetR(c), SkColorGetG(c), SkColorGetB(c));
  } else {
    fprintf(stderr, "mb_compositor4_probe: no captured bitmap\n");
  }

  sink->DetachFromClient();
  sink.reset();
  display.reset();
  shared_image_manager.reset();
  gpu_scheduler.reset();
  sync_point_manager.reset();
  base::ThreadPoolInstance::Get()->Shutdown();

  printf("mb_compositor4_probe: %s (PRODUCTION cc frame sink -> viz::Display -> bitmap)\n",
         ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
