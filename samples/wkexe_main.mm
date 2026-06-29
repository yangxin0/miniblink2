// wkexe for macOS — a port of the Windows wkexe standalone runner (wkexe/) onto
// the macOS wke library.
//
// wkexe is a generic "open an HTML file or URL" browser: it parses a command line,
// resolves the argument to a URL (a full URL is used as-is; otherwise a local
// index.html/main.html/wkexe.html is found and loaded via file://), opens a
// window (normal or transparent/frameless), and wires the usual callbacks
// (title -> window title, document-ready, close confirmation, popups, a URL hook).
//
// The Windows wkexe uses native windows (wkeCreateWebWindow) + a Win32 message
// loop + multi-threaded render — none of which the macOS wke lib supports. So,
// like port/mac/minibrowser, this hosts an off-screen wkeWebView in a Cocoa
// NSWindow, blits wkePaint() at the Retina backing scale, and forwards NSEvents.
#import <Cocoa/Cocoa.h>
#include <vector>
#include <string>
#include "wke/wke.h"

enum {
    kWM_MOUSEMOVE = 0x0200, kWM_LBUTTONDOWN = 0x0201, kWM_LBUTTONUP = 0x0202,
    kWM_RBUTTONDOWN = 0x0204, kWM_RBUTTONUP = 0x0205,
    kWM_MBUTTONDOWN = 0x0207, kWM_MBUTTONUP = 0x0208,
    kMK_LBUTTON = 0x0001, kMK_RBUTTON = 0x0002, kMK_SHIFT = 0x0004,
    kMK_CONTROL = 0x0008, kMK_MBUTTON = 0x0010,
    kVK_BACK = 0x08, kVK_TAB = 0x09, kVK_RETURN = 0x0D, kVK_ESCAPE = 0x1B,
    kVK_LEFT = 0x25, kVK_UP = 0x26, kVK_RIGHT = 0x27, kVK_DOWN = 0x28, kVK_DELETE = 0x2E,
};

static wkeWebView g_webView = nullptr;
static NSView* g_contentView = nil;
static NSWindow* g_window = nil;
static CGFloat g_scale = 1.0;
static bool g_transparent = false;

// ---- Off-screen content view (blits the wke RGBA buffer; HiDPI-correct) ------
@interface WkeExeView : NSView
@end
@implementation WkeExeView
- (BOOL)isFlipped { return YES; }

- (void)drawRect:(NSRect)dirtyRect {
    if (!g_webView) return;
    CGFloat sc = g_scale > 0 ? g_scale : 1.0;
    int lw = (int)self.bounds.size.width, lh = (int)self.bounds.size.height;
    int w = (int)(lw * sc), h = (int)(lh * sc);
    if (w <= 0 || h <= 0) return;

    int pitch = w * 4;
    static std::vector<unsigned char> buf;
    buf.assign((size_t)pitch * h, 0);
    wkePaint(g_webView, buf.data(), pitch);

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = (CGContextRef)[[NSGraphicsContext currentContext] CGContext];
    CGContextRef bmp = CGBitmapContextCreate(buf.data(), w, h, 8, pitch, cs,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGImageRef img = CGBitmapContextCreateImage(bmp);
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, 0, lh);
    CGContextScaleCTM(ctx, 1, -1);
    CGContextDrawImage(ctx, CGRectMake(0, 0, lw, lh), img);
    CGContextRestoreGState(ctx);
    CGImageRelease(img); CGContextRelease(bmp); CGColorSpaceRelease(cs);
}

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)becomeFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)e { return YES; }
- (void)updateTrackingAreas {
    for (NSTrackingArea* a in [self trackingAreas]) [self removeTrackingArea:a];
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:[self bounds]
        options:(NSTrackingMouseMoved | NSTrackingActiveAlways | NSTrackingInVisibleRect)
        owner:self userInfo:nil]];
    [super updateTrackingAreas];
}
- (void)wkePoint:(NSEvent*)e x:(int*)x y:(int*)y flags:(unsigned int*)flags {
    NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    *x = (int)(p.x * g_scale); *y = (int)(p.y * g_scale);
    unsigned int f = 0; NSUInteger m = [e modifierFlags];
    if (m & NSEventModifierFlagShift) f |= kMK_SHIFT;
    if (m & NSEventModifierFlagControl) f |= kMK_CONTROL;
    NSUInteger b = [NSEvent pressedMouseButtons];
    if (b & 1) f |= kMK_LBUTTON; if (b & 2) f |= kMK_RBUTTON; if (b & 4) f |= kMK_MBUTTON;
    *flags = f;
}
- (void)fireMouse:(unsigned int)msg event:(NSEvent*)e {
    if (!g_webView) return;
    int x, y; unsigned int fl; [self wkePoint:e x:&x y:&y flags:&fl];
    wkeFireMouseEvent(g_webView, msg, x, y, fl); [self setNeedsDisplay:YES];
}
- (void)mouseDown:(NSEvent*)e        { wkeSetFocus(g_webView); [self fireMouse:kWM_LBUTTONDOWN event:e]; }
- (void)mouseUp:(NSEvent*)e          { [self fireMouse:kWM_LBUTTONUP event:e]; }
- (void)mouseDragged:(NSEvent*)e     { [self fireMouse:kWM_MOUSEMOVE event:e]; }
- (void)mouseMoved:(NSEvent*)e       { [self fireMouse:kWM_MOUSEMOVE event:e]; }
- (void)rightMouseDown:(NSEvent*)e   {
    if (!g_webView) return;
    int x, y; unsigned int fl; [self wkePoint:e x:&x y:&y flags:&fl];
    wkeFireMouseEvent(g_webView, kWM_RBUTTONDOWN, x, y, fl);
    [self setNeedsDisplay:YES];  // (modern wke has no wkeFireContextMenuEvent)
}
- (void)rightMouseUp:(NSEvent*)e     { [self fireMouse:kWM_RBUTTONUP event:e]; }
- (void)scrollWheel:(NSEvent*)e {
    if (!g_webView) return;
    int x, y; unsigned int fl; [self wkePoint:e x:&x y:&y flags:&fl];
    double dy = [e hasPreciseScrollingDeltas] ? [e scrollingDeltaY] : [e scrollingDeltaY] * 10.0;
    int d = (int)(dy * 3.0); if (d) wkeFireMouseWheelEvent(g_webView, x, y, d, fl);
    [self setNeedsDisplay:YES];
}
static unsigned int vkFromKeyCode(unsigned short kc) {
    switch (kc) {
        case 51: return kVK_BACK; case 48: return kVK_TAB; case 36: case 76: return kVK_RETURN;
        case 53: return kVK_ESCAPE; case 117: return kVK_DELETE;
        case 123: return kVK_LEFT; case 124: return kVK_RIGHT; case 126: return kVK_UP; case 125: return kVK_DOWN;
        default: return 0;
    }
}
- (void)keyDown:(NSEvent*)e {
    if (!g_webView) return;
    unsigned int vk = vkFromKeyCode([e keyCode]); if (vk) wkeFireKeyDownEvent(g_webView, vk, 0, false);
    NSString* s = [e characters];
    for (NSUInteger i = 0; i < [s length]; ++i) { unichar c = [s characterAtIndex:i];
        if (c >= 0x20 && c != 0x7f) wkeFireKeyPressEvent(g_webView, c, 0, false); }
    [self setNeedsDisplay:YES];
}
- (void)keyUp:(NSEvent*)e {
    if (!g_webView) return;
    unsigned int vk = vkFromKeyCode([e keyCode]); if (vk) wkeFireKeyUpEvent(g_webView, vk, 0, false);
}
@end

@interface WkeExeWindow : NSWindow @end
@implementation WkeExeWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
@end

// ---- wke callbacks (ported from wkexe/app.cpp) ------------------------------
// NOTE (modern port): miniblink49's wkeOnPaintUpdated (an async HDC paint signal)
// has no equivalent in the modern wke, which renders SYNCHRONOUSLY. A 60fps timer
// in main() drives [g_contentView setNeedsDisplay], and drawRect: pulls fresh
// pixels via wkePaint() — so no onPaintUpdated callback is needed.
static void onTitleChanged(wkeWebView, void*, const wkeString title) {  // -> window title
    const utf8* t = title ? wkeGetString(title) : "";
    NSString* s = t ? [NSString stringWithUTF8String:t] : @"";
    dispatch_async(dispatch_get_main_queue(), ^{ [g_window setTitle:s.length ? s : @"wkexe"]; });
}
// HandleWindowClosing: confirm before quitting (wkexe pops a yes/no box).
@interface WkeExeDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate> @end
@implementation WkeExeDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)s { return YES; }
// Keep the web view sized to the window (in physical pixels) as it resizes,
// otherwise the page only paints in its original area and the rest stays blank.
- (void)windowDidResize:(NSNotification*)note {
    if (!g_webView) return;
    NSRect b = [g_contentView bounds];
    int w = (int)(b.size.width * g_scale), h = (int)(b.size.height * g_scale);
    if (w <= 0 || h <= 0) return;
    wkeResize(g_webView, w, h);
    [g_contentView setNeedsDisplay:YES];
}
- (BOOL)windowShouldClose:(NSWindow*)w {
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = @"确定要退出程序吗？";   // "Quit the program?" (matches wkexe)
    [a addButtonWithTitle:@"Yes"]; [a addButtonWithTitle:@"No"];
    return [a runModal] == NSAlertFirstButtonReturn;
}
@end

// ---- URL resolution (ported from wkexe/app.cpp FixupHtmlUrl) ----------------
static bool fileExists(NSString* p) { return [[NSFileManager defaultManager] fileExistsAtPath:p]; }

// Resolve an arg to a URL: a full "scheme://" is used as-is; otherwise look for a
// local file (the given name, or the index/main/wkexe defaults) in the cwd and the
// executable's directory; fall back to a default URL.
static std::string resolveUrl(const char* arg) {
    NSString* a = arg ? [NSString stringWithUTF8String:arg] : @"";
    if ([a rangeOfString:@"://"].location != NSNotFound)
        return std::string([a UTF8String]);

    NSMutableArray<NSString*>* names = [NSMutableArray array];
    if (a.length) [names addObject:a];
    else { [names addObjectsFromArray:@[@"index.html", @"main.html", @"wkexe.html"]]; }

    NSString* cwd = [[NSFileManager defaultManager] currentDirectoryPath];
    NSString* exeDir = [[[NSBundle mainBundle] executablePath] stringByDeletingLastPathComponent];
    for (NSString* base in @[cwd, exeDir ?: cwd]) {
        for (NSString* n in names) {
            NSString* full = [n hasPrefix:@"/"] ? n : [base stringByAppendingPathComponent:n];
            if (fileExists(full))
                return std::string("file://") + [full UTF8String];
        }
    }
    return "https://www.baidu.com";   // wkexe's fallback default
}

static void printHelp() {
    printf("wkexe (macOS) — a miniblink wke runner\n\n"
           "Usage: wkexe [options] [file-or-url]\n\n"
           "  file-or-url       a full http(s)://… URL, or a local .html file.\n"
           "                    With no argument, looks for index.html / main.html /\n"
           "                    wkexe.html in the current and executable directories.\n\n"
           "Options:\n"
           "  -t, --transparent   frameless (transparent) window\n"
           "  -h, --help          show this help\n");
}

int main(int argc, const char** argv) {
    @autoreleasepool {
        const char* fileArg = nullptr;
        for (int i = 1; i < argc; ++i) {
            std::string o = argv[i];
            if (o == "-t" || o == "--transparent") g_transparent = true;
            else if (o == "-h" || o == "--help") { printHelp(); return 0; }
            else if (!fileArg && o[0] != '-') fileArg = argv[i];
        }
        std::string url = resolveUrl(fileArg);
        const int W = 640, H = 480;   // wkexe's default window size

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        WkeExeDelegate* del = [[WkeExeDelegate alloc] init];
        [NSApp setDelegate:del];

        NSRect frame = NSMakeRect(0, 0, W, H);
        NSUInteger style = g_transparent
            ? NSWindowStyleMaskBorderless
            : (NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
               NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable);
        g_window = [[WkeExeWindow alloc] initWithContentRect:frame styleMask:style
            backing:NSBackingStoreBuffered defer:NO];
        [g_window setTitle:@"wkexe"];
        [g_window setDelegate:del];
        if (g_transparent) { [g_window setOpaque:NO]; [g_window setBackgroundColor:[NSColor clearColor]];
                             [g_window setMovableByWindowBackground:YES]; }

        g_contentView = [[WkeExeView alloc] initWithFrame:frame];
        [g_window setContentView:g_contentView];
        [g_window setAcceptsMouseMovedEvents:YES];
        [g_window center];
        [g_window makeKeyAndOrderFront:nil];
        [g_window makeFirstResponder:g_contentView];
        [NSApp activateIgnoringOtherApps:YES];

        g_scale = [g_window backingScaleFactor]; if (g_scale <= 0) g_scale = 1.0;
        wkeInitialize();
        g_webView = wkeCreateWebView();
        wkeResize(g_webView, (int)(W * g_scale), (int)(H * g_scale));
        wkeSetZoomFactor(g_webView, g_scale);
        wkeOnTitleChanged(g_webView, onTitleChanged, nullptr);
        wkeLoadURL(g_webView, url.c_str());
        NSLog(@"[wkexe] loading %s%s", url.c_str(), g_transparent ? "  (transparent)" : "");

        [NSTimer scheduledTimerWithTimeInterval:1.0/60.0 repeats:YES block:^(NSTimer*) {
            [g_contentView setNeedsDisplay:YES];  // synchronous render -> just repaint
        }];
        [NSApp run];
    }
    return 0;
}
