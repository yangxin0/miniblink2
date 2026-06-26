// mb_webgl — in-process WebGL context provider (WebGL bring-up milestone C).
//
// Wraps the in-process GPU command buffer proved by mb_gpu_probe (milestone B) in the
// blink::WebGraphicsContext3DProvider interface that Platform::CreateWebGLGraphics-
// ContextProvider must return, so getContext('webgl') succeeds. Backed by ANGLE over
// SwiftShader (software), so it needs no GPU. The process-wide GPU thread is spun lazily
// on the first WebGL request, so non-WebGL pages pay nothing and the software 2D-canvas
// raster path is untouched.

#ifndef MINIBLINK_HOST_PLATFORM_MB_WEBGL_H_
#define MINIBLINK_HOST_PLATFORM_MB_WEBGL_H_

#include <memory>

namespace blink {
class WebGraphicsContext3DProvider;
}

namespace mb {

// Build an in-process WebGL context provider, or null if GPU init fails. `want_webgl2`
// requests an OpenGL ES 3 context (WebGL 2); false gives ES 2 (WebGL 1).
std::unique_ptr<blink::WebGraphicsContext3DProvider> MakeWebGLContextProvider(
    bool want_webgl2);

}  // namespace mb

#endif  // MINIBLINK_HOST_PLATFORM_MB_WEBGL_H_
