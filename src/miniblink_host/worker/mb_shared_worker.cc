#include "miniblink_host/worker/mb_shared_worker.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/unguessable_token.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "mojo/public/cpp/bindings/pending_associated_remote.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/remote.h"
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

// One connection's worth of state: owns the WebSharedWorker and the page client remote, and
// IS the WebSharedWorkerClient. Self-deletes when the worker context is destroyed.
class MbSharedWorkerConnection final : public blink::WebSharedWorkerClient {
 public:
  void Start(blink::mojom::blink::SharedWorkerInfoPtr info,
             mojo::PendingRemote<blink::mojom::blink::SharedWorkerClient> client,
             blink::mojom::blink::SharedWorkerCreationContextType ctx_type,
             blink::MessagePortDescriptor message_port) {
    client_.Bind(std::move(client));
    client_->OnCreated(ctx_type);

    auto params = MakeWorkerMainScriptParams(info->url.GetString().Utf8());
    if (!params) {
      client_->OnScriptLoadFailed("shared worker script failed to load");
      delete this;
      return;
    }

    blink::WebSecurityOrigin origin =
        blink::WebSecurityOrigin::Create(blink::WebURL(info->url));

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
        blink::UserAgentMetadata(),
        std::vector<blink::WebContentSecurityPolicy>(),
        blink::WebFetchClientSettingsObject(
            blink::WebPolicyContainerPolicies(), blink::WebURL(),
            blink::mojom::InsecureRequestsPolicy::kDoNotUpgrade),
        base::UnguessableToken::Create(), std::move(content_settings),
        MakeFrameInterfaceBroker(), /*pause_worker_context_on_start=*/false,
        std::move(params),
        std::make_unique<blink::WebPolicyContainer>(
            blink::WebPolicyContainerPolicies(), policy_host_.BindRemote()),
        std::move(fetch_context), std::move(host), this, ukm::kInvalidSourceId,
        /*require_cross_site_request_for_cookies=*/false, mojo::NullReceiver(),
        mojo::NullReceiver(), /*is_cross_origin_isolated=*/false);

    worker_->Connect(/*connection_request_id=*/0, std::move(message_port));
    client_->OnConnected({});
  }

  // blink::WebSharedWorkerClient:
  void WorkerContextDestroyed() override { delete this; }

 private:
  MbSwPolicyContainerHost policy_host_;  // must outlive the worker (bound remote)
  std::unique_ptr<blink::WebSharedWorker> worker_;
  mojo::Remote<blink::mojom::blink::SharedWorkerClient> client_;
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
    // Self-managed: lives until its worker context is destroyed.
    (new MbSharedWorkerConnection())
        ->Start(std::move(info), std::move(client), creation_context_type,
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
