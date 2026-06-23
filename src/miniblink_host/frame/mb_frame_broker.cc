// mb_frame_broker.cc — in-process BrowserInterfaceBroker + RestrictedCookieManager.
#include "miniblink_host/frame/mb_frame_broker.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/read_only_shared_memory_region.h"
#include "miniblink_host/loader/mb_url_loader.h"
#include "mojo/public/cpp/bindings/generic_pending_receiver.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "services/network/public/mojom/cookie_manager.mojom-blink.h"
#include "services/network/public/mojom/restricted_cookie_manager.mojom-blink.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace mb {
namespace {

// One process-wide in-memory cookie store, keyed by origin. document.cookie is
// origin-scoped; the network (curl) jar is separate. Session-only (no disk), which
// is the right default for a headless host. Single-threaded, so no lock.
using CookieList = std::vector<std::pair<std::string, std::string>>;
std::map<std::string, CookieList>& CookieStore() {
  static std::map<std::string, CookieList>* store =
      new std::map<std::string, CookieList>();
  return *store;
}

std::string OriginKey(const blink::KURL& url) {
  return blink::SecurityOrigin::Create(url)->ToString().Utf8();
}

void Trim(std::string* s) {
  while (!s->empty() && (s->front() == ' ' || s->front() == '\t'))
    s->erase(s->begin());
  while (!s->empty() && (s->back() == ' ' || s->back() == '\t'))
    s->pop_back();
}

std::string ToLowerAscii(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

class MbCookieManager : public network::mojom::blink::RestrictedCookieManager {
 public:
  void SetCookieFromString(
      const blink::KURL& url,
      const net::SiteForCookies&,
      const scoped_refptr<const blink::SecurityOrigin>&,
      net::StorageAccessApiStatus,
      bool /*is_ad_tagged*/,
      bool /*apply_devtools_overrides*/,
      const blink::String& cookie) override {
    std::string raw = cookie.Utf8();
    // "name=value; attr; attr" — take the name=value, inspect attrs for deletion.
    std::string pair = raw;
    std::string attrs;
    if (auto semi = raw.find(';'); semi != std::string::npos) {
      pair = raw.substr(0, semi);
      attrs = ToLowerAscii(raw.substr(semi + 1));
    }
    auto eq = pair.find('=');
    if (eq == std::string::npos)
      return;
    std::string name = pair.substr(0, eq);
    std::string value = pair.substr(eq + 1);
    Trim(&name);
    Trim(&value);
    if (name.empty())
      return;
    CookieList& jar = CookieStore()[OriginKey(url)];
    const bool deleting = attrs.find("max-age=0") != std::string::npos ||
                          attrs.find("expires=thu, 01 jan 1970") != std::string::npos;
    for (auto it = jar.begin(); it != jar.end(); ++it) {
      if (it->first == name) {
        if (deleting) jar.erase(it);
        else it->second = value;
        return;
      }
    }
    if (!deleting)
      jar.emplace_back(std::move(name), std::move(value));
    // Bridge to the HTTP cookie jar so a cookie set via document.cookie is also
    // sent on subsequent network requests (no-op for non-http(s) origins).
    MbAddCookieToJar(url.GetString().Utf8(), raw);
  }

  void GetCookiesString(const blink::KURL& url,
                        const net::SiteForCookies&,
                        const scoped_refptr<const blink::SecurityOrigin>&,
                        net::StorageAccessApiStatus,
                        bool /*get_version_shared_memory*/,
                        bool /*is_ad_tagged*/,
                        bool /*apply_devtools_overrides*/,
                        bool /*force_disable_third_party_cookies*/,
                        GetCookiesStringCallback callback) override {
    std::string out;
    if (auto it = CookieStore().find(OriginKey(url)); it != CookieStore().end()) {
      for (const auto& [n, v] : it->second) {
        if (!out.empty()) out += "; ";
        out += n + "=" + v;
      }
    }
    std::move(callback).Run(/*version=*/0u, base::ReadOnlySharedMemoryRegion(),
                            blink::String::FromUtf8(out));
  }

  void CookiesEnabledFor(const blink::KURL&,
                         const net::SiteForCookies&,
                         const scoped_refptr<const blink::SecurityOrigin>&,
                         net::StorageAccessApiStatus,
                         bool /*apply_devtools_overrides*/,
                         CookiesEnabledForCallback callback) override {
    std::move(callback).Run(true);
  }

  void GetAllForUrl(const blink::KURL&,
                    const net::SiteForCookies&,
                    const scoped_refptr<const blink::SecurityOrigin>&,
                    net::StorageAccessApiStatus,
                    network::mojom::blink::CookieManagerGetOptionsPtr,
                    bool /*is_ad_tagged*/,
                    bool /*apply_devtools_overrides*/,
                    bool /*force_disable_third_party_cookies*/,
                    GetAllForUrlCallback callback) override {
    // document.cookie doesn't use this path; return nothing.
    std::move(callback).Run({});
  }

  void SetCanonicalCookie(
      network::mojom::blink::RestrictedCanonicalCookieParamsPtr,
      const blink::KURL&,
      const net::SiteForCookies&,
      const scoped_refptr<const blink::SecurityOrigin>&,
      net::StorageAccessApiStatus,
      bool /*is_ad_tagged*/,
      bool /*apply_devtools_overrides*/,
      SetCanonicalCookieCallback callback) override {
    std::move(callback).Run(true);
  }

  void AddChangeListener(
      const blink::KURL&,
      const net::SiteForCookies&,
      const scoped_refptr<const blink::SecurityOrigin>&,
      net::StorageAccessApiStatus,
      mojo::PendingRemote<network::mojom::blink::CookieChangeListener>,
      AddChangeListenerCallback callback) override {
    // No change notifications in this host; just acknowledge.
    std::move(callback).Run();
  }
};

class MbBrowserInterfaceBroker
    : public blink::mojom::blink::BrowserInterfaceBroker {
 public:
  void GetInterface(mojo::GenericPendingReceiver receiver) override {
    if (auto r =
            receiver.As<network::mojom::blink::RestrictedCookieManager>()) {
      mojo::MakeSelfOwnedReceiver(std::make_unique<MbCookieManager>(),
                                  std::move(r));
      return;
    }
    // Drop everything else (no browser process).
  }
};

}  // namespace

mojo::PendingRemote<blink::mojom::blink::BrowserInterfaceBroker>
MakeFrameInterfaceBroker() {
  mojo::PendingRemote<blink::mojom::blink::BrowserInterfaceBroker> remote;
  mojo::MakeSelfOwnedReceiver(std::make_unique<MbBrowserInterfaceBroker>(),
                              remote.InitWithNewPipeAndPassReceiver());
  return remote;
}

}  // namespace mb
