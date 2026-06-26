#include "miniblink_host/frame/mb_storage_buckets.h"

#include <optional>
#include <set>
#include <string>
#include <utility>

#include "base/files/file.h"
#include "base/time/time.h"
#include "miniblink_host/frame/mb_cache_storage.h"
#include "miniblink_host/frame/mb_frame_origin.h"
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
// (persist/estimate/durability/expiry) is reported for a headless host. Its IndexedDB is
// origin+bucket scoped (isolated cross-origin + from the default partition) — IndexedDB, Cache
// Storage, and OPFS all via the shared (origin, bucket) scope key (ScopeKey / the OPFS scope string).
class MbBucketHost : public m::BucketHost {
 public:
  // `frame_key` -> the opening frame's origin; `bucket_name` partitions storage
  // WITHIN that origin. The bucket's IndexedDB is scoped to (origin, bucket) so it
  // is isolated cross-origin AND kept separate from the default partition + other
  // buckets — via a synthetic frame_key whose "origin" is origin + SEP + bucket.
  MbBucketHost(uint64_t frame_key, std::string bucket_name)
      : frame_key_(frame_key), bucket_name_(std::move(bucket_name)) {}
  ~MbBucketHost() override {
    if (scope_frame_key_)
      MbClearFrameOrigin(scope_frame_key_);
  }

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
    BindIDBFactory(std::move(r), ScopeKey());  // (origin, bucket)-scoped IDB
  }
  void GetLockManager(mojo::PendingReceiver<m::LockManager> r) override {
    BindLockManager(std::move(r));
  }
  void GetCaches(mojo::PendingReceiver<m::CacheStorage> r) override {
    BindCacheStorage(std::move(r), ScopeKey());  // (origin, bucket)-scoped caches
  }
  void GetDirectory(GetDirectoryCallback cb) override {
    // Scope the bucket's OPFS to (origin, bucket) — same scope string as its IDB.
    std::move(cb).Run(FsOk(),
                      MbBindOpfsRootDirectory(MbGetFrameOrigin(frame_key_) +
                                              "\x01" + "bucket:" + bucket_name_));
  }
  void GetDirectoryForDevtools(
      const blink::Vector<blink::String>&,
      GetDirectoryForDevtoolsCallback cb) override {
    std::move(cb).Run(FsOk(),
                      MbBindOpfsRootDirectory(MbGetFrameOrigin(frame_key_) +
                                              "\x01" + "bucket:" + bucket_name_));
  }

 private:
  // Lazily allocate a synthetic frame_key whose mapped "origin" is origin + SEP +
  // bucket, scoping this bucket's IDB + Cache Storage to (origin, bucket) — distinct
  // from the default partition and other buckets, isolated cross-origin. Freed in
  // the dtor. (OPFS uses the same scope STRING directly via MbBindOpfsRootDirectory.)
  uint64_t ScopeKey() {
    if (!scope_frame_key_) {
      scope_frame_key_ = MbAllocWorkerFrameKey();
      MbSetFrameOrigin(scope_frame_key_, MbGetFrameOrigin(frame_key_) + "\x01" +
                                             "bucket:" + bucket_name_);
    }
    return scope_frame_key_;
  }

  std::optional<base::Time> expires_;
  uint64_t frame_key_ = 0;       // the opening frame's key (-> its origin)
  std::string bucket_name_;      // partitions storage within the origin
  uint64_t scope_frame_key_ = 0; // synthetic (origin,bucket) key for IDB+Cache (0=unset)
};

class MbBucketManagerHost : public m::BucketManagerHost {
 public:
  explicit MbBucketManagerHost(uint64_t frame_key) : frame_key_(frame_key) {}

  void OpenBucket(const blink::String& name, m::BucketPoliciesPtr,
                  OpenBucketCallback cb) override {
    BucketNames().insert(name.Utf8());
    mojo::PendingRemote<m::BucketHost> remote;
    mojo::MakeSelfOwnedReceiver(
        std::make_unique<MbBucketHost>(frame_key_, name.Utf8()),
        remote.InitWithNewPipeAndPassReceiver());
    std::move(cb).Run(std::move(remote), m::BucketError::kUnknown);
  }
  void GetBucketForDevtools(
      const blink::String& name,
      mojo::PendingReceiver<m::BucketHost> receiver) override {
    BucketNames().insert(name.Utf8());
    mojo::MakeSelfOwnedReceiver(
        std::make_unique<MbBucketHost>(frame_key_, name.Utf8()),
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

 private:
  uint64_t frame_key_ = 0;  // the owning frame's key (-> origin), passed to buckets
};

}  // namespace

void BindBucketManagerHost(
    mojo::PendingReceiver<blink::mojom::blink::BucketManagerHost> receiver,
    uint64_t frame_key) {
  mojo::MakeSelfOwnedReceiver(std::make_unique<MbBucketManagerHost>(frame_key),
                              std::move(receiver));
}

}  // namespace mb
