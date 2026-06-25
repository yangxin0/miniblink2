// mb_platform.cc — real blink::Platform overrides for miniblink-modern.
//
// Mirrors the SMALL override set of TestingPlatformSupport
// (vendor/reference/testing_platform_support.cc). Base blink::Platform defaults the
// rest. Fill methods lazily: when a CHECK/NOTREACHED in Blink names a missing method,
// implement THAT one. Do not pre-implement the whole Platform surface.
//
// Status: Phase 1 scaffold — signatures pinned during .cc compile.

#include "miniblink_host/platform/mb_platform.h"

#include <memory>

#include "base/containers/span.h"
#include "base/memory/scoped_refptr.h"
#include "components/webcrypto/webcrypto_impl.h"
#include "media/base/output_device_info.h"
#include "third_party/blink/public/platform/web_audio_device.h"
#include "mojo/public/cpp/bindings/generic_pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "third_party/blink/public/common/thread_safe_browser_interface_broker_proxy.h"
#include "miniblink_host/blob/mb_blob_registry.h"
#include "miniblink_host/worker/mb_dedicated_worker_host.h"
#include "third_party/blink/public/mojom/blob/blob_registry.mojom-blink.h"
#include "third_party/blink/public/mojom/mime/mime_registry.mojom-blink.h"
#include "third_party/blink/public/platform/web_data.h"
#include "third_party/blink/public/platform/web_dedicated_worker_host_factory_client.h"
#include "third_party/blink/public/platform/web_worker_fetch_context.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "ui/base/resource/resource_bundle.h"

namespace mb {
namespace {

// In-process MimeRegistry (normally a browser-process Mojo service). Blink proxies
// MIMETypeRegistry::GetMIMETypeForExtension here; without it, file:// stylesheets are
// REJECTED (CSSStyleSheetResource::CanUseSheet enforces ext->text/css for file: URLs).
class MbMimeRegistry : public blink::mojom::blink::MimeRegistry {
 public:
  void GetMimeTypeFromExtension(
      const blink::String& extension,
      GetMimeTypeFromExtensionCallback callback) override {
    std::string e = extension.Utf8();
    const char* mime = "application/octet-stream";
    if (e == "css") mime = "text/css";
    else if (e == "js" || e == "mjs") mime = "text/javascript";
    else if (e == "html" || e == "htm") mime = "text/html";
    else if (e == "svg") mime = "image/svg+xml";
    else if (e == "png") mime = "image/png";
    else if (e == "jpg" || e == "jpeg") mime = "image/jpeg";
    else if (e == "gif") mime = "image/gif";
    else if (e == "json") mime = "application/json";
    std::move(callback).Run(blink::String(mime));
  }
};

// A broker that binds the few in-process services we provide (MimeRegistry) and drops
// the rest (no browser process).
class MbEmptyBroker : public blink::ThreadSafeBrowserInterfaceBrokerProxy {
 public:
  MbEmptyBroker() = default;

  void GetInterfaceImpl(mojo::GenericPendingReceiver receiver) override {
    if (auto r = receiver.As<blink::mojom::blink::MimeRegistry>()) {
      mojo::MakeSelfOwnedReceiver(std::make_unique<MbMimeRegistry>(),
                                  std::move(r));
      return;
    }
    // Blob bytes: route to the service thread so the [Sync] Register the main
    // thread makes is answered off-thread (instead of dropped -> reads hang).
    if (auto r = receiver.As<blink::mojom::blink::BlobRegistry>()) {
      BindBlobRegistryOnServiceThread(std::move(r));
      return;
    }
    // Drop everything else.
  }

 protected:
  ~MbEmptyBroker() override = default;
};

// Silent Web Audio output device. We have no audio backend; base
// Platform::CreateAudioDevice returns nullptr, but AudioDestination's ctor
// dereferences the result unguarded (web_audio_device_->SampleRate()), so
// `new AudioContext()` would crash. This provides valid parameters and no-op
// control so an AudioContext constructs and runs without producing sound:
// Start() never pulls the render callback, so nothing is rendered, but the
// graph is fully usable and the host does not crash.
class MbSilentAudioDevice : public blink::WebAudioDevice {
 public:
  void Start() override {}
  void Stop() override {}
  void Pause() override {}
  void Resume() override {}
  double SampleRate() override { return 48000.0; }
  int FramesPerBuffer() override { return 128; }  // Web Audio render quantum
  int MaxChannelCount() override { return 2; }
  void SetDetectSilence(bool) override {}
  media::OutputDeviceStatus MaybeCreateSinkAndGetStatus() override {
    return media::OUTPUT_DEVICE_STATUS_OK;
  }
};

}  // namespace

MbPlatform::MbPlatform()
    : broker_(base::MakeRefCounted<MbEmptyBroker>()),
      web_crypto_(std::make_unique<webcrypto::WebCryptoImpl>()) {}
MbPlatform::~MbPlatform() = default;

blink::WebCrypto* MbPlatform::Crypto() {
  return web_crypto_.get();  // BoringSSL-backed; never null (see header note)
}

std::unique_ptr<blink::WebAudioDevice> MbPlatform::CreateAudioDevice(
    const blink::WebAudioSinkDescriptor&,
    unsigned /*number_of_output_channels*/,
    const blink::WebAudioLatencyHint&,
    std::optional<float> /*context_sample_rate*/,
    media::AudioRendererSink::RenderCallback*) {
  // Non-null silent device (base returns nullptr, which AudioDestination derefs).
  return std::make_unique<MbSilentAudioDevice>();
}

blink::WebString MbPlatform::DefaultLocale() {
  // Must be non-empty: layout/font selection reads DefaultLanguage() from this and
  // crashes on a null locale.
  return blink::WebString::FromUtf8("en-US");
}

blink::ThreadSafeBrowserInterfaceBrokerProxy*
MbPlatform::GetBrowserInterfaceBroker() {
  return broker_.get();  // empty broker; never null (see header note)
}

bool MbPlatform::IsThreadedAnimationEnabled() {
  return false;  // synchronous single-threaded compositing in P1
}

std::unique_ptr<blink::WebDedicatedWorkerHostFactoryClient>
MbPlatform::CreateDedicatedWorkerHostFactoryClient(
    blink::WebDedicatedWorker* worker,
    const blink::BrowserInterfaceBrokerProxy&) {
  // Drive the worker thread in-process (Step 2): the factory client synthesizes the
  // host handshake + streams the fetched main script back to blink. See
  // worker/mb_dedicated_worker_host.cc.
  return MakeDedicatedWorkerHostFactoryClient(worker);
}

blink::WebData MbPlatform::GetDataResource(int resource_id,
                                           ui::ResourceScaleFactor scale_factor) {
  if (!ui::ResourceBundle::HasSharedInstance())
    return {};
  std::string_view data =
      ui::ResourceBundle::GetSharedInstance().GetRawDataResourceForScale(
          resource_id, scale_factor);
  return blink::WebData(base::as_byte_span(data));
}

std::string MbPlatform::GetDataResourceString(int resource_id) {
  if (!ui::ResourceBundle::HasSharedInstance())
    return {};
  return ui::ResourceBundle::GetSharedInstance().LoadDataResourceString(
      resource_id);
}

}  // namespace mb
