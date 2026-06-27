// mb_compositor5_probe — SOFTWARE COMPOSITOR bring-up, milestone D2a: drive the host-library
// component mb::SoftwareCompositor (platform/mb_compositor.{h,cc}) end to end. Unlike milestones
// A–D1 (which assembled the viz machinery inline in the probe), this links the REAL component the
// host library ships: mb::SoftwareCompositor owns the production gpu primitives + viz::Display, and
// mb::MbDirectLayerTreeFrameSink is the frame sink blink's WebFrameWidgetImpl will return.
//
// The component .cc is compiled into BOTH the host library (miniblink_host) and this probe — the
// host-lib build proves it links into the real library; this probe proves it composites at runtime.
// Blink-free, testonly: a STUB cc::LayerTreeFrameSinkClient stands in for cc::LayerTreeHostImpl.
//
// Creates the compositor, gets a frame sink, binds it to the stub client (the sink creates its
// CompositorFrameSinkSupport + initializes the component's Display), submits a yellow-quad frame
// THROUGH the sink, then asks the component to DrawAndCapture; PASS if the center pixel is yellow.

#include <cstdio>
#include <memory>
#include <optional>
#include <vector>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/task/single_thread_task_executor.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/test/test_timeouts.h"
#include "cc/trees/layer_tree_frame_sink_client.h"
#include "cc/trees/managed_memory_policy.h"
#include "components/viz/common/frame_sinks/begin_frame_args.h"
#include "components/viz/common/quads/compositor_render_pass.h"
#include "components/viz/common/quads/solid_color_draw_quad.h"
#include "components/viz/test/compositor_frame_helpers.h"
#include "miniblink_host/platform/mb_compositor.h"
#include "mojo/core/embedder/embedder.h"
#include "third_party/skia/include/core/SkBitmap.h"
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

}  // namespace

int main(int argc, char** argv) {
  base::AtExitManager at_exit;
  base::CommandLine::Init(argc, argv);
  TestTimeouts::Initialize();
  mojo::core::Init();
  base::SingleThreadTaskExecutor main_task_executor;
  base::ThreadPoolInstance::CreateAndStartWithDefaultParams("mb_compositor5_probe");

  base::CommandLine* cmd = base::CommandLine::ForCurrentProcess();
  cmd->AppendSwitchASCII(switches::kUseGL, gl::kGLImplementationANGLEName);
  cmd->AppendSwitchASCII(switches::kUseANGLE,
                         gl::kANGLEImplementationSwiftShaderName);
  cmd->AppendSwitch(switches::kEnableUnsafeSwiftShader);
  if (!gl::init::InitializeGLOneOff(gl::GpuPreference::kDefault)) {
    fprintf(stderr, "mb_compositor5_probe: InitializeGLOneOff FAILED\n");
    return 1;
  }

  // The host-library component under test.
  mb::SoftwareCompositor compositor(gfx::Size(100, 100));
  std::unique_ptr<cc::LayerTreeFrameSink> sink = compositor.CreateFrameSink();
  MbSinkClient client;
  if (!sink->BindToClient(&client)) {
    fprintf(stderr, "mb_compositor5_probe: BindToClient FAILED\n");
    return 1;
  }

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
  sink->SubmitCompositorFrame(std::move(frame), /*hit_test_data_changed=*/false);

  const SkBitmap& b = compositor.DrawAndCapture();
  bool ok = false;
  if (!b.isNull() && b.width() >= 100 && b.height() >= 100) {
    const SkColor c = b.getColor(50, 50);
    ok = SkColorGetR(c) > 200 && SkColorGetG(c) > 200 && SkColorGetB(c) < 60;
    printf("mb_compositor5_probe: component center pixel = R%d G%d B%d\n",
           SkColorGetR(c), SkColorGetG(c), SkColorGetB(c));
  } else {
    fprintf(stderr, "mb_compositor5_probe: no captured bitmap\n");
  }

  sink->DetachFromClient();
  sink.reset();
  base::ThreadPoolInstance::Get()->Shutdown();

  printf("mb_compositor5_probe: %s (host-lib mb::SoftwareCompositor -> bitmap)\n",
         ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
