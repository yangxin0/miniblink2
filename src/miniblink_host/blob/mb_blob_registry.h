// In-process Blob system. We have no browser process, so blink::Platform's
// BlobRegistry request (Platform broker, blob_data.cc) would be dropped and any
// blob read (blob.text(), FileReader, arrayBuffer) would never resolve. This
// binds a real BlobRegistry/Blob on the IO/service thread (MbRuntime::Service-
// TaskRunner), so Blink's [Sync] BlobRegistry.Register is serviced off-thread
// (no main-thread deadlock) and Blob.ReadAll serves the stored bytes.
//
// Small blobs only for now: DataElementBytes carries embedded_data inline for
// <=256 KB; larger blobs (BytesProvider) and blob: URL resolution are TODO (see
// docs/design-blob-service-host.md).

#ifndef MINIBLINK_HOST_BLOB_MB_BLOB_REGISTRY_H_
#define MINIBLINK_HOST_BLOB_MB_BLOB_REGISTRY_H_

#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "third_party/blink/public/mojom/blob/blob_registry.mojom-blink.h"

namespace mb {

// Bind a BlobRegistry receiver on the service thread. Safe to call from the main
// thread (the broker does); posts the bind to MbRuntime::ServiceTaskRunner().
void BindBlobRegistryOnServiceThread(
    mojo::PendingReceiver<blink::mojom::blink::BlobRegistry> receiver);

}  // namespace mb

#endif  // MINIBLINK_HOST_BLOB_MB_BLOB_REGISTRY_H_
