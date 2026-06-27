// mb_dawn_probe — WebGPU bring-up, milestone A: stand up an IN-PROCESS Dawn (native)
// instance, enumerate adapters, and create a real WebGPU device — entirely blink-free,
// mirroring mb_gl_probe's role for the GL stack. A green run isolates "the in-process
// Dawn foundation works headlessly" ahead of wiring blink's WebGPU context provider
// (the heavier milestones B/C), so a failure here can't be confused with a blink bug.
//
// Prefers Dawn's SwiftShader fallback adapter (CPU, headless-reproducible — the same
// software philosophy as mb_gl_probe forcing ANGLE/SwiftShader), but accepts any adapter
// the system offers so the probe still passes on a box with a real GPU. PASS = a device
// (and its default queue) was created. Exits 0 on success.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/task/single_thread_task_executor.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "dawn/dawn_proc_table.h"
#include "dawn/native/DawnNative.h"

namespace {

// SwiftShader (CPU) WebGPU is gated behind this Chromium switch, matching the GL probe's
// headless path. Without it Dawn refuses the software fallback adapter.
constexpr char kEnableUnsafeSwiftShader[] = "enable-unsafe-swiftshader";

std::string SV(const WGPUStringView& s) {
  return s.data ? std::string(s.data, s.length) : std::string("(unknown)");
}

const char* BackendName(WGPUBackendType b) {
  switch (b) {
    case WGPUBackendType_Null: return "Null";
    case WGPUBackendType_WebGPU: return "WebGPU";
    case WGPUBackendType_D3D11: return "D3D11";
    case WGPUBackendType_D3D12: return "D3D12";
    case WGPUBackendType_Metal: return "Metal";
    case WGPUBackendType_Vulkan: return "Vulkan";
    case WGPUBackendType_OpenGL: return "OpenGL";
    case WGPUBackendType_OpenGLES: return "OpenGLES";
    default: return "Undefined";
  }
}

const char* AdapterTypeName(WGPUAdapterType t) {
  switch (t) {
    case WGPUAdapterType_DiscreteGPU: return "DiscreteGPU";
    case WGPUAdapterType_IntegratedGPU: return "IntegratedGPU";
    case WGPUAdapterType_CPU: return "CPU";
    default: return "Unknown";
  }
}

}  // namespace

int main(int argc, char** argv) {
  base::AtExitManager at_exit;
  base::CommandLine::Init(argc, argv);

  // Dawn posts work to a worker pool (e.g. shader compilation); keep both a client
  // message loop and a thread pool alive, exactly like mb_gpu_probe.
  base::SingleThreadTaskExecutor main_task_executor;
  base::ThreadPoolInstance::CreateAndStartWithDefaultParams("mb_dawn_probe");

  base::CommandLine::ForCurrentProcess()->AppendSwitch(kEnableUnsafeSwiftShader);

  const DawnProcTable& procs = dawn::native::GetProcs();
  auto instance = std::make_unique<dawn::native::Instance>();

  // Pass 1: ask specifically for the software fallback adapter (SwiftShader/Vulkan),
  // which is what a headless box will have. Pass 2 (if that yields nothing) enumerates
  // every adapter so the probe still works on a machine with a real GPU.
  WGPURequestAdapterOptions fallback_opts = {};
  fallback_opts.featureLevel = WGPUFeatureLevel_Core;
  fallback_opts.forceFallbackAdapter = true;
  std::vector<dawn::native::Adapter> adapters =
      instance->EnumerateAdapters(&fallback_opts);
  if (adapters.empty()) {
    fprintf(stderr,
            "mb_dawn_probe: no SwiftShader fallback adapter; trying all backends\n");
    adapters = instance->EnumerateAdapters();
  }
  if (adapters.empty()) {
    fprintf(stderr, "mb_dawn_probe: EnumerateAdapters found NO adapters\n");
    return 1;
  }

  // Print every adapter, and pick one — preferring a CPU adapter (headless-stable).
  int chosen = 0;
  for (size_t i = 0; i < adapters.size(); ++i) {
    WGPUAdapterInfo info = {};
    if (procs.adapterGetInfo(adapters[i].Get(), &info) == WGPUStatus_Success) {
      printf("mb_dawn_probe: adapter[%zu] backend=%s type=%s device=%s desc=%s\n", i,
             BackendName(info.backendType), AdapterTypeName(info.adapterType),
             SV(info.device).c_str(), SV(info.description).c_str());
      if (info.adapterType == WGPUAdapterType_CPU)
        chosen = static_cast<int>(i);
    } else {
      printf("mb_dawn_probe: adapter[%zu] (GetInfo failed)\n", i);
    }
  }

  WGPUDevice device = adapters[chosen].CreateDevice();
  if (!device) {
    fprintf(stderr, "mb_dawn_probe: CreateDevice FAILED on adapter[%d]\n", chosen);
    return 1;
  }
  WGPUQueue queue = procs.deviceGetQueue(device);
  const bool ok = queue != nullptr;
  printf("mb_dawn_probe: device created on adapter[%d], default queue %s\n", chosen,
         ok ? "OK" : "NULL");

  if (queue)
    procs.queueRelease(queue);
  procs.deviceRelease(device);
  adapters.clear();
  instance.reset();
  base::ThreadPoolInstance::Get()->Shutdown();

  printf("mb_dawn_probe: %s (in-process Dawn instance + WebGPU device)\n",
         ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
