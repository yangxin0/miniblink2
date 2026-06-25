#include "miniblink_host/frame/mb_storage_buckets.h"

#include <optional>
#include <set>
#include <string>
#include <utility>

#include "base/files/file.h"
#include "base/time/time.h"
#include "miniblink_host/frame/mb_cache_storage.h"
#include "miniblink_host/frame/mb_indexeddb.h"
#include "miniblink_host/frame/mb_lock_manager.h"
#include "miniblink_host/frame/mb_opfs.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "third_party/blink/public/mojom/buckets/bucket_manager_host.mojom-blink.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_error.mojom-blink.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"

namespace mb {
namespace {

namespace m = blink::mojom::blink;

// Names of buckets opened this run (process-wide; storageBuckets.keys() lists them).
std::set<std::string>& BucketNames() {
  static auto* names = new std::set<std::string>();
  return *names;
}

m::FileSystemAccessErrorPtr FsOk() {
  return m::FileSystemAccessError::New(m::FileSystemAccessStatus::kOk,
                                       base::File::FILE_OK,
                                       blink::String::FromUtf8(""));
}

// One storage bucket. Storage APIs delegate to the existing in-process backends; metadata
// (persist/estimate/durability/expiry) is reported for a headless host. Not yet isolated from
// the default partition — the backing IDB/Cache/OPFS stores are process-wide.
class MbBucketHost : public m::BucketHost {
 public:
  void Persist(PersistCallback cb) override {
    std::move(cb).Run(/*persisted=*/true, /*success=*/true);
  }
  void Persisted(PersistedCallback cb) override {
    std::move(cb).Run(/*persisted=*/true, /*success=*/true);
  }
  void Estimate(EstimateCallback cb) override {
    std::move(cb).Run(/*usage=*/0, /*quota=*/int64_t{2} * 1024 * 1024 * 1024,
                      /*success=*/true);
  }
  void Durability(DurabilityCallback cb) override {
    std::move(cb).Run(m::BucketDurability::kRelaxed, /*success=*/true);
  }
  void SetExpires(base::Time expires, SetExpiresCallback cb) override {
    expires_ = expires;
    std::move(cb).Run(/*success=*/true);
  }
  void Expires(ExpiresCallback cb) override {
    std::move(cb).Run(expires_, /*success=*/true);
  }
  void GetIdbFactory(mojo::PendingReceiver<m::IDBFactory> r) override {
    BindIDBFactory(std::move(r));
  }
  void GetLockManager(mojo::PendingReceiver<m::LockManager> r) override {
    BindLockManager(std::move(r));
  }
  void GetCaches(mojo::PendingReceiver<m::CacheStorage> r) override {
    BindCacheStorage(std::move(r));
  }
  void GetDirectory(GetDirectoryCallback cb) override {
    std::move(cb).Run(FsOk(), MbBindOpfsRootDirectory());
  }
  void GetDirectoryForDevtools(
      const blink::Vector<blink::String>&,
      GetDirectoryForDevtoolsCallback cb) override {
    std::move(cb).Run(FsOk(), MbBindOpfsRootDirectory());
  }

 private:
  std::optional<base::Time> expires_;
};

class MbBucketManagerHost : public m::BucketManagerHost {
 public:
  void OpenBucket(const blink::String& name, m::BucketPoliciesPtr,
                  OpenBucketCallback cb) override {
    BucketNames().insert(name.Utf8());
    mojo::PendingRemote<m::BucketHost> remote;
    mojo::MakeSelfOwnedReceiver(std::make_unique<MbBucketHost>(),
                                remote.InitWithNewPipeAndPassReceiver());
    std::move(cb).Run(std::move(remote), m::BucketError::kUnknown);
  }
  void GetBucketForDevtools(
      const blink::String& name,
      mojo::PendingReceiver<m::BucketHost> receiver) override {
    BucketNames().insert(name.Utf8());
    mojo::MakeSelfOwnedReceiver(std::make_unique<MbBucketHost>(),
                                std::move(receiver));
  }
  void Keys(KeysCallback cb) override {
    blink::Vector<blink::String> out;
    for (const std::string& n : BucketNames())
      out.push_back(blink::String::FromUtf8(n));
    std::move(cb).Run(out, /*success=*/true);
  }
  void DeleteBucket(const blink::String& name,
                    DeleteBucketCallback cb) override {
    BucketNames().erase(name.Utf8());
    std::move(cb).Run(/*success=*/true);
  }
};

}  // namespace

void BindBucketManagerHost(
    mojo::PendingReceiver<blink::mojom::blink::BucketManagerHost> receiver) {
  mojo::MakeSelfOwnedReceiver(std::make_unique<MbBucketManagerHost>(),
                              std::move(receiver));
}

}  // namespace mb
