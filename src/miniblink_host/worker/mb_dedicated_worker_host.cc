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

#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/loader/worker_fetch_context.h"
#include "third_party/blink/renderer/core/workers/dedicated_worker.h"
#include "third_party/blink/renderer/core/workers/worker_global_scope.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_fetcher.h"

#include "miniblink_host/frame/mb_frame_broker.h"
#include "miniblink_host/frame/mb_frame_client.h"
#include "miniblink_host/frame/mb_frame_origin.h"
#include "miniblink_host/loader/mb_url_loader.h"
#include "miniblink_host/session/mb_session.h"
#include "miniblink_host/view/mb_webview.h"
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
  ~MbWorkerHostFactoryClient() override {
    if (worker_frame_key_)
      MbClearFrameOrigin(worker_frame_key_);  // forget the worker's origin entry
  }

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
    // The creating document/worker's loader identity scopes storage + script fetch.
    // For a nested worker, recover it from the parent Worker's WebWorkerFetchContext.
    MbWebView* view = nullptr;
    scoped_refptr<MbLoaderViewContext> loader_view_context;
    blink::ExecutionContext* execution_context =
        static_cast<blink::DedicatedWorker*>(worker_)->GetExecutionContext();
    if (auto* window =
            blink::DynamicTo<blink::LocalDOMWindow>(execution_context)) {
      view = MbViewForFrame(window->GetFrame());
      if (view)
        loader_view_context = view->loader_view_context();
    } else if (execution_context && execution_context->IsWorkerGlobalScope()) {
      auto* scope = blink::To<blink::WorkerGlobalScope>(execution_context);
      auto& parent_fetch_context =
          static_cast<blink::WorkerFetchContext&>(scope->Fetcher()->Context());
      if (blink::WebWorkerFetchContext* parent =
              parent_fetch_context.GetWebWorkerFetchContext()) {
        loader_view_context =
            static_cast<MbWorkerFetchContext*>(parent)->loader_view_context();
      }
    }
    // Scope the worker's per-origin storage (IndexedDB) by its origin, published
    // under a synthetic worker frame_key, so a same-origin worker SHARES its
    // window's IDB (and a cross-origin worker is isolated). The script origin is
    // the worker's origin for the common http(s) case; a data:/blob: worker is
    // opaque-by-URL ("null") -> its own bucket (the documented worker residual,
    // since the true parent origin isn't carried here).
    blink::WebSecurityOrigin origin =
        blink::WebSecurityOrigin::Create(script_url);
    worker_frame_key_ = MbAllocWorkerFrameKey();
    // SESSION PARTITIONING: windows register session-PREFIXED scopes
    // (MbFrameClient::DidCommitNavigation), so a concrete worker origin must
    // carry the same prefix or a same-origin worker no longer matches its
    // window (IDB sharing, BroadcastChannel bridging). Opaque origins ("null")
    // stay UNPREFIXED: the BroadcastChannel registry treats the literal
    // "null" as a wildcard, which is what keeps data:/blob: worker<->window
    // bridging alive.
    std::string scope = origin.ToString().Utf8();
    if (loader_view_context && !loader_view_context->session_key().empty() &&
        scope != "null") {
      scope = loader_view_context->session_key() + "\x1f" + scope;
    }
    MbSetFrameOrigin(worker_frame_key_, scope);
    worker_->OnWorkerHostCreated(MakeFrameInterfaceBroker(worker_frame_key_),
                                 std::move(host_remote), origin);

    // 2-3. Fetch the top-level script (file://, http(s)://, or data:) and synthesize the
    //      browser-fetched-script load parameters (shared with shared workers).
    //      The retained loader context marshals nested fetch policy/hooks to the
    //      engine runner and preserves the parent's session cookie jar.
    auto params = MakeWorkerMainScriptParams(
        script_url.GetString().Utf8(), std::move(loader_view_context));
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
  uint64_t worker_frame_key_ = 0;      // this worker's origin-map key (0 = unset)
};

}  // namespace

std::unique_ptr<blink::WebDedicatedWorkerHostFactoryClient>
MakeDedicatedWorkerHostFactoryClient(blink::WebDedicatedWorker* worker) {
  return std::make_unique<MbWorkerHostFactoryClient>(worker);
}

}  // namespace mb
