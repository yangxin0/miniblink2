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
#include "third_party/blink/public/common/input/web_mouse_wheel_event.h"
#include "third_party/blink/public/common/input/web_pointer_event.h"
#include "third_party/blink/public/common/input/web_touch_event.h"
#include "third_party/blink/public/platform/web_input_event_result.h"
#include "ui/events/types/scroll_types.h"
#include "third_party/blink/public/mojom/page/widget.mojom-blink.h"
#include "third_party/blink/public/mojom/widget/platform_widget.mojom-blink.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/renderer/core/frame/web_frame_widget_impl.h"
#include "ui/base/ime/ime_text_span.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/range/range.h"
#include "ui/latency/latency_info.h"

namespace mb {

namespace {
// Named non-text keys -> their Win32 VK code, encoded ui::DomKey (KeyboardEvent.key,
// which gates default handlers like Tab focus-advance / Enter submit), and the kChar
// they emit (Enter -> '\r', Tab -> '\t'; 0 = no character). Shared by SendKey (full
// press) and SendKeyUp (release only).
struct KeyDef {
  const char* name;
  int vk;
  ui::DomKey dom_key;
  char16_t ch;
};
constexpr KeyDef kKeys[] = {
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
const KeyDef* FindKeyByName(const char* name) {
  for (const auto& e : kKeys)
    if (std::strcmp(e.name, name) == 0)
      return &e;
  return nullptr;
}
const KeyDef* FindKeyByVk(int vk) {
  for (const auto& e : kKeys)
    if (e.vk == vk)
      return &e;
  return nullptr;
}
}  // namespace

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

void MbWidget::SendRightClick(int x, int y) {
  if (!widget_)
    return;
  auto* impl = static_cast<blink::WebFrameWidgetImpl*>(widget_);
  auto make = [&](blink::WebInputEvent::Type type) {
    blink::WebMouseEvent e(type, blink::WebInputEvent::kNoModifiers,
                           base::TimeTicks::Now());
    e.pointer_type = blink::WebPointerProperties::PointerType::kMouse;
    e.SetPositionInWidget(x, y);
    e.SetPositionInScreen(x, y);
    e.button = blink::WebMouseEvent::Button::kRight;
    e.click_count = 1;
    return e;
  };
  // Right mousedown+up; Blink fires `contextmenu` (right-click menus).
  impl->HandleInputEvent(blink::WebCoalescedInputEvent(
      make(blink::WebInputEvent::Type::kMouseDown), ui::LatencyInfo()));
  impl->HandleInputEvent(blink::WebCoalescedInputEvent(
      make(blink::WebInputEvent::Type::kMouseUp), ui::LatencyInfo()));
}

void MbWidget::SendMouseClickEx(int x, int y, int button, int modifiers) {
  if (!widget_)
    return;
  auto* impl = static_cast<blink::WebFrameWidgetImpl*>(widget_);
  // button: 0=left, 1=middle, 2=right. modifiers bitmask: 1=ctrl 2=shift 4=alt 8=meta.
  blink::WebMouseEvent::Button btn = blink::WebMouseEvent::Button::kLeft;
  if (button == 1)
    btn = blink::WebMouseEvent::Button::kMiddle;
  else if (button == 2)
    btn = blink::WebMouseEvent::Button::kRight;
  int mods = 0;
  if (modifiers & 1)
    mods |= blink::WebInputEvent::kControlKey;
  if (modifiers & 2)
    mods |= blink::WebInputEvent::kShiftKey;
  if (modifiers & 4)
    mods |= blink::WebInputEvent::kAltKey;
  if (modifiers & 8)
    mods |= blink::WebInputEvent::kMetaKey;
  auto make = [&](blink::WebInputEvent::Type type) {
    blink::WebMouseEvent e(type, mods, base::TimeTicks::Now());
    e.pointer_type = blink::WebPointerProperties::PointerType::kMouse;
    e.SetPositionInWidget(x, y);
    e.SetPositionInScreen(x, y);
    e.button = btn;
    e.click_count = 1;
    return e;
  };
  impl->HandleInputEvent(blink::WebCoalescedInputEvent(
      make(blink::WebInputEvent::Type::kMouseDown), ui::LatencyInfo()));
  impl->HandleInputEvent(blink::WebCoalescedInputEvent(
      make(blink::WebInputEvent::Type::kMouseUp), ui::LatencyInfo()));
}

void MbWidget::SendMouseDown(int x, int y) {
  if (!widget_)
    return;
  mouse_pressed_ = true;  // so a subsequent SendMouseMove carries the held button
  auto* impl = static_cast<blink::WebFrameWidgetImpl*>(widget_);
  blink::WebMouseEvent e(blink::WebInputEvent::Type::kMouseDown,
                         blink::WebInputEvent::kLeftButtonDown,
                         base::TimeTicks::Now());
  e.pointer_type = blink::WebPointerProperties::PointerType::kMouse;
  e.SetPositionInWidget(x, y);
  e.SetPositionInScreen(x, y);
  e.button = blink::WebMouseEvent::Button::kLeft;
  e.click_count = 1;
  impl->HandleInputEvent(blink::WebCoalescedInputEvent(e, ui::LatencyInfo()));
}

void MbWidget::SendMouseUp(int x, int y) {
  if (!widget_)
    return;
  mouse_pressed_ = false;
  auto* impl = static_cast<blink::WebFrameWidgetImpl*>(widget_);
  blink::WebMouseEvent e(blink::WebInputEvent::Type::kMouseUp,
                         blink::WebInputEvent::kNoModifiers,  // button released
                         base::TimeTicks::Now());
  e.pointer_type = blink::WebPointerProperties::PointerType::kMouse;
  e.SetPositionInWidget(x, y);
  e.SetPositionInScreen(x, y);
  e.button = blink::WebMouseEvent::Button::kLeft;
  e.click_count = 1;
  impl->HandleInputEvent(blink::WebCoalescedInputEvent(e, ui::LatencyInfo()));
}

void MbWidget::SendMouseMove(int x, int y) {
  if (!widget_)
    return;
  auto* impl = static_cast<blink::WebFrameWidgetImpl*>(widget_);
  // While a button is held (between SendMouseDown and SendMouseUp), carry the
  // left-button mask so drag handlers see e.buttons == 1; otherwise a plain hover.
  blink::WebMouseEvent e(blink::WebInputEvent::Type::kMouseMove,
                         mouse_pressed_ ? blink::WebInputEvent::kLeftButtonDown
                                        : blink::WebInputEvent::kNoModifiers,
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

bool MbWidget::SendWheel(int x, int y, int delta_x, int delta_y, int modifiers) {
  if (!widget_)
    return false;
  auto* impl = static_cast<blink::WebFrameWidgetImpl*>(widget_);
  int mods = 0;
  if (modifiers & 1)
    mods |= blink::WebInputEvent::kControlKey;  // ctrl+wheel = pinch-zoom intent
  if (modifiers & 2)
    mods |= blink::WebInputEvent::kShiftKey;
  if (modifiers & 4)
    mods |= blink::WebInputEvent::kAltKey;
  if (modifiers & 8)
    mods |= blink::WebInputEvent::kMetaKey;
  // blink's delta sign is the NEGATIVE of the DOM `wheel` event's: DOM deltaY>0 means
  // "scroll content down", which is WebMouseWheelEvent.delta_y<0. Negate so the API
  // takes DOM-convention deltas.
  blink::WebMouseWheelEvent e(blink::WebInputEvent::Type::kMouseWheel, mods,
                              base::TimeTicks::Now());
  e.SetPositionInWidget(x, y);
  e.SetPositionInScreen(x, y);
  e.delta_x = -static_cast<float>(delta_x);
  e.delta_y = -static_cast<float>(delta_y);
  e.wheel_ticks_x = -static_cast<float>(delta_x) / 120.0f;
  e.wheel_ticks_y = -static_cast<float>(delta_y) / 120.0f;
  e.delta_units = ui::ScrollGranularity::kScrollByPixel;
  // A NON-phased wheel (kPhaseNone, the classic mouse-wheel style): blink's
  // main-thread EventHandler scrolls it directly. A PHASED wheel (kPhaseBegan)
  // would instead be routed to the compositor's gesture-scroll generator, which is
  // absent in our non-compositing widget -> the event would fire but not scroll.
  e.phase = blink::WebMouseWheelEvent::kPhaseNone;
  e.dispatch_type = blink::WebInputEvent::DispatchType::kBlocking;
  blink::WebInputEventResult r = impl->HandleInputEvent(
      blink::WebCoalescedInputEvent(e, ui::LatencyInfo()));
  // Consumed only if a blocking listener called preventDefault. A passive listener
  // (or none) leaves it kNotHandled -> the caller applies the default scroll.
  return r == blink::WebInputEventResult::kHandledApplication ||
         r == blink::WebInputEventResult::kHandledSystem;
}

bool MbWidget::SendTouchTap(int x, int y) {
  if (!widget_)
    return false;
  auto* impl = static_cast<blink::WebFrameWidgetImpl*>(widget_);
  // TRUSTED pointer events via a WebPointerEvent. (A raw WebTouchEvent would derive touch
  // events too but DCHECKs in this offscreen widget; the caller adds JS-synthesized touch
  // events for Touch-Events UIs.) Dispatch is async via the touch queue; callers pump.
  auto make = [&](blink::WebInputEvent::Type type, bool start) {
    blink::WebPointerEvent e(
        type,
        blink::WebPointerProperties(
            /*id=*/1, blink::WebPointerProperties::PointerType::kTouch,
            blink::WebPointerProperties::Button::kLeft),
        /*width=*/24.0f, /*height=*/24.0f);
    e.SetPositionInWidget(x, y);
    e.SetPositionInScreen(x, y);
    e.hovering = false;
    e.touch_start_or_first_touch_move = start;
    e.dispatch_type = blink::WebInputEvent::DispatchType::kBlocking;
    return e;
  };
  impl->HandleInputEvent(blink::WebCoalescedInputEvent(
      make(blink::WebInputEvent::Type::kPointerDown, /*start=*/true),
      ui::LatencyInfo()));
  impl->HandleInputEvent(blink::WebCoalescedInputEvent(
      make(blink::WebInputEvent::Type::kPointerUp, /*start=*/false),
      ui::LatencyInfo()));
  return true;
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
  // vk = a Windows VK code (drives default actions). dom_key is the encoded
  // ui::DomKey value (NOT the raw code point) — KeyboardEvent.key, which gates page
  // default handlers like Tab focus-advance (key == "Tab"). ch != 0 means the key
  // also produces a kChar (Enter -> '\r', Tab -> '\t').
  const KeyDef* k = FindKeyByName(key_name);
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

void MbWidget::SendKeyUp(int windows_key_code) {
  if (!widget_)
    return;
  // A standalone key RELEASE (kKeyUp) for the given Win32 VK code — page handlers
  // listening for `keyup` (key-release detection, games, shortcut bookkeeping) fire.
  // Derive KeyboardEvent.key (dom_key): a named special key from the table, else the
  // unshifted character for an ASCII letter/digit; e.keyCode comes from the VK.
  ui::DomKey dom = ui::DomKey::NONE;
  if (const KeyDef* k = FindKeyByVk(windows_key_code)) {
    dom = k->dom_key;
  } else if (windows_key_code >= 'A' && windows_key_code <= 'Z') {
    dom = ui::DomKey::FromCharacter(windows_key_code - 'A' + 'a');  // unshifted
  } else if (windows_key_code >= '0' && windows_key_code <= '9') {
    dom = ui::DomKey::FromCharacter(windows_key_code);
  }
  blink::WebKeyboardEvent e(blink::WebInputEvent::Type::kKeyUp,
                            blink::WebInputEvent::kNoModifiers,
                            base::TimeTicks::Now());
  e.windows_key_code = windows_key_code;
  e.dom_key = static_cast<int>(dom);
  auto* impl = static_cast<blink::WebFrameWidgetImpl*>(widget_);
  impl->HandleInputEvent(
      blink::WebCoalescedInputEvent(e, ui::LatencyInfo()));
}

void MbWidget::SendIme(const char* composing, const char* committed) {
  if (!widget_)
    return;
  auto* impl = static_cast<blink::WebFrameWidgetImpl*>(widget_);
  // Drive the focused editable through an IME sequence: SetComposition shows the
  // in-progress reading (compositionstart/compositionupdate, no value commit yet), then
  // CommitText inserts the final text and fires compositionend + input — i.e. text typed
  // via an input method (CJK, accents). Requires a focused editable (focus first).
  if (composing && *composing) {
    const blink::String c = blink::String::FromUtf8(composing);
    impl->SetComposition(c, blink::Vector<ui::ImeTextSpan>(),
                         gfx::Range::InvalidRange(),
                         static_cast<int>(c.length()),
                         static_cast<int>(c.length()),
                         blink::mojom::blink::ImeState::kNone);
  }
  if (committed && *committed) {
    impl->CommitText(blink::String::FromUtf8(committed),
                     blink::Vector<ui::ImeTextSpan>(),
                     gfx::Range::InvalidRange(), /*relative_cursor_pos=*/1);
  }
}

}  // namespace mb
