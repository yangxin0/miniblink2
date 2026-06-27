// mb_webgpu2_probe — WebGPU bring-up, milestone C1: prove the PRODUCTION (non-testonly)
// in-process WebGPU path works end to end, so it can later be wired into MbPlatform
// (milestone C2). Unlike mb_webgpu_probe (which used the testonly WebGPUInProcessContext +
// viz::TestGpuServiceHolder + a manual GPU-side poll), this links ONLY non-testonly deps:
//   - InProcessGpuThreadHolder + InProcessCommandBuffer (non-testonly via patches/0006),
//   - miniblink::MbWebGPUInProcessContext (our production context, real task runner),
//   - the dawn-wire client.
// The WebGPU decoder polls Dawn's events INLINE after each batch of wire commands
// (WebGPUDecoderImpl::HandleDawnCommands -> PerformPollingWork), so the client only needs
// to keep flushing and run its own message loop — no external decoder poke. A green run
// means milestone C's hardest piece (a non-test in-process WebGPU context that resolves
// requestAdapter/requestDevice) is solved. Blink-free. Exits 0 on success.

#include <cstdio>
#include <memory>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_executor.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "gpu/command_buffer/client/webgpu_implementation.h"
#include "gpu/command_buffer/client/webgpu_interface.h"
#include "gpu/config/gpu_preferences.h"
#include "gpu/ipc/in_process_gpu_thread_holder.h"
#include "third_party/blink/renderer/miniblink_host/platform/mb_webgpu_context.h"
#include "ui/gl/gl_display.h"
#include "ui/gl/gl_switches.h"
#include "ui/gl/init/gl_factory.h"

#include <dawn/dawn_proc.h>
#include <dawn/dawn_thread_dispatch_proc.h>
#include <dawn/native/DawnNative.h>
#include <dawn/wire/WireClient.h>
#include <webgpu/webgpu_cpp.h>

namespace {

// Flush client commands + run the message loop until *done flips. The decoder polls Dawn
// inline on the GPU thread after each flushed wire-command batch, so replies arrive on
// this thread's runner; RunUntilIdle delivers them.
bool PumpUntil(gpu::webgpu::WebGPUInterface* webgpu, const bool* done) {
  for (int spins = 0; !*done && spins < 5000; ++spins) {
    webgpu->FlushCommands();
    base::RunLoop().RunUntilIdle();
    base::PlatformThread::Sleep(base::Milliseconds(1));
  }
  return *done;
}

}  // namespace

int main(int argc, char** argv) {
  base::AtExitManager at_exit;
  base::CommandLine::Init(argc, argv);
  base::SingleThreadTaskExecutor main_task_executor;
  base::ThreadPoolInstance::CreateAndStartWithDefaultParams("mb_webgpu2_probe");

  base::CommandLine* cmd = base::CommandLine::ForCurrentProcess();
  cmd->AppendSwitchASCII(switches::kUseGL, gl::kGLImplementationANGLEName);
  cmd->AppendSwitchASCII(switches::kUseANGLE,
                         gl::kANGLEImplementationSwiftShaderName);
  cmd->AppendSwitch(switches::kEnableUnsafeSwiftShader);
  if (!gl::init::InitializeGLOneOff(gl::GpuPreference::kDefault)) {
    fprintf(stderr, "mb_webgpu2_probe: InitializeGLOneOff FAILED\n");
    return 1;
  }

  // Process-wide in-process GPU thread with WebGPU enabled (the prefs mb_webgpu_probe
  // established). Non-testonly via patches/0006.
  auto holder = std::make_unique<gpu::InProcessGpuThreadHolder>();
  gpu::GpuPreferences* prefs = holder->GetGpuPreferences();
  prefs->enable_webgpu = true;
  prefs->use_passthrough_cmd_decoder = true;
  // The WebGPU decoder owns its OWN dawn::native::Instance, independent of Skia's GPU
  // backend. InProcessGpuThreadHolder doesn't set up a dawn_context_provider, so the
  // default kGraphiteDawn Skia backend CHECK-fails in SharedContextState::InitializeGanesh;
  // use Ganesh-over-GL for the shared context (mb_gpu_probe's finding) while WebGPU keeps
  // its own Dawn.
  prefs->gr_context_type = gpu::GrContextType::kGL;
  prefs->enable_unsafe_webgpu = true;
  prefs->disabled_dawn_features_list = {"adapter_blocklist"};

  auto ctx = std::make_unique<miniblink::MbWebGPUInProcessContext>();
  gpu::ContextResult r = ctx->Initialize(holder->GetTaskExecutor());
  if (r != gpu::ContextResult::kSuccess) {
    fprintf(stderr, "mb_webgpu2_probe: MbWebGPUInProcessContext init FAILED (%d)\n",
            static_cast<int>(r));
    return 1;
  }

  gpu::webgpu::WebGPUInterface* webgpu = ctx->GetImplementation();
  // Dawn proc-table coordination for an in-process client+service (mirrors viz::
  // TestGpuServiceHolder): the GLOBAL table is the thread dispatcher; service threads
  // with no explicit procs fall back to dawn::native (DefaultThreadProcs); this (client)
  // thread uses the wire-client procs. blink sets this up itself in milestone C2.
  dawnProcSetProcs(&dawnThreadDispatchProcTable);
  dawnProcSetDefaultThreadProcs(&dawn::native::GetProcs());
  dawnProcSetPerThreadProcs(&dawn::wire::client::GetProcs());
  wgpu::Instance instance(webgpu->GetAPIChannel()->GetWGPUInstance());

  // Request an adapter through the production context.
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
  PumpUntil(webgpu, &adapter_done);
  if (!adapter) {
    fprintf(stderr, "mb_webgpu2_probe: RequestAdapter returned NO adapter\n");
    return 1;
  }
  wgpu::AdapterInfo info = {};
  adapter.GetInfo(&info);
  printf("mb_webgpu2_probe: production adapter backend=%d type=%d device=%.*s\n",
         static_cast<int>(info.backendType), static_cast<int>(info.adapterType),
         static_cast<int>(info.device.length), info.device.data);

  // Request a device through the production context.
  bool device_done = false;
  wgpu::Device device;
  adapter.RequestDevice(
      nullptr, wgpu::CallbackMode::AllowSpontaneous,
      [&](wgpu::RequestDeviceStatus status, wgpu::Device d, wgpu::StringView) {
        if (status == wgpu::RequestDeviceStatus::Success)
          device = std::move(d);
        device_done = true;
      });
  PumpUntil(webgpu, &device_done);

  const bool ok = device != nullptr && device.GetQueue() != nullptr;
  printf("mb_webgpu2_probe: production device %s\n", ok ? "OK" : "NULL");

  device = nullptr;
  adapter = nullptr;
  instance = nullptr;
  ctx.reset();
  holder.reset();
  base::ThreadPoolInstance::Get()->Shutdown();

  printf("mb_webgpu2_probe: %s (production non-test in-process WebGPU context)\n",
         ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
