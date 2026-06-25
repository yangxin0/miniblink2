// mb_storage_buckets.h — in-process Storage Buckets API (navigator.storageBuckets).
//
// A bucket is a named storage partition that re-exposes IndexedDB, CacheStorage, Web Locks,
// and OPFS. This binds blink.mojom.BucketManagerHost and hands out BucketHosts that delegate
// to the existing in-process implementations (so a bucket's storage works, though it is not
// yet isolated from the default — the backing stores are process-wide).

#ifndef MINIBLINK_HOST_FRAME_MB_STORAGE_BUCKETS_H_
#define MINIBLINK_HOST_FRAME_MB_STORAGE_BUCKETS_H_

#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "third_party/blink/public/mojom/buckets/bucket_manager_host.mojom-blink-forward.h"

namespace mb {

void BindBucketManagerHost(
    mojo::PendingReceiver<blink::mojom::blink::BucketManagerHost> receiver);

}  // namespace mb

#endif  // MINIBLINK_HOST_FRAME_MB_STORAGE_BUCKETS_H_
