// mb_compositor3_probe — SOFTWARE COMPOSITOR bring-up, milestone C: drive the REAL in-process
// frame sink (viz::DirectLayerTreeFrameSink, a cc::LayerTreeFrameSink that submits its client's
// frame as the root surface of an in-process viz::Display) and confirm cc's frame-sink contract
// composites to a bitmap. Blink-free, testonly. This is the artifact blink's WebFrameWidgetImpl::
// AllocateNewLayerTreeFrameSink will return (milestone D); here a STUB cc::LayerTreeFrameSink
// Client stands in for cc::LayerTreeHostImpl (avoiding the 37-virtual cc host delegates, which
// blink's LayerTreeView provides in the integration).
//
// Sets up the milestone-B Display, binds the sink to a stub client (the sink then creates its
// CompositorFrameSinkSupport + initializes the Display), submits a CompositorFrame (one yellow
// quad) THROUGH THE SINK, forces a Display draw, and checks the captured center pixel is yellow.

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
#include "components/viz/service/frame_sinks/frame_sink_manager_impl.h"
#include "components/viz/service/gl/gpu_service_impl.h"
#include "components/viz/test/compositor_frame_helpers.h"
#include "components/viz/test/fake_output_surface.h"
#include "components/viz/test/test_gpu_service_holder.h"
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
  base::ThreadPoolInstance::CreateAndStartWithDefaultParams("mb_compositor3_probe");

  base::CommandLine* cmd = base::CommandLine::ForCurrentProcess();
  cmd->AppendSwitchASCII(switches::kUseGL, gl::kGLImplementationANGLEName);
  cmd->AppendSwitchASCII(switches::kUseANGLE,
                         gl::kANGLEImplementationSwiftShaderName);
  cmd->AppendSwitch(switches::kEnableUnsafeSwiftShader);
  if (!gl::init::InitializeGLOneOff(gl::GpuPreference::kDefault)) {
    fprintf(stderr, "mb_compositor3_probe: InitializeGLOneOff FAILED\n");
    return 1;
  }

  viz::TestGpuServiceHolder* gpu_holder =
      viz::TestGpuServiceHolder::GetInstance();
  auto* gpu_service = gpu_holder->gpu_service();

  viz::FrameSinkManagerImpl manager{viz::FrameSinkManagerImpl::InitParams()};
  const viz::FrameSinkId frame_sink_id(1, 1);
  scoped_refptr<base::NullTaskRunner> task_runner =
      base::MakeRefCounted<base::NullTaskRunner>();

  // Display (the sink will Initialize it + create its own CompositorFrameSinkSupport).
  auto device = std::make_unique<MbCapturingSoftwareOutputDevice>();
  MbCapturingSoftwareOutputDevice* device_ptr = device.get();
  auto output_surface =
      std::make_unique<viz::FakeSoftwareOutputSurface>(std::move(device));
  auto overlay = std::make_unique<viz::OverlayProcessorStub>();
  auto begin_frame_source = std::make_unique<viz::StubBeginFrameSource>();
  auto scheduler = std::make_unique<viz::DisplayScheduler>(
      begin_frame_source.get(), task_runner.get(), viz::PendingSwapParams(1));
  viz::RendererSettings settings;
  viz::DebugRendererSettings debug;
  auto display = std::make_unique<viz::Display>(
      gpu_service->shared_image_manager(), gpu_service->gpu_scheduler(),
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
    fprintf(stderr, "mb_compositor3_probe: BindToClient FAILED\n");
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
    printf("mb_compositor3_probe: sink-routed center pixel = R%d G%d B%d\n",
           SkColorGetR(c), SkColorGetG(c), SkColorGetB(c));
  } else {
    fprintf(stderr, "mb_compositor3_probe: no captured bitmap\n");
  }

  sink->DetachFromClient();
  sink.reset();
  display.reset();
  viz::TestGpuServiceHolder::ResetInstance();
  base::ThreadPoolInstance::Get()->Shutdown();

  printf("mb_compositor3_probe: %s (cc frame sink -> viz::Display -> bitmap)\n",
         ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
