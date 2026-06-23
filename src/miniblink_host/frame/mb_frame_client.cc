// mb_frame_client.cc — blink::WebLocalFrameClient. Status: Phase 2.
#include "miniblink_host/frame/mb_frame_client.h"

#include <memory>
#include <utility>

#include "base/unguessable_token.h"
#include "miniblink_host/loader/mb_url_loader.h"
#include "miniblink_host/view/mb_webview.h"
#include "third_party/blink/public/common/tokens/tokens.h"
#include "third_party/blink/public/web/web_local_frame.h"

namespace mb {

MbFrameClient::MbFrameClient(MbWebView* owner) : owner_(owner) {}
MbFrameClient::~MbFrameClient() = default;

blink::WebLocalFrame* MbFrameClient::CreateChildFrame(
    blink::mojom::TreeScopeType scope,
    const blink::WebString& /*name*/,
    const blink::WebString& /*fallback_name*/,
    const blink::FramePolicy& /*frame_policy*/,
    const blink::WebFrameOwnerProperties&,
    blink::FrameOwnerElementType,
    blink::WebPolicyContainerBindParams /*policy_container_bind_params*/,
    ukm::SourceId /*document_ukm_source_id*/,
    FinishChildFrameCreationFn finish_creation) {
  if (!web_frame_)
    return nullptr;
  // The PolicyContainerHost receiver (browser-side, advisory CSP/referrer) is
  // left unbound — its renderer-side calls just no-op; content still loads.
  // Create a real local child frame with its own (self-owned) client.
  auto child_client = std::make_unique<MbFrameClient>(owner_);
  MbFrameClient* child_ptr = child_client.get();
  blink::WebLocalFrame* child = web_frame_->CreateLocalChild(
      scope, child_ptr, /*interface_registry=*/nullptr,
      blink::LocalFrameToken());
  child_ptr->Bind(child, std::move(child_client));
  finish_creation(child, blink::DocumentToken(), /*browser_broker=*/{},
                  std::make_unique<base::UnguessableToken>(
                      base::UnguessableToken::Create()));
  return child;
}

void MbFrameClient::FrameDetached(blink::DetachReason reason) {
  if (!self_owned_)
    return;  // main frame: MbWebView owns teardown
  if (web_frame_)
    web_frame_->Close(reason);
  self_owned_.reset();  // self-destruct — must be the last statement
}

void MbFrameClient::RunScriptsAtDocumentElementAvailable() {
  if (owner_)
    owner_->RunDocumentStartScript();
}

std::unique_ptr<blink::URLLoader> MbFrameClient::CreateURLLoaderForTesting() {
  // Subresources use the same UA + extra headers the top-level fetch does, so the
  // network identity is consistent across the document and its subresources.
  return std::make_unique<MbURLLoader>(
      user_agent_.empty() ? MbDefaultUserAgent() : user_agent_, extra_headers_);
}

blink::WebString MbFrameClient::UserAgentOverride() {
  return blink::WebString::FromUtf8(
      user_agent_.empty() ? MbDefaultUserAgent() : user_agent_);
}

void MbFrameClient::DidAddMessageToConsole(const blink::WebConsoleMessage& msg,
                                           const blink::WebString& /*source*/,
                                           unsigned /*line*/,
                                           const blink::WebString& /*stack*/) {
  const char* level = "log";
  switch (msg.level) {
    case blink::mojom::ConsoleMessageLevel::kVerbose: level = "verbose"; break;
    case blink::mojom::ConsoleMessageLevel::kInfo: level = "log"; break;
    case blink::mojom::ConsoleMessageLevel::kWarning: level = "warn"; break;
    case blink::mojom::ConsoleMessageLevel::kError: level = "error"; break;
  }
  console_.push_back(std::string(level) + ": " + msg.text.Utf8());
}

std::string MbFrameClient::DrainConsole() {
  std::string out;
  for (const auto& line : console_) {
    out += line;
    out += '\n';
  }
  console_.clear();
  return out;
}
// TODO(mb): DidStopLoading/DidMeaningfulLayout -> notify owner to paint;
// CreateChildFrame -> nullptr.
}  // namespace mb
