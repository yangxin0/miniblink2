// mb_gpu_thread — the single process-wide in-process GPU thread (ANGLE/SwiftShader),
// shared by the WebGL (mb_webgl.cc) and WebGPU (mb_webgpu.cc) context providers. Created
// lazily on the first GPU-backed request and kept for the process lifetime (windows +
// workers share it). One GPU service, not one per API.

#ifndef MINIBLINK_HOST_PLATFORM_MB_GPU_THREAD_H_
#define MINIBLINK_HOST_PLATFORM_MB_GPU_THREAD_H_

namespace gpu {
class InProcessGpuThreadHolder;
}

namespace mb {

// Returns the shared holder (its GpuPreferences enable BOTH GL and WebGPU), or null if
// in-process GL initialization fails.
gpu::InProcessGpuThreadHolder* GetSharedGpuThreadHolder();

}  // namespace mb

#endif  // MINIBLINK_HOST_PLATFORM_MB_GPU_THREAD_H_
