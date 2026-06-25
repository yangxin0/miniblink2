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

#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "third_party/blink/public/mojom/indexeddb/indexeddb.mojom-blink.h"

namespace mb {

// Bind an IDBFactory receiver to the in-process backend (self-owned). Bound on the broker's
// service thread.
void BindIDBFactory(
    mojo::PendingReceiver<blink::mojom::blink::IDBFactory> receiver);

}  // namespace mb

#endif  // MINIBLINK_HOST_FRAME_MB_INDEXEDDB_H_
