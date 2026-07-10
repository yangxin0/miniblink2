// webview_mac.h — OPTIONAL macOS companion to webview.h: translate real AppKit
// events (NSEvent) into the typed mb* input events, so a host doesn't hand-roll
// the kVK -> Windows-VK table, the RawKeyDown/Char split, or the coordinate
// flip. Header-only (static inline; no new ABI, no ObjC in the engine) — this
// header is the SDK-owned version of the mapping recipe documented at
// mbKeyEvent in webview.h.
//
// Usage (in your NSView subclass; `view` is the mbView*, `self` the NSView):
//
//   - (void)keyDown:(NSEvent*)e   { mbSendKeyNSEvent(view, e); }
//   - (void)keyUp:(NSEvent*)e     { mbSendKeyNSEvent(view, e); }
//   - (void)mouseDown:(NSEvent*)e { mbSendMouseNSEvent(view, e, self); }
//   - (void)mouseUp:(NSEvent*)e   { mbSendMouseNSEvent(view, e, self); }
//   - (void)mouseMoved:(NSEvent*)e{ mbSendMouseNSEvent(view, e, self); }
//   - (void)mouseDragged:(NSEvent*)e { mbSendMouseNSEvent(view, e, self); }
//   - (void)scrollWheel:(NSEvent*)e  { mbSendWheelNSEvent(view, e, self); }
//
// Coordinates are converted to view-local, top-left-origin LOGICAL px — the
// coordinate space every mb* input call expects. Keyboard events follow the
// browser recipe: keyDown becomes MB_KEY_RAW_DOWN, then MB_KEY_CHAR when the
// key produces text and no command/control chord is held; keyUp becomes
// MB_KEY_UP. flagsChanged (bare modifier presses) is not translated — blink
// receives modifiers on the events that carry them.

#ifndef MINIBLINK2_WEBVIEW_MAC_H_
#define MINIBLINK2_WEBVIEW_MAC_H_

#if !defined(__OBJC__) || !defined(__APPLE__)
#error "webview_mac.h is an Objective-C (macOS) header; include it from .m/.mm files only."
#endif

#import <AppKit/AppKit.h>

#include "webview.h"

// ---- Key code translation ----------------------------------------------------
// Windows virtual-key code for a mac hardware key code (NSEvent.keyCode /
// Carbon kVK_*). The classic WebKit/Chromium mapping; 0 for unmapped keys.
static inline int mbWindowsKeyCodeForMacKeyCode(unsigned short key_code) {
  static const int kMap[128] = {
      /* 0x00 kVK_ANSI_A            */ 'A',
      /* 0x01 kVK_ANSI_S            */ 'S',
      /* 0x02 kVK_ANSI_D            */ 'D',
      /* 0x03 kVK_ANSI_F            */ 'F',
      /* 0x04 kVK_ANSI_H            */ 'H',
      /* 0x05 kVK_ANSI_G            */ 'G',
      /* 0x06 kVK_ANSI_Z            */ 'Z',
      /* 0x07 kVK_ANSI_X            */ 'X',
      /* 0x08 kVK_ANSI_C            */ 'C',
      /* 0x09 kVK_ANSI_V            */ 'V',
      /* 0x0A kVK_ISO_Section       */ 0xC0 /* VK_OEM_3 */,
      /* 0x0B kVK_ANSI_B            */ 'B',
      /* 0x0C kVK_ANSI_Q            */ 'Q',
      /* 0x0D kVK_ANSI_W            */ 'W',
      /* 0x0E kVK_ANSI_E            */ 'E',
      /* 0x0F kVK_ANSI_R            */ 'R',
      /* 0x10 kVK_ANSI_Y            */ 'Y',
      /* 0x11 kVK_ANSI_T            */ 'T',
      /* 0x12 kVK_ANSI_1            */ '1',
      /* 0x13 kVK_ANSI_2            */ '2',
      /* 0x14 kVK_ANSI_3            */ '3',
      /* 0x15 kVK_ANSI_4            */ '4',
      /* 0x16 kVK_ANSI_6            */ '6',
      /* 0x17 kVK_ANSI_5            */ '5',
      /* 0x18 kVK_ANSI_Equal        */ 0xBB /* VK_OEM_PLUS */,
      /* 0x19 kVK_ANSI_9            */ '9',
      /* 0x1A kVK_ANSI_7            */ '7',
      /* 0x1B kVK_ANSI_Minus        */ 0xBD /* VK_OEM_MINUS */,
      /* 0x1C kVK_ANSI_8            */ '8',
      /* 0x1D kVK_ANSI_0            */ '0',
      /* 0x1E kVK_ANSI_RightBracket */ 0xDD /* VK_OEM_6 */,
      /* 0x1F kVK_ANSI_O            */ 'O',
      /* 0x20 kVK_ANSI_U            */ 'U',
      /* 0x21 kVK_ANSI_LeftBracket  */ 0xDB /* VK_OEM_4 */,
      /* 0x22 kVK_ANSI_I            */ 'I',
      /* 0x23 kVK_ANSI_P            */ 'P',
      /* 0x24 kVK_Return            */ 0x0D /* VK_RETURN */,
      /* 0x25 kVK_ANSI_L            */ 'L',
      /* 0x26 kVK_ANSI_J            */ 'J',
      /* 0x27 kVK_ANSI_Quote        */ 0xDE /* VK_OEM_7 */,
      /* 0x28 kVK_ANSI_K            */ 'K',
      /* 0x29 kVK_ANSI_Semicolon    */ 0xBA /* VK_OEM_1 */,
      /* 0x2A kVK_ANSI_Backslash    */ 0xDC /* VK_OEM_5 */,
      /* 0x2B kVK_ANSI_Comma        */ 0xBC /* VK_OEM_COMMA */,
      /* 0x2C kVK_ANSI_Slash        */ 0xBF /* VK_OEM_2 */,
      /* 0x2D kVK_ANSI_N            */ 'N',
      /* 0x2E kVK_ANSI_M            */ 'M',
      /* 0x2F kVK_ANSI_Period       */ 0xBE /* VK_OEM_PERIOD */,
      /* 0x30 kVK_Tab               */ 0x09 /* VK_TAB */,
      /* 0x31 kVK_Space             */ 0x20 /* VK_SPACE */,
      /* 0x32 kVK_ANSI_Grave        */ 0xC0 /* VK_OEM_3 */,
      /* 0x33 kVK_Delete (backspace)*/ 0x08 /* VK_BACK */,
      /* 0x34 (enter, powerbook)    */ 0x0D /* VK_RETURN */,
      /* 0x35 kVK_Escape            */ 0x1B /* VK_ESCAPE */,
      /* 0x36 kVK_RightCommand      */ 0x5C /* VK_RWIN */,
      /* 0x37 kVK_Command           */ 0x5B /* VK_LWIN */,
      /* 0x38 kVK_Shift             */ 0x10 /* VK_SHIFT */,
      /* 0x39 kVK_CapsLock          */ 0x14 /* VK_CAPITAL */,
      /* 0x3A kVK_Option            */ 0x12 /* VK_MENU */,
      /* 0x3B kVK_Control           */ 0x11 /* VK_CONTROL */,
      /* 0x3C kVK_RightShift        */ 0x10 /* VK_SHIFT */,
      /* 0x3D kVK_RightOption       */ 0x12 /* VK_MENU */,
      /* 0x3E kVK_RightControl      */ 0x11 /* VK_CONTROL */,
      /* 0x3F kVK_Function          */ 0,
      /* 0x40 kVK_F17               */ 0x80 /* VK_F17 */,
      /* 0x41 kVK_ANSI_KeypadDecimal*/ 0x6E /* VK_DECIMAL */,
      /* 0x42                       */ 0,
      /* 0x43 kVK_ANSI_KeypadMultiply*/ 0x6A /* VK_MULTIPLY */,
      /* 0x44                       */ 0,
      /* 0x45 kVK_ANSI_KeypadPlus   */ 0x6B /* VK_ADD */,
      /* 0x46                       */ 0,
      /* 0x47 kVK_ANSI_KeypadClear  */ 0x0C /* VK_CLEAR */,
      /* 0x48 kVK_VolumeUp          */ 0xAF /* VK_VOLUME_UP */,
      /* 0x49 kVK_VolumeDown        */ 0xAE /* VK_VOLUME_DOWN */,
      /* 0x4A kVK_Mute              */ 0xAD /* VK_VOLUME_MUTE */,
      /* 0x4B kVK_ANSI_KeypadDivide */ 0x6F /* VK_DIVIDE */,
      /* 0x4C kVK_ANSI_KeypadEnter  */ 0x0D /* VK_RETURN */,
      /* 0x4D                       */ 0,
      /* 0x4E kVK_ANSI_KeypadMinus  */ 0x6D /* VK_SUBTRACT */,
      /* 0x4F kVK_F18               */ 0x81 /* VK_F18 */,
      /* 0x50 kVK_F19               */ 0x82 /* VK_F19 */,
      /* 0x51 kVK_ANSI_KeypadEquals */ 0xBB /* VK_OEM_PLUS */,
      /* 0x52 kVK_ANSI_Keypad0      */ 0x60 /* VK_NUMPAD0 */,
      /* 0x53 kVK_ANSI_Keypad1      */ 0x61,
      /* 0x54 kVK_ANSI_Keypad2      */ 0x62,
      /* 0x55 kVK_ANSI_Keypad3      */ 0x63,
      /* 0x56 kVK_ANSI_Keypad4      */ 0x64,
      /* 0x57 kVK_ANSI_Keypad5      */ 0x65,
      /* 0x58 kVK_ANSI_Keypad6      */ 0x66,
      /* 0x59 kVK_ANSI_Keypad7      */ 0x67,
      /* 0x5A kVK_F20               */ 0x83 /* VK_F20 */,
      /* 0x5B kVK_ANSI_Keypad8      */ 0x68,
      /* 0x5C kVK_ANSI_Keypad9      */ 0x69 /* VK_NUMPAD9 */,
      /* 0x5D (yen, JIS)            */ 0xDC /* VK_OEM_5 */,
      /* 0x5E (underscore, JIS)     */ 0xBD /* VK_OEM_MINUS */,
      /* 0x5F (keypad comma, JIS)   */ 0xBC /* VK_OEM_COMMA */,
      /* 0x60 kVK_F5                */ 0x74 /* VK_F5 */,
      /* 0x61 kVK_F6                */ 0x75,
      /* 0x62 kVK_F7                */ 0x76,
      /* 0x63 kVK_F3                */ 0x72,
      /* 0x64 kVK_F8                */ 0x77,
      /* 0x65 kVK_F9                */ 0x78,
      /* 0x66 (eisu, JIS)           */ 0,
      /* 0x67 kVK_F11               */ 0x7A,
      /* 0x68 (kana, JIS)           */ 0,
      /* 0x69 kVK_F13               */ 0x7C,
      /* 0x6A kVK_F16               */ 0x7F,
      /* 0x6B kVK_F14               */ 0x7D,
      /* 0x6C                       */ 0,
      /* 0x6D kVK_F10               */ 0x79,
      /* 0x6E (menu/apps)           */ 0x5D /* VK_APPS */,
      /* 0x6F kVK_F12               */ 0x7B,
      /* 0x70                       */ 0,
      /* 0x71 kVK_F15               */ 0x7E,
      /* 0x72 kVK_Help              */ 0x2F /* VK_HELP */,
      /* 0x73 kVK_Home              */ 0x24 /* VK_HOME */,
      /* 0x74 kVK_PageUp            */ 0x21 /* VK_PRIOR */,
      /* 0x75 kVK_ForwardDelete     */ 0x2E /* VK_DELETE */,
      /* 0x76 kVK_F4                */ 0x73,
      /* 0x77 kVK_End               */ 0x23 /* VK_END */,
      /* 0x78 kVK_F2                */ 0x71,
      /* 0x79 kVK_PageDown          */ 0x22 /* VK_NEXT */,
      /* 0x7A kVK_F1                */ 0x70,
      /* 0x7B kVK_LeftArrow         */ 0x25 /* VK_LEFT */,
      /* 0x7C kVK_RightArrow        */ 0x27 /* VK_RIGHT */,
      /* 0x7D kVK_DownArrow         */ 0x28 /* VK_DOWN */,
      /* 0x7E kVK_UpArrow           */ 0x26 /* VK_UP */,
      /* 0x7F                       */ 0,
  };
  return key_code < 128 ? kMap[key_code] : 0;
}

// mb* modifier bitmask (1 ctrl, 2 shift, 4 alt, 8 meta) from NSEvent flags.
static inline int mbModifiersFromNSFlags(NSEventModifierFlags flags) {
  int m = 0;
  if (flags & NSEventModifierFlagControl) m |= 1;
  if (flags & NSEventModifierFlagShift) m |= 2;
  if (flags & NSEventModifierFlagOption) m |= 4;
  if (flags & NSEventModifierFlagCommand) m |= 8;
  return m;
}

// ---- Keyboard ------------------------------------------------------------------
// mbKeyEvent plus the storage its text pointers reference (mbKeyEvent holds
// const char*; the struct keeps the copies alive until you hand it to
// mbSendKeyEvent).
typedef struct mbNSKeyEvent {
  mbKeyEvent event;
  char text[16];
  char unmodified_text[16];
} mbNSKeyEvent;

// Fill `out` from a keyDown/keyUp NSEvent. keyDown maps to MB_KEY_RAW_DOWN
// (send an MB_KEY_CHAR after it yourself, or use mbSendKeyNSEvent which does
// the full recipe); keyUp maps to MB_KEY_UP. Returns 1 on success, 0 for
// event types this helper does not translate (flagsChanged etc.).
static inline int mbKeyEventFromNSEvent(NSEvent* evt, mbNSKeyEvent* out) {
  if (!evt || !out) return 0;
  NSEventType t = evt.type;
  if (t != NSEventTypeKeyDown && t != NSEventTypeKeyUp) return 0;
  memset(out, 0, sizeof(*out));
  out->event.struct_size = (int)sizeof(mbKeyEvent);
  out->event.type = (t == NSEventTypeKeyDown) ? MB_KEY_RAW_DOWN : MB_KEY_UP;
  out->event.modifiers = mbModifiersFromNSFlags(evt.modifierFlags);
  out->event.windows_key_code = mbWindowsKeyCodeForMacKeyCode(evt.keyCode);
  out->event.native_key_code = evt.keyCode;
  out->event.is_keypad = (evt.modifierFlags & NSEventModifierFlagNumericPad) ? 1 : 0;
  out->event.is_auto_repeat = evt.ARepeat ? 1 : 0;
  const char* chars = evt.characters.UTF8String;
  const char* unmod = evt.charactersIgnoringModifiers.UTF8String;
  if (chars && chars[0]) {
    strlcpy(out->text, chars, sizeof(out->text));
    out->event.text = out->text;
  }
  if (unmod && unmod[0]) {
    strlcpy(out->unmodified_text, unmod, sizeof(out->unmodified_text));
    out->event.unmodified_text = out->unmodified_text;
  }
  return 1;
}

// 1 when a keyDown's characters are real typed text (not a control/function
// key code point in the 0xF700 PUA range or an ASCII control char).
static inline int mbNSEventProducesText(NSEvent* evt) {
  NSString* s = evt.characters;
  if (s.length == 0) return 0;
  unichar c = [s characterAtIndex:0];
  if (c >= 0xF700 && c <= 0xF8FF) return 0;  // NSUpArrowFunctionKey etc.
  if (c < 0x20 || c == 0x7F) {
    // Control characters: let Enter and Tab through as text (blink inserts
    // the newline/tab from the Char event), drop the rest.
    if (c != '\r' && c != '\t') return 0;
  }
  return 1;
}

// The full browser recipe for one NSEvent:
//   keyDown -> RAW_DOWN, then CHAR when the key produces text and no
//              command/control chord is held (chords are shortcuts, not text);
//   keyUp   -> UP.
static inline void mbSendKeyNSEvent(mbView* view, NSEvent* evt) {
  mbNSKeyEvent e;
  if (!mbKeyEventFromNSEvent(evt, &e)) return;
  mbSendKeyEvent(view, &e.event);
  if (e.event.type == MB_KEY_RAW_DOWN && mbNSEventProducesText(evt) &&
      !(evt.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl))) {
    e.event.type = MB_KEY_CHAR;
    mbSendKeyEvent(view, &e.event);
  }
}

// ---- Mouse ---------------------------------------------------------------------
// Convert an NSEvent location to view-local, top-left-origin logical px.
static inline NSPoint mbViewLocalPointForNSEvent(NSEvent* evt, NSView* in_view) {
  NSPoint p = [in_view convertPoint:evt.locationInWindow fromView:nil];
  if (!in_view.isFlipped) p.y = in_view.bounds.size.height - p.y;
  return p;
}

// Fill `out` from a mouse down/up/move/drag NSEvent. AppKit button numbers
// (0 left, 1 right, 2 middle) are remapped to the mb* convention (0 left,
// 1 middle, 2 right). Returns 1 on success, 0 for untranslated event types.
static inline int mbMouseEventFromNSEvent(NSEvent* evt, NSView* in_view,
                                          mbMouseEvent* out) {
  if (!evt || !in_view || !out) return 0;
  int type;
  int button = 0;
  switch (evt.type) {
    case NSEventTypeLeftMouseDown: type = MB_MOUSE_DOWN; button = 0; break;
    case NSEventTypeLeftMouseUp: type = MB_MOUSE_UP; button = 0; break;
    case NSEventTypeRightMouseDown: type = MB_MOUSE_DOWN; button = 2; break;
    case NSEventTypeRightMouseUp: type = MB_MOUSE_UP; button = 2; break;
    case NSEventTypeOtherMouseDown: type = MB_MOUSE_DOWN; button = 1; break;
    case NSEventTypeOtherMouseUp: type = MB_MOUSE_UP; button = 1; break;
    case NSEventTypeMouseMoved:
    case NSEventTypeLeftMouseDragged:
    case NSEventTypeRightMouseDragged:
    case NSEventTypeOtherMouseDragged: type = MB_MOUSE_MOVE; break;
    default: return 0;
  }
  memset(out, 0, sizeof(*out));
  out->struct_size = (int)sizeof(mbMouseEvent);
  out->type = type;
  NSPoint p = mbViewLocalPointForNSEvent(evt, in_view);
  out->x = (int)p.x;
  out->y = (int)p.y;
  out->button = button;
  out->click_count = (type == MB_MOUSE_MOVE) ? 0 : (int)evt.clickCount;
  out->modifiers = mbModifiersFromNSFlags(evt.modifierFlags);
  return 1;
}

static inline void mbSendMouseNSEvent(mbView* view, NSEvent* evt, NSView* in_view) {
  mbMouseEvent e;
  if (mbMouseEventFromNSEvent(evt, in_view, &e)) mbSendMouseEvent(view, &e);
}

// ---- Wheel ---------------------------------------------------------------------
// Fill `out` from a scrollWheel NSEvent. AppKit deltas are "content moves
// with the gesture" (scroll up = positive); the DOM convention is the
// opposite (deltaY > 0 = content moves down), so the deltas are negated.
// Line-based (non-precise) wheels are scaled to ~40 px per line.
static inline int mbWheelEventFromNSEvent(NSEvent* evt, NSView* in_view,
                                          mbWheelEvent* out) {
  if (!evt || !in_view || !out || evt.type != NSEventTypeScrollWheel) return 0;
  memset(out, 0, sizeof(*out));
  out->struct_size = (int)sizeof(mbWheelEvent);
  NSPoint p = mbViewLocalPointForNSEvent(evt, in_view);
  out->x = (int)p.x;
  out->y = (int)p.y;
  if (evt.hasPreciseScrollingDeltas) {
    out->precise = 1;
    out->delta_x = (float)-evt.scrollingDeltaX;
    out->delta_y = (float)-evt.scrollingDeltaY;
  } else {
    out->delta_x = (float)(-evt.scrollingDeltaX * 40.0);
    out->delta_y = (float)(-evt.scrollingDeltaY * 40.0);
  }
  out->modifiers = mbModifiersFromNSFlags(evt.modifierFlags);
  return 1;
}

// Fire the wheel; returns 1 when the page consumed it (preventDefault) — the
// host then suppresses any default scroll behavior of its own.
static inline int mbSendWheelNSEvent(mbView* view, NSEvent* evt, NSView* in_view) {
  mbWheelEvent e;
  if (!mbWheelEventFromNSEvent(evt, in_view, &e)) return 0;
  return mbSendWheelEvent(view, &e);
}

#endif  // MINIBLINK2_WEBVIEW_MAC_H_
