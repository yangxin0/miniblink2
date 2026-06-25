// mb_frame_client.cc — blink::WebLocalFrameClient. Status: Phase 2.
#include "miniblink_host/frame/mb_frame_client.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/single_thread_task_runner.h"
#include "base/unguessable_token.h"
#include "third_party/blink/public/platform/task_type.h"
#include "miniblink_host/blob/mb_blob_registry.h"
#include "miniblink_host/loader/mb_url_loader.h"
#include "miniblink_host/view/mb_webview.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "mojo/public/cpp/bindings/pending_associated_remote.h"
#include "services/network/public/cpp/web_sandbox_flags.h"
#include "services/network/public/mojom/web_sandbox_flags.mojom-shared.h"
#include "third_party/blink/public/common/loader/http_body_element_type.h"
#include "third_party/blink/public/common/tokens/tokens.h"
#include "third_party/blink/public/platform/web_http_body.h"
#include "third_party/blink/public/platform/web_url_request.h"
#include "third_party/blink/public/mojom/frame/policy_container.mojom-blink.h"
#include "third_party/blink/public/platform/web_policy_container.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_history_commit_type.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_navigation_params.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/renderer/core/html/html_frame_owner_element.h"
#include "third_party/blink/renderer/core/html_names.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"

namespace mb {

MbFrameClient::MbFrameClient(MbWebView* owner) : owner_(owner) {}
MbFrameClient::~MbFrameClient() {
  delete nav_assoc_interfaces_;
}

blink::AssociatedInterfaceProvider*
MbFrameClient::GetRemoteNavigationAssociatedInterfaces() {
  if (!nav_assoc_interfaces_)
    nav_assoc_interfaces_ = MakeBlobUrlNavAssociatedInterfaces();
  return nav_assoc_interfaces_;
}

blink::WebLocalFrame* MbFrameClient::CreateChildFrame(
    blink::mojom::TreeScopeType scope,
    const blink::WebString& /*name*/,
    const blink::WebString& /*fallback_name*/,
    const blink::FramePolicy& frame_policy,
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
  // Carry the owner's sandbox flags (<iframe sandbox>) onto the child client so
  // its commit enforces them — normally the browser computes these and ships
  // them in the WebNavigationParams; here we apply them in BeginNavigation.
  child_ptr->SetSandboxFlags(frame_policy.sandbox_flags);
  child_ptr->Bind(child, std::move(child_client));
  finish_creation(child, blink::DocumentToken(), /*browser_broker=*/{},
                  std::make_unique<base::UnguessableToken>(
                      base::UnguessableToken::Create()));
  return child;
}

void MbFrameClient::BeginNavigation(
    std::unique_ptr<blink::WebNavigationInfo> info) {
  if (!web_frame_)
    return;
  if (self_owned_) {
    // Child frame: BeginNavigation fires during the parent's parse (not from JS
    // event handling), so committing synchronously is safe — and is what the
    // iframe path has always done.
    DoCommit(std::move(info));
    return;
  }
  // Main frame: a page-initiated navigation (a link click, location= assignment,
  // or form submit). The *initial* document is committed by MbWebView
  // (CommitHtml), not here, so only handle real document navigations and leave
  // about:* / empty alone. Commit on a task rather than synchronously: we are
  // called from inside JS / event handling, where re-entrantly committing a new
  // document is unsafe. This mirrors frame_test_helpers, which also posts it.
  blink::KURL url = info->url_request.Url();
  if (url.IsEmpty() || url.ProtocolIsAbout())
    return;
  // Navigation policy: let the host veto a page-initiated navigation (link click,
  // location= assignment, form submit, JS redirect) before it commits — block
  // popups/redirects/leaving the page. Host-driven loads (MbWebView::LoadURL) don't
  // come through here, so they are never vetoed by this.
  if (owner_ && !owner_->OnBeginNavigation(url.GetString().Utf8()))
    return;
  web_frame_->GetTaskRunner(blink::TaskType::kInternalLoading)
      ->PostTask(FROM_HERE,
                 base::BindOnce(&MbFrameClient::DoCommit,
                                weak_factory_.GetWeakPtr(), std::move(info)));
}

blink::WebView* MbFrameClient::CreateNewWindow(
    const blink::WebURLRequest& request, const blink::WebWindowFeatures&,
    const blink::WebString& name, const gfx::Rect&, blink::WebNavigationPolicy,
    network::mojom::WebSandboxFlags, const blink::SessionStorageNamespaceId&,
    bool& /*consumed_user_gesture*/, const std::optional<blink::Impression>&,
    const std::optional<blink::WebPictureInPictureWindowOptions>&,
    const blink::WebURL& /*base_url*/) {
  // Notify the host that the page tried to open a new window (window.open /
  // target=_blank). We return null (deny the auto-popup — the embedder owns view
  // creation) but surface the URL so it can react (e.g. load it somewhere).
  if (owner_)
    owner_->OnCreateNewWindow(request.Url().GetString().Utf8(), name.Utf8());
  return nullptr;
}

void MbFrameClient::DoCommit(std::unique_ptr<blink::WebNavigationInfo> info) {
  if (!web_frame_)
    return;
  auto params = blink::WebNavigationParams::CreateFromInfo(*info);
  blink::KURL url = info->url_request.Url();
  std::string body;  // CommitNavigation requires a body_loader (even for srcdoc)
  std::string mime = "text/html";
  // Child-frame charset: default UTF-8 (srcdoc body is extracted as UTF-8; safe for the
  // common case), but honor an explicit charset= from the fetched Content-Type so a
  // non-UTF-8 iframe (Shift-JIS, GBK, ...) decodes correctly instead of mojibake.
  std::string charset = "UTF-8";
  if (url.IsAboutSrcdocUrl()) {
    params->fallback_base_url = info->requestor_base_url;
    // The srcdoc text lives on the owner element, not in WebNavigationInfo.
    auto* impl = blink::To<blink::WebLocalFrameImpl>(web_frame_);
    if (blink::LocalFrame* lf = impl->GetFrame()) {
      if (lf->Owner()) {
        body = blink::To<blink::HTMLFrameOwnerElement>(lf->Owner())
                   ->FastGetAttribute(blink::html_names::kSrcdocAttr)
                   .Utf8();
      }
    }
  } else if (!url.IsEmpty() && !url.ProtocolIsAbout()) {
    // src=file/http/data, link click, location=, or form submit: fetch the body
    // via the same loader subresources use. For an HTTP POST (form submission)
    // pull the request body + content-type off the navigation and POST them.
    std::string content_type;
    std::string post_body, post_ct;
    if (info->url_request.HttpMethod().Utf8() == "POST") {
      blink::WebHTTPBody http_body = info->url_request.HttpBody();
      if (!http_body.IsNull()) {
        for (size_t i = 0; i < http_body.ElementCount(); ++i) {
          blink::WebHTTPBody::Element el;
          if (http_body.ElementAt(i, el) &&
              el.type == blink::HTTPBodyElementType::kTypeData) {
            std::vector<uint8_t> bytes = el.data.Copy();
            post_body.append(reinterpret_cast<const char*>(bytes.data()),
                             bytes.size());
          }
        }
      }
      post_ct = info->url_request.HttpContentType().Utf8();
    }
    // Honor a registered mock (mbMockResponse) for a page navigation too — consistent
    // with subresource/fetch interception — so a navigation can be served offline.
    std::string mock_body, mock_ct;
    int mock_status = 0;
    if (MbFindMock(url.GetString().Utf8(), &mock_body, &mock_ct, &mock_status)) {
      body = std::move(mock_body);
      content_type = mock_ct;
    } else {
      MbFetchUrl(url.GetString().Utf8(), &body, &content_type, user_agent_,
                 extra_headers_, post_body, post_ct);
    }
    if (!content_type.empty()) {
      std::string m = content_type.substr(0, content_type.find(';'));
      while (!m.empty() && m.back() == ' ')
        m.pop_back();
      if (!m.empty())
        mime = m;
      // Pull an explicit charset= off the Content-Type (authoritative). Absent ->
      // keep UTF-8 (no regression for the existing UTF-8/data: iframe cases).
      std::string lc = content_type;
      for (char& c : lc)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (auto p = lc.find("charset="); p != std::string::npos) {
        p += 8;
        std::string::size_type end = content_type.find_first_of("; \t", p);
        std::string cs = content_type.substr(
            p, end == std::string::npos ? end : end - p);
        if (!cs.empty())
          charset = cs;
      }
    }
  }
  // (about:blank / empty children commit an empty document — correct.)
  blink::WebNavigationParams::FillStaticResponse(
      params.get(), blink::WebString::FromUtf8(mime),
      blink::WebString::FromUtf8(charset),
      base::span<const char>(body.data(), body.size()));
  // CommitNavigation requires a policy container for non-empty documents.
  MbPolicyContainerHost policy_host;
  params->policy_container = std::make_unique<blink::WebPolicyContainer>(
      blink::WebPolicyContainerPolicies(), policy_host.BindRemote());
  // Enforce the owner's sandbox flags (<iframe sandbox>). Normally the browser
  // folds these into the committed policy container; we do it here. If the
  // kOrigin bit is set (no allow-same-origin), force a fresh opaque origin so
  // the child is cross-origin to its parent — mirrors frame_test_helpers.
  params->policy_container->policies.sandbox_flags |= sandbox_flags_;
  if ((params->policy_container->policies.sandbox_flags &
       network::mojom::WebSandboxFlags::kOrigin) !=
      network::mojom::WebSandboxFlags::kNone) {
    params->origin_to_commit =
        blink::SecurityOrigin::Create(url)->DeriveNewOpaqueOrigin();
  }
  blink::To<blink::WebLocalFrameImpl>(web_frame_)->CommitNavigation(
      std::move(params), /*extra_data=*/nullptr);
}

void MbFrameClient::DidCommitNavigation(
    blink::WebHistoryCommitType commit_type,
    bool /*should_reset_browser_interface_broker*/,
    const network::ParsedPermissionsPolicy& /*permissions_policy_header*/,
    const blink::DocumentPolicyFeatureState& /*document_policy_header*/) {
  // Only the main frame feeds the view's history (child/iframe commits don't).
  if (self_owned_ || !web_frame_ || !owner_)
    return;
  owner_->OnDidCommitMainFrame(
      web_frame_->GetDocument().Url().GetString().Utf8(),
      commit_type == blink::kWebStandardCommit);
}

void MbFrameClient::DidFinishLoad() {
  // Main frame only — child/iframe load-finishes don't signal the page's completion.
  if (self_owned_ || !owner_)
    return;
  owner_->OnDidFinishLoad();
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
  return blink::WebString::FromUtf8(EffectiveUserAgent());
}

const std::string& MbFrameClient::EffectiveUserAgent() const {
  return user_agent_.empty() ? MbDefaultUserAgent() : user_agent_;
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
  if (owner_)
    owner_->OnConsoleMessage(level, msg.text.Utf8());  // live push (vs. DrainConsole poll)
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
