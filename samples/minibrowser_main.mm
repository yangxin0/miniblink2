// miniblink2 mini-browser for macOS — a minimal host that drives the mb C API.
//
// The engine renders the page off-screen into a memory bitmap (BGRA8888). This host:
//   1. mbInitialize() + creates an off-screen mbView,
//   2. hosts it in an NSWindow/NSView,
//   3. a 60fps timer marks the view dirty; drawRect: pulls the view's pixels via
//      mbRepaintToBitmap() (the fast interactive repaint) and blits with CoreGraphics,
//   4. forwards mouse/keyboard/resize, and runs the Cocoa run loop — blink's
//      scheduler is pumped by the GCD-backed SharedTimerMac, so timers/loading advance.
//
// Build: scripts/build-samples.sh (links libminiblink2.dylib or the merged .a).
#import <Cocoa/Cocoa.h>
#include <vector>
#include "miniblink2/automation.h"

static mbView* g_view = nullptr;
static NSView* g_contentView = nil;
static CGFloat g_scale = 1.0;             // Retina backing scale: render at this factor for crisp output
static NSTextField* g_addressBar = nil;   // URL entry
static NSButton* g_backButton = nil;      // ◀
static NSButton* g_forwardButton = nil;   // ▶
static int g_cursor = MB_CURSOR_POINTER;  // latest engine-pushed cursor
static const CGFloat kToolbarHeight = 40.0;

// mb modifier bitmask: 1=ctrl 2=shift 4=alt 8=meta.
static int mbModifiers(NSEvent* e) {
    NSUInteger m = [e modifierFlags];
    int mods = 0;
    if (m & NSEventModifierFlagControl) mods |= 1;
    if (m & NSEventModifierFlagShift)   mods |= 2;
    if (m & NSEventModifierFlagOption)  mods |= 4;
    if (m & NSEventModifierFlagCommand) mods |= 8;
    return mods;
}

// Reflect the engine's current URL + history availability into the chrome.
static void mbUpdateChrome() {
    if (!g_view) return;
    char url[4096];
    if (mbGetURL(g_view, url, sizeof url) > 0 && g_addressBar) {
        // Don't stomp the field while the user is editing it (an active NSTextField
        // edit makes the window's firstResponder the shared field editor, an NSTextView).
        id fr = [g_addressBar.window firstResponder];
        if (![fr isKindOfClass:[NSTextView class]])
            [g_addressBar setStringValue:[NSString stringWithUTF8String:url]];
    }
    [g_backButton setEnabled:mbCanGoBack(g_view) != 0];
    [g_forwardButton setEnabled:mbCanGoForward(g_view) != 0];
}

// ---- The content view: blits the engine's BGRA buffer -----------------------
@interface MbBrowserView : NSView
@end

@implementation MbBrowserView
- (BOOL)isFlipped { return YES; }  // top-left origin, matching blink

- (void)drawRect:(NSRect)dirtyRect {
    if (!g_view)
        return;
    CGFloat sc = g_scale > 0 ? g_scale : 1.0;
    int lw = (int)self.bounds.size.width;        // logical points
    int lh = (int)self.bounds.size.height;
    int w = (int)(lw * sc);                       // physical pixels (matches the view)
    int h = (int)(lh * sc);
    if (w <= 0 || h <= 0)
        return;

    // Pull the rendered page pixels (BGRA, w*4 pitch) at physical resolution.
    // mbRepaintToBitmap is the interactive variant: no per-call lifecycle settle,
    // so a 60fps blit loop stays fast on live pages (vs. one-shot mbPaintToBitmap).
    int pitch = w * 4;
    static std::vector<unsigned char> buf;
    buf.assign((size_t)pitch * h, 0);
    mbRepaintToBitmap(g_view, buf.data(), w, h, pitch);

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = (CGContextRef)[[NSGraphicsContext currentContext] CGContext];
    // The engine renders BGRA8888 (byte 0 = B). Read it as such: AlphaFirst + 32-little
    // makes the word ARGB, whose little-endian bytes are B,G,R,A = BGRA.
    CGContextRef bmp = CGBitmapContextCreate(buf.data(), w, h, 8, pitch, cs,
        kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(bmp);
    // The buffer is top-down (row 0 = top). In this flipped NSView, draw the
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

// ---- Input: forward Cocoa events to the engine ------------------------------
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

// Engine (x, y) from an NSEvent. The view is flipped, so the converted point is
// already top-left origin (matching blink). The viewport is LOGICAL (CSS px)
// with a device scale factor for HiDPI raster, so input is in logical points.
- (void)mbPoint:(NSEvent*)e x:(int*)x y:(int*)y {
    NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    *x = (int)p.x; *y = (int)p.y;
}
- (void)mouseDown:(NSEvent*)e {
    if (!g_view) return;
    mbSetFocus(g_view, 1);
    int x, y; [self mbPoint:e x:&x y:&y];
    mbSendMouseDown(g_view, x, y);
    [self setNeedsDisplay:YES];
}
- (void)mouseUp:(NSEvent*)e {
    if (!g_view) return;
    int x, y; [self mbPoint:e x:&x y:&y];
    mbSendMouseUp(g_view, x, y);
    [self setNeedsDisplay:YES];
}
- (void)mouseDragged:(NSEvent*)e     { [self forwardMove:e]; }
- (void)mouseMoved:(NSEvent*)e       { [self forwardMove:e]; }
- (void)otherMouseDragged:(NSEvent*)e{ [self forwardMove:e]; }
- (void)forwardMove:(NSEvent*)e {
    if (!g_view) return;
    int x, y; [self mbPoint:e x:&x y:&y];
    // mbSendMouseDown/Up track the held button, so moves in between are a drag.
    mbSendMouseMove(g_view, x, y);
    // Pointer UI: the engine pushes cursor changes (mbOnCursorChanged) — show
    // an I-beam over selectable text and a hand over links, like a browser.
    switch (g_cursor) {
        case MB_CURSOR_HAND:  [[NSCursor pointingHandCursor] set]; break;
        case MB_CURSOR_IBEAM: [[NSCursor IBeamCursor] set]; break;
        default:              [[NSCursor arrowCursor] set]; break;
    }
    [self setNeedsDisplay:YES];
}
// Right/middle: the mb API models these as complete clicks (down+up), which also
// fire contextmenu/auxclick like a real browser.
- (void)rightMouseDown:(NSEvent*)e {
    if (!g_view) return;
    int x, y; [self mbPoint:e x:&x y:&y];
    mbSendMouseClickEx(g_view, x, y, /*button=right*/2, mbModifiers(e));
    [self setNeedsDisplay:YES];
}
- (void)otherMouseDown:(NSEvent*)e {
    if (!g_view) return;
    int x, y; [self mbPoint:e x:&x y:&y];
    mbSendMouseClickEx(g_view, x, y, /*button=middle*/1, mbModifiers(e));
    [self setNeedsDisplay:YES];
}

- (void)scrollWheel:(NSEvent*)e {
    if (!g_view) return;
    int x, y; [self mbPoint:e x:&x y:&y];
    // mbSendWheel takes DOM-convention pixel deltas (deltaY>0 = scroll down),
    // the opposite sign of Cocoa's scrolling deltas. Line-based wheels scale
    // toward a typical line height.
    double dy = [e hasPreciseScrollingDeltas] ? [e scrollingDeltaY] : [e scrollingDeltaY] * 40.0;
    double dx = [e hasPreciseScrollingDeltas] ? [e scrollingDeltaX] : [e scrollingDeltaX] * 40.0;
    if ((int)dy || (int)dx)
        mbSendWheel(g_view, x, y, (int)-dx, (int)-dy, mbModifiers(e));
    [self setNeedsDisplay:YES];
}

// Named non-text keys -> mb key names (mbSendKey triggers the browser default
// action and includes the release, so there is no separate keyUp forwarding).
static const char* mbKeyName(unsigned short kc) {
    switch (kc) {
        case 51: return "Backspace"; case 48: return "Tab";
        case 36: case 76: return "Enter";
        case 53: return "Escape";    case 117: return "Delete";
        case 123: return "ArrowLeft"; case 124: return "ArrowRight";
        case 126: return "ArrowUp";   case 125: return "ArrowDown";
        case 115: return "Home";      case 119: return "End";
        case 116: return "PageUp";    case 121: return "PageDown";
        default: return nullptr;
    }
}
- (void)keyDown:(NSEvent*)e {
    if (!g_view) return;
    int mods = mbModifiers(e);
    const char* name = mbKeyName([e keyCode]);
    if (name) {
        mbSendKeyEx(g_view, name, mods);
    } else if (mods & (1 | 8)) {
        // ctrl/cmd shortcuts (select-all, etc.): per-character trusted key events.
        NSString* chars = [[e charactersIgnoringModifiers] lowercaseString];
        for (NSUInteger i = 0; i < [chars length]; ++i) {
            char key[2] = { (char)[chars characterAtIndex:i], 0 };
            if (key[0] >= 0x20 && key[0] < 0x7f) mbSendKeyEx(g_view, key, mods);
        }
    } else {
        // Plain text input (full Unicode) into the focused editable.
        NSString* chars = [e characters];
        if ([chars length]) mbSendText(g_view, [chars UTF8String]);
    }
    [self setNeedsDisplay:YES];
}
@end

// Route page console.log / warnings / errors to stdout (useful for debugging).
static void onConsole(mbView*, void*, const char* level, const char* message) {
    NSLog(@"[console:%s] %s", level ? level : "", message ? message : "");
}

// URL committed in the engine -> refresh address bar + back/forward state.
static void onUrlChanged(mbView*, void*, const char* /*url*/) {
    dispatch_async(dispatch_get_main_queue(), ^{ mbUpdateChrome(); });
}
// Page finished loading -> refresh chrome (final URL after redirects, history).
static void onLoadFinish(mbView*, void*) {
    dispatch_async(dispatch_get_main_queue(), ^{ mbUpdateChrome(); });
}
// The engine wants a different pointer (I-beam over text, hand over links).
static void onCursorChanged(mbView*, void*, int cursor) {
    g_cursor = cursor;
}

// window.open / target=_blank: a single-window browser NAVIGATES to the popup
// URL instead of spawning a window (mbOnCreateChildView is the multi-window
// answer — it hands the host a live, opener-wired child view).
static void onNewWindow(mbView* v, void*, const char* url, const char* /*name*/) {
    if (url && *url) mbLoadURL(v, url);
}

// Title changed -> window title.
static void onTitleChanged(mbView*, void*, const char* title) {
    NSString* s = title ? [NSString stringWithUTF8String:title] : @"";
    dispatch_async(dispatch_get_main_queue(), ^{
        [[g_contentView window] setTitle:s.length ? s : @"miniblink2 (macOS)"];
    });
}

// ---- Browser chrome: back/forward/reload buttons + address bar --------------
@interface MbChrome : NSObject <NSWindowDelegate>
@end
@implementation MbChrome
- (void)goBack:(id)sender    { if (g_view && mbCanGoBack(g_view))    mbGoBack(g_view); }
- (void)goForward:(id)sender { if (g_view && mbCanGoForward(g_view)) mbGoForward(g_view); }
- (void)reload:(id)sender    { if (g_view) mbReload(g_view); }

// Enter in the address bar -> normalize + load.
- (void)navigate:(id)sender {
    if (!g_view) return;
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
    mbLoadURL(g_view, [urlStr UTF8String]);
    // hand keyboard focus back to the page
    [[g_addressBar window] makeFirstResponder:g_contentView];
}

// Keep the web view sized to the area below the toolbar as the window resizes.
- (void)windowDidResize:(NSNotification*)note {
    NSWindow* win = [note object];
    NSRect cr = [[win contentView] bounds];
    int w = (int)cr.size.width;                       // LOGICAL viewport (CSS px);
    int h = (int)(cr.size.height - kToolbarHeight);   // the device scale handles HiDPI
    if (w <= 0 || h <= 0 || !g_view) return;
    mbResize(g_view, w, h);
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
        [win setTitle:@"miniblink2 (macOS)"];

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

        // Bring up the engine and load the page. Lay out at the LOGICAL window
        // size (CSS px) and set the DEVICE SCALE FACTOR so the page renders
        // retina-crisp (devicePixelRatio == backing scale) — page zoom would
        // scale content instead of raster resolution.
        g_scale = [win backingScaleFactor];
        if (g_scale <= 0) g_scale = 1.0;
        mbInitialize();
        g_view = mbCreateView(W, H);                    // LOGICAL viewport
        mbSetDeviceScaleFactor(g_view, (float)g_scale); // HiDPI raster (not page zoom)
        mbOnConsoleMessage(g_view, onConsole, nullptr);
        // Chrome wiring: keep the address bar + back/forward state in sync.
        mbOnUrlChanged(g_view, onUrlChanged, nullptr);
        mbOnLoadFinish(g_view, onLoadFinish, nullptr);
        mbOnTitleChanged(g_view, onTitleChanged, nullptr);
        mbOnCursorChanged(g_view, onCursorChanged, nullptr);
        mbOnNewWindow(g_view, onNewWindow, nullptr);
        mbLoadURL(g_view, url);

        NSLog(@"[minibrowser] loading %s", url);

        // Drive the render: each frame, repaint + blit. Without an active pump the
        // offscreen page never composites and the window stays blank.
        __block int ticks = 0;
        [NSTimer scheduledTimerWithTimeInterval:1.0/60.0 repeats:YES block:^(NSTimer*) {
            ++ticks;
            [g_contentView setNeedsDisplay:YES];  // drawRect -> mbRepaintToBitmap drives+blits
            if (ticks % 15 == 0) mbUpdateChrome();   // keep nav buttons/URL fresh
            if (ticks % 60 == 0)
                NSLog(@"[minibrowser] tick=%d loadFinished=%d", ticks, mbIsLoadFinished(g_view));
        }];
        NSLog(@"[minibrowser] timer scheduled, entering run loop");

        // Cocoa run loop; blink's scheduler advances via the GCD SharedTimerMac.
        [NSApp run];
    }
    return 0;
}
