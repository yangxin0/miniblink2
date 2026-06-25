// mb_dom_storage — an in-process DOM Storage (localStorage) backend.
//
// blink's StorageController asks Platform's BrowserInterfaceBroker for a
// DomStorageProvider; with none bound, every CachedStorageArea ran cache-only,
// so same-origin contexts did NOT share localStorage and the `storage` event
// never fired. This binds a real backend: a process-wide per-origin key/value
// store whose StorageArea broadcasts KeyChanged/KeyDeleted/AllDeleted to every
// observing context — which is what makes cross-context sharing and the window
// `storage` event work. Session storage is intentionally left cache-only
// (receivers dropped, exactly as before) to avoid changing its semantics.
//
// StorageArea.GetAll is [Sync] (called from the blocked main thread), so the
// backend is bound on the runtime SERVICE thread, like BlobRegistry/BlobURLStore.

#ifndef MINIBLINK_HOST_FRAME_MB_DOM_STORAGE_H_
#define MINIBLINK_HOST_FRAME_MB_DOM_STORAGE_H_

#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "third_party/blink/public/mojom/dom_storage/dom_storage.mojom-blink-forward.h"

namespace mb {

// Bind the DomStorageProvider on the service thread (drops if pre-init). Called
// from the platform broker when blink's StorageController requests it.
void BindDomStorageProviderOnServiceThread(
    mojo::PendingReceiver<blink::mojom::blink::DomStorageProvider> receiver);

}  // namespace mb

#endif  // MINIBLINK_HOST_FRAME_MB_DOM_STORAGE_H_
