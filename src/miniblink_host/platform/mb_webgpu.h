// mb_webgpu — in-process WebGPU context provider (WebGPU bring-up milestone C2).
//
// Wraps the production in-process WebGPU command buffer (mb_webgpu_context.h, proved by
// mb_webgpu2_probe in milestone C1) in the blink::WebGraphicsContext3DProvider interface
// that Platform::CreateWebGPUGraphicsContext3DProviderAsync must return, so a page's
// navigator.gpu.requestAdapter() resolves to a real adapter. Backed by Dawn over
// SwiftShader (software) on the shared process-wide GPU thread (mb_gpu_thread.h).

#ifndef MINIBLINK_HOST_PLATFORM_MB_WEBGPU_H_
#define MINIBLINK_HOST_PLATFORM_MB_WEBGPU_H_

#include <memory>

namespace blink {
class WebGraphicsContext3DProvider;
}

namespace mb {

// Build an in-process WebGPU context provider, or null if GPU init fails. MUST be called
// on the thread that will consume the provider's WebGPUInterface (it installs that
// thread's dawn wire-client procs).
std::unique_ptr<blink::WebGraphicsContext3DProvider> MakeWebGPUContextProvider();

}  // namespace mb

#endif  // MINIBLINK_HOST_PLATFORM_MB_WEBGPU_H_
