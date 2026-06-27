// See mb_webgpu_context.h. Logic mirrors gpu::WebGPUInProcessContext::Initialize/~ exactly,
// with the testonly base::TestSimpleTaskRunner replaced by the current thread's real
// SingleThreadTaskRunner and the (unused) gtest include dropped.

#include "third_party/blink/renderer/miniblink_host/platform/mb_webgpu_context.h"

#include <utility>

#include "base/logging.h"
#include "gpu/command_buffer/client/shared_memory_limits.h"
#include "gpu/command_buffer/client/transfer_buffer.h"
#include "gpu/command_buffer/client/webgpu_cmd_helper.h"
#include "gpu/command_buffer/client/webgpu_implementation.h"
#include "gpu/command_buffer/common/constants.h"
#include "gpu/ipc/common/gpu_channel.mojom.h"
#include "gpu/ipc/in_process_command_buffer.h"
#include "url/gurl.h"

namespace miniblink {

MbWebGPUInProcessContext::MbWebGPUInProcessContext() = default;

MbWebGPUInProcessContext::~MbWebGPUInProcessContext() {
  // Drop the client before the service objects. The owning thread's message loop
  // delivers any pending replies; unlike the testonly variant there is no
  // TestSimpleTaskRunner::RunUntilIdle to force here.
  webgpu_implementation_.reset();
  transfer_buffer_.reset();
  helper_.reset();
  command_buffer_.reset();
}

gpu::ContextResult MbWebGPUInProcessContext::Initialize(
    gpu::CommandBufferTaskExecutor* task_executor) {
  auto attribs = gpu::mojom::ContextCreationAttribs::NewWebgpu(
      gpu::mojom::WebGPUCreationAttribs::New());

  client_task_runner_ = base::SingleThreadTaskRunner::GetCurrentDefault();
  command_buffer_ =
      std::make_unique<gpu::InProcessCommandBuffer>(task_executor, GURL());

  auto result =
      command_buffer_->Initialize(std::move(attribs), client_task_runner_,
                                  /*gr_shader_cache=*/nullptr,
                                  /*use_shader_cache_shm_count=*/nullptr);
  if (result != gpu::ContextResult::kSuccess) {
    DLOG(ERROR) << "MbWebGPUInProcessContext: InProcessCommandBuffer init failed";
    return result;
  }

  const auto memory_limits = gpu::SharedMemoryLimits::ForWebGPUContext();

  auto webgpu_helper =
      std::make_unique<gpu::webgpu::WebGPUCmdHelper>(command_buffer_.get());
  result = webgpu_helper->Initialize(memory_limits.command_buffer_size);
  if (result != gpu::ContextResult::kSuccess) {
    LOG(ERROR) << "MbWebGPUInProcessContext: WebGPUCmdHelper init failed";
    return result;
  }
  transfer_buffer_ = std::make_unique<gpu::TransferBuffer>(webgpu_helper.get());

  webgpu_implementation_ = std::make_unique<gpu::webgpu::WebGPUImplementation>(
      webgpu_helper.get(), transfer_buffer_.get(), command_buffer_.get());
  helper_ = std::move(webgpu_helper);
  webgpu_implementation_->Initialize(memory_limits);
  return result;
}

gpu::webgpu::WebGPUImplementation* MbWebGPUInProcessContext::GetImplementation() {
  return webgpu_implementation_.get();
}

gpu::InProcessCommandBuffer* MbWebGPUInProcessContext::GetCommandBuffer() {
  return command_buffer_.get();
}

}  // namespace miniblink
