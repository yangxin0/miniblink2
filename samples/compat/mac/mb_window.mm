// compat/mac/mb_window.mm — Cocoa backend of the shared sample window host
// (compat/mb_window.h). The Win32 peer is compat/win/mb_window.cc.
#import <Cocoa/Cocoa.h>

#include <stdlib.h>

#include <mutex>
#include <utility>
#include <vector>

#include "../mb_window.h"

// mb modifier bitmask: 1=ctrl 2=shift 4=alt 8=meta.
static int MbMods(NSEvent* e) {
    NSUInteger m = [e modifierFlags];
    int mods = 0;
    if (m & NSEventModifierFlagControl) mods |= 1;
    if (m & NSEventModifierFlagShift)   mods |= 2;
    if (m & NSEventModifierFlagOption)  mods |= 4;
    if (m & NSEventModifierFlagCommand) mods |= 8;
    return mods;
}

// ---- Content view: blit + input forwarding ----------------------------------
@interface MbContentView : NSView
@property(nonatomic, assign) mbView* mb;
@property(nonatomic, assign) int cursor;  // latest MB_CURSOR_* pushed by the engine
@end

@implementation MbContentView
- (BOOL)isFlipped { return YES; }  // top-left origin, matching blink
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)e { return YES; }

- (void)drawRect:(NSRect)dirtyRect {
    if (!self.mb) return;
    CGFloat sc = self.window.backingScaleFactor ?: 1.0;
    int lw = (int)self.bounds.size.width, lh = (int)self.bounds.size.height;
    int w = (int)(lw * sc), h = (int)(lh * sc);
    if (w <= 0 || h <= 0) return;
    // Interactive repaint: BGRA8888, premultiplied, at physical resolution.
    int pitch = w * 4;
    static std::vector<unsigned char> buf;  // main-thread only; shared scratch
    buf.assign((size_t)pitch * h, 0);
    if (!mbRepaintToBitmap(self.mb, buf.data(), w, h, pitch))
        return;  // engine busy: keep the previous frame (never blit zeros)
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = (CGContextRef)[[NSGraphicsContext currentContext] CGContext];
    CGContextRef bmp = CGBitmapContextCreate(buf.data(), w, h, 8, pitch, cs,
        kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);  // = BGRA bytes
    CGImageRef img = CGBitmapContextCreateImage(bmp);
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, 0, lh);
    CGContextScaleCTM(ctx, 1, -1);  // buffer is top-down; the view is flipped
    CGContextDrawImage(ctx, CGRectMake(0, 0, lw, lh), img);
    CGContextRestoreGState(ctx);
    CGImageRelease(img);
    CGContextRelease(bmp);
    CGColorSpaceRelease(cs);
}

- (void)updateTrackingAreas {
    for (NSTrackingArea* a in [self trackingAreas]) [self removeTrackingArea:a];
    [self addTrackingArea:[[NSTrackingArea alloc]
        initWithRect:self.bounds
             options:(NSTrackingMouseMoved | NSTrackingActiveAlways | NSTrackingInVisibleRect)
               owner:self userInfo:nil]];
    [super updateTrackingAreas];
}

- (void)mbPoint:(NSEvent*)e x:(int*)x y:(int*)y {
    NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    *x = (int)p.x; *y = (int)p.y;  // flipped view: already top-left, logical px
}
- (void)mouseDown:(NSEvent*)e {
    if (!self.mb) return;
    [[self window] makeFirstResponder:self];   // keys follow the clicked pane
    mbSetFocus(self.mb, 1);
    int x, y; [self mbPoint:e x:&x y:&y];
    mbSendMouseDown(self.mb, x, y);
}
- (void)mouseUp:(NSEvent*)e {
    if (!self.mb) return;
    int x, y; [self mbPoint:e x:&x y:&y];
    mbSendMouseUp(self.mb, x, y);
}
- (void)mouseDragged:(NSEvent*)e { [self forwardMove:e]; }
- (void)mouseMoved:(NSEvent*)e   { [self forwardMove:e]; }
- (void)forwardMove:(NSEvent*)e {
    if (!self.mb) return;
    int x, y; [self mbPoint:e x:&x y:&y];
    mbSendMouseMove(self.mb, x, y);
    // Pointer UI: reflect the engine's pushed cursor (mbOnCursorChanged).
    switch (self.cursor) {
        case MB_CURSOR_HAND:  [[NSCursor pointingHandCursor] set]; break;
        case MB_CURSOR_IBEAM: [[NSCursor IBeamCursor] set]; break;
        default:              [[NSCursor arrowCursor] set]; break;
    }
}
- (void)rightMouseDown:(NSEvent*)e {
    if (!self.mb) return;
    int x, y; [self mbPoint:e x:&x y:&y];
    mbSendMouseClickEx(self.mb, x, y, /*button=*/2, MbMods(e));
}
- (void)scrollWheel:(NSEvent*)e {
    if (!self.mb) return;
    int x, y; [self mbPoint:e x:&x y:&y];
    double unit = [e hasPreciseScrollingDeltas] ? 1.0 : 40.0;  // lines -> px
    int dy = (int)([e scrollingDeltaY] * unit), dx = (int)([e scrollingDeltaX] * unit);
    if (dx || dy)  // DOM sign convention (deltaY>0 = content down) = -Cocoa
        mbSendWheel(self.mb, x, y, -dx, -dy, MbMods(e));
}

static const char* MbKeyName(unsigned short kc) {
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
    if (!self.mb) return;
    int mods = MbMods(e);
    const char* name = MbKeyName([e keyCode]);
    if (name) {
        mbSendKeyEx(self.mb, name, mods);           // default actions fire
    } else if (mods & (1 | 8)) {
        NSString* chars = [[e charactersIgnoringModifiers] lowercaseString];
        for (NSUInteger i = 0; i < [chars length]; ++i) {
            char key[2] = { (char)[chars characterAtIndex:i], 0 };
            if (key[0] >= 0x20 && key[0] < 0x7f) mbSendKeyEx(self.mb, key, mods);
        }
    } else {
        NSString* chars = [e characters];           // plain text (full Unicode)
        if ([chars length]) mbSendText(self.mb, [chars UTF8String]);
    }
}
@end

// ---- Host window --------------------------------------------------------------
@interface MbHostWindow : NSObject <NSWindowDelegate>
@property(nonatomic, assign) mbView* view;
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) MbContentView* content;
@end

static NSMutableArray<MbHostWindow*>* g_hosts;  // keeps hosts (and views) alive

@implementation MbHostWindow
- (void)windowDidResize:(NSNotification*)note {
    NSRect b = [self.content bounds];
    if (self.view && b.size.width > 0 && b.size.height > 0)
        mbResize(self.view, (int)b.size.width, (int)b.size.height);
}
- (void)windowWillClose:(NSNotification*)note {
    if (self.view) {
        mbDestroyView(self.view);
        self.view = nullptr;
        self.content.mb = nullptr;
    }
    [g_hosts removeObject:self];
    if (g_hosts.count == 0) [NSApp terminate:nil];
}
@end

// ---- Main-thread post queue (MbPostToMain) --------------------------------------
static std::mutex g_post_lock;
static std::vector<std::pair<void (*)(void*), void*>>* g_post_queue;

static void DrainPosted() {
    std::vector<std::pair<void (*)(void*), void*>> work;
    {
        std::lock_guard<std::mutex> al(g_post_lock);
        if (g_post_queue) work.swap(*g_post_queue);
    }
    for (auto& [fn, ud] : work) fn(ud);
}

// ---- Browser window (chrome strip + page area) -----------------------------------
@interface MbBrowserHost : NSObject <NSWindowDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) MbContentView* chromePane;
@property(nonatomic, strong) MbContentView* pagePane;
@property(nonatomic, assign) mbView* chrome;
@property(nonatomic, assign) int chromeHeight;
@property(nonatomic, assign) MbBrowserResizeFn resizeFn;
@property(nonatomic, assign) void* resizeUd;
@end

static NSMutableArray<MbBrowserHost*>* g_browsers;

@implementation MbBrowserHost
- (void)windowDidResize:(NSNotification*)note {
    NSRect cr = [[self.window contentView] bounds];
    int w = (int)cr.size.width;
    int page_h = (int)cr.size.height - self.chromeHeight;
    if (w <= 0) return;
    if (self.chrome) mbResize(self.chrome, w, self.chromeHeight);
    if (self.pagePane.mb && page_h > 0) mbResize(self.pagePane.mb, w, page_h);
    if (self.resizeFn && page_h > 0)
        self.resizeFn((MbBrowserWindow*)(__bridge void*)self, w, page_h,
                      self.resizeUd);
}
- (void)windowWillClose:(NSNotification*)note {
    [g_browsers removeObject:self];
    if (g_browsers.count == 0 && (!g_hosts || g_hosts.count == 0))
        [NSApp terminate:nil];   // the sample quits with its browser window
}
@end

// ---- The C interface (mb_window.h) --------------------------------------------
extern "C" {

void MbPostToMain(void (*fn)(void*), void* ud) {
    std::lock_guard<std::mutex> al(g_post_lock);
    if (!g_post_queue)
        g_post_queue = new std::vector<std::pair<void (*)(void*), void*>>();
    g_post_queue->emplace_back(fn, ud);
}

MbBrowserWindow* MbBrowserWindowCreate(const char* title, int width,
                                       int height, int chrome_height) {
    if (NSApp == nil) [NSApplication sharedApplication];
    if (!g_browsers) g_browsers = [NSMutableArray array];

    NSRect frame = NSMakeRect(0, 0, width, height + chrome_height);
    NSWindow* win = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered defer:NO];
    [win setTitle:(title ? [NSString stringWithUTF8String:title] : @"miniblink2")];
    [win setAcceptsMouseMovedEvents:YES];

    NSView* root = [[NSView alloc] initWithFrame:frame];
    [win setContentView:root];

    MbContentView* chromePane = [[MbContentView alloc]
        initWithFrame:NSMakeRect(0, height, width, chrome_height)];
    [chromePane setAutoresizingMask:(NSViewWidthSizable | NSViewMinYMargin)];
    [root addSubview:chromePane];

    MbContentView* pagePane =
        [[MbContentView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
    [pagePane setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
    [root addSubview:pagePane];

    CGFloat sc = [win backingScaleFactor] ?: 1.0;
    mbView* chrome = mbCreateView(width, chrome_height);
    if (!chrome) return nullptr;
    mbSetDeviceScaleFactor(chrome, (float)sc);
    chromePane.mb = chrome;
    chromePane.cursor = MB_CURSOR_POINTER;

    MbBrowserHost* h = [[MbBrowserHost alloc] init];
    h.window = win;
    h.chromePane = chromePane;
    h.pagePane = pagePane;
    h.chrome = chrome;
    h.chromeHeight = chrome_height;
    [win setDelegate:h];
    [win center];
    [win makeKeyAndOrderFront:nil];
    [win makeFirstResponder:pagePane];
    [g_browsers addObject:h];
    return (MbBrowserWindow*)(__bridge void*)h;
}

mbView* MbBrowserWindowChrome(MbBrowserWindow* w) {
    return w ? ((__bridge MbBrowserHost*)(void*)w).chrome : nullptr;
}

void MbBrowserWindowSetPage(MbBrowserWindow* w, mbView* page) {
    if (!w) return;
    MbBrowserHost* h = (__bridge MbBrowserHost*)(void*)w;
    h.pagePane.mb = page;
    if (page) {
        NSRect b = [h.pagePane bounds];
        CGFloat sc = [h.window backingScaleFactor] ?: 1.0;
        mbSetDeviceScaleFactor(page, (float)sc);
        mbResize(page, (int)b.size.width, (int)b.size.height);
        mbViewSetDirty(page);   // the pane lost its pixels: force the blit
        mbOnCursorChanged(page, [](mbView*, void* ud, int cursor) {
            ((__bridge MbContentView*)ud).cursor = cursor;
        }, (__bridge void*)h.pagePane);
    }
    [h.pagePane setNeedsDisplay:YES];
}

void MbBrowserWindowPageSize(MbBrowserWindow* w, int* out_w, int* out_h) {
    if (!w) return;
    NSRect b = [((__bridge MbBrowserHost*)(void*)w).pagePane bounds];
    if (out_w) *out_w = (int)b.size.width;
    if (out_h) *out_h = (int)b.size.height;
}

void MbBrowserWindowSetTooltip(MbBrowserWindow* w, const char* text) {
    if (!w) return;
    MbBrowserHost* h = (__bridge MbBrowserHost*)(void*)w;
    [h.pagePane setToolTip:(text && *text) ? @(text) : nil];
}

void MbBrowserWindowOnResize(MbBrowserWindow* w, MbBrowserResizeFn fn,
                             void* userdata) {
    if (!w) return;
    MbBrowserHost* h = (__bridge MbBrowserHost*)(void*)w;
    h.resizeFn = fn;
    h.resizeUd = userdata;
}

MbWindow* MbWindowCreate(const char* title, int width, int height) {
    if (NSApp == nil) [NSApplication sharedApplication];
    if (!g_hosts) g_hosts = [NSMutableArray array];

    NSRect frame = NSMakeRect(0, 0, width, height);
    NSWindow* win = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered defer:NO];
    [win setTitle:(title ? [NSString stringWithUTF8String:title] : @"miniblink2")];
    [win setAcceptsMouseMovedEvents:YES];

    MbContentView* content = [[MbContentView alloc] initWithFrame:frame];
    [content setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
    [win setContentView:content];

    // Lay out at the LOGICAL size (CSS px); the device scale factor gives
    // HiDPI raster crispness without changing layout.
    mbView* v = mbCreateView(width, height);
    if (!v) return nullptr;
    CGFloat sc = [win backingScaleFactor] ?: 1.0;
    mbSetDeviceScaleFactor(v, (float)sc);
    content.mb = v;
    content.cursor = MB_CURSOR_POINTER;
    // Engine cursor push -> the content view's stored cursor (applied on move).
    mbOnCursorChanged(v, [](mbView*, void* ud, int cursor) {
        ((__bridge MbContentView*)ud).cursor = cursor;
    }, (__bridge void*)content);

    MbHostWindow* h = [[MbHostWindow alloc] init];
    h.view = v;
    h.window = win;
    h.content = content;
    [win setDelegate:h];
    [win makeKeyAndOrderFront:nil];
    [win makeFirstResponder:content];
    if (g_hosts.count) {  // cascade later windows so they don't stack exactly
        NSPoint tl = NSMakePoint(NSMinX(g_hosts.lastObject.window.frame) + 40,
                                 NSMaxY(g_hosts.lastObject.window.frame) - 40);
        [win cascadeTopLeftFromPoint:tl];
    } else {
        [win center];
    }
    [g_hosts addObject:h];
    return (__bridge MbWindow*)h;   // owned by g_hosts until the window closes
}

mbView* MbWindowView(MbWindow* w) {
    return w ? ((__bridge MbHostWindow*)w).view : nullptr;
}

void MbWindowSetTitle(MbWindow* w, const char* title) {
    if (!w || !title) return;
    [((__bridge MbHostWindow*)w).window setTitle:[NSString stringWithUTF8String:title]];
}

void MbRunApp(void) {
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp activateIgnoringOtherApps:YES];

    // The canonical interactive tick (webview.h header): advance the world with
    // the frame timestamp, then blit only views that are actually dirty.
    [NSTimer scheduledTimerWithTimeInterval:1.0 / 60.0 repeats:YES block:^(NSTimer*) {
        DrainPosted();               // MbPostToMain work (engine off the stack)
        mbUpdateAt(CACurrentMediaTime());
        for (MbHostWindow* h in g_hosts)
            if (h.view && mbViewIsDirty(h.view))
                [h.window.contentView setNeedsDisplay:YES];
        for (MbBrowserHost* b in g_browsers) {
            if (b.chrome && mbViewIsDirty(b.chrome))
                [b.chromePane setNeedsDisplay:YES];
            if (b.pagePane.mb && mbViewIsDirty(b.pagePane.mb))
                [b.pagePane setNeedsDisplay:YES];
        }
    }];

    // Smoke-run support: MB_SAMPLE_AUTOEXIT_MS=1500 exits cleanly after 1.5 s.
    if (const char* ms = getenv("MB_SAMPLE_AUTOEXIT_MS")) {
        double t = atof(ms) / 1000.0;
        if (t > 0)
            [NSTimer scheduledTimerWithTimeInterval:t repeats:NO block:^(NSTimer*) {
                exit(0);
            }];
    }
    [NSApp run];
}

}  // extern "C"
