// miniblink mini-browser for macOS — a minimal host that drives the wke C API.
//
// wke renders the page off-screen into a memory bitmap (the in-tree skia memory
// canvas, RGBA N32). This host:
//   1. wkeInitialize() + creates an off-screen wkeWebView,
//   2. hosts it in an NSWindow/NSView,
//   3. on wkeOnPaintUpdated marks the view dirty; drawRect: pulls the webview's
//      pixels via wkePaint() and blits them with CoreGraphics,
//   4. forwards mouse/resize, and runs the Cocoa run loop — blink's scheduler is
//      pumped by the GCD-backed SharedTimerMac, so timers/loading advance.
//
// Build: the `minibrowser` CMake target links the full engine archive set.
#import <Cocoa/Cocoa.h>
#include <vector>
#include "wke/wke.h"

// Win32 message + mouse-key-flag codes the wke input API expects (kept local so
// this Cocoa TU doesn't pull win_compat/windows.h).
enum {
    kWM_MOUSEMOVE = 0x0200, kWM_LBUTTONDOWN = 0x0201, kWM_LBUTTONUP = 0x0202,
    kWM_LBUTTONDBLCLK = 0x0203, kWM_RBUTTONDOWN = 0x0204, kWM_RBUTTONUP = 0x0205,
    kWM_MBUTTONDOWN = 0x0207, kWM_MBUTTONUP = 0x0208,
    kMK_LBUTTON = 0x0001, kMK_RBUTTON = 0x0002, kMK_SHIFT = 0x0004,
    kMK_CONTROL = 0x0008, kMK_MBUTTON = 0x0010,
    kVK_BACK = 0x08, kVK_TAB = 0x09, kVK_RETURN = 0x0D, kVK_ESCAPE = 0x1B,
    kVK_LEFT = 0x25, kVK_UP = 0x26, kVK_RIGHT = 0x27, kVK_DOWN = 0x28, kVK_DELETE = 0x2E,
};

static wkeWebView g_webView = nullptr;
static NSView* g_contentView = nil;
static CGFloat g_scale = 1.0;             // Retina backing scale: render at this factor for crisp output
static NSTextField* g_addressBar = nil;   // URL entry
static NSButton* g_backButton = nil;      // ◀
static NSButton* g_forwardButton = nil;   // ▶
static const CGFloat kToolbarHeight = 40.0;

// Reflect the engine's current URL + history availability into the chrome.
static void mbUpdateChrome() {
    if (!g_webView) return;
    const utf8* u = wkeGetURL(g_webView);
    if (u && g_addressBar) {
        // Don't stomp the field while the user is editing it (an active NSTextField
        // edit makes the window's firstResponder the shared field editor, an NSTextView).
        id fr = [g_addressBar.window firstResponder];
        if (![fr isKindOfClass:[NSTextView class]])
            [g_addressBar setStringValue:[NSString stringWithUTF8String:u]];
    }
    [g_backButton setEnabled:wkeCanGoBack(g_webView)];
    [g_forwardButton setEnabled:wkeCanGoForward(g_webView)];
}

// ---- The content view: blits the wke webview's RGBA buffer ------------------
@interface MbBrowserView : NSView
@end

@implementation MbBrowserView
- (BOOL)isFlipped { return YES; }  // top-left origin, matching wke/blink

- (void)drawRect:(NSRect)dirtyRect {
    if (!g_webView)
        return;
    CGFloat sc = g_scale > 0 ? g_scale : 1.0;
    int lw = (int)self.bounds.size.width;        // logical points
    int lh = (int)self.bounds.size.height;
    int w = (int)(lw * sc);                       // physical pixels (matches the webview)
    int h = (int)(lh * sc);
    if (w <= 0 || h <= 0)
        return;

    // Pull the rendered page pixels (RGBA, w*4 pitch) from wke, at physical resolution.
    int pitch = w * 4;
    static std::vector<unsigned char> buf;
    buf.assign((size_t)pitch * h, 0);
    wkePaint(g_webView, buf.data(), pitch);

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = (CGContextRef)[[NSGraphicsContext currentContext] CGContext];
    CGContextRef bmp = CGBitmapContextCreate(buf.data(), w, h, 8, pitch, cs,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGImageRef img = CGBitmapContextCreateImage(bmp);
    // wke's buffer is top-down (row 0 = top). In this flipped NSView, draw the
    // image with a vertical flip so it appears upright. The physical-resolution
    // image is blitted into the logical bounds; on a Retina context that maps 1:1
    // to device pixels -> crisp (no upscaling blur).
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, 0, lh);
    CGContextScaleCTM(ctx, 1, -1);
    CGContextDrawImage(ctx, CGRectMake(0, 0, lw, lh), img);
    CGContextRestoreGState(ctx);
    CGImageRelease(img);
    CGContextRelease(bmp);
    CGColorSpaceRelease(cs);
}

// ---- Input: forward Cocoa events to wke -------------------------------------
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)becomeFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)e { return YES; }

- (void)updateTrackingAreas {
    for (NSTrackingArea* a in [self trackingAreas]) [self removeTrackingArea:a];
    NSTrackingArea* ta = [[NSTrackingArea alloc]
        initWithRect:[self bounds]
        options:(NSTrackingMouseMoved | NSTrackingActiveAlways | NSTrackingInVisibleRect)
        owner:self userInfo:nil];
    [self addTrackingArea:ta];
    [super updateTrackingAreas];
}

// Compute wke (x, y, flags) from an NSEvent. The view is flipped, so the
// converted point is already top-left origin (matching wke/blink).
- (void)wkePoint:(NSEvent*)e x:(int*)x y:(int*)y flags:(unsigned int*)flags {
    NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    // The webview viewport is in physical pixels; scale logical points to match.
    *x = (int)(p.x * g_scale); *y = (int)(p.y * g_scale);
    unsigned int f = 0;
    NSUInteger m = [e modifierFlags];
    if (m & NSEventModifierFlagShift)   f |= kMK_SHIFT;
    if (m & NSEventModifierFlagControl) f |= kMK_CONTROL;
    NSUInteger b = [NSEvent pressedMouseButtons];
    if (b & 1) f |= kMK_LBUTTON;
    if (b & 2) f |= kMK_RBUTTON;
    if (b & 4) f |= kMK_MBUTTON;
    *flags = f;
}
- (void)fireMouse:(unsigned int)msg event:(NSEvent*)e {
    if (!g_webView) return;
    int x, y; unsigned int flags; [self wkePoint:e x:&x y:&y flags:&flags];
    wkeFireMouseEvent(g_webView, msg, x, y, flags);
    [self setNeedsDisplay:YES];
}
- (void)mouseDown:(NSEvent*)e        { wkeSetFocus(g_webView); [self fireMouse:kWM_LBUTTONDOWN event:e]; }
- (void)mouseUp:(NSEvent*)e          { [self fireMouse:kWM_LBUTTONUP event:e]; }
- (void)mouseDragged:(NSEvent*)e     { [self fireMouse:kWM_MOUSEMOVE event:e]; }
- (void)mouseMoved:(NSEvent*)e       { [self fireMouse:kWM_MOUSEMOVE event:e]; }
- (void)rightMouseDown:(NSEvent*)e   {
    if (!g_webView) return;
    int x, y; unsigned int flags; [self wkePoint:e x:&x y:&y flags:&flags];
    wkeFireMouseEvent(g_webView, kWM_RBUTTONDOWN, x, y, flags);
    [self setNeedsDisplay:YES];  // (modern wke has no wkeFireContextMenuEvent)
}
- (void)rightMouseUp:(NSEvent*)e     { [self fireMouse:kWM_RBUTTONUP event:e]; }
- (void)otherMouseDown:(NSEvent*)e   { [self fireMouse:kWM_MBUTTONDOWN event:e]; }
- (void)otherMouseUp:(NSEvent*)e     { [self fireMouse:kWM_MBUTTONUP event:e]; }
- (void)otherMouseDragged:(NSEvent*)e{ [self fireMouse:kWM_MOUSEMOVE event:e]; }

- (void)scrollWheel:(NSEvent*)e {
    if (!g_webView) return;
    int x, y; unsigned int flags; [self wkePoint:e x:&x y:&y flags:&flags];
    // Cocoa scroll deltas are small; scale toward a Win32 WHEEL_DELTA (120) step.
    double dy = [e hasPreciseScrollingDeltas] ? [e scrollingDeltaY] : [e scrollingDeltaY] * 10.0;
    int delta = (int)(dy * 3.0);
    if (delta) wkeFireMouseWheelEvent(g_webView, x, y, delta, flags);
    [self setNeedsDisplay:YES];
}

static unsigned int vkFromKeyCode(unsigned short kc) {
    switch (kc) {
        case 51: return kVK_BACK;   case 48: return kVK_TAB;    case 36: return kVK_RETURN;
        case 76: return kVK_RETURN; case 53: return kVK_ESCAPE; case 117: return kVK_DELETE;
        case 123: return kVK_LEFT;  case 124: return kVK_RIGHT; case 126: return kVK_UP; case 125: return kVK_DOWN;
        default: return 0;
    }
}
- (void)keyDown:(NSEvent*)e {
    if (!g_webView) return;
    unsigned int vk = vkFromKeyCode([e keyCode]);
    if (vk) wkeFireKeyDownEvent(g_webView, vk, 0, false);
    NSString* chars = [e characters];
    for (NSUInteger i = 0; i < [chars length]; ++i) {
        unichar c = [chars characterAtIndex:i];
        if (c >= 0x20 || c == '\r' || c == '\t')  // printable + enter/tab
            wkeFireKeyPressEvent(g_webView, c, 0, false);
    }
    [self setNeedsDisplay:YES];
}
- (void)keyUp:(NSEvent*)e {
    if (!g_webView) return;
    unsigned int vk = vkFromKeyCode([e keyCode]);
    if (vk) wkeFireKeyUpEvent(g_webView, vk, 0, false);
}
@end

// NOTE (modern port): miniblink49 needed an async wkeOnPaintUpdated (HDC) callback
// plus a block of JS polyfills (IntersectionObserver, ResizeObserver, customElements,
// requestIdleCallback, queueMicrotask, navigator.permissions, …) injected via
// wkeOnDidCreateScriptContext to bridge its old blink-53 platform. The modern engine
// is M150 — it ships ALL of those natively and renders SYNCHRONOUSLY — so both are
// removed: a 60fps timer drives redraw and there are no polyfills. The native UA is
// already Chrome 150, so no UA spoof is needed either.

// Route page console.log / warnings / errors to stdout (useful for debugging).
static void onConsole(wkeWebView, void*, wkeConsoleLevel level, const wkeString message,
    const wkeString sourceName, unsigned sourceLine, const wkeString /*stackTrace*/) {
    const utf8* msg = message ? wkeGetString(message) : "";
    const utf8* src = sourceName ? wkeGetString(sourceName) : "";
    NSLog(@"[console:%d] %s  (%s:%u)", (int)level, msg ? msg : "", src ? src : "", sourceLine);
}

// URL changed in the engine -> refresh address bar + back/forward state.
static void onURLChanged(wkeWebView, void*, const wkeString url) {
    dispatch_async(dispatch_get_main_queue(), ^{ mbUpdateChrome(); });
}
// Page finished loading -> refresh chrome (final URL after redirects, history).
static void onLoadingFinish(wkeWebView, void*, const wkeString, wkeLoadingResult, const wkeString) {
    dispatch_async(dispatch_get_main_queue(), ^{ mbUpdateChrome(); });
}
// Title changed -> window title.
static void onTitleChanged(wkeWebView, void*, const wkeString title) {
    const utf8* t = title ? wkeGetString(title) : "";
    if (!t) return;
    NSString* s = [NSString stringWithUTF8String:t];
    dispatch_async(dispatch_get_main_queue(), ^{
        [[g_contentView window] setTitle:s.length ? s : @"miniblink (macOS)"];
    });
}

// ---- Browser chrome: back/forward/reload buttons + address bar --------------
@interface MbChrome : NSObject <NSWindowDelegate>
@end
@implementation MbChrome
- (void)goBack:(id)sender    { if (g_webView && wkeCanGoBack(g_webView))    wkeGoBack(g_webView); }
- (void)goForward:(id)sender { if (g_webView && wkeCanGoForward(g_webView)) wkeGoForward(g_webView); }
- (void)reload:(id)sender    { if (g_webView) wkeReload(g_webView); }

// Enter in the address bar -> normalize + load.
- (void)navigate:(id)sender {
    if (!g_webView) return;
    NSString* text = [[g_addressBar stringValue]
        stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (text.length == 0) return;
    // Add a scheme if the user typed a bare host/path; treat a no-dot, no-space
    // token as a search query routed through a search engine.
    BOOL hasScheme = ([text rangeOfString:@"://"].location != NSNotFound) ||
                     [text hasPrefix:@"about:"] || [text hasPrefix:@"data:"];
    NSString* urlStr;
    if (hasScheme) {
        urlStr = text;
    } else if ([text rangeOfString:@" "].location != NSNotFound ||
               [text rangeOfString:@"."].location == NSNotFound) {
        NSString* q = [text stringByAddingPercentEncodingWithAllowedCharacters:
            [NSCharacterSet URLQueryAllowedCharacterSet]];
        urlStr = [@"https://www.bing.com/search?q=" stringByAppendingString:q ?: @""];
    } else {
        urlStr = [@"https://" stringByAppendingString:text];
    }
    wkeLoadURL(g_webView, [urlStr UTF8String]);
    // hand keyboard focus back to the page
    [[g_addressBar window] makeFirstResponder:g_contentView];
}

// Keep the web view sized to the area below the toolbar as the window resizes.
- (void)windowDidResize:(NSNotification*)note {
    NSWindow* win = [note object];
    NSRect cr = [[win contentView] bounds];
    int w = (int)(cr.size.width * g_scale);
    int h = (int)((cr.size.height - kToolbarHeight) * g_scale);
    if (w <= 0 || h <= 0 || !g_webView) return;
    wkeResize(g_webView, w, h);
    [g_contentView setNeedsDisplay:YES];
}
@end

@interface MbAppDelegate : NSObject <NSApplicationDelegate>
@end
@implementation MbAppDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)s { return YES; }
@end

int main(int argc, const char** argv) {
    @autoreleasepool {
        const char* url = (argc > 1) ? argv[1] : "https://example.com";
        const int W = 1024, H = 768;                 // web content area
        const int winH = H + (int)kToolbarHeight;    // window adds a toolbar strip

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        MbAppDelegate* del = [[MbAppDelegate alloc] init];
        [NSApp setDelegate:del];

        NSRect winFrame = NSMakeRect(0, 0, W, winH);
        NSWindow* win = [[NSWindow alloc]
            initWithContentRect:winFrame
            styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                       NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
            backing:NSBackingStoreBuffered defer:NO];
        [win setTitle:@"miniblink (macOS)"];

        NSView* root = [[NSView alloc] initWithFrame:winFrame];
        [win setContentView:root];

        // --- Toolbar (pinned to the top, full width) ---
        NSView* toolbar = [[NSView alloc] initWithFrame:
            NSMakeRect(0, winH - kToolbarHeight, W, kToolbarHeight)];
        [toolbar setAutoresizingMask:(NSViewWidthSizable | NSViewMinYMargin)];
        [root addSubview:toolbar];

        MbChrome* chrome = [[MbChrome alloc] init];
        [win setDelegate:chrome];

        NSButton* (^mkBtn)(NSString*, SEL, CGFloat) = ^NSButton*(NSString* label, SEL act, CGFloat x) {
            NSButton* b = [[NSButton alloc] initWithFrame:NSMakeRect(x, 6, 34, 28)];
            [b setTitle:label]; [b setBezelStyle:NSBezelStyleRounded];
            [b setTarget:chrome]; [b setAction:act];
            [b setAutoresizingMask:NSViewMaxXMargin];
            [toolbar addSubview:b];
            return b;
        };
        g_backButton    = mkBtn(@"◀", @selector(goBack:), 8);      // back
        g_forwardButton = mkBtn(@"▶", @selector(goForward:), 46);  // forward
        (void)            mkBtn(@"↻", @selector(reload:), 84);     // reload

        g_addressBar = [[NSTextField alloc] initWithFrame:
            NSMakeRect(124, 8, W - 124 - 10, 24)];
        [g_addressBar setAutoresizingMask:NSViewWidthSizable];
        [[g_addressBar cell] setPlaceholderString:@"Enter URL or search"];
        [g_addressBar setStringValue:[NSString stringWithUTF8String:url]];
        [g_addressBar setTarget:chrome];
        [g_addressBar setAction:@selector(navigate:)];   // fires on Enter
        [toolbar addSubview:g_addressBar];

        // --- Web content view (fills the area below the toolbar) ---
        g_contentView = [[MbBrowserView alloc] initWithFrame:NSMakeRect(0, 0, W, H)];
        [g_contentView setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
        [root addSubview:g_contentView];

        [win setAcceptsMouseMovedEvents:YES];   // deliver mouseMoved for hover
        [win center];
        [win makeKeyAndOrderFront:nil];
        [win makeFirstResponder:g_contentView]; // keyboard goes to the page
        [NSApp activateIgnoringOtherApps:YES];

        // Bring up the engine and load the page. Render at the display's backing
        // scale (Retina) so the page isn't upscaled/blurry: size the viewport in
        // physical pixels and set the zoom to the scale (wke's devicePixelRatio).
        g_scale = [win backingScaleFactor];
        if (g_scale <= 0) g_scale = 1.0;
        wkeInitialize();
        g_webView = wkeCreateWebView();
        wkeResize(g_webView, (int)(W * g_scale), (int)(H * g_scale));
        wkeSetZoomFactor(g_webView, g_scale);
        wkeOnConsole(g_webView, onConsole, nullptr);
        // Chrome wiring: keep the address bar + back/forward state in sync.
        wkeOnURLChanged(g_webView, onURLChanged, nullptr);
        wkeOnLoadingFinish(g_webView, onLoadingFinish, nullptr);
        wkeOnTitleChanged(g_webView, onTitleChanged, nullptr);
        wkeLoadURL(g_webView, url);

        NSLog(@"[minibrowser] loading %s", url);

        // Drive the render: each frame, let wke run its pending paint (which fires
        // onPaintUpdated when the page changes) and mark the view dirty. Without an
        // active pump the offscreen page never composites and the window stays blank.
        __block int ticks = 0;
        [NSTimer scheduledTimerWithTimeInterval:1.0/60.0 repeats:YES block:^(NSTimer*) {
            ++ticks;
            [g_contentView setNeedsDisplay:YES];  // synchronous render -> just repaint
            if (ticks % 15 == 0) mbUpdateChrome();   // keep nav buttons/URL fresh
            if (ticks % 60 == 0)
                NSLog(@"[minibrowser] tick=%d loading=%d complete=%d", ticks,
                      wkeIsLoading(g_webView), wkeIsLoadingCompleted(g_webView));
        }];
        NSLog(@"[minibrowser] timer scheduled, entering run loop");

        // Cocoa run loop; blink's scheduler advances via the GCD SharedTimerMac.
        [NSApp run];
    }
    return 0;
}
