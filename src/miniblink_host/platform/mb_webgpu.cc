#include "miniblink_host/platform/mb_webgpu.h"

#include "base/location.h"
#include "base/memory/ptr_util.h"
#include "base/task/sequenced_task_runner.h"
#include "gpu/command_buffer/client/webgpu_implementation.h"
#include "gpu/command_buffer/common/context_result.h"
#include "gpu/ipc/in_process_command_buffer.h"
#include "gpu/ipc/in_process_gpu_thread_holder.h"
#include "miniblink_host/platform/mb_gpu_thread.h"
#include "miniblink_host/platform/mb_webgpu_context.h"
#include "third_party/blink/public/platform/web_graphics_context_3d_provider.h"

#include <dawn/dawn_proc.h>
#include <dawn/dawn_thread_dispatch_proc.h>
#include <dawn/native/DawnNative.h>
#include <dawn/wire/WireClient.h>

namespace mb {
namespace {

// Dawn proc-table coordination for an in-process client+service (the milestone-C1 finding,
// mirroring viz::TestGpuServiceHolder): install the GLOBAL thread-dispatch table + the
// DEFAULT per-thread procs (native — for the GPU service threads) ONCE, process-wide; and
// point THIS thread (the WebGPU client) at the wire-client procs every call (a worker may
// create a provider on its own thread). blink does not install these in our embedder.
void EnsureDawnProcs() {
  static const bool global_once = [] {
    dawnProcSetProcs(&dawnThreadDispatchProcTable);
    dawnProcSetDefaultThreadProcs(&dawn::native::GetProcs());
    return true;
  }();
  (void)global_once;
  dawnProcSetPerThreadProcs(&dawn::wire::client::GetProcs());
}

// blink::WebGraphicsContext3DProvider over the in-process WebGPU command buffer. The
// WebGPUImplementation IS the WebGPUInterface AND (via ImplementationBase) the gpu::
// ContextSupport / gpu::InterfaceBase blink asks for. The GL/raster/image-decode hooks are
// unused by the WebGPU path and return null.
class MbWebGPUContextProvider : public blink::WebGraphicsContext3DProvider {
 public:
  static std::unique_ptr<MbWebGPUContextProvider> Create() {
    gpu::InProcessGpuThreadHolder* holder = GetSharedGpuThreadHolder();
    if (!holder)
      return nullptr;
    // Procs must be installed before Initialize creates the WebGPU decoder on the GPU
    // thread (it falls back to the default native procs).
    EnsureDawnProcs();
    auto context = std::make_unique<miniblink::MbWebGPUInProcessContext>();
    if (context->Initialize(holder->GetTaskExecutor()) !=
        gpu::ContextResult::kSuccess) {
      return nullptr;
    }
    return base::WrapUnique(new MbWebGPUContextProvider(std::move(context)));
  }

  gpu::InterfaceBase* InterfaceBase() override { return Impl(); }
  gpu::gles2::GLES2Interface* ContextGL() override { return nullptr; }
  gpu::raster::RasterInterface* RasterInterface() override { return nullptr; }
  gpu::webgpu::WebGPUInterface* WebGPUInterface() override { return Impl(); }
  gpu::ContextSupport* ContextSupport() override { return Impl(); }
  bool IsContextLost() override { return false; }
  bool BindToCurrentSequence() override { return true; }
  const gpu::Capabilities& GetCapabilities() const override {
    return context_->GetCapabilities();
  }
  const gpu::GpuFeatureInfo& GetGpuFeatureInfo() const override {
    return context_->GetGpuFeatureInfo();
  }
  const blink::WebglPreferences& GetWebglPreferences() const override {
    static const blink::WebglPreferences prefs;  // trivially destructible; defaults
    return prefs;
  }
  void SetLostContextCallback(base::RepeatingClosure) override {}
  void SetErrorMessageCallback(
      base::RepeatingCallback<void(const char*, int32_t)>) override {}
  cc::ImageDecodeCache* ImageDecodeCache(SkColorType) override { return nullptr; }
  gpu::SharedImageInterface* SharedImageInterface() override {
    return context_->GetCommandBuffer()->GetSharedImageInterface();
  }
  viz::RasterContextProvider* RasterContextProvider() const override {
    return nullptr;
  }

  ~MbWebGPUContextProvider() override {
    // The command buffer's WebGPUImplementation is sequence-bound to the thread it was
    // created on. For a WORKER context, V8 sweeps this provider at worker shutdown OFF
    // that sequence -> destroying context_ inline trips the GPU sequence_checker DCHECK.
    // Hand teardown back to the creation sequence (inline on the main thread).
    if (context_ && creation_runner_ &&
        !creation_runner_->RunsTasksInCurrentSequence()) {
      creation_runner_->DeleteSoon(FROM_HERE, std::move(context_));
    }
  }

 private:
  explicit MbWebGPUContextProvider(
      std::unique_ptr<miniblink::MbWebGPUInProcessContext> context)
      : context_(std::move(context)),
        creation_runner_(base::SequencedTaskRunner::GetCurrentDefault()) {}

  gpu::webgpu::WebGPUImplementation* Impl() {
    return context_->GetImplementation();
  }

  std::unique_ptr<miniblink::MbWebGPUInProcessContext> context_;
  scoped_refptr<base::SequencedTaskRunner> creation_runner_;
};

}  // namespace

std::unique_ptr<blink::WebGraphicsContext3DProvider>
MakeWebGPUContextProvider() {
  return MbWebGPUContextProvider::Create();
}

}  // namespace mb
