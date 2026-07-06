// mb_platform — the real (non-test) blink::Platform for miniblink2.
//
// Modeled on third_party/blink/renderer/platform/testing/testing_platform_support.h
// (see vendor/reference/). KEY FINDING: the base blink::Platform provides default
// impls for almost everything; TestingPlatformSupport overrides only ~6 methods.
// We override the same minimal set, with REAL behavior instead of test stubs.
//
// Status: Phase 1 scaffold. Methods are filled lazily — run, let Blink CHECK-fail
// on what it actually needs, implement that. Do NOT pre-implement the whole surface.

#ifndef MINIBLINK_HOST_PLATFORM_MB_PLATFORM_H_
#define MINIBLINK_HOST_PLATFORM_MB_PLATFORM_H_

#include <memory>
#include <optional>
#include <string>

#include "base/memory/scoped_refptr.h"
#include "media/base/audio_renderer_sink.h"  // for media::AudioRendererSink::RenderCallback
#include "third_party/blink/public/platform/platform.h"

namespace blink {
class WebAudioDevice;
class WebAudioLatencyHint;
class WebAudioSinkDescriptor;
}  // namespace blink

namespace webcrypto {
class WebCryptoImpl;
}

namespace mb {

class MbPlatform : public blink::Platform {
 public:
  // blink's default returns null; the DevTools agent (and other mojo users)
  // bind receivers on it - the CDP bridge (IMPROVEMENT.md item 13c) needs a real
  // IO-pumped thread here. Returns the runtime's existing mb-io service
  // thread; never spawns one.
  scoped_refptr<base::SingleThreadTaskRunner> GetIOTaskRunner() const override;
  // Nested loop for debugger pause (breakpoints): DevTools requires a non-null
  // runner at session attach even when never paused.
  std::unique_ptr<blink::Platform::NestedMessageLoopRunner>
  CreateNestedMessageLoopRunner() const override;

  MbPlatform();
  MbPlatform(const MbPlatform&) = delete;
  MbPlatform& operator=(const MbPlatform&) = delete;
  ~MbPlatform() override;

  // blink::Platform — the minimal override set (mirrors TestingPlatformSupport):
  blink::WebString DefaultLocale() override;
  blink::ThreadSafeBrowserInterfaceBrokerProxy* GetBrowserInterfaceBroker()
      override;
  bool IsThreadedAnimationEnabled() override;

  // WebCrypto (crypto.subtle.*): base Platform returns nullptr and SubtleCrypto
  // derefs it unconditionally (Platform::Current()->Crypto()->Digest(...)),
  // crashing on any crypto.subtle call. Return a real BoringSSL-backed impl.
  blink::WebCrypto* Crypto() override;

  // Web Audio: base Platform::CreateAudioDevice returns nullptr, but
  // AudioDestination's ctor derefs the result unguarded (web_audio_device_->
  // SampleRate()), so `new AudioContext()` crashes. Return a silent stub device
  // (valid params, no-op control, no rendering) so AudioContext constructs
  // without crashing — audio just doesn't play. (OfflineAudioContext already
  // works; it needs no device.)
  std::unique_ptr<blink::WebAudioDevice> CreateAudioDevice(
      const blink::WebAudioSinkDescriptor& sink_descriptor,
      unsigned number_of_output_channels,
      const blink::WebAudioLatencyHint& latency_hint,
      std::optional<float> context_sample_rate,
      media::AudioRendererSink::RenderCallback*) override;

  // Worker bring-up: base Platform returns nullptr here and DedicatedWorker
  // derefs it (null-deref SIGSEGV on `new Worker`). We have no worker-thread
  // infrastructure, so return an inert stub — the worker never runs, but the
  // host does not crash. See mb_platform.cc for the full rationale.
  std::unique_ptr<blink::WebDedicatedWorkerHostFactoryClient>
  CreateDedicatedWorkerHostFactoryClient(
      blink::WebDedicatedWorker*,
      const blink::BrowserInterfaceBrokerProxy&) override;

  // WebGL: base Platform::CreateWebGLGraphicsContextProvider returns nullptr, so
  // getContext('webgl') is null. Return an in-process provider backed by an ANGLE/
  // SwiftShader GLES2 command buffer (see platform/mb_webgl.cc) so WebGL works
  // headlessly. The GPU thread is spun lazily on first use; non-WebGL pages pay
  // nothing and the software 2D-canvas path is untouched.
  std::unique_ptr<blink::WebGraphicsContext3DProvider>
  CreateWebGLGraphicsContextProvider(
      bool prefer_low_power_gpu,
      bool fail_if_major_performance_caveat,
      blink::Platform::WebGLContextType context_type,
      const blink::WebURL& document_url,
      blink::Platform::WebGLContextInfo* gl_info) override;

  // Web Audio decodeAudioData(): blink's AudioBus delegates in-memory audio-file
  // decode to Platform::DecodeAudioFileData (audio_bus.cc). Base returns null ->
  // decodeAudioData rejects with EncodingError. Decode via FFmpeg (media::Audio-
  // FileReader), so decodeAudioData works (sound effects / music / analysis), and
  // the FFmpeg audio-decode foundation under <audio>/<video> is exercised.
  std::unique_ptr<blink::WebAudioBus> DecodeAudioFileData(
      base::span<const char> data) override;

  // WebGPU: we have no Dawn/WebGPU backend. The base Platform's async provider
  // creation never invokes its callback, so navigator.gpu.requestAdapter() HANGS
  // forever (a page that `await`s it never proceeds). Override to invoke the callback
  // with no provider -> blink resolves requestAdapter() to null ("WebGPU unavailable"),
  // so pages feature-detect and fall back (e.g. to WebGL) cleanly.
  void CreateWebGPUGraphicsContext3DProviderAsync(
      const blink::WebURL& document_url,
      blink::Platform::WebGPUReplyThread reply_thread,
      base::OnceCallback<void(
          std::unique_ptr<blink::WebGraphicsContext3DProvider>)> callback)
      override;

  // Resource bundle: Blink asks for built-in resources (UA stylesheet, etc.).
  // P1: back with a real bundle / packed file. TODO(mb): wire resource pak.
  blink::WebData GetDataResource(
      int resource_id,
      ui::ResourceScaleFactor scale_factor) override;
  std::string GetDataResourceString(int resource_id) override;

  // Fonts / sandbox / codecs etc. are defaulted by base Platform on macOS via
  // skia + system; override here only when a CHECK tells us to.

 private:
  // Empty broker: GetInterfaceImpl drops every receiver. Blink core init
  // (TimeZoneController etc.) calls GetInterface() during startup and crashes on a
  // null broker, so this must be a real ref-counted object, not nullptr.
  scoped_refptr<blink::ThreadSafeBrowserInterfaceBrokerProxy> broker_;
  // BoringSSL-backed WebCrypto impl returned from Crypto(); threadsafe.
  std::unique_ptr<webcrypto::WebCryptoImpl> web_crypto_;
};

}  // namespace mb

#endif  // MINIBLINK_HOST_PLATFORM_MB_PLATFORM_H_
