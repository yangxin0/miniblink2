// mb_webgpu_probe — WebGPU bring-up, milestone B: stand up an IN-PROCESS WebGPU command
// buffer over the Dawn device proved in milestone A (mb_dawn_probe), and drive it through
// gpu::webgpu::WebGPUInterface + the dawn-wire client — exactly the path blink's WebGPU
// provider consumes (the analog of mb_gpu_probe driving GLES2Interface for WebGL). We
// request an adapter AND a device through the wire and confirm both come back. Blink-free.
//
// Mirrors gpu/command_buffer/tests/webgpu_test.cc::Initialize(): a viz::TestGpuService-
// Holder owns the in-process GPU service (Graphite+Dawn), WebGPUInProcessContext wires the
// client command buffer + WebGPUImplementation, and the wire client procs are installed
// per-thread. The client task runner is a TestSimpleTaskRunner, so we pump it manually
// while scheduling the GPU service's WebGPU polling, until the async callbacks fire.
//
// Forces ANGLE/SwiftShader (headless, reproducible — same software path as mb_gl_probe).
// Exits 0 on success. testonly: it links //gpu:test_support (WebGPUInProcessContext).

#include <cstdio>
#include <memory>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/task/single_thread_task_executor.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/test/test_simple_task_runner.h"
#include "base/test/test_timeouts.h"
#include "base/threading/platform_thread.h"
#include "components/viz/test/test_gpu_service_holder.h"
#include "gpu/command_buffer/client/webgpu_implementation.h"
#include "gpu/command_buffer/client/webgpu_interface.h"
#include "gpu/command_buffer/service/webgpu_decoder.h"
#include "gpu/config/gpu_preferences.h"
#include "gpu/ipc/in_process_command_buffer.h"
#include "gpu/ipc/webgpu_in_process_context.h"
#include "mojo/core/embedder/embedder.h"
#include "ui/gl/gl_display.h"
#include "ui/gl/gl_switches.h"
#include "ui/gl/init/gl_factory.h"

#include <dawn/dawn_proc.h>
#include <dawn/dawn_thread_dispatch_proc.h>
#include <dawn/wire/WireClient.h>
#include <webgpu/webgpu_cpp.h>

namespace {

// Spin the manual client/GPU pump until `*done` flips (or we give up). Returns true if it
// completed. Each turn: drain the client TestSimpleTaskRunner (delivers wire replies),
// poke the GPU service to do its WebGPU polling (drives Dawn's async events), and flush.
bool PumpUntil(gpu::WebGPUInProcessContext* ctx,
               viz::TestGpuServiceHolder* holder,
               gpu::webgpu::WebGPUInterface* webgpu,
               gpu::webgpu::WebGPUDecoder* decoder,
               const bool* done) {
  for (int spins = 0; !*done && spins < 5000; ++spins) {
    ctx->GetTaskRunner()->RunPendingTasks();
    holder->ScheduleGpuMainTask(base::BindOnce(
        [](gpu::webgpu::WebGPUDecoder* d) {
          if (d->HasPollingWork())
            d->PerformPollingWork();
        },
        decoder));
    webgpu->FlushCommands();
    base::PlatformThread::Sleep(base::Milliseconds(1));
  }
  return *done;
}

}  // namespace

int main(int argc, char** argv) {
  base::AtExitManager at_exit;
  base::CommandLine::Init(argc, argv);
  TestTimeouts::Initialize();
  mojo::core::Init();
  base::SingleThreadTaskExecutor main_task_executor;
  base::ThreadPoolInstance::CreateAndStartWithDefaultParams("mb_webgpu_probe");

  // Headless software GL (ANGLE over SwiftShader); the GPU service reads the process-wide
  // GL implementation, so set it on the main thread first (mb_gpu_probe's finding).
  base::CommandLine* cmd = base::CommandLine::ForCurrentProcess();
  cmd->AppendSwitchASCII(switches::kUseGL, gl::kGLImplementationANGLEName);
  cmd->AppendSwitchASCII(switches::kUseANGLE,
                         gl::kANGLEImplementationSwiftShaderName);
  cmd->AppendSwitch(switches::kEnableUnsafeSwiftShader);
  if (!gl::init::InitializeGLOneOff(gl::GpuPreference::kDefault)) {
    fprintf(stderr, "mb_webgpu_probe: InitializeGLOneOff FAILED\n");
    return 1;
  }

  gpu::GpuPreferences prefs;
  prefs.enable_webgpu = true;
  prefs.use_passthrough_cmd_decoder = true;
  // This Chromium is Graphite+Dawn; use the Dawn-backed Skia path the WebGPU service wants.
  prefs.gr_context_type = gpu::GrContextType::kGraphiteDawn;
  prefs.enable_unsafe_webgpu = true;
  // Allow the SwiftShader fallback adapter (otherwise it's blocklisted).
  prefs.disabled_dawn_features_list = {"adapter_blocklist"};

  auto holder = std::make_unique<viz::TestGpuServiceHolder>(prefs);

  auto ctx = std::make_unique<gpu::WebGPUInProcessContext>();
  gpu::ContextResult r = ctx->Initialize(holder->task_executor());
  if (r != gpu::ContextResult::kSuccess) {
    fprintf(stderr,
            "mb_webgpu_probe: WebGPUInProcessContext::Initialize FAILED (%d)\n",
            static_cast<int>(r));
    return 1;
  }

  gpu::webgpu::WebGPUInterface* webgpu = ctx->GetImplementation();
  gpu::webgpu::WebGPUDecoder* decoder =
      ctx->GetCommandBufferForTest()->GetWebGPUDecoderForTest();

  // Install the dawn-wire client procs for this (the client) thread, then wrap the
  // wire's instance — every wgpu:: call now serializes over the command buffer.
  dawnProcSetPerThreadProcs(&dawn::wire::client::GetProcs());
  wgpu::Instance instance(webgpu->GetAPIChannel()->GetWGPUInstance());

  // 1) Request an adapter through the wire.
  wgpu::RequestAdapterOptions ra_opts = {};
  ra_opts.featureLevel = wgpu::FeatureLevel::Core;
  bool adapter_done = false;
  wgpu::Adapter adapter;
  instance.RequestAdapter(
      &ra_opts, wgpu::CallbackMode::AllowSpontaneous,
      [&](wgpu::RequestAdapterStatus status, wgpu::Adapter a, wgpu::StringView) {
        if (status == wgpu::RequestAdapterStatus::Success)
          adapter = std::move(a);
        adapter_done = true;
      });
  webgpu->FlushCommands();
  PumpUntil(ctx.get(), holder.get(), webgpu, decoder, &adapter_done);

  if (!adapter) {
    fprintf(stderr, "mb_webgpu_probe: RequestAdapter returned NO adapter\n");
    return 1;
  }
  wgpu::AdapterInfo info = {};
  adapter.GetInfo(&info);
  printf("mb_webgpu_probe: wire adapter backend=%d type=%d device=%.*s\n",
         static_cast<int>(info.backendType), static_cast<int>(info.adapterType),
         static_cast<int>(info.device.length), info.device.data);

  // 2) Request a device on that adapter through the wire.
  bool device_done = false;
  wgpu::Device device;
  adapter.RequestDevice(
      nullptr, wgpu::CallbackMode::AllowSpontaneous,
      [&](wgpu::RequestDeviceStatus status, wgpu::Device d, wgpu::StringView) {
        if (status == wgpu::RequestDeviceStatus::Success)
          device = std::move(d);
        device_done = true;
      });
  webgpu->FlushCommands();
  PumpUntil(ctx.get(), holder.get(), webgpu, decoder, &device_done);

  const bool ok = device != nullptr && device.GetQueue() != nullptr;
  printf("mb_webgpu_probe: device through the wire %s\n", ok ? "OK" : "NULL");

  // Tear down: drop the client objects before the service.
  device = nullptr;
  adapter = nullptr;
  instance = nullptr;
  ctx.reset();
  holder.reset();
  base::ThreadPoolInstance::Get()->Shutdown();

  printf("mb_webgpu_probe: %s (in-process WebGPU command buffer + dawn wire)\n",
         ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
