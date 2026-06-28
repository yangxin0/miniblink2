#include "miniblink_host/worker/mb_shared_worker.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/unguessable_token.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "mojo/public/cpp/bindings/pending_associated_remote.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/remote_set.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "services/metrics/public/cpp/ukm_source_id.h"
#include "third_party/blink/public/common/messaging/message_port_descriptor.h"
#include "third_party/blink/public/common/tokens/tokens.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "third_party/blink/public/mojom/devtools/devtools_agent.mojom-blink.h"
#include "third_party/blink/public/mojom/frame/policy_container.mojom-blink.h"
#include "third_party/blink/public/mojom/worker/shared_worker_client.mojom-blink.h"
#include "third_party/blink/public/mojom/worker/shared_worker_host.mojom.h"
#include "third_party/blink/public/mojom/worker/shared_worker_info.mojom-blink.h"
#include "third_party/blink/public/mojom/worker/worker_content_settings_proxy.mojom.h"
#include "third_party/blink/public/platform/web_content_security_policy_struct.h"
#include "third_party/blink/public/platform/web_fetch_client_settings_object.h"
#include "third_party/blink/public/platform/web_policy_container.h"
#include "third_party/blink/public/platform/web_security_origin.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/platform/web_url.h"
#include "third_party/blink/public/web/web_shared_worker.h"
#include "third_party/blink/public/web/web_shared_worker_client.h"

#include "miniblink_host/frame/mb_frame_broker.h"
#include "miniblink_host/frame/mb_frame_origin.h"
#include "miniblink_host/frame/mb_frame_client.h"  // MbDefaultUserAgentMetadata
#include "miniblink_host/loader/mb_url_loader.h"
#include "miniblink_host/worker/mb_worker_fetch_context.h"
#include "miniblink_host/worker/mb_worker_script.h"

namespace mb {
namespace {

// The browser-side SharedWorkerHost: the worker reports lifecycle/exceptions here. We have
// no browser, so every signal is a no-op (the receiver just keeps the pipe alive).
class MbSharedWorkerHost : public blink::mojom::SharedWorkerHost {
 public:
  void OnConnected(int32_t /*connection_id*/) override {}
  void OnContextClosed() override {}
  void OnReadyForInspection(
      mojo::PendingRemote<blink::mojom::DevToolsAgent>,
      mojo::PendingReceiver<blink::mojom::DevToolsAgentHost>) override {}
  void OnScriptLoadFailed(const std::string& /*error_message*/) override {}
  void OnReportException(blink::mojom::SharedWorkerExceptionDetailsPtr) override {}
  void OnFeatureUsed(blink::mojom::WebFeature /*feature*/) override {}
};

// Worker content-settings gatekeeper ([Sync] calls from the worker thread). No real policy
// engine — allow everything. Bound on the main thread (Connect runs there); the [Sync]
// calls block the worker thread, which the main thread answers as it pumps.
class MbWorkerContentSettingsProxy
    : public blink::mojom::WorkerContentSettingsProxy {
 public:
  void AllowIndexedDB(AllowIndexedDBCallback cb) override {
    std::move(cb).Run(true);
  }
  void AllowCacheStorage(AllowCacheStorageCallback cb) override {
    std::move(cb).Run(true);
  }
  void AllowWebLocks(AllowWebLocksCallback cb) override {
    std::move(cb).Run(true);
  }
  void RequestFileSystemAccessSync(
      RequestFileSystemAccessSyncCallback cb) override {
    std::move(cb).Run(true);
  }
};

// The worker's PolicyContainerHost (advisory CSP/referrer-policy sink). A bound receiver is
// required: SharedWorkerGlobalScope::Initialize calls UpdateReferrerPolicy through it, which
// CHECKs on an unbound remote. We ignore the updates.
class MbSwPolicyContainerHost : public blink::mojom::blink::PolicyContainerHost {
 public:
  mojo::PendingAssociatedRemote<blink::mojom::blink::PolicyContainerHost>
  BindRemote() {
    return receiver_.BindNewEndpointAndPassDedicatedRemote();
  }
  void SetReferrerPolicy(network::mojom::blink::ReferrerPolicy) override {}
  void AddContentSecurityPolicies(
      blink::Vector<network::mojom::blink::ContentSecurityPolicyPtr>) override {}

 private:
  mojo::AssociatedReceiver<blink::mojom::blink::PolicyContainerHost> receiver_{
      this};
};

class MbSharedWorkerInstance;

// Process-wide registry of live shared workers, keyed by url|name|type. A SharedWorker is
// shared across all documents that request the same key — the whole point of the API. All
// access is on the main thread (the connector runs there), so no lock is needed.
std::map<std::string, MbSharedWorkerInstance*>& SwRegistry() {
  static auto* m = new std::map<std::string, MbSharedWorkerInstance*>();
  return *m;
}

// One shared worker instance: owns the WebSharedWorker and IS its WebSharedWorkerClient.
// Multiple page connections (each its own SharedWorkerClient remote + MessagePort) attach
// to the single worker. Self-deletes (and deregisters) when the worker context is gone.
class MbSharedWorkerInstance final : public blink::WebSharedWorkerClient {
 public:
  explicit MbSharedWorkerInstance(std::string key) : key_(std::move(key)) {}

  // Create + start the worker. Returns false on script-load failure.
  bool Start(const blink::mojom::blink::SharedWorkerInfoPtr& info) {
    auto params = MakeWorkerMainScriptParams(info->url.GetString().Utf8());
    if (!params)
      return false;

    blink::WebSecurityOrigin origin =
        blink::WebSecurityOrigin::Create(blink::WebURL(info->url));
    // Scope this shared worker's per-origin storage (IndexedDB) by its origin,
    // published under a synthetic worker frame_key, so same-origin documents +
    // the worker share IDB and cross-origin ones are isolated.
    worker_frame_key_ = MbAllocWorkerFrameKey();
    MbSetFrameOrigin(worker_frame_key_, origin.ToString().Utf8());

    mojo::PendingRemote<blink::mojom::WorkerContentSettingsProxy> content_settings;
    mojo::MakeSelfOwnedReceiver(
        std::make_unique<MbWorkerContentSettingsProxy>(),
        content_settings.InitWithNewPipeAndPassReceiver());
    mojo::PendingRemote<blink::mojom::SharedWorkerHost> host;
    mojo::MakeSelfOwnedReceiver(std::make_unique<MbSharedWorkerHost>(),
                                host.InitWithNewPipeAndPassReceiver());

    auto fetch_context = base::MakeRefCounted<MbWorkerFetchContext>(
        MbDefaultUserAgent(), std::string(), origin.ToString().Utf8());

    worker_ = blink::WebSharedWorker::CreateAndStart(
        blink::SharedWorkerToken(), blink::WebURL(info->url),
        static_cast<blink::mojom::ScriptType>(info->options->type),
        info->options->credentials, blink::WebString(info->options->name), origin,
        origin, /*is_constructor_secure_context=*/false,
        blink::WebString::FromUtf8(MbDefaultUserAgent()),
        mb::MbDefaultUserAgentMetadata(),
        std::vector<blink::WebContentSecurityPolicy>(),
        blink::WebFetchClientSettingsObject(
            blink::WebPolicyContainerPolicies(), blink::WebURL(),
            blink::mojom::InsecureRequestsPolicy::kDoNotUpgrade),
        base::UnguessableToken::Create(), std::move(content_settings),
        MakeFrameInterfaceBroker(worker_frame_key_),
        /*pause_worker_context_on_start=*/false,
        std::move(params),
        std::make_unique<blink::WebPolicyContainer>(
            blink::WebPolicyContainerPolicies(), policy_host_.BindRemote()),
        std::move(fetch_context), std::move(host), this, ukm::kInvalidSourceId,
        /*require_cross_site_request_for_cookies=*/false, mojo::NullReceiver(),
        mojo::NullReceiver(), /*is_cross_origin_isolated=*/false);
    return true;
  }

  // Attach one page connection: notify its client and deliver its MessagePort to the
  // worker's `onconnect`. Each call is a distinct connect event to the SAME worker.
  void AddClient(
      mojo::PendingRemote<blink::mojom::blink::SharedWorkerClient> client,
      blink::mojom::blink::SharedWorkerCreationContextType ctx_type,
      blink::MessagePortDescriptor message_port) {
    mojo::Remote<blink::mojom::blink::SharedWorkerClient> c(std::move(client));
    c->OnCreated(ctx_type);
    worker_->Connect(next_connection_id_++, std::move(message_port));
    c->OnConnected({});
    // Track the client so we can EVICT the worker when the last one disconnects
    // (a page navigating away / closing drops its client) — the spec lifetime of
    // a SharedWorker. RemoteSet removes the entry + calls OnClientGone on drop.
    clients_.set_disconnect_handler(base::BindRepeating(
        &MbSharedWorkerInstance::OnClientGone, base::Unretained(this)));
    clients_.Add(std::move(c));  // keep the page client alive
  }

  // A page client disconnected (already removed from the set). If it was the
  // last, terminate the worker context. Deregister EAGERLY here (before the async
  // WorkerContextDestroyed) so a concurrent new SharedWorker(sameUrl) Connect can't
  // reuse this dying instance — its AddClient would be lost when WorkerContextDestroyed
  // deletes us. A fresh instance is created instead.
  void OnClientGone(mojo::RemoteSetElementId /*id*/) {
    if (clients_.empty() && worker_ && !terminating_) {
      terminating_ = true;
      auto& reg = SwRegistry();
      auto it = reg.find(key_);
      if (it != reg.end() && it->second == this)
        reg.erase(it);
      worker_->TerminateWorkerContext();
    }
  }

  // blink::WebSharedWorkerClient:
  void WorkerContextDestroyed() override {
    if (worker_frame_key_)
      MbClearFrameOrigin(worker_frame_key_);  // forget the worker's origin entry
    // Usually already deregistered at termination start; only erase if the registry
    // still maps this key to us, so we never clobber a fresh same-key instance.
    auto& reg = SwRegistry();
    auto it = reg.find(key_);
    if (it != reg.end() && it->second == this)
      reg.erase(it);
    delete this;
  }

 private:
  std::string key_;
  MbSwPolicyContainerHost policy_host_;  // must outlive the worker (bound remote)
  std::unique_ptr<blink::WebSharedWorker> worker_;
  mojo::RemoteSet<blink::mojom::blink::SharedWorkerClient> clients_;
  int next_connection_id_ = 0;
  uint64_t worker_frame_key_ = 0;  // this worker's origin-map key (0 = unset)
  bool terminating_ = false;  // last client gone; deregistered, awaiting destruction
};

class MbSharedWorkerConnector
    : public blink::mojom::blink::SharedWorkerConnector {
 public:
  void Connect(
      blink::mojom::blink::SharedWorkerInfoPtr info,
      mojo::PendingRemote<blink::mojom::blink::SharedWorkerClient> client,
      blink::mojom::blink::SharedWorkerCreationContextType creation_context_type,
      blink::MessagePortDescriptor message_port,
      mojo::PendingRemote<blink::mojom::blink::BlobURLToken> /*blob_url_token*/)
      override {
    // The key identifies a SHARED worker: same url+name+type -> same instance.
    const std::string key =
        info->url.GetString().Utf8() + "\n" + info->options->name.Utf8() + "\n" +
        base::NumberToString(static_cast<int>(info->options->type));
    auto& reg = SwRegistry();
    auto it = reg.find(key);
    MbSharedWorkerInstance* inst;
    if (it != reg.end()) {
      inst = it->second;  // reuse the running worker
    } else {
      inst = new MbSharedWorkerInstance(key);
      if (!inst->Start(info)) {
        mojo::Remote<blink::mojom::blink::SharedWorkerClient> c(std::move(client));
        c->OnCreated(creation_context_type);
        c->OnScriptLoadFailed("shared worker script failed to load");
        delete inst;
        return;
      }
      reg[key] = inst;  // instance self-deregisters on WorkerContextDestroyed
    }
    inst->AddClient(std::move(client), creation_context_type,
                    std::move(message_port));
  }
};

}  // namespace

void BindSharedWorkerConnector(
    mojo::PendingReceiver<blink::mojom::blink::SharedWorkerConnector> receiver) {
  mojo::MakeSelfOwnedReceiver(std::make_unique<MbSharedWorkerConnector>(),
                              std::move(receiver));
}

}  // namespace mb
