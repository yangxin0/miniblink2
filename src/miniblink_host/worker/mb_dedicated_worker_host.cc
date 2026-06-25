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
#include "miniblink_host/worker/mb_worker_fetch_context.h"
#include "miniblink_host/worker/mb_worker_script.h"

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

    // 2-3. Fetch the top-level script (file://, http(s)://, or data:) and synthesize the
    //      browser-fetched-script load parameters (shared with shared workers).
    auto params = MakeWorkerMainScriptParams(script_url.GetString().Utf8());
    if (!params) {
      worker_->OnScriptLoadStartFailed();
      return;
    }

    // 4. Start the worker thread + run the script.
    mojo::PendingRemote<blink::mojom::BackForwardCacheControllerHost> bfcache;
    mojo::MakeSelfOwnedReceiver(
        std::make_unique<MbBfcacheControllerHost>(),
        bfcache.InitWithNewPipeAndPassReceiver());
    worker_->OnScriptLoadStarted(std::move(params), std::move(bfcache),
                                 mojo::NullReceiver(), mojo::NullReceiver());
  }

  scoped_refptr<blink::WebWorkerFetchContext> CloneWorkerFetchContext(
      blink::WebWorkerFetchContext* parent,
      scoped_refptr<base::SingleThreadTaskRunner>) override {
    // A nested worker reuses the parent worker's network identity. The parent is always
    // one of ours (we created it), so clone it for the sub-worker's subresource loads.
    if (parent)
      return static_cast<MbWorkerFetchContext*>(parent)->CloneContext();
    return nullptr;
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
