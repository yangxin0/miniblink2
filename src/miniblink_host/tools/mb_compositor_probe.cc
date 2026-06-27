// mb_compositor_probe — SOFTWARE COMPOSITOR bring-up, milestone A: prove the in-process viz
// SOFTWARE render path (CompositorRenderPass -> pixels) works, the backend a software
// LayerTreeFrameSink will wrap. Blink-free, like mb_gl_probe / mb_dawn_probe milestone A.
//
// Mirrors cc::PixelTest::SetUpSoftwareRenderer + the SoftwareRenderer SolidColorQuad test: a
// viz::SoftwareRenderer over a SoftwareOutputDevice draws a hand-crafted AggregatedRenderPass
// (one yellow solid-color quad filling the viewport), then reads the result back via the
// output surface and checks the center pixel is yellow. No cc::LayerTreeHost or scheduler —
// just the render-pass->bitmap rasterization. testonly: links cc/viz test_support.

#include <cstdio>
#include <memory>
#include <optional>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_executor.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/test/test_timeouts.h"
#include "cc/test/fake_output_surface_client.h"
#include "cc/test/pixel_test_output_surface.h"
#include "cc/test/render_pass_test_utils.h"
#include "components/viz/common/display/renderer_settings.h"
#include "components/viz/common/quads/aggregated_render_pass.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "components/viz/service/display/display_resource_provider_software.h"
#include "components/viz/service/gl/gpu_service_impl.h"
#include "components/viz/service/display/software_output_device.h"
#include "components/viz/service/display/software_renderer.h"
#include "components/viz/test/test_gpu_service_holder.h"
#include "mojo/core/embedder/embedder.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/display_color_spaces.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gl/gl_switches.h"
#include "ui/gl/init/gl_factory.h"

int main(int argc, char** argv) {
  base::AtExitManager at_exit;
  base::CommandLine::Init(argc, argv);
  TestTimeouts::Initialize();
  mojo::core::Init();
  base::SingleThreadTaskExecutor main_task_executor;
  base::ThreadPoolInstance::CreateAndStartWithDefaultParams("mb_compositor_probe");

  // The GPU service (used for the software shared-bitmap manager) reads the process-wide GL
  // impl; set ANGLE/SwiftShader on the main thread first (the mb_gpu_probe finding).
  base::CommandLine* cmd = base::CommandLine::ForCurrentProcess();
  cmd->AppendSwitchASCII(switches::kUseGL, gl::kGLImplementationANGLEName);
  cmd->AppendSwitchASCII(switches::kUseANGLE,
                         gl::kANGLEImplementationSwiftShaderName);
  cmd->AppendSwitch(switches::kEnableUnsafeSwiftShader);
  if (!gl::init::InitializeGLOneOff(gl::GpuPreference::kDefault)) {
    fprintf(stderr, "mb_compositor_probe: InitializeGLOneOff FAILED\n");
    return 1;
  }

  // --- Software renderer setup (cc::PixelTest::SetUpSoftwareRenderer) ---
  viz::TestGpuServiceHolder* gpu_holder = viz::TestGpuServiceHolder::GetInstance();
  cc::FakeOutputSurfaceClient output_surface_client;
  auto output_surface = std::make_unique<cc::PixelTestOutputSurface>(
      std::make_unique<viz::SoftwareOutputDevice>());
  output_surface->BindToClient(&output_surface_client);

  auto* gpu_service = gpu_holder->gpu_service();
  auto resource_provider = std::make_unique<viz::DisplayResourceProviderSoftware>(
      gpu_service->shared_image_manager(), gpu_service->gpu_scheduler());

  viz::RendererSettings renderer_settings;
  viz::DebugRendererSettings debug_settings;
  auto renderer = std::make_unique<viz::SoftwareRenderer>(
      &renderer_settings, &debug_settings, output_surface.get(),
      resource_provider.get(), /*overlay_processor=*/nullptr);
  renderer->Initialize();
  renderer->SetVisible(true);

  // --- A render pass: one yellow solid-color quad filling a 100x100 viewport ---
  const gfx::Size viewport(100, 100);
  const gfx::Rect rect(viewport);
  viz::AggregatedRenderPassList list;
  viz::AggregatedRenderPass* root =
      cc::AddRenderPass(&list, viz::AggregatedRenderPassId{1}, rect,
                        gfx::Transform());
  cc::AddQuad(root, rect, SkColors::kYellow);

  // --- Composite + read back the bitmap ---
  SkBitmap result;
  base::RunLoop run_loop;
  renderer->DrawFrame(&list, /*device_scale_factor=*/1.0f, viewport,
                      gfx::DisplayColorSpaces(),
                      /*surface_damage_rect_list=*/{},
                      /*tracked_element_rects=*/{});
  renderer->SwapBuffers(viz::DirectRenderer::SwapFrameData());
  output_surface->ReadbackForTesting(base::BindOnce(
      [](base::OnceClosure quit, SkBitmap* out,
         std::unique_ptr<viz::CopyOutputResult> r) {
        if (r && !r->IsEmpty()) {
          auto scoped = r->ScopedAccessSkBitmap();
          auto m = scoped.GetOutScopedBitmapAndMetadata();
          if (m.has_value())
            *out = m->bitmap;
        }
        std::move(quit).Run();
      },
      run_loop.QuitClosure(), &result));
  run_loop.Run();

  if (result.isNull() || result.width() != 100 || result.height() != 100) {
    fprintf(stderr, "mb_compositor_probe: readback FAILED (null/size bitmap)\n");
    return 1;
  }
  const SkColor c = result.getColor(50, 50);
  const bool ok = SkColorGetR(c) > 200 && SkColorGetG(c) > 200 &&
                  SkColorGetB(c) < 60;  // yellow
  printf("mb_compositor_probe: composited center pixel = R%d G%d B%d\n",
         SkColorGetR(c), SkColorGetG(c), SkColorGetB(c));

  renderer.reset();
  resource_provider.reset();
  output_surface.reset();
  viz::TestGpuServiceHolder::ResetInstance();
  base::ThreadPoolInstance::Get()->Shutdown();

  printf("mb_compositor_probe: %s (in-process viz software render-pass -> bitmap)\n",
         ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
