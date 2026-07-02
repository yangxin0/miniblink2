#include "miniblink_host/worker/mb_worker_script.h"

#include <stdint.h>

#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/data_pipe_producer.h"
#include "mojo/public/cpp/system/string_data_source.h"
#include "net/http/http_response_headers.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/gurl.h"

#include <vector>

#include "base/synchronization/waitable_event.h"
#include "miniblink_host/blob/mb_blob_registry.h"
#include "miniblink_host/loader/mb_url_loader.h"
#include "miniblink_host/runtime/mb_runtime.h"

namespace mb {
namespace {

// Streams the worker's main script to blink and then self-destructs. blink (the
// WorkerMainScriptLoader) binds the URLLoaderClient end and reads the body data pipe to
// EOF; it finishes only once it has BOTH the pipe EOF and our OnComplete. So we write the
// script bytes, drop the producer (EOF), and push OnComplete. We are also the (inert)
// URLLoader, and tie our lifetime to that receiver: when blink drops the loader remote
// (script consumed), we delete ourselves.
class MbWorkerScript : public network::mojom::URLLoader {
 public:
  MbWorkerScript(mojo::PendingReceiver<network::mojom::URLLoader> loader_receiver,
                 mojo::Remote<network::mojom::URLLoaderClient> client,
                 mojo::ScopedDataPipeProducerHandle producer,
                 std::string body)
      : receiver_(this, std::move(loader_receiver)),
        client_(std::move(client)),
        producer_(std::make_unique<mojo::DataPipeProducer>(std::move(producer))),
        body_(std::move(body)) {
    receiver_.set_disconnect_handler(
        base::BindOnce(&MbWorkerScript::OnDone, base::Unretained(this)));
    producer_->Write(
        std::make_unique<mojo::StringDataSource>(
            base::span<const char>(body_),
            mojo::StringDataSource::AsyncWritingMode::
                STRING_STAYS_VALID_UNTIL_COMPLETION),
        base::BindOnce(&MbWorkerScript::OnBodyWritten, base::Unretained(this)));
  }

  // network::mojom::URLLoader (blink never drives these for a pre-fetched script):
  void FollowRedirect(network::HttpRequestHeadersUpdateParams,
                      const std::optional<GURL>&) override {}
  void SetPriority(net::RequestPriority, int32_t) override {}

 private:
  void OnBodyWritten(MojoResult /*result*/) {
    producer_.reset();  // close the producer -> blink sees end-of-data
    client_->OnComplete(network::URLLoaderCompletionStatus(net::OK));
    // Stay alive until blink drops the loader remote (OnDone deletes us).
  }
  void OnDone() { delete this; }

  mojo::Receiver<network::mojom::URLLoader> receiver_;
  mojo::Remote<network::mojom::URLLoaderClient> client_;
  std::unique_ptr<mojo::DataPipeProducer> producer_;
  std::string body_;  // must outlive the async write (STAYS_VALID_UNTIL_COMPLETION)
};

}  // namespace

std::unique_ptr<blink::WorkerMainScriptLoadParameters> MakeWorkerMainScriptParams(
    const std::string& url) {
  std::string body, content_type;
  if (url.rfind("blob:", 0) == 0) {
    // `new Worker(URL.createObjectURL(blob))` — the bundler-standard way to spawn a
    // worker (Baidu MapGL's tile worker, webpack worker-loader, ...). The script bytes
    // live in our in-process blob registry on the SERVICE thread, not anywhere curl can
    // fetch — so resolve there and wait. This runs on the main thread while the registry
    // lives on the service thread, so the brief sync wait cannot deadlock (same
    // off-thread-service reasoning as the [Sync] BlobRegistry.Register handshake).
    // Before this, a blob: worker hung forever with no error event: MbFetchUrl failed,
    // OnScriptLoadStartFailed was reported, but the page-visible symptom was silence.
    base::WaitableEvent done;
    std::vector<uint8_t> bytes;
    MbRuntime::ServiceTaskRunner()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](const std::string& u, std::vector<uint8_t>* out,
               base::WaitableEvent* done) {
              MbResolveBlobUrlBytes(
                  u, base::BindOnce(
                         [](std::vector<uint8_t>* out, base::WaitableEvent* done,
                            std::vector<uint8_t> b) {
                           *out = std::move(b);
                           done->Signal();
                         },
                         out, done));
            },
            url, &bytes, &done));
    done.Wait();
    if (bytes.empty())
      return nullptr;  // unknown/revoked blob URL
    body.assign(bytes.begin(), bytes.end());
    content_type = "text/javascript";
  } else if (!MbFetchUrl(url, &body, &content_type)) {
    return nullptr;
  }
  std::string mime = content_type.substr(0, content_type.find(';'));
  if (mime.empty())
    mime = "text/javascript";

  auto params = std::make_unique<blink::WorkerMainScriptLoadParameters>();
  params->request_id = 1;
  params->response_head = network::mojom::URLResponseHead::New();
  params->response_head->mime_type = mime;
  // A Content-Type HEADER (not just the mime_type field) is required for MODULE workers:
  // WorkerModuleScriptFetcher reads ResourceResponse::HttpContentType() and enforces a
  // JavaScript MIME type. Classic workers don't check, but it's harmless for them.
  params->response_head->headers = net::HttpResponseHeaders::TryToCreate(
      "HTTP/1.1 200 OK\nContent-Type: " + mime + "\n\n");

  mojo::ScopedDataPipeProducerHandle producer;
  mojo::ScopedDataPipeConsumerHandle consumer;
  if (mojo::CreateDataPipe(nullptr, producer, consumer) != MOJO_RESULT_OK)
    return nullptr;
  params->response_body = std::move(consumer);

  mojo::PendingRemote<network::mojom::URLLoader> loader_remote;
  auto loader_receiver = loader_remote.InitWithNewPipeAndPassReceiver();
  mojo::Remote<network::mojom::URLLoaderClient> client_remote;
  auto client_receiver = client_remote.BindNewPipeAndPassReceiver();
  params->url_loader_client_endpoints =
      network::mojom::URLLoaderClientEndpoints::New(std::move(loader_remote),
                                                    std::move(client_receiver));
  // Owns its endpoints; deletes itself when blink finishes consuming the script.
  new MbWorkerScript(std::move(loader_receiver), std::move(client_remote),
                     std::move(producer), std::move(body));
  return params;
}

}  // namespace mb
