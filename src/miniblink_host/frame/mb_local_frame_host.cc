// mb_local_frame_host.cc — see header. No-op bodies copied from blink's
// FakeLocalFrameHost (core/testing/fake_local_frame_host.cc); GoToEntryAtOffset
// has real behavior (routes page-driven history traversal to the main thread).
#include "miniblink_host/frame/mb_local_frame_host.h"

#include <cstdint>
#include <map>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "mojo/public/cpp/bindings/self_owned_associated_receiver.h"
#include "skia/public/mojom/skcolor.mojom-blink.h"
#include "third_party/blink/public/mojom/choosers/popup_menu.mojom-blink.h"
#include "third_party/blink/public/mojom/frame/frame_owner_properties.mojom-blink.h"
#include "third_party/blink/public/mojom/frame/frame_replication_state.mojom-blink.h"
#include "third_party/blink/public/mojom/frame/fullscreen.mojom-blink.h"
#include "third_party/blink/public/mojom/frame/remote_frame.mojom-blink.h"
#include "third_party/blink/public/mojom/timing/resource_timing.mojom-blink.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

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
    base::RepeatingCallback<void(const std::string&)> favicon_handler) {
  SinkRegistry& s = Sinks();
  base::AutoLock guard(s.lock);
  s.by_frame[frame_key] = SinkEntry{std::move(runner), std::move(handler),
                                    std::move(key_handler),
                                    std::move(favicon_handler)};
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
    blink::mojom::blink::DownloadURLParamsPtr) {}
void MbLocalFrameHost::FocusedElementChanged(bool,
                                             bool,
                                             const gfx::Rect&,
                                             blink::mojom::FocusType) {}
void MbLocalFrameHost::TextSelectionChanged(const blink::String&,
                                            uint32_t,
                                            const gfx::Range&) {}
void MbLocalFrameHost::ShowPopupMenu(
    mojo::PendingRemote<blink::mojom::blink::PopupMenuClient>,
    const gfx::Rect&,
    double,
    int32_t,
    blink::Vector<blink::mojom::blink::MenuItemPtr>,
    bool,
    bool) {}
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
