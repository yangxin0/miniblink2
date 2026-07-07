#include "miniblink_host/blob/mb_blob_registry.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include <map>
#include <string>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_runner.h"
#include "miniblink_host/frame/mb_broadcast_channel.h"
#include "miniblink_host/frame/mb_local_frame_host.h"
#include "miniblink_host/runtime/mb_runtime.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/self_owned_associated_receiver.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/data_pipe_drainer.h"
#include "mojo/public/cpp/system/simple_watcher.h"
#include "net/base/net_errors.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/data_pipe_getter.mojom-blink.h"
#include "services/network/public/mojom/url_loader.mojom-blink.h"
#include "services/network/public/mojom/url_loader_factory.mojom-blink.h"
#include "services/network/public/mojom/url_response_head.mojom-blink.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "third_party/blink/public/mojom/associated_interfaces/associated_interfaces.mojom.h"
#include "third_party/blink/public/mojom/blob/blob.mojom-blink.h"
#include "third_party/blink/public/mojom/blob/blob_url_store.mojom-blink.h"
#include "third_party/blink/public/mojom/broadcastchannel/broadcast_channel.mojom-blink.h"
#include "third_party/blink/public/mojom/blob/data_element.mojom-blink.h"
#include "third_party/blink/renderer/platform/blob/blob_data.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

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

// A network URLLoader that serves a blob's bytes for a blob: URL fetch. Owns the
// URLLoaderClient remote and, on construction, delivers a 200 response whose body
// pipe is fed by a BlobReadSession. Self-owned: lives until the loader endpoint
// (held by the fetch) is dropped. URLLoader control methods are no-ops.
class MbBlobURLLoader : public network::mojom::blink::URLLoader {
 public:
  MbBlobURLLoader(
      const std::vector<uint8_t>& data,
      const blink::String& content_type,
      mojo::PendingRemote<network::mojom::blink::URLLoaderClient> client)
      : client_(std::move(client)) {
    // The blob's stored type drives MIME sniffing for blob: URL consumers that
    // enforce strict MIME (module scripts, Worker(), CSS @import). Fall back to a
    // generic binary type when the blob carried no Content-Type.
    blink::String mime = content_type.empty()
                             ? blink::String("application/octet-stream")
                             : content_type;
    auto head = network::mojom::blink::URLResponseHead::New();
    head->mime_type = mime;
    // Several URLResponseHead fields are non-nullable (mojo validation aborts on
    // a null), so give them valid empty/default values.
    head->charset = blink::String("utf-8");
    auto lt = network::mojom::blink::LoadTimingInfo::New();
    lt->connect_timing =
        network::mojom::blink::LoadTimingInfoConnectTiming::New();
    head->load_timing = std::move(lt);
    head->alpn_negotiated_protocol = blink::String("");
    head->cache_storage_cache_name = blink::String("");
    head->headers = base::MakeRefCounted<net::HttpResponseHeaders>(
        std::string("HTTP/1.1 200 OK\0", 16));
    head->headers->SetHeader("Content-Type", mime.Utf8());
    head->headers->SetHeader("Content-Length", std::to_string(data.size()));
    mojo::ScopedDataPipeProducerHandle producer;
    mojo::ScopedDataPipeConsumerHandle consumer;
    if (mojo::CreateDataPipe(nullptr, producer, consumer) != MOJO_RESULT_OK) {
      // Don't leave the fetch hanging (loader bound, no response/complete sent) —
      // fail it cleanly so the blob: URL fetch rejects instead of stalling.
      client_->OnComplete(network::URLLoaderCompletionStatus(
          net::ERR_INSUFFICIENT_RESOURCES));
      return;
    }
    client_->OnReceiveResponse(std::move(head), std::move(consumer),
                               std::nullopt);
    BlobReadSession::Start(data, std::move(producer), mojo::NullRemote());
    client_->OnComplete(network::URLLoaderCompletionStatus(net::OK));
  }
  void FollowRedirect(network::HttpRequestHeadersUpdateParams,
                      const std::optional<blink::KURL>&) override {}
  void SetPriority(net::RequestPriority, int32_t) override {}

 private:
  mojo::Remote<network::mojom::blink::URLLoaderClient> client_;
};

// Streams a blob's bytes as a network DataPipeGetter. Blink binds one of these
// per Blob/File request body (FormDataElement::kEncodedBlob) to upload it: Read
// reports the byte count, then pushes the bytes into the supplied pipe. Holds its
// own copy of the materialized bytes so it can be re-Read/Cloned by the network
// stack. Self-owned on the service thread.
class MbDataPipeGetter : public network::mojom::blink::DataPipeGetter {
 public:
  explicit MbDataPipeGetter(std::vector<uint8_t> data)
      : data_(std::move(data)) {}

  void Read(mojo::ScopedDataPipeProducerHandle pipe,
            ReadCallback callback) override {
    // Report the body size up front (status 0 == net::OK), then stream the bytes.
    // BlobReadSession owns the write loop and closes the pipe (EOF) when done.
    std::move(callback).Run(net::OK, data_.size());
    BlobReadSession::Start(data_, std::move(pipe), mojo::NullRemote());
  }
  void Clone(mojo::PendingReceiver<network::mojom::blink::DataPipeGetter>
                 receiver) override {
    mojo::MakeSelfOwnedReceiver(std::make_unique<MbDataPipeGetter>(data_),
                                std::move(receiver));
  }

 private:
  std::vector<uint8_t> data_;
};

// Reads another Blob fully (ReadAll into a pipe, drained), then hands back the
// bytes — used to resolve is_blob DataElements (a blob composed of / slicing
// another blob, e.g. Response.blob(), Blob.slice()). Self-owned on the service
// thread; deletes itself when the read completes.
class BlobRefReader : public mojo::DataPipeDrainer::Client {
 public:
  static void Read(mojo::Remote<blink::mojom::blink::Blob> blob,
                   uint64_t offset,
                   uint64_t length,
                   base::OnceCallback<void(std::vector<uint8_t>)> done) {
    auto* r = new BlobRefReader(std::move(blob), offset, length,
                                std::move(done));
    r->Begin();
  }

 private:
  BlobRefReader(mojo::Remote<blink::mojom::blink::Blob> blob,
                uint64_t offset,
                uint64_t length,
                base::OnceCallback<void(std::vector<uint8_t>)> done)
      : blob_(std::move(blob)),
        offset_(offset),
        length_(length),
        done_(std::move(done)) {}

  void Begin() {
    mojo::ScopedDataPipeProducerHandle producer;
    mojo::ScopedDataPipeConsumerHandle consumer;
    if (mojo::CreateDataPipe(nullptr, producer, consumer) != MOJO_RESULT_OK) {
      Finish();
      return;
    }
    blob_->ReadAll(std::move(producer), /*client=*/mojo::NullRemote());
    drainer_ = std::make_unique<mojo::DataPipeDrainer>(this, std::move(consumer));
  }

  void OnDataAvailable(base::span<const uint8_t> data) override {
    bytes_.insert(bytes_.end(), data.begin(), data.end());
  }
  void OnDataComplete() override { Finish(); }

  void Finish() {
    // Apply the element's [offset, offset+length) slice.
    std::vector<uint8_t> out;
    if (offset_ < bytes_.size()) {
      // Overflow-safe end: comparing against the remaining count avoids the
      // offset_ + length_ (uint64_t) wrap that would invert the iterator range.
      size_t end = (length_ > bytes_.size() - offset_)
                       ? bytes_.size()
                       : static_cast<size_t>(offset_ + length_);
      out.assign(bytes_.begin() + offset_, bytes_.begin() + end);
    }
    std::move(done_).Run(std::move(out));
    delete this;
  }

  mojo::Remote<blink::mojom::blink::Blob> blob_;
  uint64_t offset_;
  uint64_t length_;
  base::OnceCallback<void(std::vector<uint8_t>)> done_;
  std::vector<uint8_t> bytes_;
  std::unique_ptr<mojo::DataPipeDrainer> drainer_;
};

// In-process Blob. Holds the bytes; serves reads. Lives on the service thread.
// Bytes may arrive inline (embedded_data), via a BytesProvider (>256 KB), or by
// reading another blob (is_blob elements) — all fetched asynchronously AFTER
// Register replies. Reads arriving before the bytes are materialized are queued
// and drained when ready.
class MbBlob : public blink::mojom::blink::Blob {
 public:
  struct Part {
    std::vector<uint8_t> inline_bytes;                          // inline
    mojo::Remote<blink::mojom::blink::BytesProvider> provider;  // or fetch
    mojo::Remote<blink::mojom::blink::Blob> blob_ref;           // or read a blob
    uint64_t offset = 0;
    uint64_t length = std::numeric_limits<uint64_t>::max();
  };

  MbBlob(blink::String uuid,
         blink::String content_type,
         std::vector<Part> parts)
      : uuid_(std::move(uuid)),
        content_type_(std::move(content_type)),
        parts_(std::move(parts)) {
    Materialize(0);  // assemble data_ in element order (async if providers)
  }

  // Bind the REGISTERED blob with deferred-delete ownership: the object dies
  // only when its pipe has disconnected AND materialization finished (queues
  // drained). The old MakeSelfOwnedReceiver deleted the blob the instant the
  // page dropped its last JS handle — which under rapid cache.put/match
  // happened MID-MATERIALIZE: the Part remotes died (tearing down the
  // BytesProvider before it replied) and every queued read/clone was
  // stranded, the B1 large-body empty/hang. cache_storage and friends
  // reference the bytes through those queued ops, so the blob must outlive
  // its own pipe until they're served.
  static void BindOwned(std::unique_ptr<MbBlob> blob,
                        mojo::PendingReceiver<blink::mojom::blink::Blob> r) {
    MbBlob* raw = blob.release();
    raw->receiver_.Bind(std::move(r));
    raw->receiver_.set_disconnect_handler(
        base::BindOnce(&MbBlob::OnPipeDisconnected, base::Unretained(raw)));
  }

  void Clone(
      mojo::PendingReceiver<blink::mojom::blink::Blob> receiver) override {
    // The clone must serve the FULLY materialized bytes. If this blob isn't ready
    // yet (a BytesProvider / blob_ref part is still being fetched asynchronously),
    // data_ is partial — binding a clone from it NOW would serve TRUNCATED bytes
    // (the bug that made a blob: URL fetched right after createObjectURL of a large
    // blob come back short/empty). Defer until DrainPending(), like pending reads/
    // loads: messages queue on the as-yet-unbound receiver pipe and flush on bind.
    if (!ready_) {
      pending_clones_.push_back(std::move(receiver));
      return;
    }
    CloneNow(std::move(receiver));
  }
  void AsDataPipeGetter(
      mojo::PendingReceiver<network::mojom::blink::DataPipeGetter> receiver)
      override {
    // Must stream the FULLY materialized bytes (see Clone()). Defer until ready.
    if (!ready_) {
      pending_getters_.push_back(std::move(receiver));
      return;
    }
    BindDataPipeGetter(std::move(receiver));
  }
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
      uint64_t offset,
      uint64_t length,
      mojo::ScopedDataPipeProducerHandle pipe,
      mojo::PendingRemote<blink::mojom::blink::BlobReaderClient> client)
      override {
    if (!ready_) {
      pending_ranges_.push_back(
          {offset, length, std::move(pipe), std::move(client)});
      return;
    }
    StartRange(offset, length, std::move(pipe), std::move(client));
  }
  void Load(mojo::PendingReceiver<network::mojom::blink::URLLoader> loader,
            const blink::String& /*method*/,
            const net::HttpRequestHeaders& /*headers*/,
            mojo::PendingRemote<network::mojom::blink::URLLoaderClient> client)
      override {
    // Serves this blob for a blob: URL fetch. Defer until the bytes are
    // materialized, then hand a URLLoader that streams data_ to the client.
    if (!ready_) {
      pending_loads_.push_back({std::move(loader), std::move(client)});
      return;
    }
    mojo::MakeSelfOwnedReceiver(
        std::make_unique<MbBlobURLLoader>(data_, content_type_,
                                          std::move(client)),
        std::move(loader));
  }
  void ReadSideData(ReadSideDataCallback callback) override {
    std::move(callback).Run(std::nullopt);
  }
  void CaptureSnapshot(CaptureSnapshotCallback callback) override {
    // data_ is only complete once ready_; reporting its partial size here would
    // give File.size/lastModified a wrong (short) value. Defer until materialized.
    if (!ready_) {
      pending_snapshots_.push_back(std::move(callback));
      return;
    }
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
  struct PendingLoad {
    mojo::PendingReceiver<network::mojom::blink::URLLoader> loader;
    mojo::PendingRemote<network::mojom::blink::URLLoaderClient> client;
  };
  struct PendingRange {
    uint64_t offset;
    uint64_t length;
    mojo::ScopedDataPipeProducerHandle pipe;
    mojo::PendingRemote<blink::mojom::blink::BlobReaderClient> client;
  };

  // Walk parts in order, appending bytes to data_. Inline parts append
  // synchronously; provider parts fetch via BytesProvider.RequestAsReply and
  // blob_ref parts read the referenced blob (both async). WeakPtr guards the
  // callbacks so a destroyed MbBlob is handled safely (a self-owned BlobRefReader
  // may outlive it).
  void Materialize(size_t i) {
    while (i < parts_.size()) {
      Part& p = parts_[i];
      if (p.provider) {
        p.provider->RequestAsReply(base::BindOnce(
            &MbBlob::OnProviderBytes, weak_factory_.GetWeakPtr(), i));
        return;
      }
      if (p.blob_ref) {
        BlobRefReader::Read(
            std::move(p.blob_ref), p.offset, p.length,
            base::BindOnce(&MbBlob::OnBlobRefBytes,
                           weak_factory_.GetWeakPtr(), i));
        return;
      }
      data_.insert(data_.end(), p.inline_bytes.begin(), p.inline_bytes.end());
      ++i;
    }
    ready_ = true;
    DrainPending();
    MaybeDelete();  // if the pipe already dropped, we lived to serve the queues
  }

  void OnPipeDisconnected() {
    disconnected_ = true;
    MaybeDelete();
  }

  // Deferred delete (BindOwned blobs only; self-owned clones never set
  // disconnected_): gone once the page dropped its handle AND the bytes were
  // assembled + queued consumers served. NOT deleted while a BytesProvider
  // reply is outstanding — that keeps the Part remotes (and so the provider
  // itself) alive until it answers.
  void MaybeDelete() {
    if (disconnected_ && ready_)
      delete this;
  }

  void OnProviderBytes(size_t i, const blink::Vector<uint8_t>& bytes) {
    // B1 diagnostics: an EMPTY provider reply is the smoking gun for the
    // large-blob stall (the part's bytes are gone; the blob completes as
    // net::OK size 0 — the silent-empty). Gated so normal runs stay quiet.
    if (bytes.empty() && getenv("MB_BLOB_DEBUG")) {
      fprintf(stderr, "mb_blob: BytesProvider part %zu replied EMPTY (blob so far %zu bytes)\n",
              i, data_.size());
    }
    data_.insert(data_.end(), bytes.begin(), bytes.end());
    Materialize(i + 1);
  }
  void OnBlobRefBytes(size_t i, std::vector<uint8_t> bytes) {
    data_.insert(data_.end(), bytes.begin(), bytes.end());
    Materialize(i + 1);
  }

  // Bind a clone serving the (now fully materialized) bytes.
  void CloneNow(mojo::PendingReceiver<blink::mojom::blink::Blob> receiver) {
    std::vector<Part> parts;
    Part p;
    p.inline_bytes = data_;
    parts.push_back(std::move(p));
    mojo::MakeSelfOwnedReceiver(
        std::make_unique<MbBlob>(uuid_, content_type_, std::move(parts)),
        std::move(receiver));
  }

  // Bind a DataPipeGetter that streams the (now materialized) bytes.
  void BindDataPipeGetter(
      mojo::PendingReceiver<network::mojom::blink::DataPipeGetter> receiver) {
    mojo::MakeSelfOwnedReceiver(std::make_unique<MbDataPipeGetter>(data_),
                                std::move(receiver));
  }

  // Stream data_'s [offset, offset+length) slice to a BlobReaderClient, reusing
  // the BlobReadSession path. Offset/length are clamped overflow-safe.
  void StartRange(
      uint64_t offset,
      uint64_t length,
      mojo::ScopedDataPipeProducerHandle pipe,
      mojo::PendingRemote<blink::mojom::blink::BlobReaderClient> client) {
    std::vector<uint8_t> slice;
    if (offset < data_.size()) {
      size_t end = (length > data_.size() - offset)
                       ? data_.size()
                       : static_cast<size_t>(offset + length);
      slice.assign(data_.begin() + offset, data_.begin() + end);
    }
    BlobReadSession::Start(std::move(slice), std::move(pipe),
                           std::move(client));
  }

  void DrainPending() {
    for (auto& pr : pending_reads_)
      BlobReadSession::Start(data_, std::move(pr.pipe), std::move(pr.client));
    pending_reads_.clear();
    for (auto& pr : pending_ranges_)
      StartRange(pr.offset, pr.length, std::move(pr.pipe),
                 std::move(pr.client));
    pending_ranges_.clear();
    for (auto& pl : pending_loads_) {
      mojo::MakeSelfOwnedReceiver(
          std::make_unique<MbBlobURLLoader>(data_, content_type_,
                                            std::move(pl.client)),
          std::move(pl.loader));
    }
    pending_loads_.clear();
    for (auto& pg : pending_getters_)
      BindDataPipeGetter(std::move(pg));
    pending_getters_.clear();
    for (auto& cb : pending_snapshots_)
      std::move(cb).Run(data_.size(), std::nullopt);
    pending_snapshots_.clear();
    for (auto& rc : pending_clones_)
      CloneNow(std::move(rc));
    pending_clones_.clear();
  }

  blink::String uuid_;
  blink::String content_type_;
  std::vector<Part> parts_;
  std::vector<uint8_t> data_;
  bool ready_ = false;
  std::vector<PendingRead> pending_reads_;
  std::vector<PendingRange> pending_ranges_;
  std::vector<PendingLoad> pending_loads_;
  std::vector<mojo::PendingReceiver<network::mojom::blink::DataPipeGetter>>
      pending_getters_;
  std::vector<CaptureSnapshotCallback> pending_snapshots_;
  // Clone requests received before the bytes finished materializing (see Clone()).
  std::vector<mojo::PendingReceiver<blink::mojom::blink::Blob>> pending_clones_;
  // BindOwned lifetime (registered blobs): bound pipe + disconnect flag.
  mojo::Receiver<blink::mojom::blink::Blob> receiver_{this};
  bool disconnected_ = false;
  base::WeakPtrFactory<MbBlob> weak_factory_{this};
};

// Services BlobRegistry.RegisterFromStream: drains the body data pipe into
// bytes, builds an MbBlob, and replies with a BlobDataHandle (the blink-variant
// type mapping of the SerializedBlob reply). Used when a blob is built from a
// streamed body — e.g. fetch(url).blob() (FetchDataLoader::CreateLoaderAsBlobHandle,
// fetch_data_loader.cc:84). Self-owned on the service thread.
class StreamRegistration : public mojo::DataPipeDrainer::Client {
 public:
  static void Start(
      blink::String content_type,
      mojo::ScopedDataPipeConsumerHandle data,
      blink::mojom::blink::BlobRegistry::RegisterFromStreamCallback callback) {
    (new StreamRegistration(std::move(content_type), std::move(callback)))
        ->Begin(std::move(data));
  }

 private:
  StreamRegistration(
      blink::String content_type,
      blink::mojom::blink::BlobRegistry::RegisterFromStreamCallback callback)
      : content_type_(std::move(content_type)),
        callback_(std::move(callback)) {}

  void Begin(mojo::ScopedDataPipeConsumerHandle data) {
    drainer_ = std::make_unique<mojo::DataPipeDrainer>(this, std::move(data));
  }
  void OnDataAvailable(base::span<const uint8_t> data) override {
    bytes_.insert(bytes_.end(), data.begin(), data.end());
  }
  void OnDataComplete() override {
    static uint64_t next_id = 0;  // service-thread only
    blink::String uuid = "mb-stream-" + blink::String::Number(next_id++);
    mojo::PendingRemote<blink::mojom::blink::Blob> remote;
    std::vector<MbBlob::Part> parts;
    MbBlob::Part p;
    p.inline_bytes = bytes_;
    parts.push_back(std::move(p));
    mojo::MakeSelfOwnedReceiver(
        std::make_unique<MbBlob>(uuid, content_type_, std::move(parts)),
        remote.InitWithNewPipeAndPassReceiver());
    std::move(callback_).Run(blink::BlobDataHandle::Create(
        uuid, content_type_, bytes_.size(), std::move(remote)));
    delete this;
  }

  blink::String content_type_;
  blink::mojom::blink::BlobRegistry::RegisterFromStreamCallback callback_;
  std::vector<uint8_t> bytes_;
  std::unique_ptr<mojo::DataPipeDrainer> drainer_;
};

// In-process BlobRegistry. Lives on the service thread; turns Register's
// DataElements into an MbBlob (inline bytes or BytesProvider per element).
class MbBlobRegistry : public blink::mojom::blink::BlobRegistry {
 public:
  void Register(mojo::PendingReceiver<blink::mojom::blink::Blob> blob,
                const blink::String& uuid,
                const blink::String& content_type,
                const blink::String& /*content_disposition*/,
                blink::Vector<blink::mojom::blink::DataElementPtr> elements,
                RegisterCallback callback) override {
    std::vector<MbBlob::Part> parts;
    for (auto& el : elements) {
      if (!el)
        continue;
      MbBlob::Part p;
      if (el->is_bytes()) {
        auto& b = el->get_bytes();
        if (b->embedded_data) {
          const auto& ed = *b->embedded_data;
          p.inline_bytes.assign(ed.begin(), ed.end());
        } else if (b->data) {
          p.provider.Bind(std::move(b->data));  // fetched lazily, post-reply
        }
      } else if (el->is_blob()) {
        // A blob composed of / slicing another blob (Response.blob(),
        // Blob.slice()): read the referenced blob's [offset,offset+length).
        auto& eb = el->get_blob();
        p.blob_ref.Bind(std::move(eb->blob));
        p.offset = eb->offset;
        p.length = eb->length;
      } else {
        continue;  // is_file: unsupported (no filesystem)
      }
      parts.push_back(std::move(p));
    }
    MbBlob::BindOwned(
        std::make_unique<MbBlob>(uuid, content_type, std::move(parts)),
        std::move(blob));
    std::move(callback).Run();  // reply () — unblocks Blink's [Sync] Register
  }

  void RegisterFromStream(
      const blink::String& content_type,
      const blink::String& /*content_disposition*/,
      uint64_t /*length_hint*/,
      mojo::ScopedDataPipeConsumerHandle data,
      mojo::PendingAssociatedRemote<blink::mojom::blink::ProgressClient>,
      RegisterFromStreamCallback callback) override {
    StreamRegistration::Start(content_type, std::move(data), std::move(callback));
  }
};

// Process-global blob: URL -> Blob remote map. Touched only on the service
// thread (MbBlobURLStore is bound there), so no lock.
std::map<std::string, mojo::Remote<blink::mojom::blink::Blob>>& BlobUrlMap() {
  static base::NoDestructor<
      std::map<std::string, mojo::Remote<blink::mojom::blink::Blob>>>
      m;
  return *m;
}

// A URLLoaderFactory bound to one blob: forwards every CreateLoaderAndStart to
// the Blob's own Load() (which streams the bytes). Self-owned per resolve.
class MbBlobURLLoaderFactory : public network::mojom::blink::URLLoaderFactory {
 public:
  explicit MbBlobURLLoaderFactory(
      mojo::PendingRemote<blink::mojom::blink::Blob> blob)
      : blob_(std::move(blob)) {}
  void CreateLoaderAndStart(
      mojo::PendingReceiver<network::mojom::blink::URLLoader> loader,
      int32_t /*request_id*/,
      uint32_t /*options*/,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::blink::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag& /*ta*/) override {
    blob_->Load(std::move(loader),
                blink::String::FromUtf8(request.method.c_str()),
                net::HttpRequestHeaders(), std::move(client));
  }
  void Clone(mojo::PendingReceiver<network::mojom::blink::URLLoaderFactory>
                 factory) override {
    mojo::PendingRemote<blink::mojom::blink::Blob> cloned;
    blob_->Clone(cloned.InitWithNewPipeAndPassReceiver());
    mojo::MakeSelfOwnedReceiver(
        std::make_unique<MbBlobURLLoaderFactory>(std::move(cloned)),
        std::move(factory));
  }

 private:
  mojo::Remote<blink::mojom::blink::Blob> blob_;
};

// In-process BlobURLStore: maps blob: URLs (URL.createObjectURL) to their Blob
// and resolves them for fetch via a URLLoaderFactory. Bound on the service
// thread, so the [Sync] Register from the main thread is serviced off-thread.
class MbBlobURLStore : public blink::mojom::blink::BlobURLStore {
 public:
  void Register(mojo::PendingRemote<blink::mojom::blink::Blob> blob,
                const blink::KURL& url,
                RegisterCallback callback) override {
    BlobUrlMap()[url.GetString().Utf8()] =
        mojo::Remote<blink::mojom::blink::Blob>(std::move(blob));
    std::move(callback).Run();
  }
  void Revoke(const blink::KURL& url) override {
    BlobUrlMap().erase(url.GetString().Utf8());
  }
  void ResolveAsURLLoaderFactory(
      const blink::KURL& url,
      mojo::PendingReceiver<network::mojom::blink::URLLoaderFactory> factory)
      override {
    auto it = BlobUrlMap().find(url.GetString().Utf8());
    if (it == BlobUrlMap().end())
      return;  // unknown blob: URL -> drop (the fetch fails, as it should)
    mojo::PendingRemote<blink::mojom::blink::Blob> cloned;
    it->second->Clone(cloned.InitWithNewPipeAndPassReceiver());
    mojo::MakeSelfOwnedReceiver(
        std::make_unique<MbBlobURLLoaderFactory>(std::move(cloned)),
        std::move(factory));
  }
  void ResolveAsBlobURLToken(
      const blink::KURL& /*url*/,
      mojo::PendingReceiver<blink::mojom::blink::BlobURLToken> /*token*/,
      bool /*is_top_level_navigation*/) override {}
};

// The frame's navigation-associated-interface provider, bound on the SERVICE
// thread. Routing GetAssociatedInterface here (off the main thread) lets us bind
// BlobURLStore on the service thread, so createObjectURL's [Sync] Register — made
// from the blocked main thread — is serviced instead of deadlocking.
class MbNavAssociatedInterfaceProvider
    : public blink::mojom::AssociatedInterfaceProvider {
 public:
  explicit MbNavAssociatedInterfaceProvider(uint64_t frame_key)
      : frame_key_(frame_key) {}

  void GetAssociatedInterface(
      const std::string& name,
      mojo::PendingAssociatedReceiver<blink::mojom::AssociatedInterface>
          receiver) override {
    if (name == blink::mojom::blink::BlobURLStore::Name_) {
      mojo::MakeSelfOwnedAssociatedReceiver(
          std::make_unique<MbBlobURLStore>(),
          mojo::PendingAssociatedReceiver<blink::mojom::blink::BlobURLStore>(
              receiver.PassHandle()));
      return;
    }
    // navigator BroadcastChannel (window path): in-process same-name fan-out.
    if (name == blink::mojom::blink::BroadcastChannelProvider::Name_) {
      BindBroadcastChannelProvider(receiver.PassHandle(), frame_key_);
      return;
    }
    // The frame's host channel. We bind a (mostly no-op) LocalFrameHost so that
    // page-driven history.back()/forward()/go() — sent as GoToEntryAtOffset — is
    // serviced (replayed onto the main frame) instead of dropped into the void.
    if (name == blink::mojom::blink::LocalFrameHost::Name_) {
      MbBindLocalFrameHost(receiver.PassHandle(), frame_key_);
      return;
    }
    // The WebView-level main-frame host: window.close() (RequestClose) routes
    // to the host's mbOnRequestClose; everything else no-ops (with replies run).
    if (name == blink::mojom::blink::LocalMainFrameHost::Name_) {
      MbBindLocalMainFrameHost(receiver.PassHandle(), frame_key_);
      return;
    }
    // Other associated interfaces are not provided here (dropped).
  }

 private:
  const uint64_t frame_key_;
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

void MbResolveBlobUrlBytes(
    const std::string& url,
    base::OnceCallback<void(std::vector<uint8_t>)> done) {
  // Service thread: the BlobURLStore registry and the Blob remotes live here.
  auto& map = BlobUrlMap();
  auto it = map.find(url);
  if (it == map.end() || !it->second) {
    std::move(done).Run({});  // unknown/revoked blob: URL
    return;
  }
  // Clone the Blob (the stored remote stays owned by the registry), then drain it
  // fully. length=max => the whole blob (BlobRefReader applies [0, size)).
  mojo::PendingRemote<blink::mojom::blink::Blob> cloned;
  it->second->Clone(cloned.InitWithNewPipeAndPassReceiver());
  BlobRefReader::Read(
      mojo::Remote<blink::mojom::blink::Blob>(std::move(cloned)), /*offset=*/0,
      /*length=*/std::numeric_limits<uint64_t>::max(), std::move(done));
}

void MbReadBlobRemoteBytes(
    mojo::PendingRemote<blink::mojom::blink::Blob> blob,
    base::OnceCallback<void(std::vector<uint8_t>)> done) {
  if (!blob) {
    std::move(done).Run({});
    return;
  }
  BlobRefReader::Read(
      mojo::Remote<blink::mojom::blink::Blob>(std::move(blob)), /*offset=*/0,
      /*length=*/std::numeric_limits<uint64_t>::max(), std::move(done));
}

scoped_refptr<blink::BlobDataHandle> MbCreateInlineBlob(
    const std::string& bytes,
    const blink::String& content_type) {
  static uint64_t next_id = 0;  // service-thread only
  blink::String uuid = "mb-inline-" + blink::String::Number(next_id++);
  MbBlob::Part part;
  part.inline_bytes.assign(bytes.begin(), bytes.end());
  std::vector<MbBlob::Part> parts;
  parts.push_back(std::move(part));
  mojo::PendingRemote<blink::mojom::blink::Blob> remote;
  mojo::MakeSelfOwnedReceiver(
      std::make_unique<MbBlob>(uuid, content_type, std::move(parts)),
      remote.InitWithNewPipeAndPassReceiver());
  return blink::BlobDataHandle::Create(uuid, content_type, bytes.size(),
                                       std::move(remote));
}

blink::AssociatedInterfaceProvider* MakeBlobUrlNavAssociatedInterfaces(
    uint64_t frame_key) {
  // Build a dedicated associated pipe and bind the provider impl on the SERVICE
  // thread. The provider's master endpoint then lives off the main thread, so
  // when the main thread blocks in createObjectURL's [Sync] BlobURLStore.Register,
  // the service thread independently dispatches GetAssociatedInterface (binding
  // BlobURLStore) and services Register -> no deadlock. (A local provider binds
  // via a main-thread task that can't run during the sync wait -> deadlock.)
  mojo::AssociatedRemote<blink::mojom::AssociatedInterfaceProvider> remote;
  auto receiver = remote.BindNewEndpointAndPassDedicatedReceiver();
  if (scoped_refptr<base::SingleThreadTaskRunner> runner =
          MbRuntime::ServiceTaskRunner()) {
    runner->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](mojo::PendingAssociatedReceiver<
                   blink::mojom::AssociatedInterfaceProvider> r,
               uint64_t key) {
              mojo::MakeSelfOwnedAssociatedReceiver(
                  std::make_unique<MbNavAssociatedInterfaceProvider>(key),
                  std::move(r));
            },
            std::move(receiver), frame_key));
  }
  return new blink::AssociatedInterfaceProvider(
      remote.Unbind(), base::SingleThreadTaskRunner::GetCurrentDefault());
}

}  // namespace mb
