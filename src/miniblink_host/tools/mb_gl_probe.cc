// mb_gl_probe — WebGL bring-up, milestone A: prove that ANGLE + SwiftShader GL can be
// initialized IN-PROCESS in the standalone tree, and that an offscreen context can be
// made current and queried. This is the foundation the in-process command buffer (and
// then blink's WebGL provider) will sit on. It is deliberately blink-free: it links
// only //ui/gl + //ui/gl/init, so a green run isolates "the GL stack works" from any
// blink/compositor concern.
//
// Selects the software path (ANGLE over SwiftShader) so it runs on a headless box with
// no GPU. Prints the GL_VENDOR/RENDERER/VERSION strings and exits 0 on success.

#include <cstdio>
#include <string>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gl/gl_bindings.h"
#include "ui/gl/gl_context.h"
#include "ui/gl/gl_display.h"
#include "ui/gl/gl_surface.h"
#include "ui/gl/gl_switches.h"
#include "ui/gl/init/gl_factory.h"

namespace {

const char* GlString(unsigned name) {
  const GLubyte* s = glGetString(name);
  return s ? reinterpret_cast<const char*>(s) : "(null)";
}

}  // namespace

int main(int argc, char** argv) {
  base::AtExitManager at_exit;
  base::CommandLine::Init(argc, argv);

  // Force the headless software path: ANGLE backed by SwiftShader (no GPU needed).
  base::CommandLine* cmd = base::CommandLine::ForCurrentProcess();
  cmd->AppendSwitchASCII(switches::kUseGL, gl::kGLImplementationANGLEName);
  cmd->AppendSwitchASCII(switches::kUseANGLE,
                         gl::kANGLEImplementationSwiftShaderName);
  cmd->AppendSwitch(switches::kEnableUnsafeSwiftShader);

  gl::GLDisplay* display =
      gl::init::InitializeGLOneOff(gl::GpuPreference::kDefault);
  if (!display) {
    fprintf(stderr, "mb_gl_probe: InitializeGLOneOff FAILED\n");
    return 1;
  }

  scoped_refptr<gl::GLSurface> surface =
      gl::init::CreateOffscreenGLSurface(display, gfx::Size(1, 1));
  if (!surface) {
    fprintf(stderr, "mb_gl_probe: CreateOffscreenGLSurface FAILED\n");
    return 1;
  }

  scoped_refptr<gl::GLContext> context = gl::init::CreateGLContext(
      /*share_group=*/nullptr, surface.get(), gl::GLContextAttribs());
  if (!context) {
    fprintf(stderr, "mb_gl_probe: CreateGLContext FAILED\n");
    return 1;
  }
  if (!context->MakeCurrent(surface.get())) {
    fprintf(stderr, "mb_gl_probe: MakeCurrent FAILED\n");
    return 1;
  }

  const std::string renderer = GlString(GL_RENDERER);
  printf("mb_gl_probe: GL_VENDOR   = %s\n", GlString(GL_VENDOR));
  printf("mb_gl_probe: GL_RENDERER = %s\n", renderer.c_str());
  printf("mb_gl_probe: GL_VERSION  = %s\n", GlString(GL_VERSION));

  context->ReleaseCurrent(surface.get());

  // SwiftShader's renderer string contains "SwiftShader" (via ANGLE it reads e.g.
  // "ANGLE (... SwiftShader Device ...)"). Treat a non-empty renderer as success and
  // flag whether the software backend is the one we got.
  const bool ok = renderer.find("SwiftShader") != std::string::npos ||
                  renderer.find("ANGLE") != std::string::npos;
  printf("mb_gl_probe: %s (in-process ANGLE/SwiftShader GL)\n",
         ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
