// mb_gpu_probe — WebGL bring-up, milestone B: stand up an IN-PROCESS GPU command buffer
// over the ANGLE/SwiftShader GL foundation proved in milestone A (mb_gl_probe), and drive
// it through a real gpu::gles2::GLES2Interface — the interface blink's WebGL provider
// (milestone C) ultimately consumes.
//
// Path: InProcessGpuThreadHolder spins a "GpuThread" (its own SyncPointManager +
// Scheduler + CommandBufferTaskExecutor); GLInProcessContext::Initialize wires a client
// command buffer + GLES2Implementation talking to it. We then issue real GL commands
// (clear + GetIntegerv + GetString) and confirm GL_NO_ERROR. Blink-free, like milestone A.
//
// Forces the headless software path (ANGLE over SwiftShader) + the passthrough decoder
// (ANGLE requires it). Exits 0 on success.

#include <cstdio>
#include <memory>
#include <string>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/task/single_thread_task_executor.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "gpu/command_buffer/client/gles2_implementation.h"
#include "gpu/command_buffer/client/gles2_interface.h"
#include "gpu/command_buffer/common/context_result.h"
#include "gpu/config/gpu_preferences.h"
#include "gpu/ipc/gl_in_process_context.h"
#include "gpu/ipc/in_process_gpu_thread_holder.h"
#include "ui/gl/gl_display.h"
#include "ui/gl/gl_switches.h"
#include "ui/gl/init/gl_factory.h"

// A handful of GLES2 enums (avoid pulling a full GL header into this tiny probe).
#define MB_GL_NO_ERROR 0
#define MB_GL_RENDERER 0x1F01
#define MB_GL_VERSION 0x1F02
#define MB_GL_MAX_TEXTURE_SIZE 0x0D33
#define MB_GL_COLOR_BUFFER_BIT 0x00004000

int main(int argc, char** argv) {
  base::AtExitManager at_exit;
  base::CommandLine::Init(argc, argv);

  // Client message loop (GLInProcessContext uses the current default task runner) +
  // thread pool (base::Thread / GPU service rely on it).
  base::SingleThreadTaskExecutor main_task_executor;
  base::ThreadPoolInstance::CreateAndStartWithDefaultParams("mb_gpu_probe");

  // Headless software GL: ANGLE over SwiftShader.
  base::CommandLine* cmd = base::CommandLine::ForCurrentProcess();
  cmd->AppendSwitchASCII(switches::kUseGL, gl::kGLImplementationANGLEName);
  cmd->AppendSwitchASCII(switches::kUseANGLE,
                         gl::kANGLEImplementationSwiftShaderName);
  cmd->AppendSwitch(switches::kEnableUnsafeSwiftShader);

  // Set the PROCESS-WIDE GL implementation to ANGLE/SwiftShader on the main thread
  // first (milestone A's call). The in-process GPU thread's own GL init reads this
  // global; without it, GetGLImplementation() on the GPU thread isn't ANGLE and the
  // Mac surface factory hits a NOTREACHED.
  if (!gl::init::InitializeGLOneOff(gl::GpuPreference::kDefault)) {
    fprintf(stderr, "mb_gpu_probe: main-thread InitializeGLOneOff FAILED\n");
    return 1;
  }

  auto holder = std::make_unique<gpu::InProcessGpuThreadHolder>();
  // ANGLE only supports the passthrough command decoder.
  holder->GetGpuPreferences()->use_passthrough_cmd_decoder = true;
  // This Chromium is built with Graphite+Dawn, so the GPU service defaults its Skia
  // backend to kGraphiteDawn (which needs a Dawn context provider we don't set up).
  // Force the Ganesh-over-GL backend, which runs on the ANGLE/SwiftShader GL above.
  holder->GetGpuPreferences()->gr_context_type = gpu::GrContextType::kGL;

  auto context = std::make_unique<gpu::GLInProcessContext>();
  gpu::ContextResult result = context->Initialize(holder->GetTaskExecutor());
  if (result != gpu::ContextResult::kSuccess) {
    fprintf(stderr, "mb_gpu_probe: GLInProcessContext::Initialize FAILED (result=%d)\n",
            static_cast<int>(result));
    return 1;
  }

  gpu::gles2::GLES2Implementation* gl = context->GetImplementation();
  if (!gl) {
    fprintf(stderr, "mb_gpu_probe: GetImplementation() returned null\n");
    return 1;
  }

  // Drive real GL commands through the command buffer.
  gl->ClearColor(0.0f, 1.0f, 0.0f, 1.0f);
  gl->Clear(MB_GL_COLOR_BUFFER_BIT);
  int max_tex = 0;
  gl->GetIntegerv(MB_GL_MAX_TEXTURE_SIZE, &max_tex);
  gl->Finish();
  unsigned err = gl->GetError();

  auto str = [&](unsigned name) -> std::string {
    const unsigned char* s = gl->GetString(name);
    return s ? reinterpret_cast<const char*>(s) : "(null)";
  };
  printf("mb_gpu_probe: command-buffer GL_RENDERER = %s\n", str(MB_GL_RENDERER).c_str());
  printf("mb_gpu_probe: command-buffer GL_VERSION  = %s\n", str(MB_GL_VERSION).c_str());
  printf("mb_gpu_probe: GL_MAX_TEXTURE_SIZE = %d, glGetError = %u\n", max_tex, err);

  // Tear down the client before the GPU thread holder.
  context.reset();
  holder.reset();
  base::ThreadPoolInstance::Get()->Shutdown();

  const bool ok = err == MB_GL_NO_ERROR && max_tex > 0;
  printf("mb_gpu_probe: %s (in-process GPU command buffer + GLES2Interface)\n",
         ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
