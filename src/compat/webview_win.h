// webview_win.h — Windows input glue for mb hosts: translate Win32 window-
// message input (WM_KEY*/WM_*BUTTON*/WM_MOUSEWHEEL) into the typed mb* input
// events, so a host doesn't hand-roll the message decode, the RawKeyDown/Char
// split, or the wheel-delta conversion. Header-only (static inline; no new
// ABI, no Win32 types in the engine).
//
// Like samples/compat/mb_window.h, this is an INTERNAL repo helper (src/compat/)
// — it is NOT part of the shipped SDK; copy it into your host. It is the
// Windows peer of src/compat/webview_mac.h.
//
// Usage (in your WndProc; `view` is the mbView*, `hwnd` the HWND):
//
//   case WM_KEYDOWN: case WM_SYSKEYDOWN:
//   case WM_KEYUP:   case WM_SYSKEYUP:
//   case WM_CHAR:    case WM_SYSCHAR: {
//     mbKeyEvent e;
//     if (mbKeyEventFromWin32(msg, wParam, lParam, &e)) mbSendKeyEvent(view, &e);
//     break;
//   }
//   case WM_MOUSEMOVE:
//   case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
//   case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
//   case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK: {
//     mbMouseEvent e;
//     if (mbMouseEventFromWin32(msg, wParam, lParam, &e)) mbSendMouseEvent(view, &e);
//     break;
//   }
//   case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
//     mbWheelEvent e;
//     if (mbWheelEventFromWin32(msg, wParam, lParam, hwnd, &e))
//       mbSendWheelEvent(view, &e);
//     break;
//   }
//
// Win32 client coordinates are already top-left-origin LOGICAL px (the space
// every mb* input call expects), except WM_MOUSEWHEEL/WM_MOUSEHWHEEL carry
// SCREEN coordinates — those are mapped back to client space via `hwnd`. The
// keyboard path follows the browser recipe: WM_KEYDOWN/WM_SYSKEYDOWN become
// MB_KEY_RAW_DOWN, WM_CHAR/WM_SYSCHAR become MB_KEY_CHAR (Windows already
// splits a key press into these two messages — send both through as they
// arrive), and WM_KEYUP/WM_SYSKEYUP become MB_KEY_UP.

#ifndef MINIBLINK2_WEBVIEW_WIN_H_
#define MINIBLINK2_WEBVIEW_WIN_H_

#if !defined(_WIN32)
#error "webview_win.h is a Windows header; include it only in Windows builds."
#endif

#include <windows.h>
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

#include "miniblink2/webview.h"

// mb* modifier bitmask (1 ctrl, 2 shift, 4 alt, 8 meta) from the current async
// key state (Win32 key messages don't carry a modifier field, unlike mouse).
static __inline int mbModifiersFromWin32KeyState(void) {
  int m = 0;
  if (GetKeyState(VK_CONTROL) & 0x8000) m |= 1;
  if (GetKeyState(VK_SHIFT) & 0x8000) m |= 2;
  if (GetKeyState(VK_MENU) & 0x8000) m |= 4;  // Alt
  if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000))
    m |= 8;
  return m;
}

// mb* modifier bitmask from a mouse message's wParam (MK_* flags) plus Alt/Meta
// (which mouse messages don't report) read from the key state.
static __inline int mbModifiersFromWin32Mouse(WPARAM wParam) {
  int m = 0;
  if (wParam & MK_CONTROL) m |= 1;
  if (wParam & MK_SHIFT) m |= 2;
  if (GetKeyState(VK_MENU) & 0x8000) m |= 4;
  if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000))
    m |= 8;
  return m;
}

// ---- Keyboard ------------------------------------------------------------------
// mbKeyEvent plus the storage its text pointer references (mbKeyEvent holds a
// const char*; keep the copy alive until you hand it to mbSendKeyEvent). The
// Win32 message split maps 1:1 to the mb key types, so — unlike the mac helper
// — there is no CHAR synthesis here: WM_CHAR already IS the char event.
typedef struct mbWinKeyEvent {
  mbKeyEvent event;
  char text[8];  // UTF-8 of a WM_CHAR code unit
} mbWinKeyEvent;

// Fill `out` from a Win32 keyboard message. Returns 1 on success, 0 for a
// message this helper does not translate.
//   WM_KEYDOWN / WM_SYSKEYDOWN -> MB_KEY_RAW_DOWN (wParam is the VK code)
//   WM_KEYUP   / WM_SYSKEYUP   -> MB_KEY_UP
//   WM_CHAR    / WM_SYSCHAR    -> MB_KEY_CHAR    (wParam is a UTF-16 unit)
static __inline int mbKeyEventFromWin32Ex(UINT message, WPARAM wParam,
                                          LPARAM lParam, mbWinKeyEvent* out) {
  if (!out)
    return 0;
  memset(out, 0, sizeof(*out));
  out->event.struct_size = (int)sizeof(mbKeyEvent);
  out->event.modifiers = mbModifiersFromWin32KeyState();
  // lParam: bits 16-23 scan code, bit 24 extended, bit 30 previous-down
  // (auto-repeat for a keydown), bit 29 context (Alt held -> system key).
  out->event.native_key_code = (int)((lParam >> 16) & 0xff);
  out->event.is_auto_repeat = (message == WM_KEYDOWN || message == WM_SYSKEYDOWN)
                                  ? (int)((lParam >> 30) & 0x1)
                                  : 0;
  out->event.is_system_key =
      (message == WM_SYSKEYDOWN || message == WM_SYSKEYUP ||
       message == WM_SYSCHAR);
  // Keypad: the numeric-pad VKs, or an extended-flag Enter (keypad Enter).
  const int extended = (int)((lParam >> 24) & 0x1);
  switch (message) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
      out->event.type = MB_KEY_RAW_DOWN;
      out->event.windows_key_code = (int)wParam;
      break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
      out->event.type = MB_KEY_UP;
      out->event.windows_key_code = (int)wParam;
      break;
    case WM_CHAR:
    case WM_SYSCHAR: {
      out->event.type = MB_KEY_CHAR;
      // wParam is one UTF-16 code unit; encode BMP characters as UTF-8. (A
      // surrogate pair arrives as two WM_CHARs — each is passed through; blink
      // reassembles from the byte stream.)
      WCHAR wc = (WCHAR)wParam;
      int n = WideCharToMultiByte(CP_UTF8, 0, &wc, 1, out->text,
                                  (int)sizeof(out->text) - 1, NULL, NULL);
      if (n > 0) {
        out->text[n] = '\0';
        out->event.text = out->text;
        out->event.unmodified_text = out->text;
      }
      break;
    }
    default:
      return 0;
  }
  out->event.is_keypad =
      ((wParam >= VK_NUMPAD0 && wParam <= VK_DIVIDE) ||
       (wParam == VK_RETURN && extended))
          ? 1
          : 0;
  return 1;
}

// Convenience: fill a bare mbKeyEvent (text points into a caller-provided
// mbWinKeyEvent is preferable when you need CHAR text to survive; this variant
// is for RAW_DOWN/UP where no text pointer is needed).
static __inline int mbKeyEventFromWin32(UINT message, WPARAM wParam,
                                        LPARAM lParam, mbKeyEvent* out) {
  static mbWinKeyEvent tmp;  // holds text for the immediately-following send
  if (!out || !mbKeyEventFromWin32Ex(message, wParam, lParam, &tmp))
    return 0;
  *out = tmp.event;
  if (tmp.event.text == tmp.text) {
    // Repoint at the static buffer so the text survives the return; valid until
    // the next mbKeyEventFromWin32 call (send the event before translating the
    // next message, the normal WndProc pattern).
    out->text = tmp.text;
    out->unmodified_text = tmp.text;
  }
  return 1;
}

// ---- Mouse ---------------------------------------------------------------------
// Fill `out` from a Win32 mouse message (coordinates are client-space logical
// px already). Returns 1 on success, 0 for an untranslated message.
static __inline int mbMouseEventFromWin32(UINT message, WPARAM wParam,
                                          LPARAM lParam, mbMouseEvent* out) {
  if (!out)
    return 0;
  int type, button = 0, click_count = 1;
  switch (message) {
    case WM_MOUSEMOVE: type = MB_MOUSE_MOVE; click_count = 0; break;
    case WM_LBUTTONDOWN: type = MB_MOUSE_DOWN; button = 0; break;
    case WM_LBUTTONDBLCLK: type = MB_MOUSE_DOWN; button = 0; click_count = 2; break;
    case WM_LBUTTONUP: type = MB_MOUSE_UP; button = 0; break;
    case WM_RBUTTONDOWN: type = MB_MOUSE_DOWN; button = 2; break;
    case WM_RBUTTONDBLCLK: type = MB_MOUSE_DOWN; button = 2; click_count = 2; break;
    case WM_RBUTTONUP: type = MB_MOUSE_UP; button = 2; break;
    case WM_MBUTTONDOWN: type = MB_MOUSE_DOWN; button = 1; break;
    case WM_MBUTTONDBLCLK: type = MB_MOUSE_DOWN; button = 1; click_count = 2; break;
    case WM_MBUTTONUP: type = MB_MOUSE_UP; button = 1; break;
    default: return 0;
  }
  memset(out, 0, sizeof(*out));
  out->struct_size = (int)sizeof(mbMouseEvent);
  out->type = type;
  out->x = GET_X_LPARAM(lParam);
  out->y = GET_Y_LPARAM(lParam);
  out->button = button;
  out->click_count = click_count;
  out->modifiers = mbModifiersFromWin32Mouse(wParam);
  return 1;
}

// ---- Wheel ---------------------------------------------------------------------
// Fill `out` from WM_MOUSEWHEEL (vertical) or WM_MOUSEHWHEEL (horizontal).
// Win32 wheel deltas are multiples of WHEEL_DELTA (120) and carry SCREEN
// coordinates, mapped to client space via `hwnd`. Sign: a Win32 vertical wheel
// is positive when rotated AWAY from the user (scroll up); the DOM convention
// is deltaY > 0 = content down, so it's negated. ~40 px per notch.
static __inline int mbWheelEventFromWin32(UINT message, WPARAM wParam,
                                          LPARAM lParam, HWND hwnd,
                                          mbWheelEvent* out) {
  if (!out || (message != WM_MOUSEWHEEL && message != WM_MOUSEHWHEEL))
    return 0;
  memset(out, 0, sizeof(*out));
  out->struct_size = (int)sizeof(mbWheelEvent);
  POINT p;
  p.x = GET_X_LPARAM(lParam);
  p.y = GET_Y_LPARAM(lParam);
  ScreenToClient(hwnd, &p);
  out->x = p.x;
  out->y = p.y;
  const float notches = (float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA;
  if (message == WM_MOUSEWHEEL)
    out->delta_y = -notches * 40.0f;  // wheel-up (positive) -> content up
  else
    out->delta_x = notches * 40.0f;   // WM_MOUSEHWHEEL: right is positive
  out->modifiers = mbModifiersFromWin32Mouse(GET_KEYSTATE_WPARAM(wParam));
  return 1;
}

#endif  // MINIBLINK2_WEBVIEW_WIN_H_
