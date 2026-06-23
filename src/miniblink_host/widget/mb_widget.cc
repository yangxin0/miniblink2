// mb_widget.cc — non-compositing frame widget. Status: Phase 1.
#include "miniblink_host/widget/mb_widget.h"

#include <cstring>
#include <string>
#include <string_view>
#include <tuple>

#include "base/strings/utf_string_conversions.h"
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
#include "ui/events/keycodes/dom/dom_key.h"
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

void MbWidget::SendDoubleClick(int x, int y) {
  if (!widget_)
    return;
  auto* impl = static_cast<blink::WebFrameWidgetImpl*>(widget_);
  auto make = [&](blink::WebInputEvent::Type type, int click_count) {
    blink::WebMouseEvent e(type, blink::WebInputEvent::kNoModifiers,
                           base::TimeTicks::Now());
    e.pointer_type = blink::WebPointerProperties::PointerType::kMouse;
    e.SetPositionInWidget(x, y);
    e.SetPositionInScreen(x, y);
    e.button = blink::WebMouseEvent::Button::kLeft;
    e.click_count = click_count;
    return e;
  };
  // First click (count 1), then a second at count 2 — Blink emits `dblclick` on
  // the count-2 mouseup.
  impl->HandleInputEvent(blink::WebCoalescedInputEvent(
      make(blink::WebInputEvent::Type::kMouseDown, 1), ui::LatencyInfo()));
  impl->HandleInputEvent(blink::WebCoalescedInputEvent(
      make(blink::WebInputEvent::Type::kMouseUp, 1), ui::LatencyInfo()));
  impl->HandleInputEvent(blink::WebCoalescedInputEvent(
      make(blink::WebInputEvent::Type::kMouseDown, 2), ui::LatencyInfo()));
  impl->HandleInputEvent(blink::WebCoalescedInputEvent(
      make(blink::WebInputEvent::Type::kMouseUp, 2), ui::LatencyInfo()));
}

void MbWidget::SendMouseMove(int x, int y) {
  if (!widget_)
    return;
  auto* impl = static_cast<blink::WebFrameWidgetImpl*>(widget_);
  blink::WebMouseEvent e(blink::WebInputEvent::Type::kMouseMove,
                         blink::WebInputEvent::kNoModifiers,
                         base::TimeTicks::Now());
  e.pointer_type = blink::WebPointerProperties::PointerType::kMouse;
  e.SetPositionInWidget(x, y);
  e.SetPositionInScreen(x, y);
  e.button = blink::WebMouseEvent::Button::kNoButton;
  // Hit-test + :hover/:active recalculation runs on the main thread inside the
  // event handler, so this updates hover state and fires mouseover/mousemove
  // without a compositor.
  impl->HandleInputEvent(blink::WebCoalescedInputEvent(e, ui::LatencyInfo()));
}

void MbWidget::SendText(const char* utf8) {
  if (!widget_ || !utf8)
    return;
  auto* impl = static_cast<blink::WebFrameWidgetImpl*>(widget_);
  // Decode UTF-8 to UTF-16 so non-ASCII text (accents, CJK, emoji) types
  // correctly. Iterate by code point: a supplementary character is a UTF-16
  // surrogate pair that must travel in one kChar event's text[] together.
  const std::u16string u16 = base::UTF8ToUTF16(std::string_view(utf8));
  for (size_t i = 0; i < u16.size();) {
    const char16_t hi = u16[i];
    const bool is_pair = hi >= 0xD800 && hi <= 0xDBFF && i + 1 < u16.size() &&
                         u16[i + 1] >= 0xDC00 && u16[i + 1] <= 0xDFFF;
    const size_t units = is_pair ? 2 : 1;

    // windows_key_code is a VK code (ASCII letters use the uppercase form);
    // it's only meaningful for ASCII. For non-ASCII the text[] field below is
    // what drives insertion, so leaving the VK code as the raw unit is fine.
    int vk = (hi >= 'a' && hi <= 'z') ? (hi - 'a' + 'A') : hi;
    auto key = [&](blink::WebInputEvent::Type type, bool with_text) {
      blink::WebKeyboardEvent e(type, blink::WebInputEvent::kNoModifiers,
                                base::TimeTicks::Now());
      e.windows_key_code = vk;
      e.dom_key = hi;
      if (with_text) {
        for (size_t k = 0; k < units; ++k) {
          e.text[k] = u16[i + k];
          e.unmodified_text[k] = u16[i + k];
        }
      }
      return e;
    };
    impl->HandleInputEvent(blink::WebCoalescedInputEvent(
        key(blink::WebInputEvent::Type::kRawKeyDown, false), ui::LatencyInfo()));
    impl->HandleInputEvent(blink::WebCoalescedInputEvent(
        key(blink::WebInputEvent::Type::kChar, true), ui::LatencyInfo()));
    impl->HandleInputEvent(blink::WebCoalescedInputEvent(
        key(blink::WebInputEvent::Type::kKeyUp, false), ui::LatencyInfo()));
    i += units;
  }
}

void MbWidget::SendKey(const char* key_name) {
  if (!widget_ || !key_name)
    return;
  // vk = a Windows VK code (drives default actions). dom_key must be the encoded
  // ui::DomKey value (NOT the raw code point) — KeyboardEvent.key, which gates
  // page default handlers like Tab focus-advance (key == "Tab"), is derived from
  // it. ch != 0 means the key also produces a kChar (Enter -> '\r', Tab -> '\t').
  struct KeyDef {
    const char* name;
    int vk;
    ui::DomKey dom_key;
    char16_t ch;
  };
  static const KeyDef kKeys[] = {
      {"Enter", 0x0D, ui::DomKey::ENTER, u'\r'},
      {"Tab", 0x09, ui::DomKey::TAB, u'\t'},
      {"Escape", 0x1B, ui::DomKey::ESCAPE, 0},
      {"Backspace", 0x08, ui::DomKey::BACKSPACE, 0},
      {"Delete", 0x2E, ui::DomKey::DEL, 0},
      {"ArrowLeft", 0x25, ui::DomKey::ARROW_LEFT, 0},
      {"ArrowUp", 0x26, ui::DomKey::ARROW_UP, 0},
      {"ArrowRight", 0x27, ui::DomKey::ARROW_RIGHT, 0},
      {"ArrowDown", 0x28, ui::DomKey::ARROW_DOWN, 0},
      {"Home", 0x24, ui::DomKey::HOME, 0},
      {"End", 0x23, ui::DomKey::END, 0},
      {"PageUp", 0x21, ui::DomKey::PAGE_UP, 0},
      {"PageDown", 0x22, ui::DomKey::PAGE_DOWN, 0},
  };
  const KeyDef* k = nullptr;
  for (const auto& e : kKeys) {
    if (std::strcmp(e.name, key_name) == 0) {
      k = &e;
      break;
    }
  }
  if (!k)
    return;
  auto* impl = static_cast<blink::WebFrameWidgetImpl*>(widget_);
  auto make = [&](blink::WebInputEvent::Type type, bool with_text) {
    blink::WebKeyboardEvent e(type, blink::WebInputEvent::kNoModifiers,
                              base::TimeTicks::Now());
    e.windows_key_code = k->vk;
    e.dom_key = static_cast<int>(k->dom_key);
    if (with_text && k->ch) {
      e.text[0] = k->ch;
      e.unmodified_text[0] = k->ch;
    }
    return e;
  };
  impl->HandleInputEvent(blink::WebCoalescedInputEvent(
      make(blink::WebInputEvent::Type::kRawKeyDown, false), ui::LatencyInfo()));
  if (k->ch) {
    impl->HandleInputEvent(blink::WebCoalescedInputEvent(
        make(blink::WebInputEvent::Type::kChar, true), ui::LatencyInfo()));
  }
  impl->HandleInputEvent(blink::WebCoalescedInputEvent(
      make(blink::WebInputEvent::Type::kKeyUp, false), ui::LatencyInfo()));
}

}  // namespace mb
