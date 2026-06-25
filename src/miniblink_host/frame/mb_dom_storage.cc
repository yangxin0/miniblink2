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
#include "miniblink_host/runtime/mb_runtime.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "third_party/blink/public/mojom/dom_storage/dom_storage.mojom-blink.h"
#include "third_party/blink/public/mojom/dom_storage/session_storage_namespace.mojom-blink.h"
#include "third_party/blink/public/mojom/dom_storage/storage_area.mojom-blink.h"
#include "third_party/blink/renderer/platform/storage/blink_storage_key.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"

namespace mb {
namespace {

using Bytes = blink::Vector<uint8_t>;
using StorageAreaObserver = blink::mojom::blink::StorageAreaObserver;

// One process-wide store per origin key: ordered key/value entries + the set of
// observing contexts. Service-thread only, so no lock.
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
  return origin->ToString().Utf8();
}

// A StorageArea over one origin's shared AreaStore. Many instances (one per
// context) share the same AreaStore keyed by origin; each registers its own
// observer, so a write in any instance fans out to all the others.
class MbStorageArea : public blink::mojom::blink::StorageArea {
 public:
  explicit MbStorageArea(std::string key) : key_(std::move(key)) {}

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
    ForEachObserver(s, [&](StorageAreaObserver* o) {
      o->KeyChanged(key, value, old_value,
                    source ? source.Clone() : nullptr);
    });
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
    ForEachObserver(s, [&](StorageAreaObserver* o) {
      o->KeyDeleted(key, old_value, source ? source.Clone() : nullptr);
    });
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
    ForEachObserver(s, [&](StorageAreaObserver* o) {
      o->AllDeleted(was_nonempty, source ? source.Clone() : nullptr);
    });
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
  template <typename Fn>
  static void ForEachObserver(AreaStore& s, Fn&& fn) {
    for (auto it = s.observers.begin(); it != s.observers.end();) {
      if (!it->is_connected()) {
        it = s.observers.erase(it);
        continue;
      }
      fn(it->get());
      ++it;
    }
  }

  std::string key_;
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
      const blink::LocalFrameToken& /*local_frame_token*/,
      mojo::PendingReceiver<blink::mojom::blink::StorageArea> area) override {
    mojo::MakeSelfOwnedReceiver(
        std::make_unique<MbStorageArea>(KeyForStorageKey(storage_key)),
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
      const blink::BlinkStorageKey& /*storage_key*/,
      const blink::LocalFrameToken& /*local_frame_token*/,
      const blink::String& /*namespace_id*/,
      mojo::PendingReceiver<blink::mojom::blink::StorageArea> /*session_area*/)
      override {
    // Drop: session storage stays cache-only (its prior behavior — the provider
    // used to be unbound). Not sharing across contexts avoids any regression.
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

}  // namespace mb
