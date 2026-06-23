// mb_widget.cc — non-compositing frame widget. Status: Phase 1.
#include "miniblink_host/widget/mb_widget.h"

#include <tuple>

#include "components/viz/common/surfaces/frame_sink_id.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "third_party/blink/public/mojom/page/widget.mojom-blink.h"
#include "third_party/blink/public/mojom/widget/platform_widget.mojom-blink.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "ui/gfx/geometry/size.h"

namespace mb {

MbWidget::MbWidget() = default;
MbWidget::~MbWidget() = default;

void MbWidget::Attach(blink::WebLocalFrame* main_frame, int width, int height) {
  // Create four associated channels. Blink keeps the FrameWidget/Widget receivers
  // and the *Host remotes; the browser-side ends (FrameWidget/Widget remotes and
  // *Host receivers) are simply dropped — no browser process services them, which is
  // fine for offscreen rendering (host calls are silently no-ops).
  mojo::AssociatedRemote<blink::mojom::blink::FrameWidget> frame_widget_remote;
  auto frame_widget_receiver =
      frame_widget_remote.BindNewEndpointAndPassDedicatedReceiver();

  mojo::AssociatedRemote<blink::mojom::blink::FrameWidgetHost> frame_widget_host;
  std::ignore = frame_widget_host.BindNewEndpointAndPassDedicatedReceiver();

  mojo::AssociatedRemote<blink::mojom::blink::Widget> widget_remote;
  auto widget_receiver =
      widget_remote.BindNewEndpointAndPassDedicatedReceiver();

  mojo::AssociatedRemote<blink::mojom::blink::WidgetHost> widget_host;
  std::ignore = widget_host.BindNewEndpointAndPassDedicatedReceiver();

  widget_ = main_frame->InitializeFrameWidget(
      frame_widget_host.Unbind(), std::move(frame_widget_receiver),
      widget_host.Unbind(), std::move(widget_receiver),
      viz::FrameSinkId(1, 1));

  widget_->InitializeNonCompositing(this);
  widget_->Resize(gfx::Size(width, height));
}

void MbWidget::Resize(int width, int height) {
  if (widget_)
    widget_->Resize(gfx::Size(width, height));
}

}  // namespace mb
