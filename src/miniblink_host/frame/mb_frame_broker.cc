// mb_frame_broker.cc — in-process BrowserInterfaceBroker + RestrictedCookieManager.
#include "miniblink_host/frame/mb_frame_broker.h"

#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/read_only_shared_memory_region.h"
#include "base/task/single_thread_task_runner.h"
#include "miniblink_host/frame/mb_broadcast_channel.h"
#include "miniblink_host/frame/mb_cache_storage.h"
#include "miniblink_host/frame/mb_indexeddb.h"
#include "miniblink_host/frame/mb_lock_manager.h"
#include "miniblink_host/frame/mb_notification_service.h"
#include "miniblink_host/frame/mb_websocket.h"
#include "miniblink_host/loader/mb_url_loader.h"
#include "miniblink_host/runtime/mb_runtime.h"
#include "miniblink_host/worker/mb_shared_worker.h"
#include "mojo/public/cpp/bindings/generic_pending_receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_constants.h"
#include "net/cookies/cookie_inclusion_status.h"
#include "services/network/public/mojom/cookie_manager.mojom-blink.h"
#include "services/network/public/mojom/restricted_cookie_manager.mojom-blink.h"
#include "url/gurl.h"
#include "base/synchronization/lock.h"
#include "base/time/time.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "services/device/public/mojom/battery_monitor.mojom-blink.h"
#include "services/device/public/mojom/battery_status.mojom-blink.h"
#include "services/device/public/mojom/geolocation.mojom-blink.h"
#include "base/files/file.h"
#include "third_party/blink/public/mojom/clipboard/clipboard.mojom-blink.h"
#include "third_party/blink/public/mojom/credentialmanagement/credential_manager.mojom-blink.h"
#include "third_party/blink/public/mojom/installedapp/installed_app_provider.mojom-blink.h"
#include "media/mojo/mojom/video_decode_perf_history.mojom-blink.h"
#include "third_party/blink/public/mojom/ai/ai_classifier.mojom-blink.h"
#include "third_party/blink/public/mojom/ai/ai_language_model.mojom-blink.h"
#include "third_party/blink/public/mojom/ai/ai_manager.mojom-blink.h"
#include "third_party/blink/public/mojom/ai/ai_proofreader.mojom-blink.h"
#include "third_party/blink/public/mojom/ai/ai_rewriter.mojom-blink.h"
#include "third_party/blink/public/mojom/ai/ai_summarizer.mojom-blink.h"
#include "components/language_detection/content/common/language_detection.mojom-blink.h"
#include "third_party/blink/public/mojom/ai/ai_writer.mojom-blink.h"
#include "services/device/public/mojom/hid.mojom-blink.h"
#include "third_party/blink/public/mojom/bluetooth/web_bluetooth.mojom-blink.h"
#include "services/device/public/mojom/serial.mojom-blink.h"
#include "services/device/public/mojom/usb_device.mojom-blink.h"
#include "third_party/blink/public/mojom/hid/hid.mojom-blink.h"
#include "third_party/blink/public/mojom/on_device_translation/translation_manager.mojom-blink.h"
#include "third_party/blink/public/mojom/serial/serial.mojom-blink.h"
#include "third_party/blink/public/mojom/usb/web_usb_service.mojom-blink.h"
#include "third_party/blink/public/mojom/browsing_topics/browsing_topics.mojom-blink.h"
#include "media/mojo/mojom/webrtc_video_perf.mojom-blink.h"
#include "third_party/blink/public/mojom/sms/webotp_service.mojom-blink.h"
#include "third_party/blink/public/mojom/installedapp/related_application.mojom-blink.h"
#include "third_party/blink/public/mojom/webauthn/authenticator.mojom-blink.h"
#include "miniblink_host/frame/mb_opfs.h"
#include "miniblink_host/frame/mb_storage_buckets.h"
#include "third_party/blink/public/mojom/buckets/bucket_manager_host.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_manager.mojom-blink.h"
#include "third_party/blink/public/common/mediastream/media_devices.h"
#include "third_party/blink/public/mojom/mediastream/media_devices.mojom-blink.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "services/device/public/mojom/geoposition.mojom-blink.h"
#include "third_party/blink/public/mojom/geolocation/geolocation_service.mojom-blink.h"
#include "third_party/blink/public/mojom/permissions/permission.mojom-blink.h"
#include "services/device/public/mojom/wake_lock.mojom-blink.h"
#include "third_party/blink/public/mojom/quota/quota_manager_host.mojom-blink.h"
#include "third_party/blink/public/mojom/quota/quota_types.mojom-blink.h"
#include "third_party/blink/public/mojom/wake_lock/wake_lock.mojom-blink.h"
#include "third_party/blink/public/mojom/permissions/permission_status.mojom-blink.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace mb {
namespace {

// One process-wide in-memory cookie store, keyed by origin. document.cookie is
// origin-scoped; the network (curl) jar is separate. Session-only (no disk), which
// is the right default for a headless host. Only ever touched on the runtime
// service thread (MbCookieManager is bound there — see MakeFrameInterfaceBroker),
// so no lock is needed.
using CookieList = std::vector<std::pair<std::string, std::string>>;
std::map<std::string, CookieList>& CookieStore() {
  static std::map<std::string, CookieList>* store =
      new std::map<std::string, CookieList>();
  return *store;
}

std::string OriginKey(const blink::KURL& url) {
  return blink::SecurityOrigin::Create(url)->ToString().Utf8();
}

// Process-wide registry of cookieStore change observers, paired with the origin
// they watch. A page calls cookieStore.addEventListener('change',...) ->
// AddChangeListener; a cookie write (document.cookie or cookieStore.set/delete)
// fans out an OnCookieChange to every connected listener on the same origin.
// Service-thread only (same as CookieStore()), so no lock.
using CookieListenerEntry =
    std::pair<std::string,
              mojo::Remote<network::mojom::blink::CookieChangeListener>>;
std::vector<CookieListenerEntry>& CookieListeners() {
  static std::vector<CookieListenerEntry>* l =
      new std::vector<CookieListenerEntry>();
  return *l;
}

// Build a net::CanonicalCookie for (url, name, value) — the post-change cookie
// state carried in a CookieChangeInfo. Returns null if the cookie is invalid.
std::unique_ptr<net::CanonicalCookie> MakeCanonicalCookie(
    const blink::KURL& url,
    const std::string& name,
    const std::string& value) {
  net::CookieInclusionStatus status;
  return net::CanonicalCookie::CreateSanitizedCookie(
      GURL(url.GetString().Utf8()), name, value, /*domain=*/std::string(),
      /*path=*/"/", base::Time::Now(), /*expiration=*/base::Time(),
      base::Time::Now(), /*secure=*/false, /*http_only=*/false,
      net::CookieSameSite::UNSPECIFIED, net::COOKIE_PRIORITY_DEFAULT,
      /*partition_key=*/std::nullopt, &status);
}

// Notify cookieStore 'change' observers on `origin` of a cookie change. `cause`
// is INSERTED for a set (blink files it under changed[]) or EXPLICIT for a delete
// (deleted[]). Prunes disconnected listeners as it goes.
void NotifyCookieChange(const blink::KURL& url,
                        const std::string& name,
                        const std::string& value,
                        network::mojom::blink::CookieChangeCause cause) {
  auto& listeners = CookieListeners();
  if (listeners.empty())
    return;
  const std::string origin = OriginKey(url);
  std::unique_ptr<net::CanonicalCookie> cc = MakeCanonicalCookie(url, name, value);
  if (!cc)
    return;
  for (auto it = listeners.begin(); it != listeners.end();) {
    if (!it->second.is_connected()) {
      it = listeners.erase(it);
      continue;
    }
    if (it->first == origin) {
      it->second->OnCookieChange(network::mojom::blink::CookieChangeInfo::New(
          *cc, network::mojom::blink::CookieAccessResult::New(), cause));
    }
    ++it;
  }
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
      jar.emplace_back(name, value);
    // Bridge to the HTTP cookie jar so a cookie set via document.cookie is also
    // sent on subsequent network requests (no-op for non-http(s) origins).
    MbAddCookieToJar(url.GetString().Utf8(), raw);
    // Fan out a cookieStore 'change' event (document.cookie writes are observable).
    NotifyCookieChange(url, name, value,
                       deleting
                           ? network::mojom::blink::CookieChangeCause::EXPLICIT
                           : network::mojom::blink::CookieChangeCause::INSERTED);
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
    std::set<std::string> seen;  // names already emitted (store is authoritative)
    if (auto it = CookieStore().find(OriginKey(url)); it != CookieStore().end()) {
      for (const auto& [n, v] : it->second) {
        if (!out.empty()) out += "; ";
        out += n + "=" + v;
        seen.insert(n);
      }
    }
    // Also surface the shared HTTP jar's non-HttpOnly cookies (server Set-Cookie and
    // mbSetCookie-restored cookies never reach the in-memory store) so document.cookie
    // reflects them like a real browser. Empty for non-http(s). Store names win.
    const std::string jar = MbGetCookiesForUrl(url.GetString().Utf8());
    for (size_t pos = 0; pos < jar.size();) {
      const size_t semi = jar.find("; ", pos);
      const std::string pair =
          jar.substr(pos, semi == std::string::npos ? semi : semi - pos);
      pos = semi == std::string::npos ? jar.size() : semi + 2;
      const size_t eq = pair.find('=');
      if (eq == std::string::npos)
        continue;
      const std::string name = pair.substr(0, eq);
      if (name.empty() || seen.count(name))
        continue;
      if (!out.empty()) out += "; ";
      out += pair;
      seen.insert(name);
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

  // cookieStore.get()/getAll(): return the origin's cookies as CanonicalCookies, applying the
  // options filter (EQUALS = exact name, STARTS_WITH = name prefix; the empty-prefix form
  // getAll() uses matches every cookie). Shares the same jar as document.cookie.
  void GetAllForUrl(const blink::KURL& url,
                    const net::SiteForCookies&,
                    const scoped_refptr<const blink::SecurityOrigin>&,
                    net::StorageAccessApiStatus,
                    network::mojom::blink::CookieManagerGetOptionsPtr options,
                    bool /*is_ad_tagged*/,
                    bool /*apply_devtools_overrides*/,
                    bool /*force_disable_third_party_cookies*/,
                    GetAllForUrlCallback callback) override {
    blink::Vector<network::mojom::blink::CookieWithAccessResultPtr> out;
    GURL gurl(url.GetString().Utf8());
    const std::string filter = options ? options->name.Utf8() : std::string();
    const bool exact =
        options &&
        options->match_type == network::mojom::blink::CookieMatchType::EQUALS;
    const auto matches = [&](const std::string& n) {
      return exact ? (n == filter) : (n.rfind(filter, 0) == 0);
    };
    std::set<std::string> seen;  // store names are authoritative
    const auto add_cookie = [&](const std::string& n, const std::string& v) {
      net::CookieInclusionStatus status;
      std::unique_ptr<net::CanonicalCookie> cc =
          net::CanonicalCookie::CreateSanitizedCookie(
              gurl, n, v, /*domain=*/std::string(), /*path=*/"/",
              base::Time::Now(), /*expiration=*/base::Time(), base::Time::Now(),
              /*secure=*/false, /*http_only=*/false,
              net::CookieSameSite::UNSPECIFIED, net::COOKIE_PRIORITY_DEFAULT,
              /*partition_key=*/std::nullopt, &status);
      if (cc) {
        out.push_back(network::mojom::blink::CookieWithAccessResult::New(
            *cc, network::mojom::blink::CookieAccessResult::New()));
        seen.insert(n);
      }
    };
    if (auto it = CookieStore().find(OriginKey(url)); it != CookieStore().end()) {
      for (const auto& [n, v] : it->second) {
        if (matches(n))
          add_cookie(n, v);
      }
    }
    // Also surface the HTTP jar's non-HttpOnly cookies (server Set-Cookie / mbSetCookie),
    // names not already from the store — keeping cookieStore.getAll consistent with
    // document.cookie (GetCookiesString).
    const std::string jar = MbGetCookiesForUrl(url.GetString().Utf8());
    for (size_t pos = 0; pos < jar.size();) {
      const size_t semi = jar.find("; ", pos);
      const std::string pair =
          jar.substr(pos, semi == std::string::npos ? semi : semi - pos);
      pos = semi == std::string::npos ? jar.size() : semi + 2;
      const size_t eq = pair.find('=');
      if (eq == std::string::npos)
        continue;
      const std::string n = pair.substr(0, eq);
      if (n.empty() || seen.count(n) || !matches(n))
        continue;
      add_cookie(n, pair.substr(eq + 1));
    }
    std::move(callback).Run(std::move(out));
  }

  // cookieStore.set()/delete(): write into the shared jar (and bridge to the HTTP jar). An
  // empty value with an expiry in the past (how CookieStore::Delete is encoded) removes it.
  void SetCanonicalCookie(
      network::mojom::blink::RestrictedCanonicalCookieParamsPtr params,
      const blink::KURL& url,
      const net::SiteForCookies&,
      const scoped_refptr<const blink::SecurityOrigin>&,
      net::StorageAccessApiStatus,
      bool /*is_ad_tagged*/,
      bool /*apply_devtools_overrides*/,
      SetCanonicalCookieCallback callback) override {
    if (params && !params->name.empty()) {
      std::string name = params->name.Utf8();
      std::string value = params->value.Utf8();
      const bool deleting =
          !params->expires.is_null() && params->expires <= base::Time::Now();
      CookieList& jar = CookieStore()[OriginKey(url)];
      bool found = false;
      for (auto it = jar.begin(); it != jar.end(); ++it) {
        if (it->first == name) {
          if (deleting)
            jar.erase(it);
          else
            it->second = value;
          found = true;
          break;
        }
      }
      if (!found && !deleting)
        jar.emplace_back(name, value);
      MbAddCookieToJar(url.GetString().Utf8(),
                       deleting ? (name + "=; max-age=0") : (name + "=" + value));
      // cookieStore.set()/delete() is observable via cookieStore.onchange.
      NotifyCookieChange(url, name, value,
                         deleting
                             ? network::mojom::blink::CookieChangeCause::EXPLICIT
                             : network::mojom::blink::CookieChangeCause::INSERTED);
    }
    std::move(callback).Run(true);
  }

  void AddChangeListener(
      const blink::KURL& url,
      const net::SiteForCookies&,
      const scoped_refptr<const blink::SecurityOrigin>&,
      net::StorageAccessApiStatus,
      mojo::PendingRemote<network::mojom::blink::CookieChangeListener> listener,
      AddChangeListenerCallback callback) override {
    // Register the observer against its origin; a later cookie write fans out an
    // OnCookieChange to it (see NotifyCookieChange). cookieStore.onchange now fires.
    CookieListeners().emplace_back(
        OriginKey(url),
        mojo::Remote<network::mojom::blink::CookieChangeListener>(
            std::move(listener)));
    std::move(callback).Run();
  }
};

// Minimal in-process PermissionService. With no browser, navigator.permissions.query()
// otherwise never resolves (the request is dropped) and a page awaiting it HANGS. Answer
// every query DENIED — the headless reality (no permission is granted; geolocation etc.
// already error out), so a permission-gated page takes its no-permission path instead of
// stalling. Requests/observers are no-ops. Bound on the broker's thread (async; no [Sync]).
class MbPermissionService : public blink::mojom::blink::PermissionService {
 public:
  void HasPermission(blink::mojom::blink::PermissionDescriptorPtr permission,
                     HasPermissionCallback callback) override {
    std::move(callback).Run(StatusFor(permission));
  }
  void RegisterPageEmbeddedPermissionControl(
      blink::Vector<blink::mojom::blink::PermissionDescriptorPtr>,
      blink::mojom::blink::EmbeddedPermissionRequestDescriptorPtr,
      mojo::PendingRemote<blink::mojom::blink::EmbeddedPermissionControlClient>)
      override {}
  void RequestPageEmbeddedPermission(
      blink::Vector<blink::mojom::blink::PermissionDescriptorPtr>,
      blink::mojom::blink::EmbeddedPermissionRequestDescriptorPtr,
      RequestPageEmbeddedPermissionCallback callback) override {
    std::move(callback).Run(
        blink::mojom::blink::EmbeddedPermissionControlResult::kDismissed);
  }
  void RequestPermission(blink::mojom::blink::PermissionDescriptorPtr permission,
                         RequestPermissionCallback callback) override {
    std::move(callback).Run(StatusFor(permission));
  }
  void RequestPermissions(
      blink::Vector<blink::mojom::blink::PermissionDescriptorPtr> permissions,
      RequestPermissionsCallback callback) override {
    blink::Vector<blink::mojom::blink::PermissionStatusWithDetailsPtr> out;
    out.reserve(permissions.size());
    for (blink::wtf_size_t i = 0; i < permissions.size(); ++i)
      out.push_back(StatusFor(permissions[i]));
    std::move(callback).Run(std::move(out));
  }
  void RevokePermission(blink::mojom::blink::PermissionDescriptorPtr,
                        RevokePermissionCallback callback) override {
    std::move(callback).Run(Denied());
  }
  void AddPermissionObserver(
      blink::mojom::blink::PermissionDescriptorPtr,
      blink::mojom::blink::PermissionStatusWithDetailsPtr,
      mojo::PendingRemote<blink::mojom::blink::PermissionObserver>) override {}
  void AddPageEmbeddedPermissionObserver(
      blink::mojom::blink::PermissionDescriptorPtr,
      blink::mojom::blink::PermissionStatus,
      mojo::PendingRemote<blink::mojom::blink::PermissionObserver>) override {}
  void NotifyEventListener(blink::mojom::blink::PermissionDescriptorPtr,
                           const blink::String&, bool) override {}

 private:
  static blink::mojom::blink::PermissionStatusWithDetailsPtr Denied() {
    return blink::mojom::blink::PermissionStatusWithDetails::New(
        blink::mojom::blink::PermissionStatus::DENIED, nullptr);
  }
  // GRANT clipboard read/write (so navigator.clipboard works against our in-process
  // ClipboardHost); everything else stays DENIED (the headless default).
  static blink::mojom::blink::PermissionStatusWithDetailsPtr StatusFor(
      const blink::mojom::blink::PermissionDescriptorPtr& d) {
    const bool grant =
        d && (d->name == blink::mojom::blink::PermissionName::CLIPBOARD_READ ||
              d->name == blink::mojom::blink::PermissionName::CLIPBOARD_WRITE ||
              d->name == blink::mojom::blink::PermissionName::NOTIFICATIONS ||
              d->name == blink::mojom::blink::PermissionName::SCREEN_WAKE_LOCK);
    return blink::mojom::blink::PermissionStatusWithDetails::New(
        grant ? blink::mojom::blink::PermissionStatus::GRANTED
              : blink::mojom::blink::PermissionStatus::DENIED,
        nullptr);
  }
};

// Process-wide in-memory clipboard (plain text). Shared by the page (ClipboardHost) and
// the host C-ABI (mbGet/SetClipboard). Guarded by a lock: ClipboardHost runs on the
// broker's service thread, the C-ABI on the main thread.
base::Lock& ClipLock() {
  static base::Lock* l = new base::Lock();
  return *l;
}
std::string& ClipText() {
  static std::string* s = new std::string();
  return *s;
}
uint64_t& ClipSeqRef() {
  static uint64_t* n = new uint64_t(1);
  return *n;
}
void ClipSet(const std::string& text) {
  base::AutoLock al(ClipLock());
  ClipText() = text;
  ++ClipSeqRef();
}
std::string ClipGet() {
  base::AutoLock al(ClipLock());
  return ClipText();
}
uint64_t ClipSeq() {
  base::AutoLock al(ClipLock());
  return ClipSeqRef();
}

// blink.mojom.ClipboardHost: backs navigator.clipboard (read/write text) and
// execCommand('copy'/'paste') with the in-memory store. Only plain text is kept; the
// other formats read empty / write no-op. Reports the clipboard permission as allowed.
class MbClipboardHost : public blink::mojom::blink::ClipboardHost {
 public:
  void GetSequenceNumber(blink::mojom::blink::ClipboardBuffer,
                         GetSequenceNumberCallback cb) override {
    std::move(cb).Run(absl::uint128(ClipSeq()));
  }
  void IsFormatAvailable(blink::mojom::blink::ClipboardFormat format,
                         blink::mojom::blink::ClipboardBuffer,
                         IsFormatAvailableCallback cb) override {
    std::move(cb).Run(format == blink::mojom::blink::ClipboardFormat::kPlaintext &&
                      !ClipGet().empty());
  }
  void ReadAvailableTypes(blink::mojom::blink::ClipboardBuffer,
                          ReadAvailableTypesCallback cb) override {
    blink::Vector<blink::String> types;
    if (!ClipGet().empty())
      types.push_back("text/plain");
    std::move(cb).Run(std::move(types));
  }
  void ReadText(blink::mojom::blink::ClipboardBuffer,
                ReadTextCallback cb) override {
    std::move(cb).Run(blink::String::FromUtf8(ClipGet()));
  }
  void ReadHtml(blink::mojom::blink::ClipboardBuffer,
                ReadHtmlCallback cb) override {
    std::move(cb).Run(blink::String(), blink::KURL(), 0u, 0u);
  }
  void ReadSvg(blink::mojom::blink::ClipboardBuffer,
               ReadSvgCallback cb) override {
    std::move(cb).Run(blink::String());
  }
  void ReadRtf(blink::mojom::blink::ClipboardBuffer,
               ReadRtfCallback cb) override {
    std::move(cb).Run(blink::String());
  }
  void ReadPng(blink::mojom::blink::ClipboardBuffer,
               ReadPngCallback cb) override {
    std::move(cb).Run(mojo_base::BigBuffer());
  }
  void ReadFiles(blink::mojom::blink::ClipboardBuffer,
                 ReadFilesCallback cb) override {
    std::move(cb).Run(blink::mojom::blink::ClipboardFiles::New());
  }
  void ReadDataTransferCustomData(
      blink::mojom::blink::ClipboardBuffer, const blink::String&,
      ReadDataTransferCustomDataCallback cb) override {
    std::move(cb).Run(blink::String());
  }
  void ReadAvailableCustomAndStandardFormats(
      ReadAvailableCustomAndStandardFormatsCallback cb) override {
    std::move(cb).Run(blink::Vector<blink::String>());
  }
  void ReadUnsanitizedCustomFormat(
      const blink::String&, ReadUnsanitizedCustomFormatCallback cb) override {
    std::move(cb).Run(mojo_base::BigBuffer());
  }
  void WriteText(const blink::String& text) override { ClipSet(text.Utf8()); }
  void WriteHtml(const blink::String&, const blink::KURL&) override {}
  void WriteSvg(const blink::String&) override {}
  void WriteSmartPasteMarker() override {}
  void WriteDataTransferCustomData(
      const blink::HashMap<blink::String, blink::String>&) override {}
  void WriteBookmark(const blink::String&, const blink::String&) override {}
  void WriteImage(const SkBitmap&) override {}
  void WriteUnsanitizedCustomFormat(const blink::String&,
                                    mojo_base::BigBuffer) override {}
  void CommitWrite() override {}
  void WriteStringToFindPboard(const blink::String&) override {}
  void GetPlatformPermissionState(
      GetPlatformPermissionStateCallback cb) override {
    std::move(cb).Run(
        blink::mojom::blink::PlatformClipboardPermissionState::kAllow);
  }
  void RegisterClipboardListener(
      mojo::PendingRemote<blink::mojom::blink::ClipboardListener>) override {}
};

// navigator.storage.estimate(): report a generous quota and zero usage. We have no real
// persistent quota system; a non-zero quota lets apps that gate features on
// storage.estimate() proceed instead of treating storage as unavailable.
class MbQuotaManagerHost : public blink::mojom::blink::QuotaManagerHost {
 public:
  void QueryStorageUsageAndQuota(
      QueryStorageUsageAndQuotaCallback callback) override {
    std::move(callback).Run(blink::mojom::blink::QuotaStatusCode::kOk,
                            /*current_usage=*/0,
                            /*current_quota=*/int64_t{2} * 1024 * 1024 * 1024,
                            blink::mojom::blink::UsageBreakdown::New());
  }
};

// navigator.wakeLock — a headless no-op device WakeLock (no real screen to keep awake), so
// request('screen') resolves with a live sentinel instead of being unavailable.
class MbWakeLock : public device::mojom::blink::WakeLock {
 public:
  void RequestWakeLock() override {}
  void CancelWakeLock() override {}
  void AddClient(
      mojo::PendingReceiver<device::mojom::blink::WakeLock> receiver) override {
    mojo::MakeSelfOwnedReceiver(std::make_unique<MbWakeLock>(),
                                std::move(receiver));
  }
  void ChangeType(device::mojom::blink::WakeLockType,
                  ChangeTypeCallback callback) override {
    std::move(callback).Run(true);
  }
  void HasWakeLockForTests(HasWakeLockForTestsCallback callback) override {
    std::move(callback).Run(false);
  }
};

class MbWakeLockService : public blink::mojom::blink::WakeLockService {
 public:
  void GetWakeLock(
      device::mojom::blink::WakeLockType,
      device::mojom::blink::WakeLockReason,
      const blink::String&,
      mojo::PendingReceiver<device::mojom::blink::WakeLock> receiver) override {
    mojo::MakeSelfOwnedReceiver(std::make_unique<MbWakeLock>(),
                                std::move(receiver));
  }
};

// device.mojom.BatteryMonitor for navigator.getBattery(). Headless: reports a static
// "plugged in, fully charged" battery. The spec long-polls via QueryNextStatus (each call
// resolves only when the status changes), so we answer the first call with the fixed status
// and hold every later call open forever — the value never changes in a headless host.
class MbBatteryMonitor : public device::mojom::blink::BatteryMonitor {
 public:
  void QueryNextStatus(QueryNextStatusCallback callback) override {
    if (answered_) {
      held_ = std::move(callback);  // no further changes; keep the long-poll pending
      return;
    }
    answered_ = true;
    auto status = device::mojom::blink::BatteryStatus::New();
    status->charging = true;
    status->charging_time = 0.0;  // fully charged
    status->discharging_time = std::numeric_limits<double>::infinity();
    status->level = 1.0;
    std::move(callback).Run(std::move(status));
  }

 private:
  bool answered_ = false;
  QueryNextStatusCallback held_;
};

// blink.mojom.CredentialManager for navigator.credentials.get/store (password + federated). A
// headless host has no credential store, so Get returns SUCCESS + no credential (get() resolves
// to null) and Store/PreventSilentAccess just ack. This MUST be bound: the basic
// CredentialManager remote has NO disconnect handler in blink, so an unbound pipe leaves
// navigator.credentials.get()'s promise pending forever — a hang on login pages that probe for
// stored credentials at load.
class MbCredentialManager : public blink::mojom::blink::CredentialManager {
 public:
  void Store(blink::mojom::blink::CredentialInfoPtr,
             StoreCallback callback) override {
    std::move(callback).Run();
  }
  void PreventSilentAccess(PreventSilentAccessCallback callback) override {
    std::move(callback).Run();
  }
  void Get(blink::mojom::blink::CredentialMediationRequirement,
           bool,
           const blink::Vector<blink::KURL>&,
           GetCallback callback) override {
    // SUCCESS requires a non-null CredentialInfo (blink DCHECKs); an EMPTY-type one converts
    // to a null Credential, so get() resolves to null ("no stored credential").
    auto info = blink::mojom::blink::CredentialInfo::New();
    info->type = blink::mojom::blink::CredentialType::EMPTY;
    std::move(callback).Run(blink::mojom::blink::CredentialManagerError::SUCCESS,
                            std::move(info));
  }
};

// blink.mojom.Authenticator for WebAuthn (navigator.credentials publicKey + PublicKeyCredential
// statics). Headless has no authenticator. Like the CredentialManager, the Authenticator remote
// has NO disconnect handler, so the feature-detection statics commonly called at page load —
// isUserVerifyingPlatformAuthenticatorAvailable() / isConditionalMediationAvailable() — would
// HANG unbound. Answer those false; make/get credential reject cleanly (NOT_ALLOWED_ERROR).
class MbAuthenticator : public blink::mojom::blink::Authenticator {
 public:
  void MakeCredential(
      blink::mojom::blink::PublicKeyCredentialCreationOptionsPtr,
      MakeCredentialCallback callback) override {
    std::move(callback).Run(
        blink::mojom::blink::AuthenticatorStatus::NOT_ALLOWED_ERROR, nullptr,
        nullptr);
  }
  void GetCredential(blink::mojom::blink::GetCredentialOptionsPtr,
                     GetCredentialCallback callback) override {
    auto assertion = blink::mojom::blink::GetAssertionResponse::New(
        blink::mojom::blink::AuthenticatorStatus::NOT_ALLOWED_ERROR, nullptr,
        nullptr);
    std::move(callback).Run(
        blink::mojom::blink::GetCredentialResponse::NewGetAssertionResponse(
            std::move(assertion)));
  }
  void IsUserVerifyingPlatformAuthenticatorAvailable(
      IsUserVerifyingPlatformAuthenticatorAvailableCallback callback) override {
    std::move(callback).Run(false);
  }
  void IsConditionalMediationAvailable(
      IsConditionalMediationAvailableCallback callback) override {
    std::move(callback).Run(false);
  }
  void Report(blink::mojom::blink::PublicKeyCredentialReportOptionsPtr,
              ReportCallback callback) override {
    std::move(callback).Run(
        blink::mojom::blink::AuthenticatorStatus::NOT_ALLOWED_ERROR, nullptr);
  }
  void GetClientCapabilities(GetClientCapabilitiesCallback callback) override {
    std::move(callback).Run({});
  }
  void Cancel() override {}
};

// blink.mojom.InstalledAppProvider for navigator.getInstalledRelatedApps() — PWAs probe it on
// load to detect a companion native app. A headless host has none, so it returns []. MUST be
// bound: blink sets no disconnect handler here (there's an explicit TODO in
// installed_app_controller.cc), so unbound the promise hangs forever.
class MbInstalledAppProvider
    : public blink::mojom::blink::InstalledAppProvider {
 public:
  void FilterInstalledApps(
      blink::Vector<blink::mojom::blink::RelatedApplicationPtr>,
      const blink::KURL&,
      bool,
      FilterInstalledAppsCallback callback) override {
    std::move(callback).Run({});  // no installed related apps
  }
};

// blink.mojom.WebOTPService for navigator.credentials.get({otp}) (SMS one-time-code autofill on
// login pages). A headless host has no SMS backend, so Receive reports kBackendNotAvailable (the
// get() promise rejects cleanly). Like the other credential services this remote has no
// disconnect handler, so unbound the OTP request would hang during a login flow.
class MbWebOTPService : public blink::mojom::blink::WebOTPService {
 public:
  void Receive(ReceiveCallback callback) override {
    std::move(callback).Run(
        blink::mojom::blink::SmsStatus::kBackendNotAvailable, blink::String());
  }
  void Abort() override {}
};

// media.mojom.VideoDecodePerfHistory for navigator.mediaCapabilities.decodingInfo(). Video sites
// call decodingInfo on load to pick a codec; for a SUPPORTED video codec blink queries the perf
// history for the smooth/powerEfficient hints. That remote has no disconnect handler, so unbound
// the decodingInfo() promise HANGS (verified: a vp8/vp9/av1 config — codecs this build supports —
// triggers the query). Report smooth + power-efficient (the optimistic headless default).
class MbVideoDecodePerfHistory
    : public media::mojom::blink::VideoDecodePerfHistory {
 public:
  void GetPerfInfo(media::mojom::blink::PredictionFeaturesPtr,
                   GetPerfInfoCallback callback) override {
    std::move(callback).Run(/*is_smooth=*/true, /*is_power_efficient=*/true);
  }
};

// media.mojom.WebrtcVideoPerfHistory — the same, for decodingInfo({type:'webrtc'}).
class MbWebrtcVideoPerfHistory
    : public media::mojom::blink::WebrtcVideoPerfHistory {
 public:
  void GetPerfInfo(media::mojom::blink::WebrtcPredictionFeaturesPtr,
                   int32_t,
                   GetPerfInfoCallback callback) override {
    std::move(callback).Run(/*is_smooth=*/true);
  }
};

// blink.mojom.BrowsingTopicsDocumentService for document.browsingTopics() (Privacy Sandbox ad
// topics, called by ad scripts). The remote has no disconnect handler, so unbound the promise
// HANGS (verified). A headless host has no topics, so return an empty list — browsingTopics()
// resolves to [].
class MbBrowsingTopicsDocumentService
    : public blink::mojom::blink::BrowsingTopicsDocumentService {
 public:
  void GetBrowsingTopics(bool, GetBrowsingTopicsCallback callback) override {
    std::move(callback).Run(blink::Vector<blink::mojom::blink::EpochTopicPtr>());
  }
};

// blink.mojom.AIManager for the built-in on-device AI APIs (LanguageModel, Summarizer, Writer,
// Rewriter, Proofreader, Classifier — window.* surfaces). Sites probe `X.availability()` on load.
// This headless host has no on-device model, so every CanCreate* reports the model UNAVAILABLE
// (availability() -> 'unavailable') and every create() rejects via the client's OnError. MUST be
// bound: the AIManager remote has no disconnect handler, so an availability() probe HANGS unbound
// (verified — it crashed teardown with an unsettled ScriptPromiseResolver).
class MbAIManager : public blink::mojom::blink::AIManager {
 public:
  void CanCreateLanguageModel(
      blink::mojom::blink::AILanguageModelCreateOptionsPtr,
      CanCreateLanguageModelCallback cb) override {
    Unavailable(std::move(cb));
  }
  void CreateLanguageModel(
      mojo::PendingRemote<
          blink::mojom::blink::AIManagerCreateLanguageModelClient> client,
      blink::mojom::blink::AILanguageModelCreateOptionsPtr,
      mojo::PendingRemote<on_device_model::mojom::blink::DownloadObserver>)
      override {
    RejectCreate(std::move(client));
  }
  void CanCreateSummarizer(blink::mojom::blink::AISummarizerCreateOptionsPtr,
                           CanCreateSummarizerCallback cb) override {
    Unavailable(std::move(cb));
  }
  void CreateSummarizer(
      mojo::PendingRemote<blink::mojom::blink::AIManagerCreateSummarizerClient>
          client,
      blink::mojom::blink::AISummarizerCreateOptionsPtr,
      mojo::PendingRemote<on_device_model::mojom::blink::DownloadObserver>)
      override {
    RejectCreate(std::move(client));
  }
  void GetLanguageModelParams(GetLanguageModelParamsCallback cb) override {
    std::move(cb).Run(nullptr);
  }
  void CanCreateWriter(blink::mojom::blink::AIWriterCreateOptionsPtr,
                       CanCreateWriterCallback cb) override {
    Unavailable(std::move(cb));
  }
  void CreateWriter(
      mojo::PendingRemote<blink::mojom::blink::AIManagerCreateWriterClient>
          client,
      blink::mojom::blink::AIWriterCreateOptionsPtr,
      mojo::PendingRemote<on_device_model::mojom::blink::DownloadObserver>)
      override {
    RejectCreate(std::move(client));
  }
  void CanCreateRewriter(blink::mojom::blink::AIRewriterCreateOptionsPtr,
                         CanCreateRewriterCallback cb) override {
    Unavailable(std::move(cb));
  }
  void CreateRewriter(
      mojo::PendingRemote<blink::mojom::blink::AIManagerCreateRewriterClient>
          client,
      blink::mojom::blink::AIRewriterCreateOptionsPtr,
      mojo::PendingRemote<on_device_model::mojom::blink::DownloadObserver>)
      override {
    RejectCreate(std::move(client));
  }
  void CanCreateProofreader(blink::mojom::blink::AIProofreaderCreateOptionsPtr,
                            CanCreateProofreaderCallback cb) override {
    Unavailable(std::move(cb));
  }
  void CreateProofreader(
      mojo::PendingRemote<blink::mojom::blink::AIManagerCreateProofreaderClient>
          client,
      blink::mojom::blink::AIProofreaderCreateOptionsPtr,
      mojo::PendingRemote<on_device_model::mojom::blink::DownloadObserver>)
      override {
    RejectCreate(std::move(client));
  }
  void CanCreateClassifier(blink::mojom::blink::AIClassifierCreateOptionsPtr,
                           CanCreateClassifierCallback cb) override {
    Unavailable(std::move(cb));
  }
  void CreateClassifier(
      mojo::PendingRemote<blink::mojom::blink::AIManagerCreateClassifierClient>
          client,
      blink::mojom::blink::AIClassifierCreateOptionsPtr,
      mojo::PendingRemote<on_device_model::mojom::blink::DownloadObserver>)
      override {
    RejectCreate(std::move(client));
  }

 private:
  template <typename Cb>
  static void Unavailable(Cb cb) {
    std::move(cb).Run(blink::mojom::blink::ModelAvailabilityCheckResult::
                          kUnavailableServiceNotRunning);
  }
  template <typename Client>
  static void RejectCreate(mojo::PendingRemote<Client> client) {
    mojo::Remote<Client> remote(std::move(client));
    remote->OnError(
        blink::mojom::blink::AIManagerCreateClientError::kUnableToCreateSession,
        nullptr);
  }
};

// blink.mojom.TranslationManager for the Translator API. Translator.availability() probes this;
// no on-device translation service, so report unavailable and reject create().
class MbTranslationManager : public blink::mojom::blink::TranslationManager {
 public:
  void CreateTranslator(
      mojo::PendingRemote<
          blink::mojom::blink::TranslationManagerCreateTranslatorClient> client,
      blink::mojom::blink::TranslatorCreateOptionsPtr) override {
    mojo::Remote<blink::mojom::blink::TranslationManagerCreateTranslatorClient>
        remote(std::move(client));
    remote->OnResult(
        blink::mojom::blink::CreateTranslatorResult::NewError(
            blink::mojom::blink::CreateTranslatorError::kFailedToCreateTranslator),
        nullptr, nullptr);
  }
  void TranslationAvailable(blink::mojom::blink::TranslatorLanguageCodePtr,
                            blink::mojom::blink::TranslatorLanguageCodePtr,
                            TranslationAvailableCallback cb) override {
    std::move(cb).Run(
        blink::mojom::blink::CanCreateTranslatorResult::kNoServiceCrashed);
  }
};

// blink.mojom.WebUsbService for navigator.usb. Device dashboards call usb.getDevices() on load to
// list already-permitted devices; the service has no disconnect handler, so unbound that promise
// HANGS (verified — left an unsettled resolver, crashing teardown). Headless: no devices.
class MbWebUsbService : public blink::mojom::blink::WebUsbService {
 public:
  void GetDevices(GetDevicesCallback cb) override {
    std::move(cb).Run({});  // no permitted USB devices
  }
  void GetDevice(
      const blink::String&,
      mojo::PendingReceiver<device::mojom::blink::UsbDevice>) override {}
  void GetPermission(blink::mojom::blink::WebUsbRequestDeviceOptionsPtr,
                     GetPermissionCallback cb) override {
    std::move(cb).Run(nullptr);  // no device chosen -> requestDevice() rejects
  }
  void ForgetDevice(const blink::String&, ForgetDeviceCallback cb) override {
    std::move(cb).Run();
  }
  void SetClient(mojo::PendingAssociatedRemote<
                 device::mojom::blink::UsbDeviceManagerClient>) override {}
};

// blink.mojom.HidService for navigator.hid. getDevices() (list permitted devices) is called on
// load; no disconnect handler -> unbound it HANGS. Headless: no HID devices.
class MbHidService : public blink::mojom::blink::HidService {
 public:
  void RegisterClient(mojo::PendingAssociatedRemote<
                      device::mojom::blink::HidManagerClient>) override {}
  void GetDevices(GetDevicesCallback cb) override { std::move(cb).Run({}); }
  void RequestDevice(blink::Vector<blink::mojom::blink::HidDeviceFilterPtr>,
                     blink::Vector<blink::mojom::blink::HidDeviceFilterPtr>,
                     RequestDeviceCallback cb) override {
    std::move(cb).Run({});  // no device chosen -> requestDevice() resolves []
  }
  void Connect(const blink::String&,
               mojo::PendingRemote<device::mojom::blink::HidConnectionClient>,
               ConnectCallback cb) override {
    std::move(cb).Run(mojo::NullRemote());  // no connection
  }
  void Forget(device::mojom::blink::HidDeviceInfoPtr,
              ForgetCallback cb) override {
    std::move(cb).Run();
  }
};

// blink.mojom.SerialService for navigator.serial. getPorts() is called on load; same hang.
class MbSerialService : public blink::mojom::blink::SerialService {
 public:
  void SetClient(mojo::PendingRemote<blink::mojom::blink::SerialServiceClient>)
      override {}
  void GetPorts(GetPortsCallback cb) override { std::move(cb).Run({}); }
  void RequestPort(blink::Vector<blink::mojom::blink::SerialPortFilterPtr>,
                   const blink::Vector<blink::String>&,
                   RequestPortCallback cb) override {
    std::move(cb).Run(nullptr);  // no port chosen -> requestPort() rejects
  }
  void OpenPort(
      const base::UnguessableToken&,
      device::mojom::blink::SerialConnectionOptionsPtr,
      mojo::PendingRemote<device::mojom::blink::SerialPortClient>,
      OpenPortCallback cb) override {
    std::move(cb).Run(mojo::NullRemote());
  }
  void ForgetPort(const base::UnguessableToken&,
                  ForgetPortCallback cb) override {
    std::move(cb).Run();
  }
};

// blink.mojom.WebBluetoothService for navigator.bluetooth. getAvailability()/getDevices() are
// called on load; no disconnect handler -> unbound they HANG. Headless has no Bluetooth adapter,
// so availability is false, device lists are empty, and every GATT op (only reachable after a
// connect that never succeeds) reports NO_BLUETOOTH_ADAPTER.
class MbWebBluetoothService : public blink::mojom::blink::WebBluetoothService {
  using R = blink::mojom::blink::WebBluetoothResult;
  using Quantity = blink::mojom::blink::WebBluetoothGATTQueryQuantity;

 public:
  void GetAvailability(GetAvailabilityCallback cb) override {
    std::move(cb).Run(false);
  }
  void RequestDevice(blink::mojom::blink::WebBluetoothRequestDeviceOptionsPtr,
                     RequestDeviceCallback cb) override {
    std::move(cb).Run(R::NO_BLUETOOTH_ADAPTER, nullptr);
  }
  void GetDevices(GetDevicesCallback cb) override { std::move(cb).Run({}); }
  void ForgetDevice(const blink::WebBluetoothDeviceId&,
                    ForgetDeviceCallback cb) override {
    std::move(cb).Run();
  }
  void RemoteServerConnect(
      const blink::WebBluetoothDeviceId&,
      mojo::PendingAssociatedRemote<blink::mojom::blink::WebBluetoothServerClient>,
      RemoteServerConnectCallback cb) override {
    std::move(cb).Run(R::NO_BLUETOOTH_ADAPTER);
  }
  void RemoteServerDisconnect(const blink::WebBluetoothDeviceId&) override {}
  void RemoteServerGetPrimaryServices(
      const blink::WebBluetoothDeviceId&, Quantity, const blink::String&,
      RemoteServerGetPrimaryServicesCallback cb) override {
    std::move(cb).Run(R::NO_BLUETOOTH_ADAPTER, std::nullopt);
  }
  void RemoteServiceGetCharacteristics(
      const blink::String&, Quantity, const blink::String&,
      RemoteServiceGetCharacteristicsCallback cb) override {
    std::move(cb).Run(R::NO_BLUETOOTH_ADAPTER, std::nullopt);
  }
  void RemoteCharacteristicReadValue(
      const blink::String&,
      RemoteCharacteristicReadValueCallback cb) override {
    std::move(cb).Run(R::NO_BLUETOOTH_ADAPTER, {});
  }
  void RemoteCharacteristicWriteValue(
      const blink::String&, base::span<const uint8_t>,
      blink::mojom::blink::WebBluetoothWriteType,
      RemoteCharacteristicWriteValueCallback cb) override {
    std::move(cb).Run(R::NO_BLUETOOTH_ADAPTER);
  }
  void RemoteCharacteristicStartNotifications(
      const blink::String&,
      mojo::PendingAssociatedRemote<
          blink::mojom::blink::WebBluetoothCharacteristicClient>,
      RemoteCharacteristicStartNotificationsCallback cb) override {
    std::move(cb).Run(R::NO_BLUETOOTH_ADAPTER);
  }
  void RemoteCharacteristicStopNotifications(
      const blink::String&,
      RemoteCharacteristicStopNotificationsCallback cb) override {
    std::move(cb).Run();
  }
  void RemoteCharacteristicGetDescriptors(
      const blink::String&, Quantity, const blink::String&,
      RemoteCharacteristicGetDescriptorsCallback cb) override {
    std::move(cb).Run(R::NO_BLUETOOTH_ADAPTER, std::nullopt);
  }
  void RemoteDescriptorReadValue(
      const blink::String&, RemoteDescriptorReadValueCallback cb) override {
    std::move(cb).Run(R::NO_BLUETOOTH_ADAPTER, {});
  }
  void RemoteDescriptorWriteValue(
      const blink::String&, base::span<const uint8_t>,
      RemoteDescriptorWriteValueCallback cb) override {
    std::move(cb).Run(R::NO_BLUETOOTH_ADAPTER);
  }
  void RequestScanningStart(
      mojo::PendingAssociatedRemote<
          blink::mojom::blink::WebBluetoothAdvertisementClient>,
      blink::mojom::blink::WebBluetoothRequestLEScanOptionsPtr,
      RequestScanningStartCallback cb) override {
    std::move(cb).Run(R::NO_BLUETOOTH_ADAPTER);
  }
  void WatchAdvertisementsForDevice(
      const blink::WebBluetoothDeviceId&,
      mojo::PendingAssociatedRemote<
          blink::mojom::blink::WebBluetoothAdvertisementClient>,
      WatchAdvertisementsForDeviceCallback cb) override {
    std::move(cb).Run(R::NO_BLUETOOTH_ADAPTER);
  }
};

// language_detection.mojom ContentLanguageDetectionDriver for LanguageDetector.availability().
// No on-device model: report the model not-available and hand back an invalid File.
class MbContentLanguageDetectionDriver
    : public language_detection::mojom::blink::ContentLanguageDetectionDriver {
 public:
  void GetLanguageDetectionModel(
      GetLanguageDetectionModelCallback cb) override {
    std::move(cb).Run(base::File());  // invalid — no model
  }
  void GetLanguageDetectionModelStatus(
      GetLanguageDetectionModelStatusCallback cb) override {
    std::move(cb).Run(
        language_detection::mojom::blink::LanguageDetectionModelStatus::
            kNotAvailable);
  }
};

// blink.mojom.MediaDevicesDispatcherHost for navigator.mediaDevices. Headless has no cameras,
// mics, or speakers, so every query returns an EMPTY list. This must be bound: if the pipe is
// left unbound, blink's disconnect handler REJECTS enumerateDevices() with an AbortError
// instead of resolving to [] — so a page's feature probe breaks. The capability getters return
// empty; the rarely-used output-selection methods are never reached in a headless host.
class MbMediaDevicesDispatcherHost
    : public blink::mojom::blink::MediaDevicesDispatcherHost {
 public:
  void EnumerateDevices(bool, bool, bool, bool, bool,
                        EnumerateDevicesCallback callback) override {
    // blink DCHECKs the outer list has exactly kNumMediaDeviceTypes entries (audio input,
    // video input, audio output) — each an empty per-type list in a headless host.
    blink::Vector<blink::Vector<blink::WebMediaDeviceInfo>> devices;
    devices.resize(static_cast<blink::wtf_size_t>(
        blink::mojom::blink::MediaDeviceType::kNumMediaDeviceTypes));
    std::move(callback).Run(devices, {}, {});
  }
  void GetVideoInputCapabilities(
      GetVideoInputCapabilitiesCallback callback) override {
    std::move(callback).Run({});
  }
  void GetAllVideoInputDeviceFormats(
      const blink::String&,
      GetAllVideoInputDeviceFormatsCallback callback) override {
    std::move(callback).Run({});
  }
  void GetAvailableVideoInputDeviceFormats(
      const blink::String&,
      GetAvailableVideoInputDeviceFormatsCallback callback) override {
    std::move(callback).Run({});
  }
  void GetAudioInputCapabilities(
      GetAudioInputCapabilitiesCallback callback) override {
    std::move(callback).Run({});
  }
  void AddMediaDevicesListener(
      bool, bool, bool,
      mojo::PendingRemote<blink::mojom::blink::MediaDevicesListener>) override {}
  void SetCaptureHandleConfig(
      blink::mojom::blink::CaptureHandleConfigPtr) override {}
  void CloseFocusWindowOfOpportunity(const blink::String&) override {}
  void ProduceSubCaptureTargetId(
      media::mojom::blink::SubCaptureTargetType,
      ProduceSubCaptureTargetIdCallback callback) override {
    std::move(callback).Run(blink::String());
  }
  // Output-device selection needs a real device to answer; never reached headless.
  void SetPreferredSinkId(const blink::String&,
                          SetPreferredSinkIdCallback) override {}
  void SelectAudioOutput(const blink::String&,
                         SelectAudioOutputCallback) override {}
};


// Process-wide configured geolocation fix (set via mbSetGeolocation). Read on the
// broker's service thread, written from the main thread, so guard with a lock. When
// unset, geolocation stays denied (the headless default — getCurrentPosition errors).
struct MbGeoFix {
  bool set = false;
  double lat = 0, lng = 0, accuracy = 0;
};
base::Lock& GeoLock() {
  static base::Lock* l = new base::Lock();
  return *l;
}
MbGeoFix& GeoFix() {
  static MbGeoFix* g = new MbGeoFix();
  return *g;
}
bool GeoGet(MbGeoFix* out) {
  base::AutoLock al(GeoLock());
  *out = GeoFix();
  return out->set;
}

// device.mojom.Geolocation: hands back the configured fix on every position query.
class MbGeolocation : public device::mojom::blink::Geolocation {
 public:
  void SetHighAccuracyHint(bool) override {}
  void QueryNextPosition(QueryNextPositionCallback callback) override {
    std::move(callback).Run(MakeResult());
  }
  void QueryCachedPosition(QueryCachedPositionCallback callback) override {
    std::move(callback).Run(MakeResult());
  }

 private:
  static device::mojom::blink::GeopositionResultPtr MakeResult() {
    MbGeoFix fix;
    GeoGet(&fix);  // only reached when granted, i.e. fix.set is true
    auto pos = device::mojom::blink::Geoposition::New();
    pos->latitude = fix.lat;
    pos->longitude = fix.lng;
    pos->accuracy = fix.accuracy > 0 ? fix.accuracy : 10.0;
    pos->timestamp = base::Time::Now();
    return device::mojom::blink::GeopositionResult::NewPosition(std::move(pos));
  }
};

// blink.mojom.GeolocationService: grants only when a fix is configured (else the page's
// getCurrentPosition errors PERMISSION_DENIED, the headless default), then binds an
// MbGeolocation that serves the fix.
class MbGeolocationService : public blink::mojom::blink::GeolocationService {
 public:
  void CreateGeolocation(
      mojo::PendingReceiver<device::mojom::blink::Geolocation> receiver,
      bool /*user_gesture*/, blink::mojom::blink::GeolocationAccuracy,
      CreateGeolocationCallback callback) override {
    MbGeoFix fix;
    const bool granted = GeoGet(&fix);
    if (granted) {
      mojo::MakeSelfOwnedReceiver(std::make_unique<MbGeolocation>(),
                                  std::move(receiver));
    }
    std::move(callback).Run(granted
                                ? blink::mojom::blink::PermissionStatus::GRANTED
                                : blink::mojom::blink::PermissionStatus::DENIED);
  }
};

class MbBrowserInterfaceBroker
    : public blink::mojom::blink::BrowserInterfaceBroker {
 public:
  // `main_runner` is the renderer main-thread runner. This broker is bound on the SERVICE
  // thread, so any interface that must run on the main thread (SharedWorkerConnector ->
  // WebSharedWorker::CreateAndStart) is posted there.
  explicit MbBrowserInterfaceBroker(
      scoped_refptr<base::SingleThreadTaskRunner> main_runner)
      : main_runner_(std::move(main_runner)) {}

  void GetInterface(mojo::GenericPendingReceiver receiver) override {
    // new SharedWorker(): run the connector on the main thread (CreateAndStart is
    // main-thread only); we're on the service thread here.
    if (auto r = receiver.As<blink::mojom::blink::SharedWorkerConnector>()) {
      if (main_runner_) {
        main_runner_->PostTask(
            FROM_HERE,
            base::BindOnce(&BindSharedWorkerConnector, std::move(r)));
      }
      return;
    }
    if (auto r =
            receiver.As<network::mojom::blink::RestrictedCookieManager>()) {
      mojo::MakeSelfOwnedReceiver(std::make_unique<MbCookieManager>(),
                                  std::move(r));
      return;
    }
    // navigator.permissions.query / .request — answer so the promise resolves.
    if (auto r = receiver.As<blink::mojom::blink::PermissionService>()) {
      mojo::MakeSelfOwnedReceiver(std::make_unique<MbPermissionService>(),
                                  std::move(r));
      return;
    }
    // navigator.geolocation — serve the configured fix (or deny if none set).
    if (auto r = receiver.As<blink::mojom::blink::GeolocationService>()) {
      mojo::MakeSelfOwnedReceiver(std::make_unique<MbGeolocationService>(),
                                  std::move(r));
      return;
    }
    // navigator.clipboard / execCommand copy+paste — in-memory text clipboard.
    if (auto r = receiver.As<blink::mojom::blink::ClipboardHost>()) {
      mojo::MakeSelfOwnedReceiver(std::make_unique<MbClipboardHost>(),
                                  std::move(r));
      return;
    }
    // navigator.locks — real exclusive/shared lock serialization.
    if (auto r = receiver.As<blink::mojom::blink::LockManager>()) {
      BindLockManager(std::move(r));
      return;
    }
    // Worker BroadcastChannel (the worker path asks its broker; windows use the
    // nav-associated provider instead). Same process-wide registry as windows.
    if (auto r = receiver.As<blink::mojom::blink::BroadcastChannelProvider>()) {
      BindBroadcastChannelProviderPipe(std::move(r));
      return;
    }
    // Notification API — permission granted; new Notification() fires onshow.
    if (auto r = receiver.As<blink::mojom::blink::NotificationService>()) {
      BindNotificationService(std::move(r));
      return;
    }
    // WebSocket — establishes the connection (onopen) + a loopback echo data plane.
    if (auto r = receiver.As<blink::mojom::blink::WebSocketConnector>()) {
      BindWebSocketConnector(std::move(r));
      return;
    }
    // navigator.storage.estimate() — report a generous quota.
    if (auto r = receiver.As<blink::mojom::blink::QuotaManagerHost>()) {
      mojo::MakeSelfOwnedReceiver(std::make_unique<MbQuotaManagerHost>(),
                                  std::move(r));
      return;
    }
    // IndexedDB — in-memory backend (open + object stores; reads/writes in step 2).
    if (auto r = receiver.As<blink::mojom::blink::IDBFactory>()) {
      BindIDBFactory(std::move(r));
      return;
    }
    // navigator.wakeLock — headless no-op (permission granted below).
    if (auto r = receiver.As<blink::mojom::blink::WakeLockService>()) {
      mojo::MakeSelfOwnedReceiver(std::make_unique<MbWakeLockService>(),
                                  std::move(r));
      return;
    }
    // caches (Cache Storage) — in-process Request/Response cache.
    if (auto r = receiver.As<blink::mojom::blink::CacheStorage>()) {
      BindCacheStorage(std::move(r));
      return;
    }
    // navigator.getBattery() — headless static "full, charging" battery.
    if (auto r = receiver.As<device::mojom::blink::BatteryMonitor>()) {
      mojo::MakeSelfOwnedReceiver(std::make_unique<MbBatteryMonitor>(),
                                  std::move(r));
      return;
    }
    // navigator.credentials.get/store — headless: no credential store (get() -> null).
    if (auto r = receiver.As<blink::mojom::blink::CredentialManager>()) {
      mojo::MakeSelfOwnedReceiver(std::make_unique<MbCredentialManager>(),
                                  std::move(r));
      return;
    }
    // WebAuthn (navigator.credentials publicKey) — headless: no authenticator.
    if (auto r = receiver.As<blink::mojom::blink::Authenticator>()) {
      mojo::MakeSelfOwnedReceiver(std::make_unique<MbAuthenticator>(),
                                  std::move(r));
      return;
    }
    // navigator.getInstalledRelatedApps() — headless: no installed apps ([]).
    if (auto r = receiver.As<blink::mojom::blink::InstalledAppProvider>()) {
      mojo::MakeSelfOwnedReceiver(std::make_unique<MbInstalledAppProvider>(),
                                  std::move(r));
      return;
    }
    // navigator.credentials.get({otp}) (WebOTP/SMS) — headless: no SMS backend.
    if (auto r = receiver.As<blink::mojom::blink::WebOTPService>()) {
      mojo::MakeSelfOwnedReceiver(std::make_unique<MbWebOTPService>(),
                                  std::move(r));
      return;
    }
    // navigator.mediaCapabilities.decodingInfo() perf hints — smooth + power-efficient.
    if (auto r = receiver.As<media::mojom::blink::VideoDecodePerfHistory>()) {
      mojo::MakeSelfOwnedReceiver(std::make_unique<MbVideoDecodePerfHistory>(),
                                  std::move(r));
      return;
    }
    if (auto r = receiver.As<media::mojom::blink::WebrtcVideoPerfHistory>()) {
      mojo::MakeSelfOwnedReceiver(std::make_unique<MbWebrtcVideoPerfHistory>(),
                                  std::move(r));
      return;
    }
    // document.browsingTopics() (Privacy Sandbox) — headless: no topics ([]).
    if (auto r =
            receiver.As<blink::mojom::blink::BrowsingTopicsDocumentService>()) {
      mojo::MakeSelfOwnedReceiver(
          std::make_unique<MbBrowsingTopicsDocumentService>(), std::move(r));
      return;
    }
    // Built-in on-device AI (LanguageModel/Summarizer/...) — headless: model unavailable.
    if (auto r = receiver.As<blink::mojom::blink::AIManager>()) {
      mojo::MakeSelfOwnedReceiver(std::make_unique<MbAIManager>(), std::move(r));
      return;
    }
    // navigator.usb.getDevices() — headless: no permitted USB devices ([]).
    if (auto r = receiver.As<blink::mojom::blink::WebUsbService>()) {
      mojo::MakeSelfOwnedReceiver(std::make_unique<MbWebUsbService>(),
                                  std::move(r));
      return;
    }
    // navigator.hid.getDevices() — headless: no HID devices ([]).
    if (auto r = receiver.As<blink::mojom::blink::HidService>()) {
      mojo::MakeSelfOwnedReceiver(std::make_unique<MbHidService>(),
                                  std::move(r));
      return;
    }
    // navigator.serial.getPorts() — headless: no serial ports ([]).
    if (auto r = receiver.As<blink::mojom::blink::SerialService>()) {
      mojo::MakeSelfOwnedReceiver(std::make_unique<MbSerialService>(),
                                  std::move(r));
      return;
    }
    // navigator.bluetooth — headless: no adapter (getAvailability -> false).
    if (auto r = receiver.As<blink::mojom::blink::WebBluetoothService>()) {
      mojo::MakeSelfOwnedReceiver(std::make_unique<MbWebBluetoothService>(),
                                  std::move(r));
      return;
    }
    // Translator API — headless: no translation service.
    if (auto r = receiver.As<blink::mojom::blink::TranslationManager>()) {
      mojo::MakeSelfOwnedReceiver(std::make_unique<MbTranslationManager>(),
                                  std::move(r));
      return;
    }
    // LanguageDetector API — headless: no detection model.
    if (auto r = receiver.As<
            language_detection::mojom::blink::ContentLanguageDetectionDriver>()) {
      mojo::MakeSelfOwnedReceiver(
          std::make_unique<MbContentLanguageDetectionDriver>(), std::move(r));
      return;
    }
    // navigator.mediaDevices — headless: no devices (enumerateDevices() -> []).
    if (auto r =
            receiver.As<blink::mojom::blink::MediaDevicesDispatcherHost>()) {
      mojo::MakeSelfOwnedReceiver(std::make_unique<MbMediaDevicesDispatcherHost>(),
                                  std::move(r));
      return;
    }
    // navigator.storage.getDirectory() (OPFS) — in-memory directory/file tree.
    if (auto r =
            receiver.As<blink::mojom::blink::FileSystemAccessManager>()) {
      BindFileSystemAccessManager(std::move(r));
      return;
    }
    // navigator.storageBuckets — named partitions re-exposing IDB/Cache/Locks/OPFS.
    if (auto r = receiver.As<blink::mojom::blink::BucketManagerHost>()) {
      BindBucketManagerHost(std::move(r));
      return;
    }
    // Drop everything else (no browser process).
  }

 private:
  scoped_refptr<base::SingleThreadTaskRunner> main_runner_;
};

}  // namespace

void MbSetGeolocation(double lat, double lng, double accuracy) {
  base::AutoLock al(GeoLock());
  GeoFix() = MbGeoFix{/*set=*/true, lat, lng, accuracy};
}

void MbClearGeolocation() {
  base::AutoLock al(GeoLock());
  GeoFix() = MbGeoFix{};
}

void MbSetClipboardText(const std::string& text) {
  ClipSet(text);
}

std::string MbGetClipboardText() {
  return ClipGet();
}

mojo::PendingRemote<blink::mojom::blink::BrowserInterfaceBroker>
MakeFrameInterfaceBroker() {
  mojo::PendingRemote<blink::mojom::blink::BrowserInterfaceBroker> remote;
  auto receiver = remote.InitWithNewPipeAndPassReceiver();
  // Bind the broker (and therefore the RestrictedCookieManager it hands out) on
  // the runtime service thread, NOT the main thread. document.cookie reads call
  // RestrictedCookieManager::GetCookiesString, which is a [Sync] mojo method: if
  // the receiver lived on the main thread, the main thread would block waiting
  // for a reply it must itself produce — a self-deadlock (the same hazard the
  // blob subsystem solved this way). Off-thread, the [Sync] call is serviced
  // while the main thread waits. The receiver must be bound *on* the service
  // sequence (PostTask + MakeSelfOwnedReceiver inside), not merely handed a
  // task runner from here, so its router is created on that sequence — see
  // BindBlobRegistryOnServiceThread.
  // Captured on the MAIN thread (here) so the service-thread broker can post main-thread-
  // only interfaces (SharedWorkerConnector) back.
  scoped_refptr<base::SingleThreadTaskRunner> main_runner =
      base::SingleThreadTaskRunner::GetCurrentDefault();
  scoped_refptr<base::SingleThreadTaskRunner> runner =
      MbRuntime::ServiceTaskRunner();
  if (runner) {
    runner->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](mojo::PendingReceiver<
                   blink::mojom::blink::BrowserInterfaceBroker> r,
               scoped_refptr<base::SingleThreadTaskRunner> main) {
              mojo::MakeSelfOwnedReceiver(
                  std::make_unique<MbBrowserInterfaceBroker>(std::move(main)),
                  std::move(r));
            },
            std::move(receiver), std::move(main_runner)));
  } else {
    mojo::MakeSelfOwnedReceiver(
        std::make_unique<MbBrowserInterfaceBroker>(std::move(main_runner)),
        std::move(receiver));
  }
  return remote;
}

}  // namespace mb
