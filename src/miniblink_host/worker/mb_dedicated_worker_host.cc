#include "miniblink_host/worker/mb_dedicated_worker_host.h"

#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/containers/span.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "url/gurl.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/data_pipe_producer.h"
#include "mojo/public/cpp/system/string_data_source.h"
#include "net/http/http_response_headers.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "third_party/blink/public/common/loader/worker_main_script_load_parameters.h"
#include "third_party/blink/public/mojom/frame/back_forward_cache_controller.mojom.h"
#include "third_party/blink/public/mojom/worker/dedicated_worker_host.mojom.h"
#include "third_party/blink/public/platform/web_dedicated_worker.h"
#include "third_party/blink/public/platform/web_dedicated_worker_host_factory_client.h"
#include "third_party/blink/public/platform/web_security_origin.h"
#include "third_party/blink/public/platform/web_url.h"

#include "miniblink_host/frame/mb_frame_broker.h"
#include "miniblink_host/loader/mb_url_loader.h"

namespace mb {
namespace {

// The browser-side DedicatedWorkerHost is an EMPTY mojom interface — a receiver that
// keeps the worker's host pipe alive and answers nothing.
class MbDedicatedWorkerHost : public blink::mojom::DedicatedWorkerHost {};

// The back/forward-cache controller the worker's scheduler talks to. We don't bf-cache,
// so both signals are no-ops; binding a real receiver (vs. a null remote) avoids a CHECK
// if the worker reports feature usage.
class MbBfcacheControllerHost
    : public blink::mojom::BackForwardCacheControllerHost {
 public:
  void EvictFromBackForwardCache(
      blink::mojom::RendererEvictionReason /*reason*/,
      blink::mojom::ScriptSourceLocationPtr /*source*/) override {}
  void DidChangeBackForwardCacheDisablingFeatures(
      std::vector<blink::mojom::BlockingDetailsPtr> /*details*/) override {}
};

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

class MbWorkerHostFactoryClient
    : public blink::WebDedicatedWorkerHostFactoryClient {
 public:
  explicit MbWorkerHostFactoryClient(blink::WebDedicatedWorker* worker)
      : worker_(worker) {}

  void CreateWorkerHost(
      const blink::DedicatedWorkerToken&,
      const blink::WebURL& script_url,
      network::mojom::CredentialsMode,
      const blink::WebFetchClientSettingsObject&,
      blink::CrossVariantMojoRemote<blink::mojom::BlobURLTokenInterfaceBase>,
      net::StorageAccessApiStatus) override {
    // 1. Host handshake: hand blink the frame's interface broker, an empty
    //    DedicatedWorkerHost receiver, and the script's origin.
    mojo::PendingRemote<blink::mojom::DedicatedWorkerHost> host_remote;
    mojo::MakeSelfOwnedReceiver(
        std::make_unique<MbDedicatedWorkerHost>(),
        host_remote.InitWithNewPipeAndPassReceiver());
    worker_->OnWorkerHostCreated(
        MakeFrameInterfaceBroker(), std::move(host_remote),
        blink::WebSecurityOrigin::Create(script_url));

    // 2. Fetch the top-level script (file://, http(s)://, or data:).
    const std::string url = script_url.GetString().Utf8();
    std::string body, content_type;
    if (!MbFetchUrl(url, &body, &content_type)) {
      worker_->OnScriptLoadStartFailed();
      return;
    }
    std::string mime = content_type.substr(0, content_type.find(';'));
    if (mime.empty())
      mime = "text/javascript";

    // 3. Synthesize the browser-fetched-script params: a 200 response head, the
    //    script bytes over a data pipe, and a URLLoaderClient endpoint we drive.
    auto params = std::make_unique<blink::WorkerMainScriptLoadParameters>();
    params->request_id = 1;
    params->response_head = network::mojom::URLResponseHead::New();
    params->response_head->mime_type = mime;
    // A Content-Type HEADER (not just the mime_type field) is required for MODULE
    // workers: WorkerModuleScriptFetcher reads ResourceResponse::HttpContentType() and
    // enforces a JavaScript MIME type. Classic workers don't check, but it's harmless
    // for them.
    params->response_head->headers = net::HttpResponseHeaders::TryToCreate(
        "HTTP/1.1 200 OK\nContent-Type: " + mime + "\n\n");

    mojo::ScopedDataPipeProducerHandle producer;
    mojo::ScopedDataPipeConsumerHandle consumer;
    if (mojo::CreateDataPipe(nullptr, producer, consumer) != MOJO_RESULT_OK) {
      worker_->OnScriptLoadStartFailed();
      return;
    }
    params->response_body = std::move(consumer);

    mojo::PendingRemote<network::mojom::URLLoader> loader_remote;
    auto loader_receiver = loader_remote.InitWithNewPipeAndPassReceiver();
    mojo::Remote<network::mojom::URLLoaderClient> client_remote;
    auto client_receiver = client_remote.BindNewPipeAndPassReceiver();
    params->url_loader_client_endpoints =
        network::mojom::URLLoaderClientEndpoints::New(std::move(loader_remote),
                                                      std::move(client_receiver));
    // Owns its endpoints; deletes itself when blink finishes the script.
    new MbWorkerScript(std::move(loader_receiver), std::move(client_remote),
                       std::move(producer), std::move(body));

    // 4. Start the worker thread + run the script.
    mojo::PendingRemote<blink::mojom::BackForwardCacheControllerHost> bfcache;
    mojo::MakeSelfOwnedReceiver(
        std::make_unique<MbBfcacheControllerHost>(),
        bfcache.InitWithNewPipeAndPassReceiver());
    worker_->OnScriptLoadStarted(std::move(params), std::move(bfcache),
                                 mojo::NullReceiver(), mojo::NullReceiver());
  }

  scoped_refptr<blink::WebWorkerFetchContext> CloneWorkerFetchContext(
      blink::WebWorkerFetchContext*,
      scoped_refptr<base::SingleThreadTaskRunner>) override {
    return nullptr;  // nested workers unsupported
  }

 private:
  blink::WebDedicatedWorker* worker_;  // owns this factory client
};

}  // namespace

std::unique_ptr<blink::WebDedicatedWorkerHostFactoryClient>
MakeDedicatedWorkerHostFactoryClient(blink::WebDedicatedWorker* worker) {
  return std::make_unique<MbWorkerHostFactoryClient>(worker);
}

}  // namespace mb
