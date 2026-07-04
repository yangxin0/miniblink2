// mb_indexeddb — an in-process, in-memory IndexedDB backend (step 1: open + schema).
//
// blink requests a mojom::IDBFactory from the frame's BrowserInterfaceBroker. Without one,
// indexedDB.open() fails via onerror. This implements the OPEN handshake against an
// in-memory store keyed by database name: open at a new version fires onupgradeneeded
// (where the page creates object stores, recorded into the database metadata), then the
// version-change transaction's commit fires onsuccess. Reopening at the same version skips
// the upgrade and succeeds immediately.
//
// STEP 1 scope: databases open, object stores are created and reflected in
// db.objectStoreNames. Reads/writes (Put/Get/cursors) are stubbed and land in step 2.

#ifndef MINIBLINK_HOST_FRAME_MB_INDEXEDDB_H_
#define MINIBLINK_HOST_FRAME_MB_INDEXEDDB_H_

#include <cstdint>
#include <string>

#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "third_party/blink/public/mojom/indexeddb/indexeddb.mojom-blink.h"

namespace mb {

// Bind an IDBFactory receiver to the in-process backend (self-owned). Bound on the broker's
// service thread. `frame_key` scopes the databases to the frame's current document origin
// (per the IndexedDB spec, IDB is strictly per-origin); 0 = unknown origin (worker), which
// shares one unscoped bucket.
void BindIDBFactory(
    mojo::PendingReceiver<blink::mojom::blink::IDBFactory> receiver,
    uint64_t frame_key);

// Persist / restore the whole in-memory IndexedDB store (every database, by name) to/from
// `path` as a private binary file — the IndexedDB peer of the cookie/localStorage jars, for
// carrying app state (auth tokens, caches) across process runs. Save returns false if the
// file can't be written; Load returns false if it's missing/unreadable/corrupt. Call Load
// BEFORE the page opens its databases. Blob-valued records are not captured.
bool MbSaveIndexedDB(const std::string& path);
// Session-scoped variants (IMPROVEMENT2 item 6 stage 3): registry keys embed
// the session-prefixed scope, so Save filters by prefix, LoadMerge restores a
// per-session file without touching other sessions, ClearScoped erases by
// prefix. All hop to the service thread like their unscoped peers.
bool MbSaveIndexedDBScoped(const std::string& path,
                           const std::string& scope_prefix);
bool MbLoadIndexedDBMerge(const std::string& path);
void MbClearIndexedDBScoped(const std::string& scope_prefix);
bool MbLoadIndexedDB(const std::string& path);

}  // namespace mb

#endif  // MINIBLINK_HOST_FRAME_MB_INDEXEDDB_H_
