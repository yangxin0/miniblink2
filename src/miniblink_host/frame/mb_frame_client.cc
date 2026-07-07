// mb_frame_client.cc — blink::WebLocalFrameClient. Status: Phase 2.
#include "miniblink_host/frame/mb_frame_client.h"

#include "miniblink_host/session/mb_session.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/task/single_thread_task_runner.h"
#include "base/unguessable_token.h"
#include "third_party/blink/public/platform/task_type.h"
#include "miniblink_host/blob/mb_blob_registry.h"
#include "miniblink_host/frame/mb_frame_broker.h"
#include "miniblink_host/frame/mb_frame_origin.h"
#include "miniblink_host/frame/mb_local_frame_host.h"
#include "miniblink_host/loader/mb_url_loader.h"
#include "miniblink_host/media/mb_audio_player.h"
#include "miniblink_host/media/mb_media_player.h"
#include "third_party/blink/public/platform/media/web_media_player_builder.h"
#include "third_party/blink/public/platform/web_media_player_source.h"  // source.IsURL/GetAsURL
#include "miniblink_host/view/mb_webview.h"
#include "miniblink_host/worker/mb_worker_fetch_context.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "mojo/public/cpp/bindings/pending_associated_remote.h"
#include "net/base/schemeful_site.h"
#include "services/network/public/cpp/web_sandbox_flags.h"
#include "services/network/public/mojom/web_sandbox_flags.mojom-shared.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "third_party/blink/public/mojom/storage_key/ancestor_chain_bit.mojom-shared.h"
#include "third_party/blink/public/platform/web_security_origin.h"
#include "third_party/blink/public/web/web_frame.h"
#include "url/origin.h"
#include "third_party/blink/public/common/loader/http_body_element_type.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "third_party/blink/public/common/tokens/tokens.h"
#include "third_party/blink/public/platform/web_http_body.h"
#include "third_party/blink/public/platform/web_url_request.h"
#include "third_party/blink/public/mojom/frame/policy_container.mojom-blink.h"
#include "third_party/blink/public/platform/web_policy_container.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_history_commit_type.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_view.h"
#include "third_party/blink/public/web/web_navigation_params.h"
#include "third_party/blink/public/web/web_frame_load_type.h"
#include "third_party/blink/public/mojom/commit_result/commit_result.mojom-shared.h"
#include "third_party/blink/public/mojom/frame/triggering_event_info.mojom-blink.h"
#include "third_party/blink/renderer/core/frame/frame_types.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/renderer/core/loader/document_loader.h"
#include "third_party/blink/renderer/core/loader/frame_loader.h"
#include "third_party/blink/renderer/core/loader/history_item.h"
#include "third_party/blink/renderer/core/html/html_frame_owner_element.h"
#include "third_party/blink/renderer/core/html_names.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"

namespace mb {

namespace {
uint64_t NextFrameKey() {
  static uint64_t counter = 0;  // main-thread only (clients created on main thread)
  return ++counter;
}

// frame_key -> owning view, for broker-routed paths (shared worker connector)
// that carry a frame_key but no pointer. Main-thread only, like NextFrameKey.
// Leaked (no exit-time destructor); entries erased in ~MbFrameClient.
std::map<uint64_t, MbWebView*>& FrameKeyViews() {
  static auto* m = new std::map<uint64_t, MbWebView*>();
  return *m;
}
}  // namespace

MbWebView* MbViewForFrameKey(uint64_t frame_key) {
  auto& m = FrameKeyViews();
  auto it = m.find(frame_key);
  return it != m.end() ? it->second : nullptr;
}

MbWebView* MbViewForFrame(blink::LocalFrame* frame) {
  if (!frame)
    return nullptr;
  auto* web_frame = blink::WebLocalFrameImpl::FromFrame(frame);
  if (!web_frame)
    return nullptr;
  // Every local frame in this embedder is created with an MbFrameClient (main
  // frame in MbWebView, children in CreateChildFrame), so the cast is safe.
  auto* client = static_cast<MbFrameClient*>(web_frame->Client());
  return client ? client->owner() : nullptr;
}

MbFrameClient::MbFrameClient(MbWebView* owner)
    : owner_(owner), frame_key_(NextFrameKey()) {
  if (owner_)
    FrameKeyViews()[frame_key_] = owner_;
}
MbFrameClient::~MbFrameClient() {
  FrameKeyViews().erase(frame_key_);
  // Clear our history-traversal sink (the main frame registers via SetFrame,
  // child frames via CreateChildFrame's joint-history routing) so a stray
  // GoToEntryAtOffset doesn't post to a freed client. Erasing by frame_key is a
  // no-op if nothing was registered.
  MbClearHistoryGoToHandler(frame_key_);
  MbClearFrameOrigin(frame_key_);
  delete nav_assoc_interfaces_;
}

void MbFrameClient::SetFrame(blink::WebLocalFrame* frame) {
  web_frame_ = frame;
  // Main frame only: route page-driven history.back()/forward()/go() (which
  // blink sends to LocalFrameHost.GoToEntryAtOffset) back to this client on the
  // current (main/blink) thread, where GoToHistoryOffset replays the entry.
  MbSetHistoryGoToHandler(
      frame_key_, base::SingleThreadTaskRunner::GetCurrentDefault(),
      base::BindRepeating(&MbFrameClient::GoToHistoryOffset,
                          weak_factory_.GetWeakPtr()),
      base::BindRepeating(&MbFrameClient::GoToHistoryKey,
                          weak_factory_.GetWeakPtr()),
      base::BindRepeating(&MbFrameClient::OnFaviconUrls,
                          weak_factory_.GetWeakPtr()),
      base::BindRepeating(&MbFrameClient::OnPageDownload,
                          weak_factory_.GetWeakPtr()),
      base::BindRepeating(&MbFrameClient::OnPageDownloadFetch,
                          weak_factory_.GetWeakPtr()));
  // window.close() (LocalMainFrameHost::RequestClose) — registered after the
  // history sink so it lands on the same entry/runner.
  MbSetRequestCloseHandler(
      frame_key_, base::BindRepeating(&MbFrameClient::OnRequestClose,
                                      weak_factory_.GetWeakPtr()));
}

void MbFrameClient::OnRequestClose() {
  if (!self_owned_ && owner_)
    owner_->OnRequestClose();
}

void MbFrameClient::OnPageDownload(const std::string& url,
                                   const std::string& suggested_name,
                                   const std::string& body) {
  // A page-initiated blob download resolved by MbLocalFrameHost, hopped to this
  // (main) thread. Hand it to the view's download callback.
  if (!self_owned_ && owner_)
    owner_->OnPageDownload(url, suggested_name, body);
}

void MbFrameClient::OnPageDownloadFetch(const std::string& url,
                                        const std::string& suggested_name) {
  // A page-initiated data:/http(s) download routed by MbLocalFrameHost. The view
  // fetches the bytes through the engine and fires the download callback.
  if (!self_owned_ && owner_)
    owner_->OnPageDownloadFetch(url, suggested_name);
}

void MbFrameClient::OnFaviconUrls(const std::string& favicon_urls) {
  if (!self_owned_ && owner_)
    owner_->OnFaviconChanged(favicon_urls);
}

blink::AssociatedInterfaceProvider*
MbFrameClient::GetRemoteNavigationAssociatedInterfaces() {
  if (!nav_assoc_interfaces_)
    nav_assoc_interfaces_ = MakeBlobUrlNavAssociatedInterfaces(frame_key_);
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
  // Route the child's page-driven history traversal (blink delivers it to the
  // child's LocalFrameHost.GoToEntryAtOffset, keyed by the child's frame_key) to
  // the MAIN frame's JOINT session history. The main frame registers its own sink
  // via SetFrame; children forward to it. favicon/download sinks stay empty for
  // children (unchanged behavior). The child's dtor clears this by frame_key.
  MbSetHistoryGoToHandler(
      child_ptr->frame_key(),
      base::SingleThreadTaskRunner::GetCurrentDefault(),
      base::BindRepeating(&MbFrameClient::ForwardHistoryToMainOffset,
                          child_ptr->weak_factory_.GetWeakPtr()),
      base::BindRepeating(&MbFrameClient::ForwardHistoryToMainKey,
                          child_ptr->weak_factory_.GetWeakPtr()),
      base::DoNothing(), base::DoNothing(), base::DoNothing());
  // Give the child frame its OWN BrowserInterfaceBroker (scoped to its frame_key -> origin, set
  // on DidCommitNavigation), like the main frame — without it every broker-backed API
  // (storage / locks / permissions / geolocation) HANGS in an iframe (the request is dropped).
  finish_creation(child, blink::DocumentToken(),
                  MakeFrameInterfaceBroker(child_ptr->frame_key()),
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
    if (MbFindMock(url.GetString().Utf8(), &mock_body, &mock_ct, &mock_status,
                   owner_)) {
      body = std::move(mock_body);
      content_type = mock_ct;
    } else {
      MbFetchUrl(url.GetString().Utf8(), &body, &content_type, user_agent_,
                 extra_headers_, post_body, post_ct, /*http_method=*/"",
                 /*out_final_url=*/nullptr, /*out_status=*/nullptr,
                 /*out_headers=*/nullptr, /*out_error=*/nullptr, owner_);
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
  // Third-party storage partitioning: compute the child frame's StorageKey so a
  // cross-site iframe gets a top-level-site-partitioned key (matching the
  // broker-scoped backends). Without this blink defaults the key to first-party
  // and the iframe's localStorage/IndexedDB would be SHARED across embedding
  // sites. Only meaningful for non-opaque child frames; the main frame and
  // opaque/sandboxed children keep the document loader's first-party default.
  const bool opaque_sandbox =
      (params->policy_container->policies.sandbox_flags &
       network::mojom::WebSandboxFlags::kOrigin) !=
      network::mojom::WebSandboxFlags::kNone;
  if (web_frame_ && web_frame_->Parent() && !opaque_sandbox) {
    url::Origin child_origin = blink::WebSecurityOrigin::Create(url);
    blink::WebFrame* top = web_frame_->Top();
    url::Origin top_origin =
        top ? url::Origin(top->GetSecurityOrigin()) : child_origin;
    if (!child_origin.opaque() && !top_origin.opaque()) {
      net::SchemefulSite top_site(top_origin);
      bool cross_site = net::SchemefulSite(child_origin) != top_site;
      // kCrossSite if any ancestor below the top is itself cross-site with the
      // top (already-committed ancestors expose their security origin).
      for (blink::WebFrame* f = web_frame_->Parent(); f && f != top && !cross_site;
           f = f->Parent()) {
        if (net::SchemefulSite(url::Origin(f->GetSecurityOrigin())) != top_site)
          cross_site = true;
      }
      params->storage_key = blink::StorageKey::Create(
          child_origin, top_site,
          cross_site ? blink::mojom::AncestorChainBit::kCrossSite
                     : blink::mojom::AncestorChainBit::kSameSite);
    }
  }
  blink::To<blink::WebLocalFrameImpl>(web_frame_)->CommitNavigation(
      std::move(params), /*extra_data=*/nullptr);
}

void MbFrameClient::DidCommitNavigation(
    blink::WebHistoryCommitType commit_type,
    bool /*should_reset_browser_interface_broker*/,
    const network::ParsedPermissionsPolicy& /*permissions_policy_header*/,
    const blink::DocumentPolicyFeatureState& /*document_policy_header*/) {
  // Publish this frame's storage scope (main AND child frames) so origin-keyed services
  // (IndexedDB / Cache / DOM-storage / locks / BroadcastChannel) partition correctly.
  // THIRD-PARTY STORAGE PARTITIONING: a frame's storage key is its own origin PLUS the
  // top-level origin when they differ — so a third-party iframe (e.g. widget.com embedded in
  // a.com vs b.com) gets ISOLATED storage per embedding site, while a top frame and a
  // same-origin (first-party) iframe key by the bare origin (Top()'s origin == ours, so the
  // partition suffix is skipped — first-party storage is unchanged + still shared).
  if (web_frame_) {
    std::string origin = web_frame_->GetSecurityOrigin().ToString().Utf8();
    std::string top_origin =
        web_frame_->Top()->GetSecurityOrigin().ToString().Utf8();
    std::string scope =
        (top_origin == origin) ? origin : (origin + "\x1f""3p""\x1f" + top_origin);
    // SESSION PARTITIONING (IMPROVEMENT.md item 12): the owning view's session id
    // prefixes the scope, so every origin-keyed service (DOM storage, IDB,
    // OPFS, buckets, locks) isolates per profile. All views share Default()
    // unless rebound, which keys identically to the pre-session world modulo
    // the constant prefix.
    if (owner_ && owner_->session())
      scope = owner_->session()->id() + "\x1f" + scope;
    MbSetFrameOrigin(frame_key_, scope);
  }
  // Only the main frame feeds the view's history (child/iframe commits don't).
  if (self_owned_ || !web_frame_ || !owner_)
    return;
  owner_->OnDidCommitMainFrame(
      web_frame_->GetDocument().Url().GetString().Utf8(),
      commit_type == blink::kWebStandardCommit);
  // A page-driven cross-document traversal (GoToHistoryTarget -> owner LoadURL) is a MOVE,
  // not a new entry — the index is already at the target. Skip the record so it doesn't
  // double-grow this list (owner_->set_page_history_nav suppressed the view's append).
  if (suppress_history_record_)
    return;
  // Capture this (cross-document) entry for page-driven history traversal.
  if (commit_type == blink::kWebStandardCommit)
    RecordHistoryCommit(/*is_standard=*/true);
  else if (commit_type == blink::kWebHistoryInertCommit)
    RecordHistoryCommit(/*is_standard=*/false);
}

void MbFrameClient::DidFinishSameDocumentNavigation(
    blink::WebHistoryCommitType commit_type,
    bool /*is_synchronously_committed*/,
    blink::mojom::SameDocumentNavigationType /*type*/,
    bool /*is_client_redirect*/,
    const std::optional<blink::SameDocNavigationScreenshotDestinationToken>&
    /*screenshot_destination*/,
    base::UnguessableToken /*same_document_metrics_token*/,
    bool /*caused_by_ad*/) {
  // A standard same-document commit (pushState, a new fragment entry) appends a session-history
  // entry — advance blink's history index/length, matching RenderFrameImpl. replaceState/reload
  // come through as kWebHistoryInertCommit and must NOT advance.
  if (self_owned_ || !web_frame_)
    return;
  if (commit_type == blink::kWebStandardCommit) {
    // pushState: a new session-history entry. RecordHistoryCommit appends and
    // restates blink's index+length (history.length, back/forward gating).
    RecordHistoryCommit(/*is_standard=*/true);
  } else if (commit_type == blink::kWebHistoryInertCommit) {
    // replaceState: overwrite the current entry without growing the list.
    RecordHistoryCommit(/*is_standard=*/false);
  }
  // kWebBackForwardCommit (our own traversal): index already updated; no record.
}

void MbFrameClient::RecordHistoryCommit(bool is_standard) {
  if (self_owned_ || !web_frame_)
    return;
  auto* impl = blink::To<blink::WebLocalFrameImpl>(web_frame_);
  blink::LocalFrame* frame = impl ? impl->GetFrame() : nullptr;
  if (!frame || !frame->Loader().GetDocumentLoader())
    return;
  blink::HistoryItem* item =
      frame->Loader().GetDocumentLoader()->GetHistoryItem();
  if (!item)
    return;
  if (history_items_.empty()) {
    history_items_.emplace_back(item);
    history_index_ = 0;
  } else if (is_standard) {
    // A new entry supersedes any forward history, then is appended as current.
    history_items_.resize(history_index_ + 1);
    history_items_.emplace_back(item);
    history_index_ = static_cast<int>(history_items_.size()) - 1;
  } else if (history_index_ >= 0 &&
             history_index_ < static_cast<int>(history_items_.size())) {
    // replaceState / reload: overwrite the current entry in place.
    history_items_[history_index_] = item;
  }
  // Cap the session history like blink (kMaxSessionHistoryEntries == 50): drop
  // the oldest entries, shifting the current index down. blink's WebView CHECKs
  // history_length <= 50, so our list must not exceed it either.
  constexpr int kMaxHistoryEntries = 50;
  while (static_cast<int>(history_items_.size()) > kMaxHistoryEntries) {
    history_items_.erase(history_items_.begin());
    if (history_index_ > 0)
      --history_index_;
  }
  SyncBlinkHistoryCursor();
}

void MbFrameClient::SyncBlinkHistoryCursor() {
  // Our history_items_ is the source of truth for the page's session history.
  // blink's WebView keeps its own index/length (used to gate NavigateBackForward
  // and to answer history.length), but our cross-document commits reset it to 0,
  // so we restate it from our list after every change. Without this the two
  // desync and back/forward get wrongly short-circuited.
  if (blink::WebView* view = web_frame_ ? web_frame_->View() : nullptr) {
    view->SetHistoryListFromNavigation(
        history_index_, static_cast<int>(history_items_.size()));
  }
}

void MbFrameClient::EndHostHistoryTraversal(const std::string& committed_url) {
  suppress_history_record_ = false;
  if (committed_url.empty())
    return;  // the host traversal never committed — nothing to realign
  // Move our cursor to the page-history entry whose URL matches the one the host
  // traversed to, so blink's index (history.length / back-forward gating) and a
  // subsequent page-driven history.back()/forward() continue from the right place.
  for (size_t i = 0; i < history_items_.size(); ++i) {
    if (history_items_[i] &&
        history_items_[i]->Url().GetString().Utf8() == committed_url) {
      history_index_ = static_cast<int>(i);
      break;
    }
  }
  SyncBlinkHistoryCursor();
}

void MbFrameClient::GoToHistoryOffset(int offset, bool has_user_gesture) {
  if (offset == 0)
    return;
  GoToHistoryTarget(history_index_ + offset, has_user_gesture);
}

void MbFrameClient::GoToHistoryKey(const std::string& key,
                                  bool has_user_gesture) {
  // The Navigation API identifies the target entry by its key. Map it back to a
  // position in our session-history list, then traverse like history.go().
  for (size_t i = 0; i < history_items_.size(); ++i) {
    if (history_items_[i] && history_items_[i]->GetNavigationApiKey().Utf8() == key) {
      GoToHistoryTarget(static_cast<int>(i), has_user_gesture);
      return;
    }
  }
}

void MbFrameClient::ForwardHistoryToMainOffset(int offset, bool has_user_gesture) {
  // window.history is joint across the browsing context: an iframe's history.go()
  // traverses the MAIN frame's session history, not the child's own list.
  if (owner_ && owner_->main_frame_client() &&
      owner_->main_frame_client() != this)
    owner_->main_frame_client()->GoToHistoryOffset(offset, has_user_gesture);
}

void MbFrameClient::ForwardHistoryToMainKey(const std::string& key,
                                            bool has_user_gesture) {
  if (owner_ && owner_->main_frame_client() &&
      owner_->main_frame_client() != this)
    owner_->main_frame_client()->GoToHistoryKey(key, has_user_gesture);
}

void MbFrameClient::GoToHistoryTarget(int target, bool has_user_gesture) {
  if (self_owned_ || !web_frame_)
    return;
  if (target < 0 || target >= static_cast<int>(history_items_.size()) ||
      target == history_index_)
    return;
  blink::HistoryItem* item = history_items_[target].Get();
  if (!item)
    return;
  auto* impl = blink::To<blink::WebLocalFrameImpl>(web_frame_);
  blink::LocalFrame* frame = impl ? impl->GetFrame() : nullptr;
  if (!frame || !frame->Loader().GetDocumentLoader())
    return;
  // Try a same-document traversal first: this restores the entry's state object
  // and fires popstate (the whole point of page-driven back/forward in an SPA).
  // blink returns Ok if the target really is same-document; otherwise it signals
  // a cross-document restart and we re-navigate to the entry's URL.
  blink::mojom::CommitResult result =
      frame->Loader().GetDocumentLoader()->CommitSameDocumentNavigation(
          item->Url(), blink::WebFrameLoadType::kBackForward, item,
          blink::ClientRedirectPolicy::kNotClientRedirect, has_user_gesture,
          /*initiator_origin=*/nullptr, /*is_synchronously_committed=*/false,
          /*source_element=*/nullptr,
          blink::mojom::blink::TriggeringEventInfo::kNotFromEvent,
          /*is_browser_initiated=*/true, /*has_ua_visual_transition=*/false,
          /*task_state_id=*/std::nullopt, /*should_skip_screenshot=*/false);
  history_index_ = target;
  if (result == blink::mojom::CommitResult::Ok) {
    // Keep blink's session-history cursor in step with the traversal so the
    // opposite direction isn't short-circuited (HistoryForwardListCount /
    // HistoryBackListCount both derive from this index + length).
    SyncBlinkHistoryCursor();
  } else if (owner_) {
    // Cross-document target: re-navigate to the entry's URL (full reload). Mark the
    // traversal on BOTH lists so the resulting commit moves each cursor instead of
    // appending a duplicate entry: suppress our record, and tell the host to move
    // its cursor to the matching entry (set_page_history_nav) — without which the
    // host's history_index_ would go stale after a page-driven back/forward. Then
    // realign blink.
    std::string target_url = item->Url().GetString().Utf8();
    suppress_history_record_ = true;
    owner_->set_page_history_nav(target_url);
    owner_->LoadURL(target_url.c_str());
    suppress_history_record_ = false;
    SyncBlinkHistoryCursor();
  }
}

void MbFrameClient::DidReceiveTitle(const blink::WebString& title) {
  // Main frame only — push the new document title to the host's title callback.
  if (self_owned_ || !owner_)
    return;
  owner_->OnTitleChanged(title.Utf8());
}

void MbFrameClient::DidFinishLoad() {
  // Main frame only — child/iframe load-finishes don't signal the page's completion.
  if (self_owned_ || !owner_)
    return;
  owner_->OnDidFinishLoad();
}

void MbFrameClient::DidDispatchDOMContentLoadedEvent() {
  // Main frame only — DOMContentLoaded fires when the DOM is parsed and deferred scripts
  // have run (before subresources/images), the earliest "interactive" signal.
  if (self_owned_ || !owner_)
    return;
  owner_->OnDOMContentLoaded();
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
      user_agent_.empty() ? MbDefaultUserAgent() : user_agent_, extra_headers_,
      owner_);
}

std::unique_ptr<blink::WebMediaPlayer> MbFrameClient::CreateMediaPlayer(
    const blink::WebMediaPlayerSource& source,
    blink::WebMediaPlayerClient* client,
    blink::MediaInspectorContext*,
    blink::WebMediaPlayerEncryptedMediaClient*,
    blink::WebContentDecryptionModule*,
    const blink::WebString& /*sink_id*/,
    const cc::LayerTreeSettings* /*settings*/,
    scoped_refptr<base::TaskRunner> /*compositor_worker_task_runner*/) {
  // Real playback path: Chromium's WebMediaPlayerImpl (full MSE + software VP9/Opus
  // pipeline) so <video> (YouTube) actually plays. Used for real resource schemes
  // (http/https/file/blob: — including YouTube's MSE blob: source).
  //
  // data: media URLs go to the lightweight MbAudioPlayer instead: WMPI's network
  // DataSource doesn't drive a non-network (data:) scheme to completion and spins, and
  // MbAudioPlayer already handles self-contained data: audio + video metadata. (Also the
  // fallback if media support can't initialize.)
  const std::string src_url =
      source.IsURL() ? source.GetAsURL().GetString().Utf8() : std::string();
  const bool is_data_uri = src_url.rfind("data:", 0) == 0;
  // The builder owns this frame's UrlIndex (resource cache) and MUST outlive the players
  // it builds — keep one per frame (mirrors content's RenderFrameImpl::media_player_builder_).
  if (web_frame_ && !is_data_uri) {
    if (!media_player_builder_) {
      media_player_builder_ = std::make_unique<blink::WebMediaPlayerBuilder>(
          *web_frame_, base::SingleThreadTaskRunner::GetCurrentDefault());
    }
    if (auto player =
            MbCreateWebMediaPlayer(*media_player_builder_, web_frame_, client))
      return player;
  }
  return std::make_unique<MbAudioPlayer>(
      client, base::SingleThreadTaskRunner::GetCurrentDefault());
}

scoped_refptr<blink::WebWorkerFetchContext>
MbFrameClient::CreateWorkerFetchContext(
    blink::WebDedicatedWorkerHostFactoryClient*) {
  // Same network identity (UA + extra headers) as the frame's own subresources, so a
  // worker's importScripts()/fetch() looks identical on the wire to the page's loads.
  // The worker's top-frame origin is this document's origin (required non-null for a
  // dedicated worker — see MbWorkerFetchContext::TopFrameOrigin).
  std::string top_origin;
  if (web_frame_)
    top_origin = web_frame_->GetSecurityOrigin().ToString().Utf8();
  return base::MakeRefCounted<MbWorkerFetchContext>(
      user_agent_.empty() ? MbDefaultUserAgent() : user_agent_, extra_headers_,
      std::move(top_origin));
}

blink::WebString MbFrameClient::UserAgentOverride() {
  return blink::WebString::FromUtf8(EffectiveUserAgent());
}

const std::string& MbFrameClient::EffectiveUserAgent() const {
  return user_agent_.empty() ? MbDefaultUserAgent() : user_agent_;
}

blink::UserAgentMetadata MbDefaultUserAgentMetadata() {
  // UA Client Hints consistent with MbDefaultUserAgent() (Chrome 150 / macOS /
  // Intel). brand_version_list is the low-entropy navigator.userAgentData.brands;
  // brand_full_version_list backs the high-entropy "fullVersionList" hint. The
  // GREASE brand ("Not.A/Brand") mirrors what real Chrome emits.
  blink::UserAgentMetadata m;
  m.brand_version_list = {
      {"Not.A/Brand", "24"}, {"Chromium", "150"}, {"Google Chrome", "150"}};
  m.brand_full_version_list = {{"Not.A/Brand", "24.0.0.0"},
                               {"Chromium", "150.0.0.0"},
                               {"Google Chrome", "150.0.0.0"}};
  m.full_version = "150.0.0.0";
  m.platform = "macOS";
  m.platform_version = "10.15.7";  // matches "Mac OS X 10_15_7" in the UA string
  m.architecture = "x86";          // matches "Intel" in the UA string
  m.model = "";
  m.mobile = false;
  m.bitness = "64";
  m.wow64 = false;
  m.form_factors = {"Desktop"};
  return m;
}

std::optional<blink::UserAgentMetadata> MbFrameClient::UserAgentMetadataOverride() {
  // Supply metadata only for the built-in UA, so navigator.userAgentData matches
  // navigator.userAgent. A caller-set custom UA gets nullopt (blink falls back to
  // Platform's empty metadata) rather than a contradicting Chrome brand list.
  if (!user_agent_.empty())
    return std::nullopt;
  return MbDefaultUserAgentMetadata();
}

void MbFrameClient::DidAddMessageToConsole(const blink::WebConsoleMessage& msg,
                                           const blink::WebString& source,
                                           unsigned line,
                                           const blink::WebString& stack) {
  const char* level = "log";
  switch (msg.level) {
    case blink::mojom::ConsoleMessageLevel::kVerbose: level = "verbose"; break;
    case blink::mojom::ConsoleMessageLevel::kInfo: level = "log"; break;
    case blink::mojom::ConsoleMessageLevel::kWarning: level = "warn"; break;
    case blink::mojom::ConsoleMessageLevel::kError: level = "error"; break;
  }
  console_.push_back(std::string(level) + ": " + msg.text.Utf8());
  // source/line locate the message; stack is populated for errors and uncaught exceptions
  // / unhandled rejections (which blink reports here as console errors).
  if (owner_)
    owner_->OnConsoleMessage(level, msg.text.Utf8(), source.Utf8(),
                             static_cast<int>(line), stack.Utf8());
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
