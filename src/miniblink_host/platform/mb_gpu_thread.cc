#include "miniblink_host/platform/mb_gpu_thread.h"

#include "base/command_line.h"
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
    if (!cmd->HasSwitch(switches::kUseANGLE)) {
      cmd->AppendSwitchASCII(switches::kUseANGLE,
                             gl::kANGLEImplementationSwiftShaderName);
    }
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
    // Also enable WebGPU so the SAME service serves navigator.gpu (mb_webgpu.cc). These
    // are inert until a WebGPU command buffer is requested; the WebGL ES2/ES3 path is
    // unaffected. adapter_blocklist is disabled so the SwiftShader fallback is allowed.
    p->enable_webgpu = true;
    p->enable_unsafe_webgpu = true;
    p->disabled_dawn_features_list = {"adapter_blocklist"};
    return h;
  }();
  return holder;
}

}  // namespace mb
