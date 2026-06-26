// In-process Blob system. We have no browser process, so blink::Platform's
// BlobRegistry request (Platform broker, blob_data.cc) would be dropped and any
// blob read (blob.text(), FileReader, arrayBuffer) would never resolve. This
// binds a real BlobRegistry/Blob on the IO/service thread (MbRuntime::Service-
// TaskRunner), so Blink's [Sync] BlobRegistry.Register is serviced off-thread
// (no main-thread deadlock) and Blob.ReadAll serves the stored bytes.
//
// Both blob sizes and blob: URL resolution are supported: a DataElement arrives
// inline (embedded_data, <=256 KB) or via a BytesProvider (>256 KB), and the
// in-process MbBlobURLStore (below) resolves blob: URLs so createObjectURL + a
// blob: fetch round-trips. Verified by mb_smoke cases 46/46b.

#ifndef MINIBLINK_HOST_BLOB_MB_BLOB_REGISTRY_H_
#define MINIBLINK_HOST_BLOB_MB_BLOB_REGISTRY_H_

#include <cstdint>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "third_party/blink/public/mojom/blob/blob.mojom-blink.h"
#include "third_party/blink/public/mojom/blob/blob_registry.mojom-blink.h"

namespace blink {
class AssociatedInterfaceProvider;
class BlobDataHandle;
class String;
}

namespace mb {

// Bind a BlobRegistry receiver on the service thread. Safe to call from the main
// thread (the broker does); posts the bind to MbRuntime::ServiceTaskRunner().
void BindBlobRegistryOnServiceThread(
    mojo::PendingReceiver<blink::mojom::blink::BlobRegistry> receiver);

// A navigation-associated-interface provider (owned by the caller) that binds
// blink.mojom.BlobURLStore to our in-process store on the service thread, so
// URL.createObjectURL registration and blob: URL fetch resolution work. Returned
// from MbFrameClient::GetRemoteNavigationAssociatedInterfaces(). Created + used
// on the main thread.
blink::AssociatedInterfaceProvider* MakeBlobUrlNavAssociatedInterfaces(
    uint64_t frame_key);

// Mint a BlobDataHandle serving `bytes` inline (a self-owned in-process Blob on the
// current/service thread). Used by OPFS file reads (FileSystemAccessFileHandle.AsBlob).
scoped_refptr<blink::BlobDataHandle> MbCreateInlineBlob(
    const std::string& bytes,
    const blink::String& content_type);

// Resolve a blob: URL to its full bytes, asynchronously, on the service thread.
// Looks up the in-process createObjectURL registry; runs `done` with the bytes,
// or with an empty vector if the URL is unknown/revoked. MUST be called on the
// service thread (where the BlobURLStore and Blob remotes live). Used by
// download capture: a page-initiated <a download href="blob:..."> reports the
// blob: URL through LocalFrameHost.DownloadURL, and we read out the bytes here.
void MbResolveBlobUrlBytes(
    const std::string& url,
    base::OnceCallback<void(std::vector<uint8_t>)> done);

// Read a Blob remote to its full bytes, asynchronously, on the service thread.
// Runs `done` with the bytes (empty if the remote is null). Used by download
// capture for data: URLs: blink passes the decoded bytes as a Blob (params
// .data_url_blob) with an empty url, so there's nothing to look up — just drain.
void MbReadBlobRemoteBytes(
    mojo::PendingRemote<blink::mojom::blink::Blob> blob,
    base::OnceCallback<void(std::vector<uint8_t>)> done);

}  // namespace mb

#endif  // MINIBLINK_HOST_BLOB_MB_BLOB_REGISTRY_H_
