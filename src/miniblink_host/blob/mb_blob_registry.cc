#include "miniblink_host/blob/mb_blob_registry.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "miniblink_host/runtime/mb_runtime.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "third_party/blink/public/mojom/blob/blob.mojom-blink.h"
#include "third_party/blink/public/mojom/blob/data_element.mojom-blink.h"

namespace mb {
namespace {

// In-process Blob: holds the bytes, serves reads. Lives on the service thread.
class MbBlob : public blink::mojom::blink::Blob {
 public:
  MbBlob(blink::String uuid, std::vector<uint8_t> data)
      : uuid_(std::move(uuid)), data_(std::move(data)) {}

  void Clone(
      mojo::PendingReceiver<blink::mojom::blink::Blob> receiver) override {
    mojo::MakeSelfOwnedReceiver(std::make_unique<MbBlob>(uuid_, data_),
                                std::move(receiver));
  }
  void AsDataPipeGetter(
      mojo::PendingReceiver<network::mojom::blink::DataPipeGetter>) override {}
  void ReadAll(
      mojo::ScopedDataPipeProducerHandle pipe,
      mojo::PendingRemote<blink::mojom::blink::BlobReaderClient> client)
      override {
    mojo::Remote<blink::mojom::blink::BlobReaderClient> remote;
    if (client)
      remote.Bind(std::move(client));
    const uint64_t size = data_.size();
    if (remote)
      remote->OnCalculatedSize(size, size);
    // Small blobs fit one atomic write; larger ones (chunking) are a TODO.
    if (pipe.is_valid() && !data_.empty())
      pipe->WriteAllData(base::span<const uint8_t>(data_));
    pipe.reset();  // signal EOF to the reader
    if (remote)
      remote->OnComplete(/*status=net::OK=*/0, size);
  }
  void ReadRange(
      uint64_t /*offset*/,
      uint64_t /*length*/,
      mojo::ScopedDataPipeProducerHandle,
      mojo::PendingRemote<blink::mojom::blink::BlobReaderClient>) override {}
  void Load(mojo::PendingReceiver<network::mojom::blink::URLLoader>,
            const blink::String&,
            const net::HttpRequestHeaders&,
            mojo::PendingRemote<network::mojom::blink::URLLoaderClient>)
      override {}
  void ReadSideData(ReadSideDataCallback callback) override {
    std::move(callback).Run(std::nullopt);
  }
  void CaptureSnapshot(CaptureSnapshotCallback callback) override {
    std::move(callback).Run(data_.size(), std::nullopt);
  }
  void GetInternalUUID(GetInternalUUIDCallback callback) override {
    std::move(callback).Run(uuid_);
  }

 private:
  blink::String uuid_;
  std::vector<uint8_t> data_;
};

// In-process BlobRegistry. Lives on the service thread; stores the inline bytes
// from Register and binds an MbBlob.
class MbBlobRegistry : public blink::mojom::blink::BlobRegistry {
 public:
  void Register(mojo::PendingReceiver<blink::mojom::blink::Blob> blob,
                const blink::String& uuid,
                const blink::String& /*content_type*/,
                const blink::String& /*content_disposition*/,
                blink::Vector<blink::mojom::blink::DataElementPtr> elements,
                RegisterCallback callback) override {
    std::vector<uint8_t> bytes;
    for (const auto& el : elements) {
      if (el && el->is_bytes()) {
        const auto& b = el->get_bytes();
        if (b && b->embedded_data) {
          const auto& ed = *b->embedded_data;
          bytes.insert(bytes.end(), ed.begin(), ed.end());
        }
      }
    }
    mojo::MakeSelfOwnedReceiver(
        std::make_unique<MbBlob>(uuid, std::move(bytes)), std::move(blob));
    std::move(callback).Run();  // reply () — unblocks Blink's [Sync] Register
  }

  void RegisterFromStream(
      const blink::String&,
      const blink::String&,
      uint64_t,
      mojo::ScopedDataPipeConsumerHandle,
      mojo::PendingAssociatedRemote<blink::mojom::blink::ProgressClient>,
      RegisterFromStreamCallback callback) override {
    std::move(callback).Run(nullptr);  // not supported yet
  }
};

}  // namespace

void BindBlobRegistryOnServiceThread(
    mojo::PendingReceiver<blink::mojom::blink::BlobRegistry> receiver) {
  scoped_refptr<base::SingleThreadTaskRunner> runner =
      MbRuntime::ServiceTaskRunner();
  if (!runner)
    return;  // pre-init: drop (graceful — same as before)
  runner->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](mojo::PendingReceiver<blink::mojom::blink::BlobRegistry> r) {
            mojo::MakeSelfOwnedReceiver(std::make_unique<MbBlobRegistry>(),
                                        std::move(r));
          },
          std::move(receiver)));
}

}  // namespace mb
