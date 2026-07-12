// mb_dom_storage.cc — see header. In-process localStorage backend with
// cross-context change broadcast (the window `storage` event).
#include "miniblink_host/frame/mb_dom_storage.h"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/no_destructor.h"
#include "base/task/single_thread_task_runner.h"
#include "miniblink_host/frame/mb_frame_origin.h"
#include "miniblink_host/runtime/mb_runtime.h"
#include "miniblink_host/session/mb_session.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "third_party/blink/public/mojom/dom_storage/dom_storage.mojom-blink.h"
#include "third_party/blink/public/mojom/dom_storage/session_storage_namespace.mojom-blink.h"
#include "third_party/blink/public/mojom/dom_storage/storage_area.mojom-blink.h"
#include "third_party/blink/renderer/platform/network/blink_schemeful_site.h"
#include "third_party/blink/renderer/platform/storage/blink_storage_key.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"

namespace mb {
namespace {

using Bytes = blink::Vector<uint8_t>;
using StorageAreaObserver = blink::mojom::blink::StorageAreaObserver;

// One process-wide store per (session, area type, namespace, origin) key:
// ordered key/value entries + the set of observing contexts. Service-thread
// only, so no lock.
struct AreaStore {
  std::vector<std::pair<Bytes, Bytes>> entries;
  std::vector<mojo::Remote<StorageAreaObserver>> observers;
};

std::map<std::string, AreaStore>& Stores() {
  static base::NoDestructor<std::map<std::string, AreaStore>> s;
  return *s;
}

int FindEntry(const AreaStore& s, const Bytes& key) {
  for (size_t i = 0; i < s.entries.size(); ++i) {
    if (s.entries[i].first == key)
      return static_cast<int>(i);
  }
  return -1;
}

template <typename Fn>
void ForEachObserver(AreaStore& s, Fn&& fn) {
  for (auto it = s.observers.begin(); it != s.observers.end();) {
    if (!it->is_connected()) {
      it = s.observers.erase(it);
      continue;
    }
    fn(it->get());
    ++it;
  }
}

// The origin key for a storage area. Same non-opaque origin -> same key (shared
// store). Opaque origins are isolated (a unique key each), since they must not
// share storage and can't be serialized.
std::string KeyForStorageKey(const blink::BlinkStorageKey& storage_key) {
  const scoped_refptr<const blink::SecurityOrigin>& origin =
      storage_key.GetSecurityOrigin();
  if (!origin || origin->IsOpaque()) {
    static int counter = 0;
    return "opaque:" + std::to_string(counter++);
  }
  std::string key = origin->ToString().Utf8();
  // Third-party storage partitioning (blink kThirdPartyStoragePartitioning, on by
  // default): a cross-site embedded context (e.g. widget.example inside a.com vs
  // b.com) gets an ISOLATED store per top-level site, matching the broker-scoped
  // backends (IDB/Cache/locks). First-party and same-site frames keep the
  // bare-origin key (a shared store) — ancestor chain bit is kSameSite there.
  if (storage_key.GetAncestorChainBit() ==
      blink::mojom::blink::AncestorChainBit::kCrossSite) {
    const blink::BlinkSchemefulSite& top = storage_key.GetTopLevelSite();
    if (!top.IsOpaque())
      key += "\x1f""3p""\x1f" + top.Serialize().Utf8();
  }
  return key;
}

std::string SessionKeyForFrameToken(
    const blink::LocalFrameToken& local_frame_token) {
  const std::string scope = MbGetFrameScopeForToken(local_frame_token);
  const size_t separator = scope.find('\x1f');
  if (separator != std::string::npos)
    return scope.substr(0, separator);
  // DOM Storage can bind during initial-document setup before a committed scope
  // is published. Such a frame still belongs to the implicit default profile.
  return MbSession::Default()->id();
}

std::string ScopedAreaKey(const std::string& session_key,
                          const char* area_kind,
                          const std::string& area_key) {
  return session_key + "\x1f" + area_kind + area_key;
}

// A StorageArea over one origin's shared AreaStore. Many instances (one per
// context) share the same AreaStore keyed by origin; each registers its own
// observer, so a write in any instance fans out to all the others.
class MbStorageArea : public blink::mojom::blink::StorageArea {
 public:
  // `broadcast` fans changes out to other contexts via KeyChanged/KeyDeleted/
  // AllDeleted (localStorage). Session storage must NOT broadcast: blink dispatches
  // its 'storage' events internally over the shared per-namespace cache and DCHECKs
  // (!IsSessionStorage()) if it ever receives a mojo KeyChanged — so session areas
  // store + serve GetAll only.
  MbStorageArea(std::string key, bool broadcast)
      : key_(std::move(key)), broadcast_(broadcast) {}

  void AddObserver(
      mojo::PendingRemote<StorageAreaObserver> observer) override {
    Stores()[key_].observers.emplace_back(std::move(observer));
  }

  void Put(const Bytes& key,
           const Bytes& value,
           const std::optional<Bytes>& /*client_old_value*/,
           blink::mojom::blink::StorageAreaSourcePtr source,
           PutCallback callback) override {
    AreaStore& s = Stores()[key_];
    std::optional<Bytes> old_value;
    if (int i = FindEntry(s, key); i >= 0) {
      old_value = s.entries[i].second;
      s.entries[i].second = value;
    } else {
      s.entries.emplace_back(key, value);
    }
    std::move(callback).Run(true);
    // Tell every observing context (incl. the originator, which blink dedups via
    // its pending-mutation bookkeeping and skips firing a `storage` event on
    // itself); other contexts apply the change and fire the event.
    if (broadcast_) {
      ForEachObserver(s, [&](StorageAreaObserver* o) {
        o->KeyChanged(key, value, old_value, source ? source.Clone() : nullptr);
      });
    }
  }

  void Delete(const Bytes& key,
              const std::optional<Bytes>& /*client_old_value*/,
              blink::mojom::blink::StorageAreaSourcePtr source,
              DeleteCallback callback) override {
    AreaStore& s = Stores()[key_];
    std::optional<Bytes> old_value;
    if (int i = FindEntry(s, key); i >= 0) {
      old_value = s.entries[i].second;
      s.entries.erase(s.entries.begin() + i);
    }
    std::move(callback).Run();
    if (broadcast_) {
      ForEachObserver(s, [&](StorageAreaObserver* o) {
        o->KeyDeleted(key, old_value, source ? source.Clone() : nullptr);
      });
    }
  }

  void DeleteAll(blink::mojom::blink::StorageAreaSourcePtr source,
                 mojo::PendingRemote<StorageAreaObserver> new_observer,
                 DeleteAllCallback callback) override {
    AreaStore& s = Stores()[key_];
    const bool was_nonempty = !s.entries.empty();
    s.entries.clear();
    if (new_observer)
      s.observers.emplace_back(std::move(new_observer));
    std::move(callback).Run();
    if (broadcast_) {
      ForEachObserver(s, [&](StorageAreaObserver* o) {
        o->AllDeleted(was_nonempty, source ? source.Clone() : nullptr);
      });
    }
  }

  void GetAll(mojo::PendingRemote<StorageAreaObserver> new_observer,
              GetAllCallback callback) override {
    AreaStore& s = Stores()[key_];
    if (new_observer)
      s.observers.emplace_back(std::move(new_observer));
    blink::Vector<blink::mojom::blink::KeyValuePtr> data;
    data.ReserveInitialCapacity(
        static_cast<blink::wtf_size_t>(s.entries.size()));
    for (const auto& [k, v] : s.entries)
      data.push_back(blink::mojom::blink::KeyValue::New(k, v));
    std::move(callback).Run(std::move(data));
  }

 private:
  std::string key_;
  bool broadcast_;
};

class MbSessionStorageNamespace
    : public blink::mojom::blink::SessionStorageNamespace {
 public:
  void Clone(const blink::String& /*clone_to_namespace*/) override {}
};

class MbDomStorage : public blink::mojom::blink::DomStorage {
 public:
  void OpenLocalStorage(
      const blink::BlinkStorageKey& storage_key,
      const blink::LocalFrameToken& local_frame_token,
      mojo::PendingReceiver<blink::mojom::blink::StorageArea> area) override {
    const std::string key = ScopedAreaKey(
        SessionKeyForFrameToken(local_frame_token), "ls:",
        KeyForStorageKey(storage_key));
    mojo::MakeSelfOwnedReceiver(
        std::make_unique<MbStorageArea>(key, /*broadcast=*/true),
        std::move(area));
  }

  void BindSessionStorageNamespace(
      const blink::String& /*namespace_id*/,
      mojo::PendingReceiver<blink::mojom::blink::SessionStorageNamespace>
          receiver) override {
    mojo::MakeSelfOwnedReceiver(
        std::make_unique<MbSessionStorageNamespace>(), std::move(receiver));
  }

  void BindSessionStorageArea(
      const blink::BlinkStorageKey& storage_key,
      const blink::LocalFrameToken& local_frame_token,
      const blink::String& namespace_id,
      mojo::PendingReceiver<blink::mojom::blink::StorageArea> session_area)
      override {
    // Session storage is partitioned per top-level browsing context: key the store
    // by (namespace_id, origin). Each view has a UNIQUE namespace id (minted at
    // view creation), so same-view same-origin frames SHARE sessionStorage (incl.
    // the 'storage' event) while different views stay isolated.
    const std::string key = ScopedAreaKey(
        SessionKeyForFrameToken(local_frame_token), "ss:",
        namespace_id.Utf8() + ":" + KeyForStorageKey(storage_key));
    mojo::MakeSelfOwnedReceiver(
        std::make_unique<MbStorageArea>(key, /*broadcast=*/false),
        std::move(session_area));
  }
};

class MbDomStorageProvider : public blink::mojom::blink::DomStorageProvider {
 public:
  void BindDomStorage(
      mojo::PendingReceiver<blink::mojom::blink::DomStorage> receiver,
      mojo::PendingRemote<blink::mojom::blink::DomStorageClient> /*client*/)
      override {
    // The DomStorageClient remote (Reset*Connections) is unused — we never drop
    // connections out from under blink.
    mojo::MakeSelfOwnedReceiver(std::make_unique<MbDomStorage>(),
                                std::move(receiver));
  }
};

}  // namespace

void BindDomStorageProviderOnServiceThread(
    mojo::PendingReceiver<blink::mojom::blink::DomStorageProvider> receiver) {
  scoped_refptr<base::SingleThreadTaskRunner> runner =
      MbRuntime::ServiceTaskRunner();
  if (!runner)
    return;  // pre-init: drop (graceful)
  runner->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](mojo::PendingReceiver<blink::mojom::blink::DomStorageProvider> r) {
            mojo::MakeSelfOwnedReceiver(
                std::make_unique<MbDomStorageProvider>(), std::move(r));
          },
          std::move(receiver)));
}

void MbClearDomStorageForSession(const std::string& session_key) {
  if (session_key.empty())
    return;
  scoped_refptr<base::SingleThreadTaskRunner> runner =
      MbRuntime::ServiceTaskRunner();
  if (!runner)
    return;
  auto clear = base::BindOnce(
      [](std::string prefix) {
        for (auto& [key, area] : Stores()) {
          if (key.rfind(prefix, 0) != 0)
            continue;
          const bool was_nonempty = !area.entries.empty();
          area.entries.clear();
          // Blink requires sessionStorage invalidation to stay renderer-local;
          // a backend KeyChanged/AllDeleted for it trips !IsSessionStorage().
          if (key.compare(prefix.size(), 3, "ls:") == 0) {
            ForEachObserver(area, [&](StorageAreaObserver* observer) {
              observer->AllDeleted(was_nonempty, nullptr);
            });
          }
        }
      },
      session_key + "\x1f");
  if (runner->RunsTasksInCurrentSequence())
    std::move(clear).Run();
  else
    runner->PostTask(FROM_HERE, std::move(clear));
}

}  // namespace mb
