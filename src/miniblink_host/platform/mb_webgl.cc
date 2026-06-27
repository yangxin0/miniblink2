#include "miniblink_host/platform/mb_webgl.h"

#include "base/location.h"
#include "base/memory/ptr_util.h"
#include "base/task/sequenced_task_runner.h"
#include "gpu/command_buffer/client/gles2_implementation.h"
#include "gpu/command_buffer/common/context_creation_attribs.h"
#include "gpu/command_buffer/common/context_result.h"
#include "gpu/config/gpu_feature_info.h"
#include "gpu/ipc/gl_in_process_context.h"
#include "gpu/ipc/in_process_gpu_thread_holder.h"
#include "miniblink_host/platform/mb_gpu_thread.h"
#include "third_party/blink/public/platform/web_graphics_context_3d_provider.h"

namespace mb {
namespace {

// blink::WebGraphicsContext3DProvider over a gpu::GLInProcessContext. The GLES2
// implementation IS the gpu::gles2::GLES2Interface (via GLES2Interface) AND the
// gpu::ContextSupport / gpu::InterfaceBase (via ImplementationBase) blink asks for. The
// raster/webgpu/image-decode/raster-context hooks are unused by the WebGL path and
// return null (blink only consults them for accelerated 2D-canvas / raster).
class MbWebGLContextProvider : public blink::WebGraphicsContext3DProvider {
 public:
  static std::unique_ptr<MbWebGLContextProvider> Create(bool want_webgl2) {
    gpu::InProcessGpuThreadHolder* holder = GetSharedGpuThreadHolder();
    if (!holder)
      return nullptr;
    auto context = std::make_unique<gpu::GLInProcessContext>();
    // WebGL 2 needs an ES3 context; WebGL 1 keeps the proven ES2 path.
    const gpu::ContextType type =
        want_webgl2 ? gpu::CONTEXT_TYPE_OPENGLES3 : gpu::CONTEXT_TYPE_OPENGLES2;
    if (context->Initialize(holder->GetTaskExecutor(), type) !=
        gpu::ContextResult::kSuccess) {
      return nullptr;
    }
    return base::WrapUnique(new MbWebGLContextProvider(std::move(context)));
  }

  gpu::InterfaceBase* InterfaceBase() override { return Gl(); }
  gpu::gles2::GLES2Interface* ContextGL() override { return Gl(); }
  gpu::raster::RasterInterface* RasterInterface() override { return nullptr; }
  gpu::webgpu::WebGPUInterface* WebGPUInterface() override { return nullptr; }
  gpu::ContextSupport* ContextSupport() override {
    return context_->GetImplementation();
  }
  bool IsContextLost() override {
    return Gl()->GetGraphicsResetStatusKHR() != 0 /*GL_NO_ERROR*/;
  }
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
    return context_->GetSharedImageInterface();
  }
  viz::RasterContextProvider* RasterContextProvider() const override {
    return nullptr;
  }

  ~MbWebGLContextProvider() override {
    // The GLInProcessContext (its GLES2Implementation) is sequence-bound to the thread
    // it was CREATED on. For a WORKER context, V8's cppgc sweeps this provider at worker
    // shutdown OFF that sequence -> destroying context_ inline trips the GPU
    // sequence_checker DCHECK (fatal). Hand the teardown back to the creation sequence so
    // it runs where the context was bound. On the main thread (creation == current
    // sequence) this is a normal inline destruction.
    if (context_ && creation_runner_ &&
        !creation_runner_->RunsTasksInCurrentSequence()) {
      creation_runner_->DeleteSoon(FROM_HERE, std::move(context_));
    }
  }

 private:
  explicit MbWebGLContextProvider(
      std::unique_ptr<gpu::GLInProcessContext> context)
      : context_(std::move(context)),
        creation_runner_(base::SequencedTaskRunner::GetCurrentDefault()) {}

  gpu::gles2::GLES2Implementation* Gl() { return context_->GetImplementation(); }

  std::unique_ptr<gpu::GLInProcessContext> context_;
  // The sequence the context was created/bound on (worker or main); teardown is posted
  // here so ~GLES2Implementation runs on its bind sequence.
  scoped_refptr<base::SequencedTaskRunner> creation_runner_;
};

}  // namespace

std::unique_ptr<blink::WebGraphicsContext3DProvider> MakeWebGLContextProvider(
    bool want_webgl2) {
  return MbWebGLContextProvider::Create(want_webgl2);
}

}  // namespace mb
