// mb_widget.cc — non-compositing frame widget. Status: Phase 1.
#include "miniblink_host/widget/mb_widget.h"

#include <tuple>

#include "base/time/time.h"
#include "components/viz/common/surfaces/frame_sink_id.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "third_party/blink/public/common/input/web_coalesced_input_event.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_keyboard_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/public/mojom/page/widget.mojom-blink.h"
#include "third_party/blink/public/mojom/widget/platform_widget.mojom-blink.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/renderer/core/frame/web_frame_widget_impl.h"
#include "ui/gfx/geometry/size.h"
#include "ui/latency/latency_info.h"

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

void MbWidget::SendMouseClick(int x, int y) {
  if (!widget_)
    return;
  auto* impl = static_cast<blink::WebFrameWidgetImpl*>(widget_);
  auto make = [&](blink::WebInputEvent::Type type) {
    blink::WebMouseEvent e(type, blink::WebInputEvent::kNoModifiers,
                           base::TimeTicks::Now());
    e.pointer_type = blink::WebPointerProperties::PointerType::kMouse;
    e.SetPositionInWidget(x, y);
    e.SetPositionInScreen(x, y);
    e.button = blink::WebMouseEvent::Button::kLeft;
    e.click_count = 1;
    return e;
  };
  impl->HandleInputEvent(blink::WebCoalescedInputEvent(
      make(blink::WebInputEvent::Type::kMouseDown), ui::LatencyInfo()));
  impl->HandleInputEvent(blink::WebCoalescedInputEvent(
      make(blink::WebInputEvent::Type::kMouseUp), ui::LatencyInfo()));
}

void MbWidget::SendText(const char* utf8) {
  if (!widget_ || !utf8)
    return;
  auto* impl = static_cast<blink::WebFrameWidgetImpl*>(widget_);
  for (const char* p = utf8; *p; ++p) {
    const char16_t ch = static_cast<unsigned char>(*p);  // ASCII
    int vk = ch;
    if (ch >= 'a' && ch <= 'z')
      vk = ch - 'a' + 'A';  // VK codes use uppercase letters
    auto key = [&](blink::WebInputEvent::Type type, bool with_text) {
      blink::WebKeyboardEvent e(type, blink::WebInputEvent::kNoModifiers,
                                base::TimeTicks::Now());
      e.windows_key_code = vk;
      e.dom_key = ch;
      if (with_text) {
        e.text[0] = ch;
        e.unmodified_text[0] = ch;
      }
      return e;
    };
    impl->HandleInputEvent(blink::WebCoalescedInputEvent(
        key(blink::WebInputEvent::Type::kRawKeyDown, false), ui::LatencyInfo()));
    impl->HandleInputEvent(blink::WebCoalescedInputEvent(
        key(blink::WebInputEvent::Type::kChar, true), ui::LatencyInfo()));
    impl->HandleInputEvent(blink::WebCoalescedInputEvent(
        key(blink::WebInputEvent::Type::kKeyUp, false), ui::LatencyInfo()));
  }
}

}  // namespace mb
