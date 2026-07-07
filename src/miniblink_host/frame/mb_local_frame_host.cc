// mb_local_frame_host.cc — see header. No-op bodies copied from blink's
// FakeLocalFrameHost (core/testing/fake_local_frame_host.cc); GoToEntryAtOffset
// has real behavior (routes page-driven history traversal to the main thread).
#include "miniblink_host/frame/mb_local_frame_host.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_associated_receiver.h"
#include "skia/public/mojom/skcolor.mojom-blink.h"
#include "third_party/blink/public/mojom/choosers/popup_menu.mojom-blink.h"
#include "third_party/blink/public/mojom/frame/frame_owner_properties.mojom-blink.h"
#include "third_party/blink/public/mojom/frame/frame_replication_state.mojom-blink.h"
#include "third_party/blink/public/mojom/frame/fullscreen.mojom-blink.h"
#include "third_party/blink/public/mojom/frame/remote_frame.mojom-blink.h"
#include "third_party/blink/public/mojom/timing/resource_timing.mojom-blink.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

#include "miniblink_host/blob/mb_blob_registry.h"

namespace mb {
namespace {

// A frame's host-side callbacks: `handler` for offset-based traversal (history.go),
// `key_handler` for the Navigation API's key-based traversal, `favicon_handler` for
// favicon URLs. Posted to `runner` (the main/blink thread).
struct SinkEntry {
  scoped_refptr<base::SingleThreadTaskRunner> runner;
  base::RepeatingCallback<void(int, bool)> handler;
  base::RepeatingCallback<void(const std::string&, bool)> key_handler;
  base::RepeatingCallback<void(const std::string&)> favicon_handler;
  base::RepeatingCallback<void(const std::string&, const std::string&,
                               const std::string&)>
      download_handler;
  base::RepeatingCallback<void(const std::string&, const std::string&)>
      download_url_handler;
  base::RepeatingClosure close_handler;  // window.close() (main frame only)
};

// Per-frame registry keyed by a frame id, so multiple views each route their own
// LocalFrameHost callbacks (a single global slot would let a second view clobber
// the first). Lock-protected; touched from the service thread (bind/route) and the
// main thread (register/clear).
struct SinkRegistry {
  base::Lock lock;
  std::map<uint64_t, SinkEntry> by_frame;
};

SinkRegistry& Sinks() {
  static base::NoDestructor<SinkRegistry> s;
  return *s;
}

// Snapshot a frame's entry (returns false if none registered).
bool LookupSink(uint64_t frame_key, SinkEntry* out) {
  SinkRegistry& s = Sinks();
  base::AutoLock guard(s.lock);
  auto it = s.by_frame.find(frame_key);
  if (it == s.by_frame.end())
    return false;
  *out = it->second;
  return true;
}

}  // namespace

void MbSetHistoryGoToHandler(
    uint64_t frame_key,
    scoped_refptr<base::SingleThreadTaskRunner> runner,
    base::RepeatingCallback<void(int, bool)> handler,
    base::RepeatingCallback<void(const std::string&, bool)> key_handler,
    base::RepeatingCallback<void(const std::string&)> favicon_handler,
    base::RepeatingCallback<void(const std::string&, const std::string&,
                                 const std::string&)>
        download_handler,
    base::RepeatingCallback<void(const std::string&, const std::string&)>
        download_url_handler) {
  SinkRegistry& s = Sinks();
  base::AutoLock guard(s.lock);
  s.by_frame[frame_key] = SinkEntry{
      std::move(runner), std::move(handler), std::move(key_handler),
      std::move(favicon_handler), std::move(download_handler),
      std::move(download_url_handler)};
}

void MbClearHistoryGoToHandler(uint64_t frame_key) {
  SinkRegistry& s = Sinks();
  base::AutoLock guard(s.lock);
  s.by_frame.erase(frame_key);
}

void MbBindLocalFrameHost(mojo::ScopedInterfaceEndpointHandle handle,
                          uint64_t frame_key) {
  mojo::MakeSelfOwnedAssociatedReceiver(
      std::make_unique<MbLocalFrameHost>(frame_key),
      mojo::PendingAssociatedReceiver<blink::mojom::blink::LocalFrameHost>(
          std::move(handle)));
}

void MbSetRequestCloseHandler(uint64_t frame_key,
                              base::RepeatingClosure handler) {
  SinkRegistry& s = Sinks();
  base::AutoLock guard(s.lock);
  s.by_frame[frame_key].close_handler = std::move(handler);
}

namespace {

// The WebView-level main-frame host (bound alongside MbLocalFrameHost on the
// same navigation-associated provider). Everything is a no-op except
// RequestClose — window.close() — which routes to the main-thread close sink.
// Methods with a reply run their callback so blink is never left waiting.
class MbLocalMainFrameHost : public blink::mojom::blink::LocalMainFrameHost {
 public:
  explicit MbLocalMainFrameHost(uint64_t frame_key) : frame_key_(frame_key) {}
  ~MbLocalMainFrameHost() override = default;

  void ScaleFactorChanged(float) override {}
  void ContentsPreferredSizeChanged(const gfx::Size&) override {}
  void FocusPage() override {}
  void TakeFocus(bool) override {}
  void UpdateTargetURL(const blink::KURL&,
                       UpdateTargetURLCallback callback) override {
    std::move(callback).Run();
  }
  void RequestClose() override {
    SinkEntry e;
    if (LookupSink(frame_key_, &e) && e.runner && e.close_handler)
      e.runner->PostTask(FROM_HERE, e.close_handler);
  }
  void SetWindowRect(const gfx::Rect&, SetWindowRectCallback callback) override {
    std::move(callback).Run();
  }
  void DidFirstVisuallyNonEmptyPaint() override {}
  void DidAccessInitialMainDocument() override {}
  void DidChangeThemeColor(std::optional<::SkColor>) override {}
  void DidChangeBackgroundColor(const ::SkColor4f&, bool) override {}
  void Maximize() override {}
  void Minimize() override {}
  void Restore() override {}
  void SetResizable(bool) override {}
  void DraggableRegionsChanged(
      blink::Vector<blink::mojom::blink::DraggableRegionPtr>) override {}
  void OnFirstContentfulPaint(base::TimeDelta) override {}

 private:
  const uint64_t frame_key_;
};

}  // namespace

void MbBindLocalMainFrameHost(mojo::ScopedInterfaceEndpointHandle handle,
                              uint64_t frame_key) {
  mojo::MakeSelfOwnedAssociatedReceiver(
      std::make_unique<MbLocalMainFrameHost>(frame_key),
      mojo::PendingAssociatedReceiver<blink::mojom::blink::LocalMainFrameHost>(
          std::move(handle)));
}

void MbLocalFrameHost::GoToEntryAtOffset(
    int32_t offset,
    bool has_user_gesture,
    base::TimeTicks /*actual_navigation_start*/,
    std::optional<blink::scheduler::TaskAttributionId>) {
  if (offset == 0)
    return;
  SinkEntry e;
  if (LookupSink(frame_key_, &e) && e.runner && e.handler) {
    e.runner->PostTask(FROM_HERE,
                       base::BindOnce(e.handler, offset, has_user_gesture));
  }
}

void MbLocalFrameHost::NavigateToNavigationApiKey(
    const blink::String& key,
    bool has_user_gesture,
    base::TimeTicks /*actual_navigation_start*/,
    std::optional<blink::scheduler::TaskAttributionId>) {
  // The Navigation API's navigation.back()/forward()/traverseTo(key) routes here.
  SinkEntry e;
  if (LookupSink(frame_key_, &e) && e.runner && e.key_handler) {
    e.runner->PostTask(
        FROM_HERE, base::BindOnce(e.key_handler, key.Utf8(), has_user_gesture));
  }
}

// ---- No-op bodies (verbatim behavior from FakeLocalFrameHost) ----
void MbLocalFrameHost::EnterFullscreen(
    blink::mojom::blink::FullscreenOptionsPtr,
    EnterFullscreenCallback callback) {
  std::move(callback).Run(true);
}
void MbLocalFrameHost::ExitFullscreen() {}
void MbLocalFrameHost::FullscreenStateChanged(
    bool,
    blink::mojom::blink::FullscreenOptionsPtr) {}
void MbLocalFrameHost::RegisterProtocolHandler(const blink::String&,
                                               const ::blink::KURL&,
                                               bool) {}
void MbLocalFrameHost::UnregisterProtocolHandler(const blink::String&,
                                                 const ::blink::KURL&,
                                                 bool) {}
void MbLocalFrameHost::DidDisplayInsecureContent() {}
void MbLocalFrameHost::DidContainInsecureFormAction() {}
void MbLocalFrameHost::MainDocumentElementAvailable(bool) {}
void MbLocalFrameHost::SetNeedsOcclusionTracking(bool) {}
void MbLocalFrameHost::SetVirtualKeyboardMode(
    ui::mojom::blink::VirtualKeyboardMode) {}
void MbLocalFrameHost::VisibilityChanged(
    blink::mojom::blink::FrameVisibility) {}
void MbLocalFrameHost::DidFailLoadWithError(const ::blink::KURL&, int32_t) {}
void MbLocalFrameHost::DidFocusFrame() {}
void MbLocalFrameHost::DidCallFocus() {}
void MbLocalFrameHost::EnforceInsecureRequestPolicy(
    blink::mojom::InsecureRequestPolicy) {}
void MbLocalFrameHost::EnforceInsecureNavigationsSet(
    const blink::Vector<uint32_t>&) {}
void MbLocalFrameHost::SuddenTerminationDisablerChanged(
    bool,
    blink::mojom::SuddenTerminationDisablerType) {}
void MbLocalFrameHost::HadStickyUserActivationBeforeNavigationChanged(bool) {}
void MbLocalFrameHost::ScrollRectToVisibleInParentFrame(
    const gfx::RectF&,
    blink::mojom::blink::ScrollIntoViewParamsPtr) {}
void MbLocalFrameHost::BubbleLogicalScrollInParentFrame(
    blink::mojom::blink::ScrollDirection,
    ui::ScrollGranularity) {}
void MbLocalFrameHost::DidBlockNavigation(
    const blink::KURL&,
    blink::mojom::NavigationBlockedReason) {}
void MbLocalFrameHost::DidChangeLoadProgress(double) {}
void MbLocalFrameHost::DidFinishLoad(const blink::KURL&) {}
void MbLocalFrameHost::DispatchLoad() {}
void MbLocalFrameHost::UpdateTitle(const blink::String&) {}
void MbLocalFrameHost::UpdateApplicationTitle(const blink::String&) {}
void MbLocalFrameHost::UpdateUserActivationState(
    blink::mojom::blink::UserActivationUpdateType,
    blink::mojom::UserActivationNotificationType) {}
void MbLocalFrameHost::HandleAccessibilityFindInPageResult(
    blink::mojom::blink::FindInPageResultAXParamsPtr) {}
void MbLocalFrameHost::HandleAccessibilityFindInPageTermination() {}
void MbLocalFrameHost::DocumentOnLoadCompleted() {}
void MbLocalFrameHost::ForwardResourceTimingToParent(
    blink::mojom::blink::ResourceTimingInfoPtr) {}
void MbLocalFrameHost::DidDispatchDOMContentLoadedEvent() {}
void MbLocalFrameHost::RunModalAlertDialog(const blink::String&,
                                           bool,
                                           RunModalAlertDialogCallback callback) {
  std::move(callback).Run();
}
void MbLocalFrameHost::RunModalConfirmDialog(
    const blink::String&,
    bool,
    RunModalConfirmDialogCallback callback) {
  std::move(callback).Run(true);
}
void MbLocalFrameHost::RunModalPromptDialog(
    const blink::String&,
    const blink::String&,
    bool,
    RunModalPromptDialogCallback callback) {
  std::move(callback).Run(true, blink::g_empty_string);
}
void MbLocalFrameHost::RunBeforeUnloadConfirm(
    bool,
    RunBeforeUnloadConfirmCallback callback) {
  std::move(callback).Run(true);
}
void MbLocalFrameHost::UpdateFaviconURL(
    blink::Vector<blink::mojom::blink::FaviconURLPtr> favicon_urls) {
  // Report the page's favicon(s) — newline-separated URLs, the standard <link
  // rel=icon> first as blink ordered them — to the host (browser tab-icon use).
  std::string urls;
  for (const auto& f : favicon_urls) {
    if (!f || f->icon_url.IsEmpty())
      continue;
    if (!urls.empty())
      urls += "\n";
    urls += f->icon_url.GetString().Utf8();
  }
  if (urls.empty())
    return;
  SinkEntry e;
  if (LookupSink(frame_key_, &e) && e.runner && e.favicon_handler)
    e.runner->PostTask(FROM_HERE,
                       base::BindOnce(e.favicon_handler, std::move(urls)));
}
void MbLocalFrameHost::DownloadURL(
    blink::mojom::blink::DownloadURLParamsPtr params) {
  // A page-initiated download: <a download href="blob:...">, or
  // URL.createObjectURL(blob) + a programmatic click. blink reports it here
  // (the no-op FakeLocalFrameHost dropped it). We resolve the blob bytes and
  // route them to the frame's download sink, so a client-generated file (a
  // CSV/PDF/etc. built in JS) surfaces through the same callback as a
  // server-driven download. Only blob: is resolved — that's the client-
  // generated case; data:/http: flow through other paths and are ignored here.
  if (!params)
    return;
  std::string name =
      params->suggested_name.IsNull() ? std::string()
                                      : params->suggested_name.Utf8();
  const uint64_t fk = frame_key_;
  // data: download — blink leaves params->url empty and packs the *raw data: URL
  // string* as the bytes of params->data_url_blob (LocalFrame::DataURLToBlob; the
  // real browser decodes it download-side). So: drain the Blob to recover the
  // data: URL, then route it to the main thread's fetch path, which decodes data:
  // (net::DataURL) into the actual bytes + MIME — same path as an http download.
  if (params->data_url_blob) {
    MbReadBlobRemoteBytes(
        std::move(params->data_url_blob),
        base::BindOnce(
            [](std::string name, uint64_t fk, std::vector<uint8_t> bytes) {
              std::string data_url(bytes.begin(), bytes.end());
              SinkEntry e;
              if (!LookupSink(fk, &e) || !e.runner || !e.download_url_handler)
                return;
              e.runner->PostTask(
                  FROM_HERE, base::BindOnce(e.download_url_handler,
                                            std::move(data_url),
                                            std::move(name)));
            },
            name, fk));
    return;
  }
  std::string url = params->url.GetString().Utf8();
  if (params->url.ProtocolIs("blob")) {
    // Service thread: read the blob's bytes, then hop to the frame's main thread.
    MbResolveBlobUrlBytes(
        url,
        base::BindOnce(
            [](std::string url, std::string name, uint64_t fk,
               std::vector<uint8_t> bytes) {
              SinkEntry e;
              if (!LookupSink(fk, &e) || !e.runner || !e.download_handler)
                return;
              std::string body(bytes.begin(), bytes.end());
              e.runner->PostTask(
                  FROM_HERE,
                  base::BindOnce(e.download_handler, std::move(url),
                                 std::move(name), std::move(body)));
            },
            url, name, fk));
    return;
  }
  if (params->url.ProtocolIs("http") || params->url.ProtocolIs("https")) {
    // http(s) bytes aren't in the in-process blob store — hand the URL to the
    // frame's main thread, which fetches it through the engine (honoring the
    // interception layer + the view's cookies/headers) and fires the callback.
    SinkEntry e;
    if (LookupSink(fk, &e) && e.runner && e.download_url_handler) {
      e.runner->PostTask(FROM_HERE, base::BindOnce(e.download_url_handler,
                                                   std::move(url),
                                                   std::move(name)));
    }
  }
}
void MbLocalFrameHost::FocusedElementChanged(bool,
                                             bool,
                                             const gfx::Rect&,
                                             blink::mojom::FocusType) {}
void MbLocalFrameHost::TextSelectionChanged(const blink::String&,
                                            uint32_t,
                                            const gfx::Range&) {}
namespace {

// Wraps the PopupMenuClient remote for the host. Lives on the MAIN thread
// (constructed there; the PendingRemote is bound on first use). Exactly-once:
// a second Accept/Cancel is a no-op, and destruction without either cancels,
// so a host that forgets to reply still lets blink fire PopupDidHide.
class PopupClientImpl final : public MbSelectPopupClient {
 public:
  explicit PopupClientImpl(
      mojo::PendingRemote<blink::mojom::blink::PopupMenuClient> pending)
      : pending_(std::move(pending)) {}
  ~PopupClientImpl() override { Cancel(); }

  void Accept(const std::vector<int32_t>& indices) override {
    if (done_)
      return;
    done_ = true;
    mojo::Remote<blink::mojom::blink::PopupMenuClient> remote(
        std::move(pending_));
    if (remote)
      remote->DidAcceptIndices(blink::Vector<int32_t>(indices));
  }

  void Cancel() override {
    if (done_)
      return;
    done_ = true;
    mojo::Remote<blink::mojom::blink::PopupMenuClient> remote(
        std::move(pending_));
    if (remote)
      remote->DidCancel();
  }

 private:
  mojo::PendingRemote<blink::mojom::blink::PopupMenuClient> pending_;
  bool done_ = false;
};

}  // namespace

void MbLocalFrameHost::ShowPopupMenu(
    mojo::PendingRemote<blink::mojom::blink::PopupMenuClient> popup_client,
    const gfx::Rect& bounds,
    double font_size,
    int32_t selected_item,
    blink::Vector<blink::mojom::blink::MenuItemPtr> menu_items,
    bool right_aligned,
    bool allow_multiple_selection) {
  // A <select> (or date/color chooser menulist) opened: convert to plain data
  // and route to the owning view on the MAIN thread (we're on the service
  // thread here). The indices the host later passes to Accept are indices
  // into this items array — the same "external" index space blink expects in
  // DidAcceptIndices (ExternalPopupMenu translates them back internally).
  MbSelectPopupData data;
  data.x = bounds.x();
  data.y = bounds.y();
  data.width = bounds.width();
  data.height = bounds.height();
  data.font_size = font_size;
  data.selected_index = selected_item;
  data.right_aligned = right_aligned;
  data.allow_multiple = allow_multiple_selection;
  data.items.reserve(menu_items.size());
  for (const auto& mi : menu_items) {
    MbSelectPopupData::Item item;
    item.label = mi->label.IsNull() ? std::string() : mi->label.Utf8();
    item.type = static_cast<int>(mi->type);
    item.enabled = mi->enabled;
    item.checked = mi->checked;
    data.items.push_back(std::move(item));
  }
  SinkEntry sink;
  if (!LookupSink(frame_key_, &sink) || !sink.runner) {
    // No registered frame (shutting down): let the pipe drop -> blink cancels.
    return;
  }
  sink.runner->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](uint64_t frame_key, MbSelectPopupData data,
             mojo::PendingRemote<blink::mojom::blink::PopupMenuClient> client) {
            MbRouteSelectPopupToView(
                frame_key, std::move(data),
                std::make_unique<PopupClientImpl>(std::move(client)));
          },
          frame_key_, std::move(data), std::move(popup_client)));
}
void MbLocalFrameHost::CreateNewPopupWidget(
    mojo::PendingAssociatedReceiver<blink::mojom::blink::PopupWidgetHost>,
    mojo::PendingAssociatedReceiver<blink::mojom::blink::WidgetHost>,
    mojo::PendingAssociatedRemote<blink::mojom::blink::Widget>) {}
void MbLocalFrameHost::ShowContextMenu(
    mojo::PendingAssociatedRemote<blink::mojom::blink::ContextMenuClient>,
    const blink::UntrustworthyContextMenuParams&) {}
void MbLocalFrameHost::DidLoadResourceFromMemoryCache(
    const blink::KURL&,
    const blink::String&,
    const blink::String&,
    network::mojom::blink::RequestDestination,
    bool) {}
void MbLocalFrameHost::DidChangeFrameOwnerProperties(
    const blink::FrameToken&,
    blink::mojom::blink::FrameOwnerPropertiesPtr) {}
void MbLocalFrameHost::DidChangeOpener(
    const std::optional<blink::LocalFrameToken>&) {}
void MbLocalFrameHost::DidChangeIframeAttributes(
    const blink::FrameToken&,
    blink::mojom::blink::IframeAttributesPtr) {}
void MbLocalFrameHost::DidChangeFramePolicy(const blink::FrameToken&,
                                            const blink::FramePolicy&) {}
void MbLocalFrameHost::CapturePaintPreviewOfSubframe(
    const gfx::Rect&,
    const base::UnguessableToken&) {}
void MbLocalFrameHost::SetCloseListener(
    mojo::PendingRemote<blink::mojom::blink::CloseListener>) {}
void MbLocalFrameHost::Detach() {}
void MbLocalFrameHost::GetKeepAliveHandleFactory(
    mojo::PendingReceiver<blink::mojom::blink::KeepAliveHandleFactory>) {}
void MbLocalFrameHost::DidAddMessageToConsole(
    blink::mojom::blink::ConsoleMessageLevel,
    const blink::String&,
    uint32_t,
    const blink::String&,
    const blink::String&) {}
void MbLocalFrameHost::FrameSizeChanged(const gfx::Size&) {}
void MbLocalFrameHost::DidInferColorScheme(
    blink::mojom::PreferredColorScheme) {}
void MbLocalFrameHost::DidChangeSrcDoc(const blink::FrameToken&,
                                       const blink::String&) {}
void MbLocalFrameHost::ReceivedDelegatedCapability(
    blink::mojom::DelegatedCapability) {}
void MbLocalFrameHost::SendFencedFrameReportingBeacon(
    const blink::String&,
    const blink::String&,
    const blink::Vector<blink::FencedFrame::ReportingDestination>&,
    bool) {}
void MbLocalFrameHost::SendFencedFrameReportingBeaconToCustomURL(
    const blink::KURL&,
    bool) {}
void MbLocalFrameHost::SetFencedFrameAutomaticBeaconReportEventData(
    blink::mojom::AutomaticBeaconType,
    const blink::String&,
    const blink::Vector<blink::FencedFrame::ReportingDestination>&,
    bool,
    bool) {}
void MbLocalFrameHost::SendLegacyTechEvent(
    const blink::String&,
    blink::mojom::blink::LegacyTechEventCodeLocationPtr) {}
void MbLocalFrameHost::SendPrivateAggregationRequestsForFencedFrameEvent(
    const blink::String&) {}
void MbLocalFrameHost::CreateFencedFrame(
    mojo::PendingAssociatedReceiver<blink::mojom::blink::FencedFrameOwnerHost>,
    blink::mojom::blink::RemoteFrameInterfacesFromRendererPtr,
    const blink::RemoteFrameToken&,
    const base::UnguessableToken&) {}
void MbLocalFrameHost::StartDragging(
    const blink::WebDragData&,
    blink::DragOperationsMask,
    const SkBitmap&,
    const gfx::Vector2d&,
    const gfx::Rect&,
    blink::mojom::blink::DragEventSourceInfoPtr) {}
void MbLocalFrameHost::IssueKeepAliveHandle(
    mojo::PendingReceiver<blink::mojom::blink::NavigationStateKeepAliveHandle>) {
}
void MbLocalFrameHost::NotifyStorageAccessed(
    blink::mojom::StorageTypeAccessed,
    bool) {}
void MbLocalFrameHost::RecordWindowProxyUsageMetrics(
    const blink::FrameToken&,
    blink::mojom::WindowProxyAccessType) {}
void MbLocalFrameHost::InitializeCrashReportContext(
    uint64_t,
    InitializeCrashReportContextCallback) {}
void MbLocalFrameHost::NotifyDocumentInteractive() {}

}  // namespace mb
