#include "miniblink_host/blob/mb_blob_registry.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_runner.h"
#include "miniblink_host/runtime/mb_runtime.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/simple_watcher.h"
#include "third_party/blink/public/mojom/blob/blob.mojom-blink.h"
#include "third_party/blink/public/mojom/blob/data_element.mojom-blink.h"

namespace mb {
namespace {

// One in-flight Blob.ReadAll: streams `data` into the producer pipe, chunked
// (so blobs larger than the pipe capacity work), then fires the client's
// OnComplete. Self-owned: deletes itself when done. Lives on the service thread.
class BlobReadSession {
 public:
  static void Start(
      std::vector<uint8_t> data,
      mojo::ScopedDataPipeProducerHandle pipe,
      mojo::PendingRemote<blink::mojom::blink::BlobReaderClient> client) {
    (new BlobReadSession(std::move(data), std::move(pipe), std::move(client)))
        ->Begin();
  }

 private:
  BlobReadSession(
      std::vector<uint8_t> data,
      mojo::ScopedDataPipeProducerHandle pipe,
      mojo::PendingRemote<blink::mojom::blink::BlobReaderClient> client)
      : data_(std::move(data)),
        pipe_(std::move(pipe)),
        watcher_(FROM_HERE,
                 mojo::SimpleWatcher::ArmingPolicy::MANUAL,
                 base::SequencedTaskRunner::GetCurrentDefault()) {
    if (client)
      remote_.Bind(std::move(client));
  }

  void Begin() {
    const uint64_t size = data_.size();
    if (remote_)
      remote_->OnCalculatedSize(size, size);
    if (!pipe_.is_valid()) {
      Finish(/*ok=*/false);
      return;
    }
    watcher_.Watch(pipe_.get(), MOJO_HANDLE_SIGNAL_WRITABLE,
                   MOJO_TRIGGER_CONDITION_SIGNALS_SATISFIED,
                   base::BindRepeating(&BlobReadSession::OnWritable,
                                       base::Unretained(this)));
    WriteSome();
  }

  void WriteSome() {
    while (offset_ < data_.size()) {
      size_t written = 0;
      MojoResult r = pipe_->WriteData(
          base::span<const uint8_t>(data_).subspan(offset_),
          MOJO_WRITE_DATA_FLAG_NONE, written);
      if (r == MOJO_RESULT_OK) {
        offset_ += written;
        continue;
      }
      if (r == MOJO_RESULT_SHOULD_WAIT) {
        watcher_.ArmOrNotify();  // wait for the consumer to drain
        return;
      }
      Finish(/*ok=*/false);  // consumer closed / error
      return;
    }
    Finish(/*ok=*/true);
  }

  void OnWritable(MojoResult result, const mojo::HandleSignalsState&) {
    if (result == MOJO_RESULT_OK)
      WriteSome();
    else
      Finish(/*ok=*/false);
  }

  void Finish(bool ok) {
    watcher_.Cancel();
    pipe_.reset();  // EOF for the reader
    if (remote_) {
      remote_->OnComplete(ok ? 0 : /*net::ERR_FAILED=*/-2,
                          ok ? data_.size() : offset_);
    }
    delete this;
  }

  std::vector<uint8_t> data_;
  size_t offset_ = 0;
  mojo::ScopedDataPipeProducerHandle pipe_;
  mojo::Remote<blink::mojom::blink::BlobReaderClient> remote_;
  mojo::SimpleWatcher watcher_;
};

// In-process Blob. Holds the bytes; serves reads. Lives on the service thread.
// Bytes may arrive inline (embedded_data) or via a BytesProvider (>256 KB),
// which is fetched asynchronously AFTER Register replies (the main thread must
// be unblocked first, since the provider lives there). Reads that arrive before
// the bytes are materialized are queued and drained when ready.
class MbBlob : public blink::mojom::blink::Blob {
 public:
  struct Part {
    std::vector<uint8_t> inline_bytes;  // used when no provider
    mojo::Remote<blink::mojom::blink::BytesProvider> provider;  // else fetch
  };

  MbBlob(blink::String uuid, std::vector<Part> parts)
      : uuid_(std::move(uuid)), parts_(std::move(parts)) {
    Materialize(0);  // assemble data_ in element order (async if providers)
  }

  void Clone(
      mojo::PendingReceiver<blink::mojom::blink::Blob> receiver) override {
    // Clone shares the already-fetched bytes (simplest: only after ready).
    std::vector<Part> parts;
    Part p;
    p.inline_bytes = data_;
    parts.push_back(std::move(p));
    mojo::MakeSelfOwnedReceiver(
        std::make_unique<MbBlob>(uuid_, std::move(parts)), std::move(receiver));
  }
  void AsDataPipeGetter(
      mojo::PendingReceiver<network::mojom::blink::DataPipeGetter>) override {}
  void ReadAll(
      mojo::ScopedDataPipeProducerHandle pipe,
      mojo::PendingRemote<blink::mojom::blink::BlobReaderClient> client)
      override {
    if (ready_) {
      BlobReadSession::Start(data_, std::move(pipe), std::move(client));
    } else {
      pending_reads_.push_back({std::move(pipe), std::move(client)});
    }
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
  struct PendingRead {
    mojo::ScopedDataPipeProducerHandle pipe;
    mojo::PendingRemote<blink::mojom::blink::BlobReaderClient> client;
  };

  // Walk parts in order, appending bytes to data_. Inline parts append
  // synchronously; provider parts fetch via RequestAsReply (async). Capturing
  // `this` is safe: parts_[i].provider is a member, so destroying this cancels
  // the pending reply.
  void Materialize(size_t i) {
    while (i < parts_.size() && !parts_[i].provider) {
      const auto& b = parts_[i].inline_bytes;
      data_.insert(data_.end(), b.begin(), b.end());
      ++i;
    }
    if (i >= parts_.size()) {
      ready_ = true;
      DrainPending();
      return;
    }
    parts_[i].provider->RequestAsReply(
        base::BindOnce(&MbBlob::OnProviderBytes, base::Unretained(this), i));
  }

  void OnProviderBytes(size_t i, const blink::Vector<uint8_t>& bytes) {
    data_.insert(data_.end(), bytes.begin(), bytes.end());
    Materialize(i + 1);
  }

  void DrainPending() {
    for (auto& pr : pending_reads_)
      BlobReadSession::Start(data_, std::move(pr.pipe), std::move(pr.client));
    pending_reads_.clear();
  }

  blink::String uuid_;
  std::vector<Part> parts_;
  std::vector<uint8_t> data_;
  bool ready_ = false;
  std::vector<PendingRead> pending_reads_;
};

// In-process BlobRegistry. Lives on the service thread; turns Register's
// DataElements into an MbBlob (inline bytes or BytesProvider per element).
class MbBlobRegistry : public blink::mojom::blink::BlobRegistry {
 public:
  void Register(mojo::PendingReceiver<blink::mojom::blink::Blob> blob,
                const blink::String& uuid,
                const blink::String& /*content_type*/,
                const blink::String& /*content_disposition*/,
                blink::Vector<blink::mojom::blink::DataElementPtr> elements,
                RegisterCallback callback) override {
    std::vector<MbBlob::Part> parts;
    for (auto& el : elements) {
      if (!el || !el->is_bytes())
        continue;
      auto& b = el->get_bytes();
      MbBlob::Part p;
      if (b->embedded_data) {
        const auto& ed = *b->embedded_data;
        p.inline_bytes.assign(ed.begin(), ed.end());
      } else if (b->data) {
        p.provider.Bind(std::move(b->data));  // fetched lazily, post-reply
      }
      parts.push_back(std::move(p));
    }
    mojo::MakeSelfOwnedReceiver(
        std::make_unique<MbBlob>(uuid, std::move(parts)), std::move(blob));
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
