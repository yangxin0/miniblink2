// mb_local_frame_host — a real (mostly no-op) blink::mojom::blink::LocalFrameHost.
//
// Modeled on blink's FakeLocalFrameHost (core/testing/fake_local_frame_host.h),
// which is testonly and can't link into our (non-test) host library, so the no-op
// method bodies are copied here verbatim. The ONE method with real behavior is
// GoToEntryAtOffset: the renderer calls it for page-initiated history.back()/
// forward() (History::go -> LocalFrameClientImpl::NavigateBackForward). With no
// LocalFrameHost bound those calls were dropped, so back()/forward() did nothing.
// We bind this impl on the frame's navigation-associated-interface provider and
// route GoToEntryAtOffset to the main thread, where MbFrameClient performs the
// actual session-history traversal (same-document commit + popstate).

#ifndef MINIBLINK_HOST_FRAME_MB_LOCAL_FRAME_HOST_H_
#define MINIBLINK_HOST_FRAME_MB_LOCAL_FRAME_HOST_H_

#include <optional>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "mojo/public/cpp/bindings/scoped_interface_endpoint_handle.h"
#include "net/storage_access_api/status.h"
#include "third_party/blink/public/common/context_menu_data/untrustworthy_context_menu_params.h"
#include "third_party/blink/public/mojom/context_menu/context_menu.mojom-blink.h"
#include "third_party/blink/public/mojom/dom_storage/storage_area.mojom-blink.h"
#include "third_party/blink/public/mojom/favicon/favicon_url.mojom-blink.h"
#include "third_party/blink/public/mojom/frame/frame.mojom-blink.h"
#include "third_party/blink/public/mojom/frame/policy_container.mojom-blink.h"
#include "third_party/blink/public/mojom/frame/remote_frame.mojom-blink.h"
#include "third_party/blink/public/mojom/scroll/scroll_into_view_params.mojom-blink.h"
#include "ui/base/ime/mojom/virtual_keyboard_types.mojom-blink.h"

namespace mb {

// Register the MAIN frame's history-traversal sink. `handler` is invoked (posted
// to `runner`, the main/blink thread) with (offset, has_user_gesture) when the
// page calls history.back()/forward()/go(). Single-slot (last writer wins): this
// embedder has one main frame whose page-driven history we service. Thread-safe.
void MbSetHistoryGoToHandler(
    scoped_refptr<base::SingleThreadTaskRunner> runner,
    base::RepeatingCallback<void(int offset, bool has_user_gesture)> handler);
void MbClearHistoryGoToHandler();

// Bind a self-owned MbLocalFrameHost to `handle` (called from the frame's
// navigation-associated-interface provider on the service thread).
void MbBindLocalFrameHost(mojo::ScopedInterfaceEndpointHandle handle);

class MbLocalFrameHost : public blink::mojom::blink::LocalFrameHost {
 public:
  MbLocalFrameHost() = default;
  ~MbLocalFrameHost() override = default;

  // THE one method with real behavior — routes to the main-thread sink.
  void GoToEntryAtOffset(
      int32_t offset,
      bool has_user_gesture,
      base::TimeTicks actual_navigation_start,
      std::optional<blink::scheduler::TaskAttributionId>) override;

  // --- Everything below is a no-op, copied from FakeLocalFrameHost. ---
  void EnterFullscreen(blink::mojom::blink::FullscreenOptionsPtr options,
                       EnterFullscreenCallback callback) override;
  void ExitFullscreen() override;
  void FullscreenStateChanged(
      bool is_fullscreen,
      blink::mojom::blink::FullscreenOptionsPtr options) override;
  void RegisterProtocolHandler(const blink::String& scheme,
                               const ::blink::KURL& url,
                               bool user_gesture) override;
  void UnregisterProtocolHandler(const blink::String& scheme,
                                 const ::blink::KURL& url,
                                 bool user_gesture) override;
  void DidDisplayInsecureContent() override;
  void DidContainInsecureFormAction() override;
  void MainDocumentElementAvailable(bool uses_temporary_zoom_level) override;
  void SetNeedsOcclusionTracking(bool needs_tracking) override;
  void SetVirtualKeyboardMode(
      ui::mojom::blink::VirtualKeyboardMode mode) override;
  void VisibilityChanged(blink::mojom::blink::FrameVisibility visibility) override;
  void DidFailLoadWithError(const ::blink::KURL& url,
                            int32_t error_code) override;
  void DidFocusFrame() override;
  void DidCallFocus() override;
  void EnforceInsecureRequestPolicy(
      blink::mojom::InsecureRequestPolicy policy_bitmap) override;
  void EnforceInsecureNavigationsSet(const blink::Vector<uint32_t>& set) override;
  void SuddenTerminationDisablerChanged(
      bool present,
      blink::mojom::SuddenTerminationDisablerType disabler_type) override;
  void HadStickyUserActivationBeforeNavigationChanged(bool value) override;
  void ScrollRectToVisibleInParentFrame(
      const gfx::RectF& rect_to_scroll,
      blink::mojom::blink::ScrollIntoViewParamsPtr params) override;
  void BubbleLogicalScrollInParentFrame(
      blink::mojom::blink::ScrollDirection direction,
      ui::ScrollGranularity granularity) override;
  void StartLoadingForAsyncNavigationApiCommit() override {}
  void DidBlockNavigation(const blink::KURL& blocked_url,
                          blink::mojom::NavigationBlockedReason reason) override;
  void DidChangeLoadProgress(double load_progress) override;
  void DidFinishLoad(const blink::KURL& validated_url) override;
  void DispatchLoad() override;
  void NavigateToNavigationApiKey(
      const blink::String& key,
      bool has_user_gesture,
      base::TimeTicks actual_navigation_start,
      std::optional<blink::scheduler::TaskAttributionId> task_id) override {}
  void NavigateEventHandlerPresenceChanged(bool present) override {}
  void UpdateTitle(const blink::String& title) override;
  void UpdateApplicationTitle(const blink::String& application_title) override;
  void UpdateUserActivationState(
      blink::mojom::blink::UserActivationUpdateType update_type,
      blink::mojom::UserActivationNotificationType notification_type) override;
  void DidConsumeHistoryUserActivation() override {}
  void HandleAccessibilityFindInPageResult(
      blink::mojom::blink::FindInPageResultAXParamsPtr params) override;
  void HandleAccessibilityFindInPageTermination() override;
  void DocumentOnLoadCompleted() override;
  void ForwardResourceTimingToParent(
      blink::mojom::blink::ResourceTimingInfoPtr timing) override;
  void DidDispatchDOMContentLoadedEvent() override;
  void RunModalAlertDialog(const blink::String& alert_message,
                           bool disable_third_party_subframe_suppresion,
                           RunModalAlertDialogCallback callback) override;
  void RunModalConfirmDialog(const blink::String& alert_message,
                             bool disable_third_party_subframe_suppresion,
                             RunModalConfirmDialogCallback callback) override;
  void RunModalPromptDialog(const blink::String& alert_message,
                            const blink::String& default_value,
                            bool disable_third_party_subframe_suppresion,
                            RunModalPromptDialogCallback callback) override;
  void RunBeforeUnloadConfirm(bool is_reload,
                              RunBeforeUnloadConfirmCallback callback) override;
  void UpdateFaviconURL(
      blink::Vector<blink::mojom::blink::FaviconURLPtr> favicon_urls) override;
  void DownloadURL(blink::mojom::blink::DownloadURLParamsPtr params) override;
  void FocusedElementChanged(bool is_editable_element,
                             bool is_richly_editable_element,
                             const gfx::Rect& bounds_in_frame_widget,
                             blink::mojom::FocusType focus_type) override;
  void TextSelectionChanged(const blink::String& text,
                            uint32_t offset,
                            const gfx::Range& range) override;
  void ShowPopupMenu(
      mojo::PendingRemote<blink::mojom::blink::PopupMenuClient> popup_client,
      const gfx::Rect& bounds,
      double font_size,
      int32_t selected_item,
      blink::Vector<blink::mojom::blink::MenuItemPtr> menu_items,
      bool right_aligned,
      bool allow_multiple_selection) override;
  void CreateNewPopupWidget(
      mojo::PendingAssociatedReceiver<blink::mojom::blink::PopupWidgetHost>
          popup_widget_host,
      mojo::PendingAssociatedReceiver<blink::mojom::blink::WidgetHost> widget_host,
      mojo::PendingAssociatedRemote<blink::mojom::blink::Widget> widget) override;
  void ShowContextMenu(
      mojo::PendingAssociatedRemote<blink::mojom::blink::ContextMenuClient>
          context_menu_client,
      const blink::UntrustworthyContextMenuParams& params) override;
  void DidLoadResourceFromMemoryCache(
      const blink::KURL& url,
      const blink::String& http_method,
      const blink::String& mime_type,
      network::mojom::blink::RequestDestination request_destination,
      bool include_credentials) override;
  void DidChangeFrameOwnerProperties(
      const blink::FrameToken& child_frame_token,
      blink::mojom::blink::FrameOwnerPropertiesPtr frame_owner_properties)
      override;
  void DidChangeOpener(
      const std::optional<blink::LocalFrameToken>& opener_frame) override;
  void DidChangeIframeAttributes(
      const blink::FrameToken& child_frame_token,
      blink::mojom::blink::IframeAttributesPtr) override;
  void DidChangeFramePolicy(const blink::FrameToken& child_frame_token,
                            const blink::FramePolicy& frame_policy) override;
  void CapturePaintPreviewOfSubframe(
      const gfx::Rect& clip_rect,
      const base::UnguessableToken& guid) override;
  void SetCloseListener(
      mojo::PendingRemote<blink::mojom::blink::CloseListener>) override;
  void Detach() override;
  void GetKeepAliveHandleFactory(
      mojo::PendingReceiver<blink::mojom::blink::KeepAliveHandleFactory> receiver)
      override;
  void DidAddMessageToConsole(
      blink::mojom::blink::ConsoleMessageLevel log_level,
      const blink::String& message,
      uint32_t line_no,
      const blink::String& source_id,
      const blink::String& untrusted_stack_trace) override;
  void FrameSizeChanged(const gfx::Size& frame_size) override;
  void DidInferColorScheme(
      blink::mojom::PreferredColorScheme preferred_color_scheme) override;
  void DidChangeSrcDoc(const blink::FrameToken& child_frame_token,
                       const blink::String& srcdoc_value) override;
  void ReceivedDelegatedCapability(
      blink::mojom::DelegatedCapability delegated_capability) override;
  void SendFencedFrameReportingBeacon(
      const blink::String& event_data,
      const blink::String& event_type,
      const blink::Vector<blink::FencedFrame::ReportingDestination>& destinations,
      bool cross_origin_exposed) override;
  void SendFencedFrameReportingBeaconToCustomURL(
      const blink::KURL& destination_url,
      bool cross_origin_exposed) override;
  void SetFencedFrameAutomaticBeaconReportEventData(
      blink::mojom::AutomaticBeaconType event_type,
      const blink::String& event_data,
      const blink::Vector<blink::FencedFrame::ReportingDestination>& destinations,
      bool once,
      bool cross_origin_exposed) override;
  void SendLegacyTechEvent(
      const blink::String& type,
      blink::mojom::blink::LegacyTechEventCodeLocationPtr code_location) override;
  void SendPrivateAggregationRequestsForFencedFrameEvent(
      const blink::String& event_type) override;
  void CreateFencedFrame(
      mojo::PendingAssociatedReceiver<blink::mojom::blink::FencedFrameOwnerHost>,
      blink::mojom::blink::RemoteFrameInterfacesFromRendererPtr
          remote_frame_interfaces,
      const blink::RemoteFrameToken& frame_token,
      const base::UnguessableToken& devtools_frame_token) override;
  void OnViewTransitionOptInChanged(
      blink::mojom::blink::ViewTransitionSameOriginOptIn) override {}
  void StartDragging(const blink::WebDragData& drag_data,
                     blink::DragOperationsMask operations_allowed,
                     const SkBitmap& bitmap,
                     const gfx::Vector2d& cursor_offset_in_dip,
                     const gfx::Rect& drag_obj_rect_in_dip,
                     blink::mojom::blink::DragEventSourceInfoPtr event_info)
      override;
  void IssueKeepAliveHandle(
      mojo::PendingReceiver<blink::mojom::blink::NavigationStateKeepAliveHandle>
          receiver) override;
  void NotifyStorageAccessed(blink::mojom::StorageTypeAccessed storageType,
                             bool blocked) override;
  void RecordWindowProxyUsageMetrics(
      const blink::FrameToken& target_frame_token,
      blink::mojom::WindowProxyAccessType access_type) override;
  void InitializeCrashReportContext(
      uint64_t length,
      InitializeCrashReportContextCallback callback) override;
  void RequestUnboundedSurface(
      mojo::PendingAssociatedReceiver<blink::mojom::blink::UnboundedSurfaceHost>
          host,
      mojo::PendingAssociatedRemote<blink::mojom::blink::UnboundedSurfaceClient>
          client,
      const gfx::Rect& bounds) override {}
  void NotifyDocumentInteractive() override;
};

}  // namespace mb

#endif  // MINIBLINK_HOST_FRAME_MB_LOCAL_FRAME_HOST_H_
