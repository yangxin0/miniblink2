// mb_smoke_platform — split from the mb_smoke monolith: cookies, screenshots,
// scraping/selectors, storage, history, blob, forms, validation, PNG encode.
#include "miniblink_host/test/mb_smoke_harness.h"
#include "miniblink_host/test/mb_test_seams.h"

#include <chrono>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "net/base/filename_util.h"
#include "url/gurl.h"

using mbsmoke::Eval;
using mbsmoke::EvalIso;
using mbsmoke::Expect;

namespace {

struct DestroyNavigationProbe {
  mbView* expected_view = nullptr;
  mbNavigationId started_id = 0;
  int started = 0;
  int committed = 0;
  int terminal = 0;
  int malformed = 0;
  int callbacks_during_or_after_teardown = 0;
  bool teardown_started = false;
};

void RecordDestroyNavigationEvent(mbView* view,
                                  void* userdata,
                                  const mbNavigationEvent* event) {
  auto* probe = static_cast<DestroyNavigationProbe*>(userdata);
  if (!probe || !event)
    return;
  if (probe->teardown_started)
    ++probe->callbacks_during_or_after_teardown;
  if (view != probe->expected_view ||
      event->struct_size != static_cast<int>(sizeof(mbNavigationEvent))) {
    ++probe->malformed;
  }
  switch (event->phase) {
    case MB_NAVIGATION_PHASE_STARTED:
      ++probe->started;
      probe->started_id = event->navigation_id;
      break;
    case MB_NAVIGATION_PHASE_COMMITTED:
      ++probe->committed;
      break;
    case MB_NAVIGATION_PHASE_TERMINAL:
      ++probe->terminal;
      break;
    default:
      ++probe->malformed;
      break;
  }
}

}  // namespace

static void RunCases(mbView* v, int W, int H) {
  // 87. Cookie session round-trip: mbSetCookie injects into the HTTP jar and
  // mbGetCookies reads it back; mbClearCookies empties it. (In-memory jar, so no
  // network — exercises MbAddCookieToJar + MbGetCookiesForUrl + MbClearCookieJar.)
  {
    const char* kurl = "https://session-test.example/";
    mbClearCookies(v);  // start clean (jar is process-wide)
    mbSetCookie(v, kurl, "sid=abc123; Path=/");
    mbSetCookie(v, kurl, "theme=dark");
    char cb[512] = {0};
    mbGetCookies(v, kurl, cb, sizeof(cb));
    Expect(std::string(cb).find("sid=abc123") != std::string::npos &&
               std::string(cb).find("theme=dark") != std::string::npos,
           "mbSetCookie injects cookies that mbGetCookies reads back", cb);
    // mbGetCookie reads one cookie by name; an absent name returns -1.
    char one[64] = {0};
    const int slen = mbGetCookie(v, kurl, "sid", one, sizeof(one));
    const bool one_ok = slen == 6 && std::string(one) == "abc123";
    const bool absent = mbGetCookie(v, kurl, "nope", one, sizeof(one)) == -1;
    Expect(one_ok && absent,
           "mbGetCookie reads a single cookie by name (-1 when absent)",
           std::string("sid=") + one + " absent=" + (absent ? "1" : "0"));
    mbClearCookies(v);
    char cb2[512] = {0};
    mbGetCookies(v, kurl, cb2, sizeof(cb2));
    Expect(std::string(cb2).find("sid=") == std::string::npos,
           "mbClearCookies empties the jar", cb2);
  }

  // 87b. document.cookie reflects the HTTP jar, not just JS-set cookies. A cookie injected via
  // mbSetCookie — exactly how a server Set-Cookie or a restored session enters the jar — must show
  // up in document.cookie even though it was never set through JS, alongside a JS-set cookie.
  // (Previously document.cookie read only the in-memory JS-set store, so server cookies were
  // invisible to the page.) Offline: needs an http(s) origin for the jar.
  {
    const char* kurl = "https://jarread.test/";
    mbClearCookies(v);
    mbSetCookie(v, kurl, "srvonly=fromjar");
    mbLoadHTML(v, "<body>x</body>", kurl);
    mbRunJS(v, "document.cookie='jsset=1';");
    const std::string c = Eval(v, "document.cookie");
    Expect(c.find("srvonly=fromjar") != std::string::npos &&
               c.find("jsset=1") != std::string::npos,
           "document.cookie reflects jar cookies (server/mbSetCookie) + JS-set",
           "ck=[" + c + "]");
    mbClearCookies(v);
  }

  // 88. mbSendKey("Enter") submits a form — a TRUSTED default action that a
  // JS-dispatched (untrusted) KeyboardEvent cannot trigger. Fill+focus the input,
  // press Enter; the form's submit handler runs.
  {
    mbLoadHTML(v,
        "<body><form id='f' onsubmit='window.__s=1;return false;'>"
        "<input id='inp' name='q'></form></body>", "about:blank");
    mbFillSelector(v, "#inp", "hello");  // also focuses the input
    mbSendKey(v, "Enter");
    mbWait(v, 50);
    Expect(Eval(v, "String(window.__s||0)") == "1",
           "mbSendKey Enter triggers form submission (trusted default action)");
  }

  // 89. Page focus is set, enabling focus-dependent behavior: document.hasFocus()
  // is true, <input autofocus> grabs focus on load, and mbSendKey("Tab") advances
  // focus to the next field (FocusController::AdvanceFocus, gated on page focus).
  {
    mbLoadHTML(v, "<body><input id='a' autofocus><input id='b'></body>",
               "about:blank");
    mbWait(v, 30);
    const bool has_focus = Eval(v, "String(document.hasFocus())") == "true";
    const bool autofocused = Eval(v, "document.activeElement.id") == "a";
    mbSendKey(v, "Tab");
    mbWait(v, 30);
    const bool tab_advanced = Eval(v, "document.activeElement.id") == "b";
    Expect(has_focus && autofocused && tab_advanced,
           "page focus enables autofocus + Tab focus-advance",
           std::string("hasFocus=") + (has_focus ? "1" : "0") + " autofocus=" +
               (autofocused ? "1" : "0") + " tab=" + (tab_advanced ? "1" : "0"));
  }

  // 89b. mbSetVisibility drives page visibility: the page starts 'visible'; backgrounding it
  // makes document.visibilityState 'hidden' + document.hidden true and fires visibilitychange;
  // foregrounding restores 'visible' and fires it again. Lets a host pause work when hidden.
  {
    mbLoadHTML(v, "<body>x</body>", "about:blank");
    Eval(v,
         "window.__vc=[];"
         "document.addEventListener('visibilitychange',function(){"
         "window.__vc.push(document.visibilityState+(document.hidden?'/h':'/s'));});");
    const std::string start = Eval(v, "document.visibilityState");
    mbSetVisibility(v, 0);
    mbWait(v, 30);
    const std::string hidden = Eval(v, "document.visibilityState+','+document.hidden");
    mbSetVisibility(v, 1);
    mbWait(v, 30);
    const std::string back = Eval(v, "document.visibilityState+','+document.hidden");
    const std::string events = Eval(v, "window.__vc.join(',')");
    const std::string r = start + "|" + hidden + "|" + back + "|" + events;
    Expect(r == "visible|hidden,true|visible,false|hidden/h,visible/s",
           "mbSetVisibility toggles document.visibilityState/hidden + fires visibilitychange",
           "vis=[" + r + "]");
  }

  // 89c. mbSetOnline drives navigator.onLine + window online/offline events. The page starts
  // online; going offline flips navigator.onLine false and fires 'offline'; coming back fires
  // 'online'. Lets a host test offline-aware behavior (banners, sync pausing, PWA fallbacks).
  {
    mbLoadHTML(v, "<body>x</body>", "about:blank");
    Eval(v,
         "window.__net=[];"
         "addEventListener('offline',function(){window.__net.push('off:'+navigator.onLine);});"
         "addEventListener('online',function(){window.__net.push('on:'+navigator.onLine);});");
    const std::string start = Eval(v, "String(navigator.onLine)");
    mbSetOnline(0);
    mbWait(v, 30);
    const std::string off = Eval(v, "String(navigator.onLine)");
    mbSetOnline(1);
    mbWait(v, 30);
    const std::string on = Eval(v, "String(navigator.onLine)");
    const std::string events = Eval(v, "window.__net.join(',')");
    const std::string r = start + "|" + off + "|" + on + "|" + events;
    Expect(r == "true|false|true|off:false,on:true",
           "mbSetOnline toggles navigator.onLine + fires online/offline events",
           "net=[" + r + "]");
  }

  // 90. mbGetElementRect + element screenshot: get a colored div's box, paint
  // exactly that rect, and verify the captured center pixel is the div's color.
  {
    mbLoadHTML(v,
        "<body style='margin:0'><div id='box' style='position:absolute;"
        "left:20px;top:30px;width:40px;height:50px;background:rgb(255,0,0)'>"
        "</div></body>", "about:blank");
    std::vector<uint8_t> full(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, full.data(), W, H, W * 4);  // force layout
    int x = 0, y = 0, w = 0, h = 0;
    const bool got = mbGetElementRect(v, "#box", &x, &y, &w, &h) == 1;
    bool red = false;
    if (got && w > 0 && h > 0) {
      std::vector<uint8_t> el(static_cast<size_t>(w) * h * 4, 0);
      if (mbPaintRectToBitmap(v, el.data(), x, y, w, h, w * 4) == 1) {
        const size_t ci =
            (static_cast<size_t>(h / 2) * w + w / 2) * 4;  // center px, BGRA
        red = el[ci + 2] > 200 && el[ci + 1] < 60 && el[ci] < 60;
      }
    }
    Expect(got && x == 20 && y == 30 && w == 40 && h == 50 && red,
           "mbGetElementRect + element screenshot captures the element",
           std::to_string(x) + "," + std::to_string(y) + "," +
               std::to_string(w) + "," + std::to_string(h) +
               " red=" + (red ? "1" : "0"));
  }

  // 90a2. mbSaveElementPng does that whole dance in one call: scroll the element
  // into view + clip its box to a PNG. A below-the-fold 120x40 div -> the saved
  // PNG's IHDR dimensions equal the element size (dsf 1). No-match -> 0.
  {
    mbSetDeviceScaleFactor(v, 1.0f);
    mbResize(v, W, H);
    mbLoadHTML(v,
        "<body style='margin:0'><div style='height:1200px'></div>"
        "<div id='box' style='width:120px;height:40px;background:#0000ff'></div>"
        "</body>", "about:blank");
    const char* p = "/tmp/mb_elem_shot.png";
    const bool ok = mbSaveElementPng(v, "#box", p) == 1;
    unsigned pw = 0, ph = 0;
    bool png = false;
    if (FILE* f = std::fopen(p, "rb")) {
      unsigned char hd[24] = {0};
      const size_t n = std::fread(hd, 1, 24, f);
      std::fclose(f);
      if (n == 24 && hd[0] == 0x89 && hd[1] == 'P') {  // PNG sig + IHDR dims (BE)
        pw = (hd[16] << 24) | (hd[17] << 16) | (hd[18] << 8) | hd[19];
        ph = (hd[20] << 24) | (hd[21] << 16) | (hd[22] << 8) | hd[23];
        png = true;
      }
    }
    const bool dims_ok = png && pw == 120 && ph == 40;
    const bool none_ok = mbSaveElementPng(v, "#none", p) == 0;
    Expect(ok && dims_ok && none_ok,
           "mbSaveElementPng captures one element (PNG dims == element box)",
           std::string("ok=") + (ok ? "1" : "0") + " dims=" + std::to_string(pw) +
               "x" + std::to_string(ph) + " none=" + (none_ok ? "1" : "0"));
    std::remove(p);
  }

  // 90b. Text actually rasterizes to glyph pixels. Every other screenshot test
  // checks solid color fills, so a font/Skia regression that blanks text would
  // pass silently while screenshots became useless. Black text on white: the
  // painted bitmap must have dark (glyph) pixels AND antialiased edge pixels
  // (real glyphs, not a solid block) — and a blank page must have ~none, which
  // proves the check measures text rather than always-present noise. (Pixels are
  // BGRA; the dark/white tests are channel-symmetric so order doesn't matter.)
  {
    mbLoadHTML(v, "<body style='margin:0;background:#fff'></body>",
               "about:blank");
    mbWait(v, 30);
    std::vector<uint8_t> blank(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, blank.data(), W, H, W * 4);
    int blank_dark = 0;
    for (size_t i = 0; i + 3 < blank.size(); i += 4)
      if (blank[i] < 80 && blank[i + 1] < 80 && blank[i + 2] < 80) ++blank_dark;

    mbLoadHTML(v,
        "<body style='margin:0;background:#fff'><div style='font-size:40px;"
        "color:#000;font-family:sans-serif;padding:10px'>Hello World ABC 123"
        "</div></body>", "about:blank");
    mbWait(v, 30);
    std::vector<uint8_t> buf(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, buf.data(), W, H, W * 4);
    int dark = 0, aa = 0;
    for (size_t i = 0; i + 3 < buf.size(); i += 4) {
      const int b = buf[i], g = buf[i + 1], r = buf[i + 2];
      if (r < 80 && g < 80 && b < 80) ++dark;
      else if (!(r > 240 && g > 240 && b > 240)) ++aa;  // partial -> antialiased
    }
    Expect(blank_dark < 50 && dark > 200 && aa > 50,
           "text rasterizes to glyph pixels (font render path)",
           std::string("blankDark=") + std::to_string(blank_dark) + " dark=" +
               std::to_string(dark) + " aa=" + std::to_string(aa));
  }

  // 91. mbHoverSelector fires mouseover and applies :hover. A target with a
  // mouseover handler (sets a flag) and a :hover rule (changes color); hovering
  // must trigger both.
  {
    mbLoadHTML(v,
        "<style>#t{color:rgb(1,2,3)} #t:hover{color:rgb(9,8,7)}</style>"
        "<body style='margin:0'><div id='t' style='width:100px;height:40px' "
        "onmouseover='window.__h=1'>hover me</div></body>", "about:blank");
    std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // force layout for hit-testing
    const bool hovered = mbHoverSelector(v, "#t") == 1;
    mbWait(v, 30);
    const bool handler = Eval(v, "String(window.__h||0)") == "1";
    const bool css_hover =
        Eval(v, "getComputedStyle(document.getElementById('t')).color") ==
        "rgb(9, 8, 7)";
    Expect(hovered && handler && css_hover,
           "mbHoverSelector fires mouseover + applies :hover",
           std::string("handler=") + (handler ? "1" : "0") + " css=" +
               (css_hover ? "1" : "0"));
  }

  // 92. mbGetContentSize + full-page capture: a page taller than the viewport.
  // mbGetContentSize reports the full height; resizing to it and painting renders
  // a marker that sits below the original fold (its pixel becomes its color).
  {
    mbResize(v, W, H);  // reset viewport (a prior test may have grown it)
    // Viewport is H=300; place a blue marker at top:500 (below the fold).
    mbLoadHTML(v,
        "<body style='margin:0'><div style='height:500px'></div>"
        "<div id='m' style='height:40px;background:rgb(0,0,255)'></div></body>",
        "about:blank");
    std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // initial layout
    int cw = 0, ch = 0;
    const bool got = mbGetContentSize(v, &cw, &ch) == 1;
    // ~540 content (500+40), NOT the 300 viewport — verifies it's the document
    // size; the upper bound guards against leftover-viewport contamination.
    const bool tall = ch >= 540 && ch <= 600;
    // Full-page capture: grow to content height and paint; the marker (at y~500)
    // is now in-view and rendered blue (BGRA: B=255,G=0,R=0).
    bool blue = false;
    if (got && tall) {
      mbResize(v, W, ch);
      std::vector<uint8_t> full(static_cast<size_t>(W) * ch * 4, 0);
      mbPaintToBitmap(v, full.data(), W, ch, W * 4);
      const size_t ci = (static_cast<size_t>(520) * W + W / 2) * 4;  // y=520, BGRA
      blue = full[ci] > 200 && full[ci + 1] < 60 && full[ci + 2] < 60;
      mbResize(v, W, H);  // restore
    }
    Expect(got && tall && blue,
           "mbGetContentSize + full-page capture renders below-the-fold content",
           std::string("h=") + std::to_string(ch) + " blue=" + (blue ? "1" : "0"));
  }

  // 93. mbSelectOption: choose a <select> option by value and by visible text,
  // and confirm select.value updates + change fires. A non-matching value fails.
  {
    mbLoadHTML(v,
        "<body><select id='s' onchange='window.__c=(window.__c||0)+1'>"
        "<option value='a'>Apple</option><option value='b'>Banana</option>"
        "<option value='c'>Cherry</option></select></body>", "about:blank");
    const bool by_value = mbSelectOption(v, "#s", "b") == 1 &&
                          Eval(v, "document.getElementById('s').value") == "b";
    const bool by_text = mbSelectOption(v, "#s", "Cherry") == 1 &&
                         Eval(v, "document.getElementById('s').value") == "c";
    const bool no_match = mbSelectOption(v, "#s", "nope") == 0;
    const bool changed = Eval(v, "String(window.__c||0)") == "2";  // two selects
    Expect(by_value && by_text && no_match && changed,
           "mbSelectOption selects by value/text + fires change",
           std::string("val=") + (by_value ? "1" : "0") + " text=" +
               (by_text ? "1" : "0") + " nomatch=" + (no_match ? "1" : "0") +
               " change=" + (changed ? "1" : "0"));
  }

  // 94. mbDoubleClickSelector fires dblclick (and a single click does not).
  {
    mbLoadHTML(v,
        "<body style='margin:0'><div id='d' style='width:100px;height:40px' "
        "ondblclick='window.__dc=1' onclick='window.__sc=(window.__sc||0)+1'>"
        "x</div></body>", "about:blank");
    std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // layout for hit-testing
    mbClickSelector(v, "#d");  // single click: no dblclick
    const bool no_dc_yet = Eval(v, "String(window.__dc||0)") == "0";
    const bool dbl = mbDoubleClickSelector(v, "#d") == 1;
    mbWait(v, 30);
    const bool dc = Eval(v, "String(window.__dc||0)") == "1";
    Expect(no_dc_yet && dbl && dc,
           "mbDoubleClickSelector fires dblclick",
           std::string("single_no_dc=") + (no_dc_yet ? "1" : "0") + " dbl=" +
               (dbl ? "1" : "0") + " dc=" + (dc ? "1" : "0"));
  }

  // 95. mbRightClickSelector fires contextmenu (preventDefault to keep it inert).
  {
    mbLoadHTML(v,
        "<body style='margin:0'><div id='r' style='width:100px;height:40px' "
        "oncontextmenu='window.__cm=1;return false'>x</div></body>",
        "about:blank");
    std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // layout for hit-testing
    const bool rc = mbRightClickSelector(v, "#r") == 1;
    mbWait(v, 30);
    Expect(rc && Eval(v, "String(window.__cm||0)") == "1",
           "mbRightClickSelector fires contextmenu");
  }

  // 96. C API buffer contract: the size-returning string getters must (a) return
  // the FULL length even when the buffer is too small, (b) NUL-terminate the
  // truncated copy without overflowing, and (c) support the size-first/allocate/
  // refill two-call pattern. Stress mbGetHTML on a page whose DOM is much larger
  // than the buffer.
  {
    std::string big = "<body>";
    for (int bi = 0; bi < 200; ++bi)
      big += "<p class='x'>paragraph number " + std::to_string(bi) + "</p>";
    big += "</body>";
    mbLoadHTML(v, big.c_str(), "about:blank");
    // Tiny buffer with a guard byte just past the cap — must stay untouched.
    char tiny[17];
    std::memset(tiny, 0x7F, sizeof(tiny));
    const int full_len = mbGetHTML(v, tiny, 16);  // cap 16 -> 15 chars + NUL
    const bool full_returned = full_len > 16;     // reports the true length
    const bool nul_terminated = tiny[15] == '\0';
    const bool no_overflow = static_cast<unsigned char>(tiny[16]) == 0x7F;
    // Size-first: cap 0 returns the length without writing; then a right-sized
    // buffer captures the whole document.
    const int sized = mbGetHTML(v, nullptr, 0);
    std::vector<char> buf(sized + 1, 1);
    const int got = mbGetHTML(v, buf.data(), sized + 1);
    const bool complete = got == sized && buf[sized] == '\0' &&
                          std::string(buf.data()).find("paragraph number 199") !=
                              std::string::npos;
    Expect(full_returned && nul_terminated && no_overflow && sized == full_len &&
               complete,
           "C API string buffers: full length, NUL-terminated, no overflow",
           std::string("len=") + std::to_string(full_len) + " term=" +
               (nul_terminated ? "1" : "0") + " noovf=" +
               (no_overflow ? "1" : "0") + " complete=" + (complete ? "1" : "0"));
  }

  // 97. mbFocusSelector / mbBlurSelector fire focus/blur and update activeElement.
  // Blur is the validation trigger (the handler reads the field's value).
  {
    mbLoadHTML(v,
        "<body><input id='i' onfocus='window.__f=1' "
        "onblur='window.__b=this.value'><button id='other'>o</button></body>",
        "about:blank");
    const bool foc = mbFocusSelector(v, "#i") == 1;
    const bool focused = Eval(v, "document.activeElement.id") == "i" &&
                         Eval(v, "String(window.__f||0)") == "1";
    mbRunJS(v, "document.getElementById('i').value='typed';");
    const bool blr = mbBlurSelector(v, "#i") == 1;
    const bool blurred = Eval(v, "document.activeElement.id") != "i" &&
                         Eval(v, "String(window.__b||'')") == "typed";
    Expect(foc && focused && blr && blurred,
           "mbFocusSelector/mbBlurSelector fire focus/blur (blur = validation)",
           std::string("foc=") + (focused ? "1" : "0") + " blur=" +
               (blurred ? "1" : "0"));
  }

  // 98. History stress on a FRESH view (clean stack): deep navigation +
  // back-to-start + forward + forward-truncation. Exercises the history index/
  // dedup/truncation logic end to end.
  {
    mbView* hv = mbCreateView(W, H);
    const char ids[5] = {'A', 'B', 'C', 'D', 'E'};
    std::string paths[5];
    for (int hi = 0; hi < 5; ++hi) {
      paths[hi] = std::string("/tmp/mb_hs") + ids[hi] + ".html";
      std::string doc = std::string("<body><div id=o>") + ids[hi] + "</div></body>";
      if (FILE* f = std::fopen(paths[hi].c_str(), "wb")) {
        std::fwrite(doc.data(), 1, doc.size(), f); std::fclose(f);
      }
    }
    auto cur = [&]() {
      return Eval(hv, "document.getElementById('o').textContent");
    };
    for (int hi = 0; hi < 5; ++hi)
      mbLoadURL(hv, ("file://" + paths[hi]).c_str());  // history: A B C D E
    const bool at_e = cur() == "E" && mbCanGoForward(hv) == 0;
    int steps = 0;
    while (mbCanGoBack(hv) == 1 && steps < 10) {
      mbGoBack(hv); mbWait(hv, 40); ++steps;
    }
    const bool at_a = cur() == "A" && steps == 4 && mbCanGoBack(hv) == 0;
    mbGoForward(hv); mbWait(hv, 40);  // -> B
    mbGoForward(hv); mbWait(hv, 40);  // -> C
    const bool at_c = cur() == "C";
    std::string xdoc = "<body><div id=o>X</div></body>";
    if (FILE* f = std::fopen("/tmp/mb_hsX.html", "wb")) {
      std::fwrite(xdoc.data(), 1, xdoc.size(), f); std::fclose(f);
    }
    mbLoadURL(hv, "file:///tmp/mb_hsX.html");  // a new nav at C -> truncates D,E
    mbWait(hv, 40);
    const bool truncated = cur() == "X" && mbCanGoForward(hv) == 0;
    mbGoBack(hv); mbWait(hv, 40);
    const bool back_to_c = cur() == "C";  // back from X lands on C (not D)
    mbDestroyView(hv);
    Expect(at_e && at_a && at_c && truncated && back_to_c,
           "history stress: deep nav + back-to-start + forward-truncation",
           std::string("e=") + (at_e ? "1" : "0") + " a=" + (at_a ? "1" : "0") +
               " c=" + (at_c ? "1" : "0") + " trunc=" + (truncated ? "1" : "0") +
               " backC=" + (back_to_c ? "1" : "0"));
  }

  // 99. HTML form constraint validation: validity states, checkValidity(), the
  // :invalid pseudo-class, and setCustomValidity — a significant feature area
  // that was previously uncovered (guards against a future regression).
  {
    mbLoadHTML(v,
        "<body><form id='f'>"
        "<input id='req' required value=''>"
        "<input id='email' type='email' value='notanemail'>"
        "<input id='num' type='number' min='5' max='10' value='3'>"
        "<input id='ok' value='fine'></form></body>", "about:blank");
    const bool states =
        Eval(v, "String(document.getElementById('req').validity.valueMissing)") ==
            "true" &&
        Eval(v, "String(document.getElementById('email').validity.typeMismatch)") ==
            "true" &&
        Eval(v, "String(document.getElementById('num').validity.rangeUnderflow)") ==
            "true";
    const bool check =
        Eval(v, "String(document.getElementById('f').checkValidity())") ==
            "false" &&
        Eval(v, "String(document.getElementById('ok').checkValidity())") == "true";
    const bool invalid_sel =
        Eval(v, "String(document.querySelectorAll('input:invalid').length)") == "3";
    const bool custom = Eval(v,
        "(function(){var o=document.getElementById('ok');o.setCustomValidity('x');"
        "var r=!o.checkValidity();o.setCustomValidity('');return String(r);})()") ==
            "true";
    Expect(states && check && invalid_sel && custom,
           "form validation: validity states + checkValidity + :invalid + custom",
           std::string("states=") + (states ? "1" : "0") + " check=" +
               (check ? "1" : "0") + " sel=" + (invalid_sel ? "1" : "0") +
               " custom=" + (custom ? "1" : "0"));
  }

  // 100. fetch(blob:) end to end: URL.createObjectURL + fetch reads the blob's
  // bytes. Landed via the in-process BlobURLStore (bound on the service thread
  // through a navigation-associated-interface proxy, so the [Sync] Register
  // doesn't deadlock) + a URLLoaderFactory that delegates to Blob::Load, plus the
  // donor patch that skips the host loader for blob: URLs.
  {
    mbLoadHTML(v, "<body>x</body>", "about:blank");
    mbRunJS(v,
        "window.__bf='';var b=new Blob(['BLOBFETCH-OK'],{type:'text/plain'});"
        "var u=URL.createObjectURL(b);"
        "fetch(u).then(function(r){return r.text();}).then(function(t){"
        "window.__bf=t;}).catch(function(e){window.__bf='ERR:'+e.name;});");
    mbWait(v, 400);  // async fetch of the blob
    Expect(Eval(v, "String(window.__bf)") == "BLOBFETCH-OK",
           "fetch(blob:) reads the blob's bytes (createObjectURL + fetch)",
           Eval(v, "String(window.__bf)"));
  }

  // 101. <img src=blob:> decodes an image from a blob: URL. Same native blob
  // loader path as fetch(blob:) (BlobURLStore.ResolveAsURLLoaderFactory ->
  // Blob::Load), exercised by the image resource loader + decoder. The blob
  // must be built from a Uint8Array (a DOMString blob would UTF-8-mangle bytes
  // >127 and corrupt the PNG). 1x1 red PNG.
  {
    mbLoadHTML(v, "<body>x</body>", "about:blank");
    mbRunJS(v,
        "window.__bi='';"
        "var b64='iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAC0lEQVR42m"
        "Nk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==';"
        "var bin=atob(b64);var by=new Uint8Array(bin.length);"
        "for(var i=0;i<bin.length;i++)by[i]=bin.charCodeAt(i);"
        "var u=URL.createObjectURL(new Blob([by],{type:'image/png'}));"
        "var im=new Image();"
        "im.onload=function(){window.__bi='OK'+im.naturalWidth+'x'+"
        "im.naturalHeight;};im.onerror=function(){window.__bi='ERR';};"
        "im.src=u;");
    mbWait(v, 500);  // async image load + decode off the blob
    Expect(Eval(v, "String(window.__bi)") == "OK1x1",
           "<img src=blob:> decodes an image from a blob: URL",
           Eval(v, "String(window.__bi)"));
  }

  // 102. mbGetTextForSelector + mbGetAttribute: per-element scraping by selector.
  // Text of a specific element, an attribute's value, the no-match sentinel (-1),
  // and the empty-text vs no-match distinction.
  {
    mbLoadHTML(v,
        "<body><p id='t' data-k='v9'>Hello</p><span id='e'></span>"
        "<a id='lnk' href='https://x.test/p'>go</a></body>", "about:blank");
    char tb[256] = {0};
    int tlen = mbGetTextForSelector(v, "#t", tb, sizeof(tb));
    const bool text_ok = tlen == 5 && std::string(tb) == "Hello";

    char ab[256] = {0};
    int alen = mbGetAttribute(v, "#t", "data-k", ab, sizeof(ab));
    const bool attr_ok = alen == 2 && std::string(ab) == "v9";

    char hb[256] = {0};
    mbGetAttribute(v, "#lnk", "href", hb, sizeof(hb));
    const bool href_ok = std::string(hb) == "https://x.test/p";

    // No element matches -> -1 (distinct from a matched-but-empty element).
    const bool nomatch = mbGetTextForSelector(v, "#nope", tb, sizeof(tb)) == -1;
    char eb[8] = {0};
    const int empty_len = mbGetTextForSelector(v, "#e", eb, sizeof(eb));
    const bool empty_ok = empty_len == 0 && eb[0] == '\0';

    // Absent attribute -> -1.
    const bool absent = mbGetAttribute(v, "#t", "data-missing", ab, sizeof(ab)) == -1;

    Expect(text_ok && attr_ok && href_ok && nomatch && empty_ok && absent,
           "mbGetTextForSelector + mbGetAttribute scrape by selector",
           std::string("text=") + (text_ok ? "1" : "0") + " attr=" +
               (attr_ok ? "1" : "0") + " href=" + (href_ok ? "1" : "0") +
               " nomatch=" + (nomatch ? "1" : "0") + " empty=" +
               (empty_ok ? "1" : "0") + " absent=" + (absent ? "1" : "0"));
  }

  // 102b2. mbSetAttribute writes the static HTML attribute: a changed href reads
  // back via mbGetAttribute, a data-* round-trips, a bare boolean attr takes live
  // effect (button.disabled), and a no-match returns 0.
  {
    mbLoadHTML(v, "<body><a id='lnk' href='old'>L</a>"
                  "<button id='b'>B</button></body>", "about:blank");
    char ab2[256] = {0};
    const bool set_href = mbSetAttribute(v, "#lnk", "href", "https://new.test/") == 1;
    mbGetAttribute(v, "#lnk", "href", ab2, sizeof(ab2));
    const bool href_ok = std::string(ab2) == "https://new.test/";
    char db[64] = {0};
    const bool set_data = mbSetAttribute(v, "#lnk", "data-x", "hello") == 1;
    mbGetAttribute(v, "#lnk", "data-x", db, sizeof(db));
    const bool data_ok = std::string(db) == "hello";
    const bool bool_ok =
        mbSetAttribute(v, "#b", "disabled", "") == 1 &&
        Eval(v, "String(document.getElementById('b').disabled)") == "true";
    const bool nomatch_set = mbSetAttribute(v, "#none", "x", "y") == 0;
    Expect(set_href && href_ok && set_data && data_ok && bool_ok && nomatch_set,
           "mbSetAttribute writes the attribute (href/data-*/boolean), 0 on no match",
           std::string("href=") + (href_ok ? "1" : "0") + " data=" +
               (data_ok ? "1" : "0") + " bool=" + (bool_ok ? "1" : "0") +
               " nomatch=" + (nomatch_set ? "1" : "0"));
  }

  // 102b3. mbGetAllTextForSelector scrapes a whole list in one call: a JSON array
  // of each match's innerText; "[]" for no matches, -1 for an invalid selector.
  {
    mbLoadHTML(v, "<body><ul><li class='r'>a</li><li class='r'>b</li>"
                  "<li class='r'>c</li></ul></body>", "about:blank");
    char jb[256] = {0};
    int jlen = mbGetAllTextForSelector(v, ".r", jb, sizeof(jb));
    const bool all_ok = jlen > 0 && std::string(jb) == "[\"a\",\"b\",\"c\"]";
    char nb[16] = {0};
    mbGetAllTextForSelector(v, ".none", nb, sizeof(nb));
    const bool none_ok = std::string(nb) == "[]";
    const bool bad_ok = mbGetAllTextForSelector(v, "(((", jb, sizeof(jb)) == -1;
    Expect(all_ok && none_ok && bad_ok,
           "mbGetAllTextForSelector returns a JSON array of every match's text",
           std::string("all=") + (all_ok ? "1" : "0") + " none=" +
               (none_ok ? "1" : "0") + " bad=" + (bad_ok ? "1" : "0"));
  }

  // 102b3b. mbGetHtmlForSelector returns a fragment's outerHTML (element + markup),
  // distinct from innerText; -1 on no match.
  {
    mbLoadHTML(v, "<body><div id='card'><b>Hi</b> there</div></body>", "about:blank");
    char hb[256] = {0};
    int hlen = mbGetHtmlForSelector(v, "#card", hb, sizeof(hb));
    const std::string html(hb);
    const bool has_tag = hlen > 0 && html.find("id=\"card\"") != std::string::npos;
    const bool has_inner = html.find("<b>Hi</b> there") != std::string::npos;
    const bool none_ok = mbGetHtmlForSelector(v, "#none", hb, sizeof(hb)) == -1;
    Expect(has_tag && has_inner && none_ok,
           "mbGetHtmlForSelector returns the element's outerHTML",
           std::string("tag=") + (has_tag ? "1" : "0") + " inner=" +
               (has_inner ? "1" : "0") + " none=" + (none_ok ? "1" : "0"));
  }

  // 102b3c. mbSetHtmlForSelector replaces an element's innerHTML; the new markup
  // is then visible to the readers. No-match -> 0.
  {
    mbLoadHTML(v, "<body><div id='x'><b>old</b></div></body>", "about:blank");
    const bool set_ok = mbSetHtmlForSelector(v, "#x", "<i>new</i>") == 1;
    const bool reflected = Eval(v, "document.getElementById('x').textContent") == "new";
    char hb2[128] = {0};
    mbGetHtmlForSelector(v, "#x", hb2, sizeof(hb2));
    const bool html_ok = std::string(hb2).find("<i>new</i>") != std::string::npos;
    const bool none_ok = mbSetHtmlForSelector(v, "#none", "x") == 0;
    Expect(set_ok && reflected && html_ok && none_ok,
           "mbSetHtmlForSelector replaces innerHTML (readers see the new markup)",
           std::string("set=") + (set_ok ? "1" : "0") + " text=" +
               (reflected ? "1" : "0") + " html=" + (html_ok ? "1" : "0") +
               " none=" + (none_ok ? "1" : "0"));
  }

  // 102b4. mbGetAllAttributeForSelector scrapes one attribute across all matches
  // as a JSON array; a missing attribute -> null; raw value (href stays "/3").
  {
    mbLoadHTML(v, "<body><a class='l' href='/1'>1</a>"
                  "<a class='l' href='/2'>2</a>"
                  "<a class='l'>3</a></body>", "about:blank");  // 3rd lacks href
    char jb[256] = {0};
    int jlen = mbGetAllAttributeForSelector(v, ".l", "href", jb, sizeof(jb));
    const bool hrefs_ok = jlen > 0 && std::string(jb) == "[\"/1\",\"/2\",null]";
    char nb[16] = {0};
    mbGetAllAttributeForSelector(v, ".none", "href", nb, sizeof(nb));
    const bool none_ok = std::string(nb) == "[]";
    const bool bad_ok =
        mbGetAllAttributeForSelector(v, "(((", "href", jb, sizeof(jb)) == -1;
    Expect(hrefs_ok && none_ok && bad_ok,
           "mbGetAllAttributeForSelector returns a JSON array of an attr (null if absent)",
           std::string("hrefs=") + (hrefs_ok ? "1" : "0") + " none=" +
               (none_ok ? "1" : "0") + " bad=" + (bad_ok ? "1" : "0"));
  }

  // 102b4b. mbGetAllValueForSelector serializes a whole form's LIVE values in one
  // call; a typed-over field is reflected (unlike the static value attribute).
  {
    mbLoadHTML(v, "<body><input class='f' value='a'><input class='f' value='b'>"
                  "<input class='f' value='c'></body>", "about:blank");
    mbFillSelector(v, ".f:nth-of-type(2)", "B2");  // change the 2nd live value
    char jb[256] = {0};
    int jlen = mbGetAllValueForSelector(v, ".f", jb, sizeof(jb));
    const bool ok = jlen > 0 && std::string(jb) == "[\"a\",\"B2\",\"c\"]";
    char nb[16] = {0};
    mbGetAllValueForSelector(v, ".none", nb, sizeof(nb));
    const bool none_ok = std::string(nb) == "[]";
    Expect(ok && none_ok,
           "mbGetAllValueForSelector serializes all live form values as JSON",
           std::string("vals=") + (ok ? "1" : "0") + " none=" + (none_ok ? "1" : "0") +
               " got=" + jb);
  }

  // 102b6. mbGetLocalStorage/mbSetLocalStorage share the page's localStorage for
  // the document's origin (needs a real http(s) base, not about:blank): set via
  // C and observe in JS, set in JS and read via C, an absent key -> -1.
  {
    mbLoadHTML(v, "<body>ls</body>", "https://lsapi.test/");
    const bool set_seen_by_js =
        mbSetLocalStorage(v, "auth", "tok-42") == 1 &&
        Eval(v, "localStorage.getItem('auth')") == "tok-42";
    mbRunJS(v, "localStorage.setItem('pref','dark')");
    char sb[64] = {0};
    int slen = mbGetLocalStorage(v, "pref", sb, sizeof(sb));
    const bool c_read = slen == 4 && std::string(sb) == "dark";
    const bool absent = mbGetLocalStorage(v, "missing", sb, sizeof(sb)) == -1;
    Expect(set_seen_by_js && c_read && absent,
           "mbGetLocalStorage/mbSetLocalStorage share the page's localStorage",
           std::string("set=") + (set_seen_by_js ? "1" : "0") + " read=" +
               (c_read ? "1" : "0") + " absent=" + (absent ? "1" : "0"));

    // sessionStorage: same C<->JS sharing, and a SEPARATE store from localStorage.
    const bool ss_set = mbSetSessionStorage(v, "sk", "sv") == 1 &&
                        Eval(v, "sessionStorage.getItem('sk')") == "sv";
    char ssb[32] = {0};
    mbGetSessionStorage(v, "sk", ssb, sizeof(ssb));
    const bool ss_read = std::string(ssb) == "sv";
    // 'sk' lives in sessionStorage only -> absent from localStorage.
    const bool distinct = mbGetLocalStorage(v, "sk", ssb, sizeof(ssb)) == -1;
    Expect(ss_set && ss_read && distinct,
           "mbGet/SetSessionStorage share sessionStorage, separate from localStorage",
           std::string("set=") + (ss_set ? "1" : "0") + " read=" +
               (ss_read ? "1" : "0") + " distinct=" + (distinct ? "1" : "0"));

    // mbClearStorage empties BOTH stores (local has 'auth'/'pref', session 'sk').
    mbClearStorage(v);
    const bool cleared = Eval(v, "String(localStorage.length)") == "0" &&
                         Eval(v, "String(sessionStorage.length)") == "0";
    Expect(cleared, "mbClearStorage empties localStorage + sessionStorage",
           std::string("local=") + Eval(v, "String(localStorage.length)") +
               " session=" + Eval(v, "String(sessionStorage.length)"));
  }

  // 102b6b. Browsing profiles partition every page-visible storage surface at
  // the same origin. localStorage and both cookie APIs must not cross sessions;
  // cookieStore writes must land in the owning session's HTTP jar; listeners in
  // session A must not receive session B's changes. Clearing A leaves B intact.
  {
    mbSession* session_a = mbCreateSession("storage-isolation-a", nullptr);
    mbSession* session_b = mbCreateSession("storage-isolation-b", nullptr);
    mbView* a = session_a ? mbCreateViewInSession(240, 160, session_a) : nullptr;
    mbView* b = session_b ? mbCreateViewInSession(240, 160, session_b) : nullptr;
    bool isolated = false;
    bool clear_scoped = false;
    std::string detail = "setup failed";
    if (a && b) {
      const char* origin = "https://profile-storage.test/";
      mbLoadHTML(a, "<body>a</body>", origin);
      mbLoadHTML(b, "<body>b</body>", origin);
      mbRunJS(a,
              "localStorage.setItem('ls','A');"
              "sessionStorage.setItem('ss','A');"
              "document.cookie='doc=A';"
              "window.__isoEvents=0;window.__isoReady='';"
              "if(!window.cookieStore){window.__isoReady='noapi';}else{"
              "cookieStore.set('cs','A').then(function(){"
              "cookieStore.addEventListener('change',function(){window.__isoEvents++;});"
              "window.__isoReady='ok';"
              "}).catch(function(e){window.__isoReady='err:'+e.name;});}");
      mbWaitForFunction(a, "window.__isoReady!==''", 2000);
      mbWait(a, 30);  // let AddChangeListener bind before B writes

      const std::string b_before = Eval(
          b,
          "(localStorage.getItem('ls')===null?'null':localStorage.getItem('ls'))+','+"
          "(sessionStorage.getItem('ss')===null?'null':sessionStorage.getItem('ss'))+','+"
          "(document.cookie.indexOf('doc=A')<0?'clean':'leaked')");
      mbRunJS(b,
              "localStorage.setItem('ls','B');"
              "sessionStorage.setItem('ss','B');"
              "document.cookie='doc=B';"
              "window.__isoReady='';"
              "cookieStore.set('cs','B').then(function(){window.__isoReady='ok';})"
              ".catch(function(e){window.__isoReady='err:'+e.name;});");
      mbWaitForFunction(b, "window.__isoReady!==''", 2000);
      // Positive control instead of a fixed silence window: A's own probe write
      // must reach A's listener, and change events are delivered in dispatch
      // order — so once the probe event has arrived, a leaked event from B's
      // earlier write would already have arrived too. Exactly ONE event
      // therefore means "listener live, no cross-profile leak", with no
      // wall-clock sensitivity.
      mbRunJS(a, "cookieStore.set('probe','P').catch(function(){});");
      mbWaitForFunction(a, "window.__isoEvents>=1", 2000);

      char jar_a[16] = {0};
      char jar_b[16] = {0};
      const bool jar_a_ok =
          mbGetCookie(a, origin, "cs", jar_a, sizeof(jar_a)) == 1 &&
          std::string(jar_a) == "A";
      const bool jar_b_ok =
          mbGetCookie(b, origin, "cs", jar_b, sizeof(jar_b)) == 1 &&
          std::string(jar_b) == "B";
      const std::string a_after = Eval(
          a,
          "localStorage.getItem('ls')+','+sessionStorage.getItem('ss')+','+"
          "(document.cookie.indexOf('doc=A')>=0)+','+String(window.__isoEvents)");
      const std::string b_after = Eval(
          b,
          "localStorage.getItem('ls')+','+sessionStorage.getItem('ss')+','+"
          "(document.cookie.indexOf('doc=B')>=0)");
      // Exactly 1 event: A's own probe (positive control). A leaked event from
      // B's earlier writes would have been dispatched first, making it >= 2.
      isolated = Eval(a, "window.__isoReady") == "ok" &&
                 Eval(b, "window.__isoReady") == "ok" &&
                 b_before == "null,null,clean" &&
                 a_after == "A,A,true,1" && b_after == "B,B,true" &&
                 jar_a_ok && jar_b_ok;

      mbSessionClearStorage(session_a);
      mbWait(a, 80);
      mbWait(b, 20);
      char cleared_a[16] = {0};
      char kept_b[16] = {0};
      const bool jar_a_cleared =
          mbGetCookie(a, origin, "cs", cleared_a, sizeof(cleared_a)) == -1;
      const bool jar_b_kept =
          mbGetCookie(b, origin, "cs", kept_b, sizeof(kept_b)) == 1 &&
          std::string(kept_b) == "B";
      const std::string a_cleared = Eval(
          a,
          "(localStorage.getItem('ls')===null)+','+"
          "(document.cookie.indexOf('doc=A')<0&&document.cookie.indexOf('cs=A')<0)");
      const std::string b_kept = Eval(
          b,
          "localStorage.getItem('ls')+','+"
          "(document.cookie.indexOf('doc=B')>=0&&document.cookie.indexOf('cs=B')>=0)");
      clear_scoped = a_cleared == "true,true" && b_kept == "B,true" &&
                     jar_a_cleared && jar_b_kept;
      detail = "before=[" + b_before + "] a=[" + a_after + "] b=[" +
               b_after + "] jars=[" + jar_a + "/" + jar_b +
               "] clear=[" + a_cleared + "/" + b_kept + "]";
    }
    Expect(isolated,
           "two sessions isolate local/sessionStorage, cookies, Cookie Store + listeners",
           detail);
    Expect(clear_scoped,
           "mbSessionClearStorage clears only that session's DOM/cookie state",
           detail);
    if (a)
      mbDestroyView(a);
    if (b)
      mbDestroyView(b);
    if (session_a)
      mbDestroySession(session_a);
    if (session_b)
      mbDestroySession(session_b);
  }

  // 102b6b2. An ephemeral profile name is descriptive, not an identity key:
  // two legacy mbCreateSession calls with the same name must still create two
  // independent in-memory profiles. Exercise both Blink-visible localStorage /
  // document.cookie and the host-facing HTTP cookie jar at one origin.
  {
    static constexpr char kSessionName[] = "same-name-ephemeral";
    mbSession* session_a = mbCreateSession(kSessionName, nullptr);
    mbSession* session_b = mbCreateSession(kSessionName, nullptr);
    mbView* a = session_a ? mbCreateViewInSession(240, 160, session_a) : nullptr;
    mbView* b = session_b ? mbCreateViewInSession(240, 160, session_b) : nullptr;
    bool metadata_ok = false;
    bool isolated = false;
    std::string detail = "setup failed";
    if (a && b) {
      char name_a[64] = {0};
      char name_b[64] = {0};
      char path_a[8] = {0};
      char path_b[8] = {0};
      metadata_ok = session_a != session_b &&
                    mbViewGetSession(a) == session_a &&
                    mbViewGetSession(b) == session_b &&
                    mbSessionIsPersistent(session_a) == 0 &&
                    mbSessionIsPersistent(session_b) == 0 &&
                    mbSessionGetName(session_a, name_a, sizeof(name_a)) ==
                        static_cast<int>(sizeof(kSessionName) - 1) &&
                    mbSessionGetName(session_b, name_b, sizeof(name_b)) ==
                        static_cast<int>(sizeof(kSessionName) - 1) &&
                    std::string(name_a) == kSessionName &&
                    std::string(name_b) == kSessionName &&
                    mbSessionGetPersistPath(session_a, path_a,
                                            sizeof(path_a)) == 0 &&
                    mbSessionGetPersistPath(session_b, path_b,
                                            sizeof(path_b)) == 0;

      const char* origin = "https://same-name-ephemeral.test/";
      mbLoadHTML(a, "<body>a</body>", origin);
      mbLoadHTML(b, "<body>b</body>", origin);
      mbRunJS(a,
              "localStorage.setItem('same-name-key','A');"
              "document.cookie='same_name_doc=A; Path=/';");
      mbSetCookie(a, origin, "same_name_jar=A; Path=/");

      char b_jar_before[16] = {0};
      const bool b_jar_clean =
          mbGetCookie(b, origin, "same_name_jar", b_jar_before,
                      sizeof(b_jar_before)) == -1;
      const std::string b_before = Eval(
          b,
          "(localStorage.getItem('same-name-key')===null?'null':'leaked')+','+"
          "(document.cookie.indexOf('same_name_doc=A')<0&&"
          "document.cookie.indexOf('same_name_jar=A')<0?'clean':'leaked')");

      mbRunJS(b,
              "localStorage.setItem('same-name-key','B');"
              "document.cookie='same_name_doc=B; Path=/';");
      mbSetCookie(b, origin, "same_name_jar=B; Path=/");

      char jar_a[16] = {0};
      char jar_b[16] = {0};
      const bool jars_scoped =
          mbGetCookie(a, origin, "same_name_jar", jar_a, sizeof(jar_a)) == 1 &&
          std::string(jar_a) == "A" &&
          mbGetCookie(b, origin, "same_name_jar", jar_b, sizeof(jar_b)) == 1 &&
          std::string(jar_b) == "B";
      const std::string a_after = Eval(
          a,
          "localStorage.getItem('same-name-key')+','+"
          "(document.cookie.indexOf('same_name_doc=A')>=0)+','+"
          "(document.cookie.indexOf('same_name_doc=B')<0)");
      const std::string b_after = Eval(
          b,
          "localStorage.getItem('same-name-key')+','+"
          "(document.cookie.indexOf('same_name_doc=B')>=0)+','+"
          "(document.cookie.indexOf('same_name_doc=A')<0)");
      isolated = b_jar_clean && b_before == "null,clean" && jars_scoped &&
                 a_after == "A,true,true" && b_after == "B,true,true";
      detail = "metadata=" + std::to_string(metadata_ok) + " before=[" +
               b_before + "] after=[" + a_after + "/" + b_after +
               "] jars=[" + jar_a + "/" + jar_b + "]";
    }
    Expect(metadata_ok && isolated,
           "same-name mbCreateSession ephemeral profiles remain independent",
           detail);
    if (a)
      mbDestroyView(a);
    if (b)
      mbDestroyView(b);
    if (session_a)
      mbDestroySession(session_a);
    if (session_b)
      mbDestroySession(session_b);
  }

  // 102b6c. Src-less iframes publish their inherited about:blank document via
  // DidCommitNavigation, which is what gives their frame token a session scope.
  // Verify that invariant for both parser-created and dynamically appended
  // frames, then exercise localStorage/document.cookie in two profiles. Positive
  // same-profile controls prove both storage and Cookie Store event listeners
  // work before we assert that the other profile cannot trigger them.
  {
    mbSession* session_a = mbCreateSession("src-less-isolation-a", nullptr);
    mbSession* session_b = mbCreateSession("src-less-isolation-b", nullptr);
    mbView* a = session_a ? mbCreateViewInSession(240, 160, session_a) : nullptr;
    mbView* b = session_b ? mbCreateViewInSession(240, 160, session_b) : nullptr;
    bool controls_work = false;
    bool isolated = false;
    int empty_commits_a = 0;
    int empty_commits_b = 0;
    std::string detail = "setup failed";
    if (a && b) {
      const char* origin = "https://src-less-storage.test/";
      const char* frames = "<body><iframe id='primary'></iframe></body>";
      mbFrameLoadCallback count_empty_commit =
          [](mbView*, void* userdata, uint64_t, int is_main, int phase,
             const char* url) {
            if (!is_main && phase == MB_FRAME_LOAD_BEGIN && url &&
                std::strcmp(url, "about:blank") == 0) {
              ++*static_cast<int*>(userdata);
            }
          };
      mbOnFrameLoadEvent(a, count_empty_commit, &empty_commits_a);
      mbOnFrameLoadEvent(b, count_empty_commit, &empty_commits_b);
      mbLoadHTML(a, frames, origin);
      mbLoadHTML(b, frames, origin);
      mbRunJS(a,
              "(function(){var f=document.createElement('iframe');f.id='peer';"
              "document.body.appendChild(f);})()");
      mbRunJS(b,
              "(function(){var f=document.createElement('iframe');f.id='peer';"
              "document.body.appendChild(f);})()");

      const std::string shape = Eval(
          a,
          "(function(){var f=document.getElementById('primary'),"
          "p=document.getElementById('peer');return String(f.getAttribute('src'))+','+"
          "f.contentWindow.location.href+','+String(p.getAttribute('src'))+','+"
          "p.contentWindow.location.href;})()");
      mbRunJS(
          a,
          "(function(){var w=document.getElementById('primary').contentWindow;"
          "w.localStorage.setItem('mb-src-less-data','A');"
          "w.document.cookie='mb_src_less_doc=A';"
          "w.__slStorageEvents=0;w.__slCookieEvents=0;"
          "w.addEventListener('storage',function(e){"
          "if(e.key==='mb-src-less-event')w.__slStorageEvents++;});"
          "w.__slCookieApi=!!w.cookieStore;"
          "if(w.cookieStore)w.cookieStore.addEventListener('change',function(e){"
          "var all=Array.prototype.slice.call(e.changed).concat("
          "Array.prototype.slice.call(e.deleted));"
          "for(var i=0;i<all.length;i++){if(all[i].name==='mb_src_less_event'){"
          "w.__slCookieEvents++;break;}}});})()");
      // Let CookieStore's AddChangeListener reach the service sequence. The
      // following positive control still makes a missed registration fail
      // deterministically rather than weakening the cross-session assertion.
      mbWait(a, 40);
      mbRunJS(
          a,
          "(function(){var w=document.getElementById('peer').contentWindow;"
          "w.localStorage.setItem('mb-src-less-event','same-session');"
          "w.document.cookie='mb_src_less_event=same-session';})()");
      const bool events_delivered =
          mbWaitForFunction(
              a,
              "document.getElementById('primary').contentWindow.__slStorageEvents>0&&"
              "document.getElementById('primary').contentWindow.__slCookieEvents>0",
              2000) == 1;
      const std::string control_counts = Eval(
          a,
          "(function(){var w=document.getElementById('primary').contentWindow;"
          "return w.__slStorageEvents+','+w.__slCookieEvents+','+w.__slCookieApi;})()");
      mbRunJS(
          a,
          "(function(){var w=document.getElementById('primary').contentWindow;"
          "w.__slStorageEvents=0;w.__slCookieEvents=0;})()");

      const std::string b_before = Eval(
          b,
          "(function(){var w=document.getElementById('primary').contentWindow;"
          "return (w.localStorage.getItem('mb-src-less-data')===null?'null':'leaked')+','+"
          "(w.document.cookie.indexOf('mb_src_less_doc=A')<0?'clean':'leaked');})()");
      mbRunJS(
          b,
          "(function(){var w=document.getElementById('primary').contentWindow;"
          "w.localStorage.setItem('mb-src-less-data','B');"
          "w.document.cookie='mb_src_less_doc=B';"
          "w.localStorage.setItem('mb-src-less-event','other-session');"
          "w.document.cookie='mb_src_less_event=other-session';})()");
      const bool b_write_visible =
          mbWaitForFunction(
              b,
              "(function(){var w=document.getElementById('primary').contentWindow;"
              "return w.localStorage.getItem('mb-src-less-data')==='B'&&"
              "w.document.cookie.indexOf('mb_src_less_doc=B')>=0;})()",
              2000) == 1;
      // The positive control above establishes that both listener paths are
      // live. Pump well past their observed delivery before asserting silence.
      mbWait(a, 250);
      const std::string a_after = Eval(
          a,
          "(function(){var w=document.getElementById('primary').contentWindow;"
          "return w.localStorage.getItem('mb-src-less-data')+','+"
          "(w.document.cookie.indexOf('mb_src_less_doc=A')>=0)+','+"
          "(w.document.cookie.indexOf('mb_src_less_doc=B')<0)+','+"
          "w.__slStorageEvents+','+w.__slCookieEvents;})()");
      const std::string b_after = Eval(
          b,
          "(function(){var w=document.getElementById('primary').contentWindow;"
          "return w.localStorage.getItem('mb-src-less-data')+','+"
          "(w.document.cookie.indexOf('mb_src_less_doc=B')>=0)+','+"
          "(w.document.cookie.indexOf('mb_src_less_doc=A')<0);})()");

      controls_work = shape == "null,about:blank,null,about:blank" &&
                      empty_commits_a >= 2 && empty_commits_b >= 2 &&
                      events_delivered && control_counts == "1,1,true";
      isolated = b_before == "null,clean" && b_write_visible &&
                 a_after == "A,true,true,0,0" && b_after == "B,true,true";
      detail = "shape=[" + shape + "] commits=[" +
               std::to_string(empty_commits_a) + "/" +
               std::to_string(empty_commits_b) + "] control=[" +
               control_counts + "] before=[" + b_before + "] after=[" +
               a_after + "/" + b_after + "]";
    }
    Expect(controls_work,
           "src-less parser/dynamic iframes commit and same-session events fire",
           detail);
    Expect(isolated,
           "src-less iframes isolate localStorage, cookies and change events by session",
           detail);
    if (a)
      mbOnFrameLoadEvent(a, nullptr, nullptr);
    if (b)
      mbOnFrameLoadEvent(b, nullptr, nullptr);
    if (a)
      mbDestroyView(a);
    if (b)
      mbDestroyView(b);
    if (session_a)
      mbDestroySession(session_a);
    if (session_b)
      mbDestroySession(session_b);
  }

  // 102b5. mbInsertCSS appends a <style> that actually applies: a rule hiding #x
  // flips it from visible to hidden (verified via mbIsVisibleForSelector).
  {
    mbLoadHTML(v, "<body><div id='x'>v</div></body>", "about:blank");
    const bool before = mbIsVisibleForSelector(v, "#x") == 1;
    const bool inserted = mbInsertCSS(v, "#x{display:none}") == 1;
    const bool after = mbIsVisibleForSelector(v, "#x") == 0;
    Expect(before && inserted && after,
           "mbInsertCSS injects a stylesheet that takes effect (#x hidden)",
           std::string("before=") + (before ? "1" : "0") + " inserted=" +
               (inserted ? "1" : "0") + " after=" + (after ? "1" : "0"));
  }

  // 102c. mbGetValueForSelector reads the LIVE .value (post-typing/selection),
  // distinct from mbGetAttribute's static "value" attribute, with the same
  // -1 no-match / no-value-property sentinel.
  {
    mbLoadHTML(v,
        "<body><input id='n' value='start'>"
        "<select id='s'><option value='x'>X</option>"
        "<option value='y' selected>Y</option></select>"
        "<div id='d'>plain</div></body>", "about:blank");
    mbFillSelector(v, "#n", "typed-over");
    char vb[256] = {0};
    int vlen = mbGetValueForSelector(v, "#n", vb, sizeof(vb));
    const bool live_ok = vlen == 10 && std::string(vb) == "typed-over";
    // The static attribute still reads the ORIGINAL value — the whole point.
    char ab2[256] = {0};
    mbGetAttribute(v, "#n", "value", ab2, sizeof(ab2));
    const bool attr_unchanged = std::string(ab2) == "start";
    mbGetValueForSelector(v, "#s", vb, sizeof(vb));
    const bool select_ok = std::string(vb) == "y";
    // No value property (<div>) and no match both -> -1.
    const bool noval = mbGetValueForSelector(v, "#d", vb, sizeof(vb)) == -1;
    const bool nomatch2 = mbGetValueForSelector(v, "#none", vb, sizeof(vb)) == -1;
    Expect(live_ok && attr_unchanged && select_ok && noval && nomatch2,
           "mbGetValueForSelector reads live .value (distinct from attribute)",
           std::string("live=") + (live_ok ? "1" : "0") + " attr=" +
               (attr_unchanged ? "1" : "0") + " sel=" + (select_ok ? "1" : "0") +
               " noval=" + (noval ? "1" : "0") + " nomatch=" +
               (nomatch2 ? "1" : "0"));
  }

  // 102d. mbGetCheckedForSelector: a checkbox/radio's .checked state (1/0), -1 for
  // a non-checkable element or no match, and it tracks a click that toggles it.
  {
    mbLoadHTML(v,
        "<body><input type='checkbox' id='c1' checked>"
        "<input type='checkbox' id='c2'><div id='d'>x</div></body>",
        "about:blank");
    const bool init_ok = mbGetCheckedForSelector(v, "#c1") == 1 &&
                         mbGetCheckedForSelector(v, "#c2") == 0;
    mbClickSelector(v, "#c2");  // toggles it on
    const bool toggled = mbGetCheckedForSelector(v, "#c2") == 1;
    const bool noncheck = mbGetCheckedForSelector(v, "#d") == -1 &&
                          mbGetCheckedForSelector(v, "#none") == -1;
    Expect(init_ok && toggled && noncheck,
           "mbGetCheckedForSelector reads .checked and tracks a toggling click",
           std::string("init=") + (init_ok ? "1" : "0") + " toggled=" +
               (toggled ? "1" : "0") + " noncheck=" + (noncheck ? "1" : "0"));
  }

  // 102e. mbIsVisibleForSelector: existence (a selector matches) is NOT visibility.
  // display:none / visibility:hidden / opacity:0 all match yet report 0 (hidden);
  // a shown element is 1; no match is -1.
  {
    mbLoadHTML(v,
        "<body><div id='vis'>shown</div>"
        "<div id='dn' style='display:none'>x</div>"
        "<div id='vh' style='visibility:hidden'>x</div>"
        "<div id='op' style='opacity:0'>x</div></body>", "about:blank");
    const bool shown = mbIsVisibleForSelector(v, "#vis") == 1;
    const bool hidden_ok = mbIsVisibleForSelector(v, "#dn") == 0 &&
                           mbIsVisibleForSelector(v, "#vh") == 0 &&
                           mbIsVisibleForSelector(v, "#op") == 0;
    // The hidden element still EXISTS (count==1) — proving the distinction.
    const bool exists_distinct =
        mbCountSelector(v, "#dn") == 1 && mbIsVisibleForSelector(v, "#dn") == 0;
    const bool nomatch = mbIsVisibleForSelector(v, "#none") == -1;
    Expect(shown && hidden_ok && exists_distinct && nomatch,
           "mbIsVisibleForSelector separates visibility from existence",
           std::string("shown=") + (shown ? "1" : "0") + " hidden=" +
               (hidden_ok ? "1" : "0") + " distinct=" +
               (exists_distinct ? "1" : "0") + " nomatch=" + (nomatch ? "1" : "0"));
  }

  // 102f. mbWaitForVisibleSelector waits past existence: an element present from
  // the start but display:none until a timer reveals it. mbWaitForSelector returns
  // at once (it exists) yet it's hidden; the visible-wait blocks until the reveal;
  // an element that stays hidden times out.
  {
    mbLoadHTML(v,
        "<body><div id='m' style='display:none'>modal</div>"
        "<div id='ghost' style='visibility:hidden'>x</div>"
        "<script>setTimeout(function(){"
        "document.getElementById('m').style.display='block';},300);</script></body>",
        "about:blank");
    // #ghost exists but is permanently hidden — deterministic (no timer race).
    const bool exists_but_hidden = mbWaitForSelector(v, "#ghost", 1000) == 1 &&
                                   mbIsVisibleForSelector(v, "#ghost") == 0;
    const bool became_visible = mbWaitForVisibleSelector(v, "#m", 4000) == 1 &&
                                mbIsVisibleForSelector(v, "#m") == 1;
    const bool stays_hidden = mbWaitForVisibleSelector(v, "#ghost", 120) == 0;
    Expect(exists_but_hidden && became_visible && stays_hidden,
           "mbWaitForVisibleSelector waits for visibility, not just existence",
           std::string("exists_hidden=") + (exists_but_hidden ? "1" : "0") +
               " became_visible=" + (became_visible ? "1" : "0") +
               " stays_hidden=" + (stays_hidden ? "1" : "0"));
  }

  // 102g. mbWaitForSelectorHidden is the inverse: it resolves when an element
  // goes away/hidden (the "spinner disappeared" signal). A timer removes #spin;
  // an absent selector is instantly hidden; a shown element times out.
  {
    mbLoadHTML(v,
        "<body><div id='spin'>loading</div><div id='stay'>x</div>"
        "<script>setTimeout(function(){"
        "document.getElementById('spin').remove();},300);</script></body>",
        "about:blank");
    const bool gone_now = mbWaitForSelectorHidden(v, "#never", 500) == 1;
    const bool became_hidden = mbWaitForSelectorHidden(v, "#spin", 4000) == 1 &&
                               mbIsVisibleForSelector(v, "#spin") == -1;
    const bool stays_shown = mbWaitForSelectorHidden(v, "#stay", 120) == 0;
    Expect(gone_now && became_hidden && stays_shown,
           "mbWaitForSelectorHidden resolves when an element goes away/hidden",
           std::string("gone_now=") + (gone_now ? "1" : "0") + " became_hidden=" +
               (became_hidden ? "1" : "0") + " stays_shown=" +
               (stays_shown ? "1" : "0"));
  }

  // 102h. mbWaitForNetworkIdleEx: a page fires a deferred fetch (150ms) that routes
  // through the loader; the wait must return idle (not timeout) only after that
  // request lands — so the log holds it afterward. A second quiet page confirms
  // it doesn't false-timeout. (file:// origin so the file:// fetch is same-scheme.)
  {
    mbClearRequestLog();
    mbLoadHTML(v,
        "<body><script>setTimeout(function(){var i=document.createElement('img');"
        "i.src='file:///tmp/mb_ni_probe.png';document.body.appendChild(i);},150);"
        "</script></body>", "file:///tmp/mb_ni_page.html");
    const bool idle = mbWaitForNetworkIdleEx(v, 300, 5000) == 1;
    char rb[4096] = {0};
    mbGetRequestLog(rb, sizeof(rb));
    const bool fetched = std::string(rb).find("mb_ni_probe.png") != std::string::npos;
    mbClearRequestLog();
    mbLoadHTML(v, "<body>quiet</body>", "about:blank");
    const bool quiet_ok = mbWaitForNetworkIdleEx(v, 150, 3000) == 1;  // no false timeout
    mbClearRequestLog();
    const bool legacy_quiet = mbWaitForNetworkIdle(v, 20, 500) == 1;
    mbView* noise = mbCreateView(120, 80);
    mbMockResponse("legacy-idle-noise.test/ping", "ok", "text/plain", 200);
    mbLoadHTML(
        noise,
        "<script>setTimeout(function(){fetch('/ping')},80)</script>",
        "https://legacy-idle-noise.test/");
    mbClearRequestLog();
    const auto legacy_start = std::chrono::steady_clock::now();
    const bool legacy_process_wide = mbWaitForNetworkIdle(v, 180, 1500) == 1;
    const auto legacy_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - legacy_start)
                               .count();
    char legacy_log[1024] = {0};
    mbGetRequestLog(legacy_log, sizeof(legacy_log));
    const bool legacy_saw_other_view =
        std::string(legacy_log).find("legacy-idle-noise.test/ping") !=
        std::string::npos;
    mbDestroyView(noise);
    mbClearMocks();
    Expect(idle && fetched && quiet_ok && legacy_quiet &&
               legacy_process_wide && legacy_saw_other_view && legacy_ms >= 220,
           "network-idle Ex tracks the view; legacy wait retains process-wide semantics",
           std::string("idle=") + (idle ? "1" : "0") + " fetched=" +
               (fetched ? "1" : "0") + " quiet=" + (quiet_ok ? "1" : "0") +
               " legacy=" + (legacy_quiet ? "1" : "0") +
               " process=" + (legacy_process_wide ? "1" : "0") +
               " other=" + (legacy_saw_other_view ? "1" : "0") +
               " elapsed_ms=" + std::to_string(legacy_ms));
  }

  // 102b. mbCountSelector + indexed list scraping. Count the matches, then read
  // each one via :nth-of-type(n) selectors on mbGetTextForSelector — the standard
  // "scrape a list" pattern. Also: 0 for no matches, -1 for an invalid selector.
  {
    mbLoadHTML(v,
        "<body><ul><li class='row'>Alpha</li><li class='row'>Beta</li>"
        "<li class='row'>Gamma</li></ul></body>", "about:blank");
    const int n = mbCountSelector(v, ".row");
    const bool count_ok = n == 3;
    const bool none_ok = mbCountSelector(v, ".nope") == 0;
    const bool bad_ok = mbCountSelector(v, "li::::") == -1;  // invalid syntax
    // Walk the list by index and collect the text.
    std::string joined;
    for (int i = 1; i <= n; ++i) {
      char tb[64] = {0};
      std::string sel = "li.row:nth-of-type(" + std::to_string(i) + ")";
      if (mbGetTextForSelector(v, sel.c_str(), tb, sizeof(tb)) >= 0)
        joined += std::string(tb) + (i < n ? "," : "");
    }
    const bool walk_ok = joined == "Alpha,Beta,Gamma";
    Expect(count_ok && none_ok && bad_ok && walk_ok,
           "mbCountSelector + :nth-of-type index walk scrapes a list",
           std::string("count=") + std::to_string(n) + " none=" +
               (none_ok ? "1" : "0") + " bad=" + (bad_ok ? "1" : "0") +
               " walk=[" + joined + "]");
  }

  // 102c. mbGetComputedStyle: resolved CSS property values by selector. Covers
  // a visibility check (display:none), color/weight normalization, and the
  // no-match sentinel (-1). Distinct from mbGetAttribute (computed, not source).
  {
    mbLoadHTML(v,
        "<body><div id='h' style='display:none'>x</div>"
        "<div id='s' style='color:red;font-weight:bold'>y</div></body>",
        "about:blank");
    char cb[128] = {0};
    mbGetComputedStyle(v, "#h", "display", cb, sizeof(cb));
    const bool display_ok = std::string(cb) == "none";
    mbGetComputedStyle(v, "#s", "color", cb, sizeof(cb));
    const bool color_ok = std::string(cb) == "rgb(255, 0, 0)";
    mbGetComputedStyle(v, "#s", "font-weight", cb, sizeof(cb));
    const bool weight_ok = std::string(cb) == "700";
    const bool nomatch_ok =
        mbGetComputedStyle(v, "#none", "display", cb, sizeof(cb)) == -1;
    Expect(display_ok && color_ok && weight_ok && nomatch_ok,
           "mbGetComputedStyle returns resolved CSS values by selector",
           std::string("display=") + (display_ok ? "1" : "0") + " color=" +
               (color_ok ? "1" : "0") + " weight=" + (weight_ok ? "1" : "0") +
               " nomatch=" + (nomatch_ok ? "1" : "0"));
  }

  // 103. Below-the-fold interaction auto-scrolls. A button ~2000px down is
  // outside a 400px viewport, so a coordinate-based click would miss; the click
  // path now calls scrollIntoView first. Also exercise mbScrollIntoView directly.
  {
    mbResize(v, 400, 400);
    mbLoadHTML(v,
        "<body style='margin:0'><div style='height:2000px'></div>"
        "<button id='b' onclick='window.__c=1'>go</button>"
        "<div style='height:600px'></div></body>", "about:blank");
    mbWait(v, 30);
    // Before any scroll, the button is far below the fold.
    const bool below = std::atoi(Eval(v,
        "String(Math.round(document.getElementById('b')"
        ".getBoundingClientRect().top))").c_str()) > 400;
    // mbScrollIntoView brings it into the viewport [0,400).
    const bool scrolled = mbScrollIntoView(v, "#b") == 1;
    const int top_after = std::atoi(Eval(v,
        "String(Math.round(document.getElementById('b')"
        ".getBoundingClientRect().top))").c_str());
    const bool in_view = top_after >= 0 && top_after < 400;
    // The click path auto-scrolls + lands even starting scrolled elsewhere.
    mbRunJS(v, "window.scrollTo(0,0);window.__c=0;");
    mbWait(v, 20);
    mbClickSelector(v, "#b");
    mbWait(v, 30);
    const bool clicked = Eval(v, "String(window.__c||0)") == "1";
    Expect(below && scrolled && in_view && clicked,
           "below-fold element auto-scrolls into view for click",
           std::string("below=") + (below ? "1" : "0") + " scroll=" +
               (scrolled ? "1" : "0") + " inview=" + (in_view ? "1" : "0") +
               " click=" + (clicked ? "1" : "0"));
    mbResize(v, W, H);  // restore the shared viewport for later cases
  }

  // 104. Form submission NAVIGATES (not just the onsubmit handler). A GET form,
  // filled then submitted by clicking its submit button, must navigate to the
  // action URL with the query string and commit the new document — exercising
  // BeginNavigation + the loader + script re-run on the committed page. A file://
  // doc makes the target fetchable without network (GET appends ?q=hello; the
  // file read ignores the query). Distinct from case 88 (onsubmit handler) and
  // case 36 (POST over network): this is local GET navigation.
  {
    if (FILE* f = std::fopen("/tmp/mb_formnav.html", "wb")) {
      static const char kHtml[] =
          "<body><form method='GET'><input name='q'>"
          "<button type='submit' id='go'>go</button></form></body>";
      std::fwrite(kHtml, 1, sizeof(kHtml) - 1, f);
      std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_formnav.html");
    mbWaitForSelector(v, "#go", 2000);
    mbFillSelector(v, "input[name=q]", "hello");
    mbClickSelector(v, "#go");  // submit -> navigate to ...?q=hello
    const bool navigated =
        mbWaitForFunction(v, "location.search==='?q=hello'", 2000) == 1;
    Expect(navigated,
           "form GET submission navigates to the action URL with the query",
           Eval(v, "String(location.search)"));
  }

  // 104a. data: decoding and file: reads for mbNavigate execute on the blocking
  // pool, not on the engine sequence. The exact-URL barrier is entered by the
  // pool task itself; while it is held, no reply/commit can occur. This proves
  // the scheduling property directly instead of inferring it from wall time.
  {
    static const char kFileBody[] =
        "<body id='mb-local-pool-file'>file-pool</body>";
    base::ScopedTempDir local_dir;
    const bool temp_ready = local_dir.CreateUniqueTempDir();
    const base::FilePath file_path =
        temp_ready ? local_dir.GetPath().AppendASCII("pool-nav.html")
                   : base::FilePath();
    const bool file_written =
        temp_ready && base::WriteFile(file_path, kFileBody);

    struct LocalPoolResult {
      mbNavigationId id = 0;
      bool started = false;
      bool off_engine = false;
      bool no_early_reply = false;
      bool finished = false;
      bool replied = false;
      bool committed = false;
      bool no_timeout = false;
    };
    auto run_local_pool_case = [&](const std::string& url,
                                   const char* selector) {
      LocalPoolResult result;
      mb::MbArmLocalNavigationForTesting(url);
      result.id = mbNavigate(v, url.c_str());
      // Run the posted initial hop. The pool barrier prevents RunUntilIdle from
      // consuming the reply even if the worker starts immediately.
      mbWait(v, 0);
      result.started =
          mb::MbWaitForLocalNavigationWorkerStartForTesting(url, 2000);
      result.off_engine =
          mb::MbLocalNavigationRanOffEngineSequenceForTesting(url);
      result.no_early_reply =
          mb::MbLocalNavigationReplyCountForTesting(url) == 0;
      mb::MbReleaseLocalNavigationForTesting(url);
      result.finished =
          mb::MbWaitForLocalNavigationWorkerFinishForTesting(url, 2000);
      result.no_timeout =
          !mb::MbLocalNavigationBarrierTimedOutForTesting(url);
      result.committed = mbWaitForSelector(v, selector, 2000) == 1;
      result.replied =
          mb::MbLocalNavigationReplyCountForTesting(url) == 1;
      mb::MbClearLocalNavigationForTesting(url);
      return result;
    };

    const std::string data_url =
        "data:text/html,<body id='mb-local-pool-data'>data-pool</body>";
    const LocalPoolResult data =
        run_local_pool_case(data_url, "#mb-local-pool-data");
    const std::string file_url =
        file_written ? net::FilePathToFileURL(file_path).spec() : std::string();
    const LocalPoolResult file = file_written
                                     ? run_local_pool_case(
                                           file_url, "#mb-local-pool-file")
                                     : LocalPoolResult();
    const auto passed = [](const LocalPoolResult& r) {
      return r.id != 0 && r.started && r.off_engine && r.no_early_reply &&
             r.finished && r.replied && r.committed && r.no_timeout;
    };
    Expect(temp_ready && file_written && passed(data) && passed(file),
           "mbNavigate materializes data:/file: off the engine sequence",
           "temp=" + std::to_string(temp_ready) + "/" +
               std::to_string(file_written) + " data=" +
               std::to_string(data.id) + "/" +
               std::to_string(data.started) + "/" +
               std::to_string(data.off_engine) + "/" +
               std::to_string(data.finished) + "/" +
               std::to_string(data.replied) + "/" +
               std::to_string(data.committed) + " file=" +
               std::to_string(file.id) + "/" +
               std::to_string(file.started) + "/" +
               std::to_string(file.off_engine) + "/" +
               std::to_string(file.finished) + "/" +
               std::to_string(file.replied) + "/" +
               std::to_string(file.committed));
  }

  // 104b. Destroying a view immediately after mbNavigate must cancel the posted
  // initial hop without delivering a commit/terminal callback through the dead
  // view. STARTED is intentionally synchronous and carries the returned id;
  // pump a surviving view afterward so the cancelled loader chain really drains.
  {
    mbView* doomed = mbCreateView(180, 100);
    // Keep callback userdata alive for the process. A broken delayed callback
    // should be observed (or expose the engine UAF), never become a test-stack UAF.
    static DestroyNavigationProbe* probe = new DestroyNavigationProbe();
    *probe = DestroyNavigationProbe();
    mbNavigationId id = 0;
    bool started_before_return = false;
    int64_t destroy_ms = -1;
    if (doomed) {
      probe->expected_view = doomed;
      mbOnNavigationEvent(doomed, &RecordDestroyNavigationEvent, probe);
      id = mbNavigate(
          doomed,
          "data:text/html,<body id='must-not-commit'>destroyed-nav</body>");
      started_before_return =
          id != 0 && probe->started == 1 && probe->started_id == id &&
          probe->committed == 0 && probe->terminal == 0 &&
          probe->malformed == 0;
      probe->teardown_started = true;
      const auto destroy_start = std::chrono::steady_clock::now();
      mbDestroyView(doomed);
      destroy_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - destroy_start)
                       .count();
      doomed = nullptr;
    }
    // The first pump runs the cancelled posted initial hop and its completion;
    // the remaining interval exposes any delayed callback that escaped the
    // view's alive-token guard. Then prove the process and surviving view remain usable.
    mbWait(v, 250);
    mbLoadHTML(v, "<body id='destroy-survivor'>survivor-ok</body>",
               "about:blank");
    const bool survivor_ok =
        Eval(v, "document.getElementById('destroy-survivor').textContent") ==
        "survivor-ok";
    const bool callbacks_stopped =
        probe->started == 1 && probe->committed == 0 &&
        probe->terminal == 0 &&
        probe->callbacks_during_or_after_teardown == 0 &&
        probe->malformed == 0;
    Expect(id != 0 && started_before_return && callbacks_stopped &&
               destroy_ms >= 0 && destroy_ms < 2000 && survivor_ok,
           "mbNavigate followed by immediate view destruction drains safely",
           "id=" + std::to_string(id) + " phases=" +
               std::to_string(probe->started) + "/" +
               std::to_string(probe->committed) + "/" +
               std::to_string(probe->terminal) + " late=" +
               std::to_string(probe->callbacks_during_or_after_teardown) +
               " malformed=" + std::to_string(probe->malformed) +
               " destroy_ms=" + std::to_string(destroy_ms) +
               " survivor=" + std::to_string(survivor_ok));
  }

  // 104c. Destroy after the local-navigation pool task has definitely started,
  // then let its file read and reply complete. The probe state is heap-owned by
  // the loader and the callback userdata is process-lifetime, so this observes
  // the reply-after-death interleaving without a timing race or stack UAF.
  {
    static const char kPoolDestroyBody[] =
        "<body id='must-not-commit-pool'>destroyed-pool-nav</body>";
    base::ScopedTempDir pool_dir;
    const bool temp_ready = pool_dir.CreateUniqueTempDir();
    const base::FilePath file_path =
        temp_ready ? pool_dir.GetPath().AppendASCII("destroy-nav.html")
                   : base::FilePath();
    const bool file_written =
        temp_ready && base::WriteFile(file_path, kPoolDestroyBody);
    const std::string url =
        file_written ? net::FilePathToFileURL(file_path).spec() : std::string();
    if (file_written)
      mb::MbArmLocalNavigationForTesting(url);

    mbView* doomed = file_written ? mbCreateView(180, 100) : nullptr;
    static DestroyNavigationProbe* probe = new DestroyNavigationProbe();
    *probe = DestroyNavigationProbe();
    mbNavigationId id = 0;
    bool started_before_return = false;
    bool worker_started = false;
    bool worker_off_engine = false;
    bool held_before_destroy = false;
    if (doomed) {
      probe->expected_view = doomed;
      mbOnNavigationEvent(doomed, &RecordDestroyNavigationEvent, probe);
      id = mbNavigate(doomed, url.c_str());
      started_before_return =
          id != 0 && probe->started == 1 && probe->started_id == id &&
          probe->committed == 0 && probe->terminal == 0 &&
          probe->malformed == 0;
      // Execute the posted initial hop, then wait on the pool-side event. The
      // barrier is still closed, so materialization/reply cannot race ahead.
      mbWait(doomed, 0);
      worker_started =
          mb::MbWaitForLocalNavigationWorkerStartForTesting(url, 2000);
      worker_off_engine =
          mb::MbLocalNavigationRanOffEngineSequenceForTesting(url);
      held_before_destroy =
          mb::MbLocalNavigationReplyCountForTesting(url) == 0 &&
          !mb::MbWaitForLocalNavigationWorkerFinishForTesting(url, 0);
      probe->teardown_started = true;
      mbDestroyView(doomed);
      doomed = nullptr;
    }

    // The file read happens only after the view is gone. Wait for the pool task
    // without pumping the engine, proving its reply has not run early.
    if (file_written)
      mb::MbReleaseLocalNavigationForTesting(url);
    const bool worker_finished =
        file_written &&
        mb::MbWaitForLocalNavigationWorkerFinishForTesting(url, 2000);
    const bool reply_not_run_before_pump =
        file_written && mb::MbLocalNavigationReplyCountForTesting(url) == 0;
    for (int i = 0;
         file_written && i < 200 &&
         mb::MbLocalNavigationReplyCountForTesting(url) == 0;
         ++i) {
      mbWait(v, 10);
    }
    const bool reply_ran_after_destroy =
        file_written && mb::MbLocalNavigationReplyCountForTesting(url) == 1;
    const bool no_barrier_timeout =
        file_written &&
        !mb::MbLocalNavigationBarrierTimedOutForTesting(url);

    mbLoadHTML(v, "<body id='pool-destroy-survivor'>survivor-ok</body>",
               "about:blank");
    const bool survivor_ok =
        Eval(v, "document.getElementById('pool-destroy-survivor').textContent") ==
        "survivor-ok";
    const bool callbacks_stopped =
        probe->started == 1 && probe->committed == 0 &&
        probe->terminal == 0 &&
        probe->callbacks_during_or_after_teardown == 0 &&
        probe->malformed == 0;
    if (file_written)
      mb::MbClearLocalNavigationForTesting(url);

    Expect(temp_ready && file_written && id != 0 && started_before_return &&
               worker_started && worker_off_engine && held_before_destroy &&
               worker_finished &&
               reply_not_run_before_pump && reply_ran_after_destroy &&
               no_barrier_timeout && callbacks_stopped && survivor_ok,
           "destroy during local ThreadPool navigation handles its late reply safely",
           "temp=" + std::to_string(temp_ready) + "/" +
               std::to_string(file_written) + " id=" +
               std::to_string(id) + " worker=" +
               std::to_string(worker_started) + "/" +
               std::to_string(worker_off_engine) + "/" +
               std::to_string(worker_finished) + " reply=" +
               std::to_string(reply_not_run_before_pump) + "/" +
               std::to_string(reply_ran_after_destroy) + " phases=" +
               std::to_string(probe->started) + "/" +
               std::to_string(probe->committed) + "/" +
               std::to_string(probe->terminal) + " late=" +
               std::to_string(probe->callbacks_during_or_after_teardown) +
               " survivor=" + std::to_string(survivor_ok));
  }

  // 105. Cookie jar persistence: save the whole jar to a file, clear it, reload,
  // and the cookies come back — session reuse across process runs. Local + no
  // network (the jar is in-memory; mbSetCookie/mbGetCookies work on an https
  // origin without a fetch, as in case 87).
  {
    mbClearCookies(v);  // start from a known-empty jar
    mbSetCookie(v, "https://persist.test/", "sid=xyz789");
    mbSetCookie(v, "https://persist.test/", "theme=dark");
    const bool saved = mbSaveCookies("/tmp/mb_jar.txt") == 1;
    mbClearCookies(v);
    char c1[256] = {0};
    mbGetCookies(v, "https://persist.test/", c1, sizeof(c1));
    const bool cleared = c1[0] == '\0';
    const bool loaded = mbLoadCookies("/tmp/mb_jar.txt") == 1;
    char c2[256] = {0};
    mbGetCookies(v, "https://persist.test/", c2, sizeof(c2));
    const std::string got(c2);
    const bool restored = got.find("sid=xyz789") != std::string::npos &&
                          got.find("theme=dark") != std::string::npos;
    Expect(saved && cleared && loaded && restored,
           "cookie jar save/load round-trips a session across a clear",
           std::string("saved=") + (saved ? "1" : "0") + " cleared=" +
               (cleared ? "1" : "0") + " loaded=" + (loaded ? "1" : "0") +
               " got=[" + got + "]");
    mbClearCookies(v);  // don't leak into later cases
  }

  // 105b. mbGetHttpStatus is 0 for non-http loads (in-memory and file://), since
  // there's no HTTP response. (The 200-vs-404 http path is the net-gated case 41
  // and is verified end-to-end by mb_shot.) Guards the reset + the non-http path.
  {
    mbLoadHTML(v, "<body>x</body>", "about:blank");
    const bool inmem_zero = mbGetHttpStatus(v) == 0;
    if (FILE* f = std::fopen("/tmp/mb_status.html", "wb")) {
      std::fputs("<body>file</body>", f);
      std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_status.html");
    const bool file_zero = mbGetHttpStatus(v) == 0;
    // Response headers are likewise empty for a non-http load.
    char hb[256] = {0};
    const bool hdr_empty = mbGetResponseHeaders(v, hb, sizeof(hb)) == 0;
    Expect(inmem_zero && file_zero && hdr_empty,
           "mbGetHttpStatus/mbGetResponseHeaders empty for non-http loads",
           std::string("inmem=") + (inmem_zero ? "1" : "0") + " file=" +
               (file_zero ? "1" : "0") + " hdr=" + (hdr_empty ? "1" : "0"));
  }

  // 106. mbEncodePng: render to an in-memory PNG (no temp file) for embedders.
  // Verify the returned bytes are a valid PNG (8-byte signature) whose IHDR
  // width/height (big-endian at offsets 16 and 20) match the requested size.
  {
    mbLoadHTML(v, "<body style='background:#fff'>encode me</body>",
               "about:blank");
    mbWait(v, 30);
    const unsigned char* data = nullptr;
    const int len = mbEncodePng(v, W, H, &data);
    const bool magic = len > 24 && data && data[0] == 0x89 && data[1] == 'P' &&
                       data[2] == 'N' && data[3] == 'G' && data[4] == 0x0D &&
                       data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A;
    int iw = 0, ih = 0;
    if (magic) {
      iw = (data[16] << 24) | (data[17] << 16) | (data[18] << 8) | data[19];
      ih = (data[20] << 24) | (data[21] << 16) | (data[22] << 8) | data[23];
    }
    Expect(magic && iw == W && ih == H,
           "mbEncodePng returns a valid in-memory PNG of the requested size",
           std::string("len=") + std::to_string(len) + " dim=" +
               std::to_string(iw) + "x" + std::to_string(ih));
  }
}

MB_SMOKE_MAIN("mb_smoke_platform")
