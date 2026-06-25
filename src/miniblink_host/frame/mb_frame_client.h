// mb_frame_client — real blink::WebLocalFrameClient for miniblink-modern.
//
// Modeled on TestWebFrameClient (vendor/reference/frame_test_helpers.h:516). The
// minimal override set to load + render a main frame, with real behavior:
//   - CreateURLLoader        -> hand back an MbURLLoader (libcurl). THE network hook.
//   - BeginNavigation        -> drive the load (P1: commit synchronously/simply).
//   - DidStartLoading/DidStopLoading, DidMeaningfulLayout -> know when to paint.
//   - CreateChildFrame       -> subframes (can return null in P1).
//   - FrameDetached, SwapIn  -> lifecycle.
//
// Status: Phase 1 scaffold. Exact signatures pinned during .cc compile vs M150
// third_party/blink/public/web/web_local_frame_client.h.

#ifndef MINIBLINK_HOST_FRAME_MB_FRAME_CLIENT_H_
#define MINIBLINK_HOST_FRAME_MB_FRAME_CLIENT_H_

#include <memory>
#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "mojo/public/cpp/bindings/pending_associated_remote.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "services/network/public/mojom/web_sandbox_flags.mojom-shared.h"
#include "third_party/blink/public/mojom/frame/policy_container.mojom-blink.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/web/web_console_message.h"
#include "third_party/blink/public/web/web_local_frame_client.h"

namespace blink {
class HistoryItem;
}  // namespace blink

namespace mb {

class MbWebView;  // owner / callback sink

// In-renderer PolicyContainerHost for committing navigations: it gives each commit a
// FRESH, empty (advisory CSP/referrer; no-op) policy container with a bound dedicated
// remote, so a prior document's <meta> CSP does not leak into the next document in a
// reused frame. Used as a stack local that outlives the CommitNavigation call (like
// frame_test_helpers' MockPolicyContainerHost). A null remote here would CHECK-fail.
class MbPolicyContainerHost : public blink::mojom::blink::PolicyContainerHost {
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

class MbFrameClient : public blink::WebLocalFrameClient {
 public:
  explicit MbFrameClient(MbWebView* owner);
  ~MbFrameClient() override;

  // The production loader path calls this (loader_factory_for_frame.cc:151); returning
  // a non-null loader makes Blink use it for subresources. -> our file-backed loader.
  std::unique_ptr<blink::URLLoader> CreateURLLoaderForTesting() override;

  // A Worker's subresource fetch context (importScripts/fetch inside the worker).
  // Returns the host's libcurl-backed context so worker loads don't die at creation.
  // (Step 1 of worker bring-up; the worker thread itself is still started elsewhere.)
  scoped_refptr<blink::WebWorkerFetchContext> CreateWorkerFetchContext(
      blink::WebDedicatedWorkerHostFactoryClient*) override;

  // Child frames (<iframe>). Without this, Blink leaves subframes empty
  // (frames.length 0, contentDocument null). We create a real local child frame
  // with its own MbFrameClient (self-owned) so iframe content loads. Modeled on
  // TestWebFrameClient::CreateChildFrame.
  blink::WebLocalFrame* CreateChildFrame(
      blink::mojom::TreeScopeType,
      const blink::WebString& name,
      const blink::WebString& fallback_name,
      const blink::FramePolicy&,
      const blink::WebFrameOwnerProperties&,
      blink::FrameOwnerElementType,
      blink::WebPolicyContainerBindParams policy_container_bind_params,
      ukm::SourceId document_ukm_source_id,
      FinishChildFrameCreationFn complete_creation) override;

  // Navigation hook. Child frames commit synchronously (iframe content). The
  // main frame handles page-initiated navigations (link click, location=, form
  // submit) by posting the commit (re-entrancy-safe). The initial main-frame
  // document is still committed directly by MbWebView, not here.
  void BeginNavigation(std::unique_ptr<blink::WebNavigationInfo>) override;

  // window.open() / target=_blank: notify the host of the requested URL + name, then
  // return null (we don't auto-create popups — the embedder decides what to do, e.g.
  // load it in the current or a new view). Keeps the safe "window.open -> null" default.
  blink::WebView* CreateNewWindow(
      const blink::WebURLRequest&, const blink::WebWindowFeatures&,
      const blink::WebString& name, const gfx::Rect& requested_screen_rect,
      blink::WebNavigationPolicy, network::mojom::WebSandboxFlags,
      const blink::SessionStorageNamespaceId&, bool& consumed_user_gesture,
      const std::optional<blink::Impression>&,
      const std::optional<blink::WebPictureInPictureWindowOptions>&,
      const blink::WebURL& base_url) override;

  // The frame's navigation-associated-interface provider. We return one that
  // binds BlobURLStore to our in-process store (so URL.createObjectURL +
  // blob: fetch work); without this, Blink would talk to the absent browser.
  blink::AssociatedInterfaceProvider* GetRemoteNavigationAssociatedInterfaces()
      override;

  // Fires when this frame commits a document. For the main frame we record the
  // navigation in MbWebView's history stack (captures host- and page-initiated
  // navigations uniformly). Child-frame commits are ignored here.
  void DidCommitNavigation(
      blink::WebHistoryCommitType commit_type,
      bool should_reset_browser_interface_broker,
      const network::ParsedPermissionsPolicy& permissions_policy_header,
      const blink::DocumentPolicyFeatureState& document_policy_header) override;

  // Same-document navigations (pushState/replaceState/fragment). Replicates
  // RenderFrameImpl::UpdateNavigationHistory's history-index bookkeeping so blink's
  // session-history length is correct (history.length) and history.back() is unblocked.
  void DidFinishSameDocumentNavigation(
      blink::WebHistoryCommitType commit_type,
      bool is_synchronously_committed,
      blink::mojom::SameDocumentNavigationType,
      bool is_client_redirect,
      const std::optional<blink::SameDocNavigationScreenshotDestinationToken>&
          screenshot_destination,
      base::UnguessableToken same_document_metrics_token,
      bool caused_by_ad) override;

  // Fires when the document title changes (initial <title> + dynamic document.title
  // writes). For the main frame, pushes the new title to MbWebView's title callback.
  void DidReceiveTitle(const blink::WebString& title) override;

  // Fires when the main frame's load finishes (the document's `load` event — all
  // subresources done). Pushes the signal to MbWebView so embedders can react to
  // real completion instead of polling / a fixed settle timer. Child frames ignored.
  void DidFinishLoad() override;

  // Frame lifecycle. A child client self-destructs on detach; the main frame is
  // owned by MbWebView so it does nothing here.
  void FrameDetached(blink::DetachReason) override;

  // Associate this client with its frame. SetFrame: main frame (MbWebView-owned).
  // Bind: child frame (takes ownership of itself, freed on FrameDetached).
  void SetFrame(blink::WebLocalFrame* frame);
  void Bind(blink::WebLocalFrame* frame,
            std::unique_ptr<MbFrameClient> self_owned) {
    web_frame_ = frame;
    self_owned_ = std::move(self_owned);
  }

  // Sandbox flags for this (child) frame, captured from the owner's FramePolicy
  // in the parent's CreateChildFrame and applied to the document at commit (see
  // BeginNavigation). Without this, <iframe sandbox> would not be enforced.
  void SetSandboxFlags(network::mojom::WebSandboxFlags f) { sandbox_flags_ = f; }

  // Fires when the document element exists but before the page's own scripts run
  // (and may execute JS). We use it to run the host's init script first, so it can
  // set globals / override APIs the page then observes (cf. evaluateOnNewDocument).
  void RunScriptsAtDocumentElementAvailable() override;

  // navigator.userAgent + the value sent on every request. Blink calls this when
  // non-empty (else Platform::UserAgent(), which is empty here), so we always
  // return a real UA. Set before navigating to take effect for that load.
  blink::WebString UserAgentOverride() override;

  void SetUserAgent(const std::string& ua) { user_agent_ = ua; }
  const std::string& user_agent() const { return user_agent_; }
  // The UA actually sent to servers and reported to navigator.userAgent: the
  // override if set, else the built-in default (matches UserAgentOverride()).
  const std::string& EffectiveUserAgent() const;

  void SetExtraHeaders(const std::string& h) { extra_headers_ = h; }
  const std::string& extra_headers() const { return extra_headers_; }

  // Capture page console output (console.log/warn/error) so a host or automation
  // script can read it back. Each entry is "level: text".
  void DidAddMessageToConsole(const blink::WebConsoleMessage&,
                              const blink::WebString& source_name,
                              unsigned source_line,
                              const blink::WebString& stack_trace) override;
  // Return all captured console lines joined by '\n' and clear the buffer.
  std::string DrainConsole();

  // TODO(mb): DidStopLoading/DidMeaningfulLayout (paint signal), CreateChildFrame.

 private:
  // Build the WebNavigationParams (fetch body / srcdoc, policy container,
  // sandbox flags) and commit them into web_frame_. Called synchronously for
  // child frames and via a posted task for the main frame (see BeginNavigation).
  void DoCommit(std::unique_ptr<blink::WebNavigationInfo> info);

  // Record the main frame's just-committed HistoryItem into our session-history
  // list. `is_standard` appends a new entry (truncating any forward entries);
  // otherwise it replaces the current entry (replaceState / reload / initial).
  void RecordHistoryCommit(bool is_standard);

  // Restate blink's WebView session-history cursor (index + length) from our
  // history_items_ list — the source of truth. Called after every change so
  // history.length and back/forward gating stay correct (our cross-document
  // commits otherwise reset blink's counters to 0).
  void SyncBlinkHistoryCursor();

  // Perform a page-driven history.back()/forward()/go(delta) traversal: blink
  // routed it through LocalFrameHost.GoToEntryAtOffset -> MbLocalFrameHost ->
  // here (on the main thread). Same-document targets commit via
  // CommitSameDocumentNavigation (restoring state + firing popstate); cross-
  // document targets fall back to the host's re-navigation.
  void GoToHistoryOffset(int offset, bool has_user_gesture);

  [[maybe_unused]] MbWebView* owner_;  // not owned (used once handshake bodies land)
  std::string user_agent_;  // empty -> MbDefaultUserAgent() (resolved at use)
  std::string extra_headers_;  // newline-separated "Name: Value" request headers
  std::vector<std::string> console_;  // captured console messages
  blink::WebLocalFrame* web_frame_ = nullptr;       // this client's frame
  std::unique_ptr<MbFrameClient> self_owned_;        // set for child frames only
  network::mojom::WebSandboxFlags sandbox_flags_ =
      network::mojom::WebSandboxFlags::kNone;        // child <iframe sandbox>

  // Lazily-created navigation-associated-interface provider (owns the BlobURLStore
  // binding). Owned here; freed in the destructor.
  blink::AssociatedInterfaceProvider* nav_assoc_interfaces_ = nullptr;

  // Main-frame session history: one Persistent<HistoryItem> per back/forward
  // entry, in order, with history_index_ pointing at the current one. Captured at
  // each main-frame commit (RecordHistoryCommit) so a page-driven history.go()
  // can replay the target entry. Empty / -1 for child frames.
  std::vector<blink::Persistent<blink::HistoryItem>> history_items_;
  int history_index_ = -1;

  // Guards posted main-frame commits: if the client is torn down before the
  // task runs, it no-ops. Must be the last member.
  base::WeakPtrFactory<MbFrameClient> weak_factory_{this};

};

}  // namespace mb

#endif  // MINIBLINK_HOST_FRAME_MB_FRAME_CLIENT_H_
