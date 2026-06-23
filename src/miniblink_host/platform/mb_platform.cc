// mb_platform.cc — real blink::Platform overrides for miniblink-modern.
//
// Mirrors the SMALL override set of TestingPlatformSupport
// (vendor/reference/testing_platform_support.cc). Base blink::Platform defaults the
// rest. Fill methods lazily: when a CHECK/NOTREACHED in Blink names a missing method,
// implement THAT one. Do not pre-implement the whole Platform surface.
//
// Status: Phase 1 scaffold — signatures pinned during .cc compile.

#include "miniblink_host/platform/mb_platform.h"

#include "base/containers/span.h"
#include "base/memory/scoped_refptr.h"
#include "mojo/public/cpp/bindings/generic_pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "third_party/blink/public/common/thread_safe_browser_interface_broker_proxy.h"
#include "third_party/blink/public/mojom/mime/mime_registry.mojom-blink.h"
#include "third_party/blink/public/platform/web_data.h"
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
    // Drop everything else.
  }

 protected:
  ~MbEmptyBroker() override = default;
};

}  // namespace

MbPlatform::MbPlatform()
    : broker_(base::MakeRefCounted<MbEmptyBroker>()) {}
MbPlatform::~MbPlatform() = default;

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
