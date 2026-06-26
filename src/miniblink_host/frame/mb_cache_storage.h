// mb_cache_storage — an in-process Cache Storage (caches API) backend.
//
// blink requests a mojom::CacheStorage from the frame's BrowserInterfaceBroker. This stores
// Request/Response pairs in a process-wide, per-cache-name map keyed by request URL. The
// response body (a SerializedBlob, i.e. a Blob remote) is kept bound and Blob.Clone()'d on
// each match, so a cached response can be read any number of times.
//
// Scope: caches.open/has/delete/keys, caches.match, cache.put/delete (via Batch), cache.match.
// Matching is by URL only (ignores method / ignoreSearch / vary). In-memory, by db... cache name.

#ifndef MINIBLINK_HOST_FRAME_MB_CACHE_STORAGE_H_
#define MINIBLINK_HOST_FRAME_MB_CACHE_STORAGE_H_

#include <cstdint>

#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "third_party/blink/public/mojom/cache_storage/cache_storage.mojom-blink.h"

namespace mb {

// Bind a CacheStorage receiver to the in-process backend (self-owned). Bound on the broker's
// service thread. `frame_key` -> the frame's origin, which scopes the cache names so
// caches.open() is ISOLATED per origin (a bare-name registry leaks cache data across origins).
void BindCacheStorage(
    mojo::PendingReceiver<blink::mojom::blink::CacheStorage> receiver,
    uint64_t frame_key);

}  // namespace mb

#endif  // MINIBLINK_HOST_FRAME_MB_CACHE_STORAGE_H_
