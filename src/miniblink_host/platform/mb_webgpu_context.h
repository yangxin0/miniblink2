// MbWebGPUInProcessContext — a PRODUCTION (non-test) in-process WebGPU command buffer.
//
// It is a near-verbatim copy of gpu::WebGPUInProcessContext (gpu/ipc/webgpu_in_process_
// context.h), which is testonly ONLY because its client task runner is a base::Test-
// SimpleTaskRunner. This variant uses the CURRENT thread's real SingleThreadTaskRunner
// instead, so it links into the non-test miniblink_host library. Command-buffer replies
// are delivered by the calling thread's normal message loop (no manual pump).
//
// This is the foundation under WebGPU milestone C (wiring a real WebGPU provider into
// MbPlatform), the analog of mb_webgl.cc's GLInProcessContext usage for WebGL. It drives
// the in-process WebGPU command buffer + dawn wire proved blink-free in mb_webgpu_probe,
// but with production (non-testonly) deps only — see patches/0006 which makes the
// in-process GPU command-buffer targets linkable outside tests.

#ifndef MINIBLINK_HOST_PLATFORM_MB_WEBGPU_CONTEXT_H_
#define MINIBLINK_HOST_PLATFORM_MB_WEBGPU_CONTEXT_H_

#include <memory>

#include "base/memory/scoped_refptr.h"
#include "base/task/single_thread_task_runner.h"
#include "gpu/command_buffer/common/capabilities.h"
#include "gpu/command_buffer/common/context_result.h"
#include "gpu/command_buffer/service/command_buffer_task_executor.h"
#include "gpu/config/gpu_feature_info.h"

namespace gpu {
class CommandBufferHelper;
class InProcessCommandBuffer;
class TransferBuffer;
namespace webgpu {
class WebGPUImplementation;
}  // namespace webgpu
}  // namespace gpu

namespace miniblink {

class MbWebGPUInProcessContext {
 public:
  MbWebGPUInProcessContext();
  MbWebGPUInProcessContext(const MbWebGPUInProcessContext&) = delete;
  MbWebGPUInProcessContext& operator=(const MbWebGPUInProcessContext&) = delete;
  ~MbWebGPUInProcessContext();

  // |task_executor| comes from a (process-wide) InProcessGpuThreadHolder.
  gpu::ContextResult Initialize(gpu::CommandBufferTaskExecutor* task_executor);

  // The WebGPUImplementation IS a gpu::webgpu::WebGPUInterface — the interface blink's
  // WebGPU provider consumes.
  gpu::webgpu::WebGPUImplementation* GetImplementation();
  gpu::InProcessCommandBuffer* GetCommandBuffer();
  const gpu::Capabilities& GetCapabilities() const;
  const gpu::GpuFeatureInfo& GetGpuFeatureInfo() const;

 private:
  scoped_refptr<base::SingleThreadTaskRunner> client_task_runner_;
  std::unique_ptr<gpu::CommandBufferHelper> helper_;
  std::unique_ptr<gpu::TransferBuffer> transfer_buffer_;
  std::unique_ptr<gpu::webgpu::WebGPUImplementation> webgpu_implementation_;
  std::unique_ptr<gpu::InProcessCommandBuffer> command_buffer_;
};

}  // namespace miniblink

#endif  // MINIBLINK_HOST_PLATFORM_MB_WEBGPU_CONTEXT_H_
