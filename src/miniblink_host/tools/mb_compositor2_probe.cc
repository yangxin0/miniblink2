// mb_compositor2_probe — SOFTWARE COMPOSITOR bring-up, milestone B: stand up a full in-process
// viz::Display (FrameSinkManagerImpl + CompositorFrameSinkSupport + BeginFrameSource +
// DisplayScheduler + a software OutputSurface) and composite a SUBMITTED CompositorFrame to a
// bitmap — the surface-aggregation + display-compositing path between a LayerTreeFrameSink and
// pixels. Blink-free, testonly; mirrors components/viz DisplayTest::SetUpSoftwareDisplay.
//
// Submits a frame with one yellow solid-color quad, draws via Display::DrawAndSwap, and reads
// the composited bitmap captured off the SoftwareOutputDevice. PASS if the center pixel is
// yellow. Builds on milestone A (SoftwareRenderer) by adding the Display/surface machinery.

#include <cstdio>
#include <memory>
#include <optional>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/single_thread_task_executor.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/test/null_task_runner.h"
#include "base/test/test_timeouts.h"
#include "base/time/time.h"
#include "components/viz/common/display/renderer_settings.h"
#include "components/viz/common/frame_sinks/begin_frame_source.h"
#include "components/viz/common/quads/compositor_render_pass.h"
#include "components/viz/common/quads/solid_color_draw_quad.h"
#include "components/viz/common/surfaces/frame_sink_id.h"
#include "components/viz/common/surfaces/parent_local_surface_id_allocator.h"
#include "components/viz/service/display/display.h"
#include "components/viz/service/display/display_client.h"
#include "components/viz/service/display/display_scheduler.h"
#include "components/viz/service/display/overlay_processor_stub.h"
#include "components/viz/service/display/software_output_device.h"
#include "components/viz/service/frame_sinks/compositor_frame_sink_support.h"
#include "components/viz/service/frame_sinks/frame_sink_manager_impl.h"
#include "components/viz/service/gl/gpu_service_impl.h"
#include "components/viz/service/surfaces/surface_manager.h"
#include "components/viz/test/compositor_frame_helpers.h"
#include "components/viz/test/fake_output_surface.h"
#include "components/viz/test/test_gpu_service_holder.h"
#include "mojo/core/embedder/embedder.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/geometry/transform.h"
#include "ui/gl/gl_switches.h"
#include "ui/gl/init/gl_factory.h"

namespace {

class MbDisplayClient : public viz::DisplayClient {
 public:
  void DisplayOutputSurfaceLost() override {}
  void DisplayWillDrawAndSwap(bool, viz::AggregatedRenderPassList*) override {}
  void DisplayDidDrawAndSwap() override {}
  void DisplayDidReceiveCALayerParams(gfx::CALayerParams) override {}
  void DisplayDidCompleteSwapWithSize(const gfx::Size&) override {}
  void DisplayAddChildWindowToBrowser(gpu::SurfaceHandle) override {}
  void SetWideColorEnabled(bool) override {}
};

// Captures the composited bitmap after each EndPaint (the base allocates surface_ and the
// renderer draws into it; we snapshot it).
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
  base::ThreadPoolInstance::CreateAndStartWithDefaultParams("mb_compositor2_probe");

  base::CommandLine* cmd = base::CommandLine::ForCurrentProcess();
  cmd->AppendSwitchASCII(switches::kUseGL, gl::kGLImplementationANGLEName);
  cmd->AppendSwitchASCII(switches::kUseANGLE,
                         gl::kANGLEImplementationSwiftShaderName);
  cmd->AppendSwitch(switches::kEnableUnsafeSwiftShader);
  if (!gl::init::InitializeGLOneOff(gl::GpuPreference::kDefault)) {
    fprintf(stderr, "mb_compositor2_probe: InitializeGLOneOff FAILED\n");
    return 1;
  }

  viz::TestGpuServiceHolder* gpu_holder =
      viz::TestGpuServiceHolder::GetInstance();
  auto* gpu_service = gpu_holder->gpu_service();

  viz::FrameSinkManagerImpl manager{viz::FrameSinkManagerImpl::InitParams()};
  const viz::FrameSinkId frame_sink_id(1, 1);
  auto support = std::make_unique<viz::CompositorFrameSinkSupport>(
      nullptr, &manager, frame_sink_id, /*is_root=*/true);

  scoped_refptr<base::NullTaskRunner> task_runner =
      base::MakeRefCounted<base::NullTaskRunner>();

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
  manager.RegisterBeginFrameSource(begin_frame_source.get(), frame_sink_id);

  MbDisplayClient client;
  display->Initialize(&client, manager.surface_manager());
  display->SetVisible(true);

  viz::ParentLocalSurfaceIdAllocator id_allocator;
  id_allocator.GenerateId();
  const viz::LocalSurfaceId lsid = id_allocator.GetCurrentLocalSurfaceId();
  display->SetLocalSurfaceId(lsid, 1.f);
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
  quad->SetNew(sqs, rect, rect, SkColors::kYellow, /*force_anti_aliasing_off=*/false);
  pass_list.push_back(std::move(pass));

  viz::CompositorFrame frame = viz::CompositorFrameBuilder()
                                   .SetRenderPassList(std::move(pass_list))
                                   .Build();
  support->SubmitCompositorFrame(lsid, std::move(frame));

  viz::DrawAndSwapParams params;
  params.expected_display_time = base::TimeTicks::Now();
  display->DrawAndSwap(params);

  const SkBitmap& b = device_ptr->captured;
  bool ok = false;
  if (!b.isNull() && b.width() >= 100 && b.height() >= 100) {
    const SkColor c = b.getColor(50, 50);
    ok = SkColorGetR(c) > 200 && SkColorGetG(c) > 200 && SkColorGetB(c) < 60;
    printf("mb_compositor2_probe: display-composited center pixel = R%d G%d B%d\n",
           SkColorGetR(c), SkColorGetG(c), SkColorGetB(c));
  } else {
    fprintf(stderr, "mb_compositor2_probe: no captured bitmap\n");
  }

  manager.UnregisterBeginFrameSource(begin_frame_source.get());
  display.reset();
  support.reset();
  viz::TestGpuServiceHolder::ResetInstance();
  base::ThreadPoolInstance::Get()->Shutdown();

  printf("mb_compositor2_probe: %s (in-process viz::Display -> bitmap)\n",
         ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
