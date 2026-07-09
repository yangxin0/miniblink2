// compat/win/mb_window.cc — Win32 backend of the shared sample window host
// (compat/mb_window.h). The Cocoa peer is compat/mac/mb_window.mm.
//
// One WndProc serves every sample window: WM_PAINT blits the engine's BGRA
// buffer with GDI (32bpp DIB, top-down), input messages forward as trusted
// mbSend* events (physical px -> logical CSS px via the per-monitor DPI
// scale), WM_SIZE flows into mbResize, and WM_SETCURSOR reflects the cursor
// the engine pushed (mbOnCursorChanged).
#ifdef _WIN32

#include <windows.h>
#include <windowsx.h>

#include <stdlib.h>

#include <string>
#include <vector>

#include "../mb_window.h"

namespace {

struct MbWindow_ {
    HWND hwnd = nullptr;
    mbView* view = nullptr;
    float scale = 1.0f;         // physical px per CSS px (per-monitor DPI / 96)
    int logical_w = 0, logical_h = 0;
    int cursor = MB_CURSOR_POINTER;  // latest MB_CURSOR_* pushed by the engine
    std::vector<unsigned char> pixels;  // BGRA blit scratch
    wchar_t pending_high_surrogate = 0; // WM_CHAR surrogate-pair assembly
};

std::vector<MbWindow_*>& Windows() {
    static std::vector<MbWindow_*>* w = new std::vector<MbWindow_*>();
    return *w;
}

int MbMods() {
    int mods = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= 1;
    if (GetKeyState(VK_SHIFT) & 0x8000)   mods |= 2;
    if (GetKeyState(VK_MENU) & 0x8000)    mods |= 4;
    return mods;
}

const char* MbKeyName(WPARAM vk) {
    switch (vk) {
        case VK_BACK:   return "Backspace"; case VK_TAB:    return "Tab";
        case VK_RETURN: return "Enter";     case VK_ESCAPE: return "Escape";
        case VK_DELETE: return "Delete";
        case VK_LEFT:  return "ArrowLeft";  case VK_RIGHT: return "ArrowRight";
        case VK_UP:    return "ArrowUp";    case VK_DOWN:  return "ArrowDown";
        case VK_HOME:  return "Home";       case VK_END:   return "End";
        case VK_PRIOR: return "PageUp";     case VK_NEXT:  return "PageDown";
        default: return nullptr;
    }
}

std::string Utf16ToUtf8(const wchar_t* s, int len) {
    if (len <= 0) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n : 0, '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, s, len, &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToUtf16(const char* s) {
    if (!s || !*s) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring out(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1)
        MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], n);
    return out;
}

float WindowScale(HWND hwnd) {
    UINT dpi = GetDpiForWindow(hwnd);   // Win10 1607+; the samples target that
    return dpi > 0 ? dpi / 96.0f : 1.0f;
}

void Blit(MbWindow_* w, HDC dc) {
    int pw = (int)(w->logical_w * w->scale), ph = (int)(w->logical_h * w->scale);
    if (!w->view || pw <= 0 || ph <= 0) return;
    int pitch = pw * 4;
    w->pixels.assign((size_t)pitch * ph, 0);
    if (!mbRepaintToBitmap(w->view, w->pixels.data(), pw, ph, pitch))
        return;  // engine busy: keep the previous frame (never blit zeros)
    // 32bpp BI_RGB DIB == BGRA bytes; negative height = top-down, matching the
    // engine's buffer. StretchDIBits maps physical pixels onto the client rect.
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = pw;
    bi.bmiHeader.biHeight = -ph;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    RECT rc;
    GetClientRect(w->hwnd, &rc);
    StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, pw, ph,
                  w->pixels.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* w = reinterpret_cast<MbWindow_*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!w) return DefWindowProcW(hwnd, msg, wp, lp);
    const float sc = w->scale > 0 ? w->scale : 1.0f;
    const int x = (int)(GET_X_LPARAM(lp) / sc);   // physical -> logical CSS px
    const int y = (int)(GET_Y_LPARAM(lp) / sc);

    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            Blit(w, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;  // the blit covers the client area; avoid flicker
        case WM_SIZE: {
            w->logical_w = (int)(LOWORD(lp) / sc);
            w->logical_h = (int)(HIWORD(lp) / sc);
            if (w->view && w->logical_w > 0 && w->logical_h > 0)
                mbResize(w->view, w->logical_w, w->logical_h);
            return 0;
        }
        case WM_DPICHANGED: {
            w->scale = HIWORD(wp) / 96.0f;
            if (w->view) mbSetDeviceScaleFactor(w->view, w->scale);
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left,
                         r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (w->view) mbSendMouseMove(w->view, x, y);
            return 0;
        case WM_LBUTTONDOWN:
            SetFocus(hwnd);
            if (w->view) {
                mbSetFocus(w->view, 1);
                mbSendMouseDown(w->view, x, y);
            }
            return 0;
        case WM_LBUTTONUP:
            if (w->view) mbSendMouseUp(w->view, x, y);
            return 0;
        case WM_RBUTTONDOWN:
            if (w->view) mbSendMouseClickEx(w->view, x, y, /*button=*/2, MbMods());
            return 0;
        case WM_MOUSEWHEEL: {
            if (!w->view) return 0;
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };  // screen coords
            ScreenToClient(hwnd, &pt);
            // Wheel forward (positive) scrolls UP; DOM deltaY>0 is content DOWN.
            int dy = -GET_WHEEL_DELTA_WPARAM(wp) * 40 / WHEEL_DELTA;
            mbSendWheel(w->view, (int)(pt.x / sc), (int)(pt.y / sc), 0, dy, MbMods());
            return 0;
        }
        case WM_KEYDOWN: {
            if (!w->view) return 0;
            int mods = MbMods();
            if (const char* name = MbKeyName(wp)) {
                mbSendKeyEx(w->view, name, mods);   // default actions fire
            } else if ((mods & 1) && wp >= 'A' && wp <= 'Z') {
                char key[2] = { (char)('a' + (wp - 'A')), 0 };  // ctrl shortcuts
                mbSendKeyEx(w->view, key, mods);
            }
            return 0;
        }
        case WM_CHAR: {
            if (!w->view) return 0;
            wchar_t c = (wchar_t)wp;
            if (c < 0x20 || c == 0x7f) return 0;      // control chars ride WM_KEYDOWN
            if (GetKeyState(VK_CONTROL) & 0x8000) return 0;  // shortcut, not text
            wchar_t units[2];
            int n = 0;
            if (c >= 0xD800 && c <= 0xDBFF) {          // high surrogate: park it
                w->pending_high_surrogate = c;
                return 0;
            }
            if (c >= 0xDC00 && c <= 0xDFFF && w->pending_high_surrogate) {
                units[n++] = w->pending_high_surrogate;  // reassemble the pair
                w->pending_high_surrogate = 0;
            }
            units[n++] = c;
            std::string utf8 = Utf16ToUtf8(units, n);
            if (!utf8.empty()) mbSendText(w->view, utf8.c_str());
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {  // engine-pushed cursor in the content
                LPCWSTR id = IDC_ARROW;
                if (w->cursor == MB_CURSOR_HAND) id = IDC_HAND;
                else if (w->cursor == MB_CURSOR_IBEAM) id = IDC_IBEAM;
                SetCursor(LoadCursorW(nullptr, id));
                return TRUE;
            }
            break;
        case WM_DESTROY: {
            if (w->view) {
                mbDestroyView(w->view);
                w->view = nullptr;
            }
            auto& all = Windows();
            for (size_t i = 0; i < all.size(); ++i)
                if (all[i] == w) { all.erase(all.begin() + i); break; }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            delete w;
            if (all.empty()) PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void EnsureWindowClass() {
    static bool done = false;
    if (done) return;
    done = true;
    // Crisp per-monitor scaling; guarded, so pre-1703 systems still run.
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        using SetCtx = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        if (auto f = reinterpret_cast<SetCtx>(
                GetProcAddress(user32, "SetProcessDpiAwarenessContext")))
            f(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = nullptr;  // WM_SETCURSOR drives the cursor
    wc.lpszClassName = L"MbSampleWindow";
    RegisterClassExW(&wc);
}

}  // namespace

extern "C" {

MbWindow* MbWindowCreate(const char* title, int width, int height) {
    EnsureWindowClass();
    auto* w = new MbWindow_();
    // Create at a provisional size, read the real per-monitor DPI, then size
    // the client rect to logical*scale physical pixels.
    std::wstring wtitle = Utf8ToUtf16(title ? title : "miniblink2");
    w->hwnd = CreateWindowExW(0, L"MbSampleWindow", wtitle.c_str(),
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              width, height, nullptr, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    if (!w->hwnd) {
        delete w;
        return nullptr;
    }
    w->scale = WindowScale(w->hwnd);
    w->logical_w = width;
    w->logical_h = height;
    RECT rc = { 0, 0, (LONG)(width * w->scale), (LONG)(height * w->scale) };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(w->hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    w->view = mbCreateView(width, height);   // LOGICAL viewport (CSS px)
    if (!w->view) {
        DestroyWindow(w->hwnd);
        delete w;
        return nullptr;
    }
    mbSetDeviceScaleFactor(w->view, w->scale);  // HiDPI raster, same layout
    mbOnCursorChanged(w->view, [](mbView*, void* ud, int cursor) {
        static_cast<MbWindow_*>(ud)->cursor = cursor;
    }, w);

    SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(w));
    Windows().push_back(w);
    ShowWindow(w->hwnd, SW_SHOWNORMAL);
    UpdateWindow(w->hwnd);
    return reinterpret_cast<MbWindow*>(w);
}

mbView* MbWindowView(MbWindow* w) {
    return w ? reinterpret_cast<MbWindow_*>(w)->view : nullptr;
}

void MbWindowSetTitle(MbWindow* w, const char* title) {
    if (!w || !title) return;
    SetWindowTextW(reinterpret_cast<MbWindow_*>(w)->hwnd,
                   Utf8ToUtf16(title).c_str());
}

void MbRunApp(void) {
    ULONGLONG deadline = 0;
    if (const char* ms = getenv("MB_SAMPLE_AUTOEXIT_MS")) {  // smoke-run support
        long v = atol(ms);
        if (v > 0) deadline = GetTickCount64() + (ULONGLONG)v;
    }
    // The canonical interactive tick (webview.h header): drain input, advance
    // the engine, blit only views that are actually dirty, wait ~a frame.
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (deadline && GetTickCount64() >= deadline) exit(0);
        mbUpdate();  // re-entrancy-safe engine slice (bounded by mbSetMaxUpdateTime)
        for (MbWindow_* w : Windows())
            if (w->view && mbViewIsDirty(w->view))
                InvalidateRect(w->hwnd, nullptr, FALSE);
        MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT, 0);  // ~60 fps
    }
}

}  // extern "C"

#endif  // _WIN32
