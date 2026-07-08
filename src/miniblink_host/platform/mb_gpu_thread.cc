#include "miniblink_host/platform/mb_gpu_thread.h"

#include "base/command_line.h"
#include "build/build_config.h"
#include "gpu/config/gpu_preferences.h"
#include "gpu/ipc/in_process_gpu_thread_holder.h"
#include "ui/gl/gl_switches.h"
#include "ui/gl/init/gl_factory.h"

namespace mb {

gpu::InProcessGpuThreadHolder* GetSharedGpuThreadHolder() {
  static gpu::InProcessGpuThreadHolder* const holder =
      []() -> gpu::InProcessGpuThreadHolder* {
    base::CommandLine* cmd = base::CommandLine::ForCurrentProcess();
    if (!cmd->HasSwitch(switches::kUseGL))
      cmd->AppendSwitchASCII(switches::kUseGL, gl::kGLImplementationANGLEName);
    // Default WebGL to ANGLE's Metal backend (hardware GPU). Needs the Metal backend built
    // (angle_enable_metal=true, which needs full Xcode's `metal` shader compiler). Overridable
    // with --use-angle=swiftshader for headless/CI/no-GPU contexts (deterministic software).
    if (!cmd->HasSwitch(switches::kUseANGLE)) {
#if BUILDFLAG(IS_MAC)
      cmd->AppendSwitchASCII(switches::kUseANGLE,
                             gl::kANGLEImplementationMetalName);
#else
      // Windows: Metal doesn't exist; SwiftShader is the deterministic default
      // (works headless/RDP/CI). Override with --use-angle=d3d11 for hardware.
      cmd->AppendSwitchASCII(switches::kUseANGLE,
                             gl::kANGLEImplementationSwiftShaderName);
#endif
    }
    // Still allow SwiftShader if it's the one selected (the --use-angle override above).
    if (!cmd->HasSwitch(switches::kEnableUnsafeSwiftShader))
      cmd->AppendSwitch(switches::kEnableUnsafeSwiftShader);
    // The GPU thread reads the process-wide GL implementation; set it on the main
    // thread first (without this its surface factory NOTREACHEs on Mac).
    if (!gl::init::InitializeGLOneOff(gl::GpuPreference::kDefault))
      return nullptr;
    auto* h = new gpu::InProcessGpuThreadHolder();
    gpu::GpuPreferences* p = h->GetGpuPreferences();
    // ANGLE requires the passthrough decoder; Ganesh-over-GL Skia (this build defaults to
    // Graphite+Dawn, which needs a dawn_context_provider this holder doesn't set up).
    p->use_passthrough_cmd_decoder = true;
    p->gr_context_type = gpu::GrContextType::kGL;
    // Enable WebGPU in the GPU service ONLY when Dawn is actually built (--webgpu). Otherwise
    // Dawn is trap-stubbed (mb_dawn_stubs.cc), and turning enable_webgpu on makes the
    // shared-image system pick a Dawn/WGPU texture representation on the GPU (Metal) path ->
    // it calls gpu::DawnSharedTextureCache::* -> the stub traps (SIGTRAP). The SwiftShader
    // path never took that representation, which is why it didn't crash. So gate it on the
    // real Dawn build; with WebGPU off the WebGL ES2/ES3 (ANGLE) path is unaffected.
#if defined(MINIBLINK_ENABLE_WEBGPU)
    p->enable_webgpu = true;
    p->enable_unsafe_webgpu = true;
    p->disabled_dawn_features_list = {"adapter_blocklist"};
#if BUILDFLAG(IS_WIN)
    // Match the SwiftShader ANGLE default above: Dawn's default D3D12 path
    // pins to ANGLE's D3D11 LUID (absent under SwiftShader; patch 0032 skips
    // the query for this adapter). Vulkan/SwiftShader is always available.
    p->use_webgpu_adapter = gpu::WebGPUAdapterName::kSwiftShader;
#endif
#endif
    return h;
  }();
  return holder;
}

}  // namespace mb
