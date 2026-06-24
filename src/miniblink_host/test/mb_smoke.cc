// mb_smoke — capability test suite for the miniblink-modern engine. Each case loads
// content and ASSERTS engine behavior (mostly via mbEvalJS / getComputedStyle, which is
// robust; plus one pixel check). Prints PASS/FAIL per case and a summary; exit 0 iff all pass.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "miniblink_host/capi/mb_capi.h"

namespace {
int g_pass = 0, g_fail = 0;

std::string Eval(mbView* v, const char* js) {
  char buf[512];
  mbEvalJS(v, js, buf, sizeof(buf));
  return std::string(buf);
}

// Native function bound into JS for the mbJsBindFunction test: echoes its first
// argument with a "!" suffix and the userdata it was given.
const char* SmokeEcho(void* userdata, int argc, const char** argv,
                      const int* /*argtypes*/,
                      int* /*out_type*/) {  // default string return
  static char buf[256];
  std::snprintf(buf, sizeof(buf), "%s!%d", (argc > 0 && argv[0]) ? argv[0] : "",
                userdata ? *static_cast<int*>(userdata) : -1);
  return buf;
}

// Returns structured data as JSON (out_type 5) -> a real JS object in the page.
const char* SmokeJson(void*, int, const char**, const int*, int* out_type) {
  *out_type = 5;  // json
  return "{\"a\":1,\"b\":[2,3]}";
}

std::string EvalIso(mbView* v, const char* js) {
  char buf[512];
  mbEvalJSIsolated(v, js, buf, sizeof(buf));
  return std::string(buf);
}

void Expect(bool ok, const char* name, const std::string& got = "") {
  std::fprintf(stderr, "  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", name,
               got.empty() ? "" : " -> ", got.c_str());
  ok ? ++g_pass : ++g_fail;
}
}  // namespace

int main() {
  if (!mbInitialize()) {
    std::fprintf(stderr, "init failed\n");
    return 1;
  }
  const int W = 400, H = 300;
  mbView* v = mbCreateView(W, H);
  if (!v)
    return 1;

  // 1. HTML parse + DOM.
  mbLoadHTML(v, "<body><div id='x'>hello</div></body>", "about:blank");
  Expect(Eval(v, "document.getElementById('x').textContent") == "hello",
         "HTML/DOM parse");

  // 2. JavaScript evaluation.
  Expect(Eval(v, "2 + 2 * 10") == "22", "JS eval");

  // 3. CSS cascade via computed style (inline style attr).
  mbLoadHTML(v, "<body><p id='p' style='color:#ff0000'>x</p></body>", "about:blank");
  Expect(Eval(v, "getComputedStyle(document.getElementById('p')).color") ==
             "rgb(255, 0, 0)",
         "CSS computed style");

  // 4. UA stylesheet loaded (h1 default font-weight = bold = 700).
  mbLoadHTML(v, "<body><h1 id='h'>x</h1></body>", "about:blank");
  Expect(Eval(v, "getComputedStyle(document.getElementById('h')).fontWeight") == "700",
         "UA stylesheet (h1 bold)");

  // 5. mbRunJS drives the page; mbEvalJS reads it back.
  mbLoadHTML(v, "<body><b id='b'>0</b></body>", "about:blank");
  mbRunJS(v, "document.getElementById('b').textContent = 'driven';");
  Expect(Eval(v, "document.getElementById('b').textContent") == "driven",
         "mbRunJS + mbEvalJS bridge");

  // 6. <canvas> 2D draws (read a pixel back via getImageData).
  mbLoadHTML(v,
             "<canvas id='c' width='10' height='10'></canvas><script>"
             "var x=document.getElementById('c').getContext('2d');"
             "x.fillStyle='#00ff00';x.fillRect(0,0,10,10);</script>",
             "about:blank");
  Expect(Eval(v, "(function(){var d=document.getElementById('c').getContext('2d')"
                 ".getImageData(5,5,1,1).data;return d[0]+','+d[1]+','+d[2];})()") ==
             "0,255,0",
         "canvas 2D getImageData");

  // 7. External <link> CSS via the subresource URLLoader (+ MimeRegistry).
  {
    const char* css = "#q{color:rgb(0,128,255)}";
    if (FILE* f = std::fopen("/tmp/mb_test.css", "wb")) {
      std::fwrite(css, 1, std::strlen(css), f);
      std::fclose(f);
    }
    const char* html =
        "<head><link rel='stylesheet' href='mb_test.css'></head>"
        "<body><i id='q'>x</i></body>";
    if (FILE* f = std::fopen("/tmp/mb_test.html", "wb")) {
      std::fwrite(html, 1, std::strlen(html), f);
      std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_test.html");
    Expect(Eval(v, "getComputedStyle(document.getElementById('q')).color") ==
               "rgb(0, 128, 255)",
           "external <link> CSS subresource");
  }

  // 8. Rendering produces pixels (red bg -> red top-left pixel).
  mbLoadHTML(v, "<body style='margin:0;background:#ff0000'></body>", "about:blank");
  std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 0);
  mbPaintToBitmap(v, px.data(), W, H, W * 4);
  Expect(px[2] == 255 && px[1] == 0 && px[0] == 0, "paint to bitmap (red bg)");

  // 9. Input: synthesize a click on a button and verify its handler ran.
  mbLoadHTML(v,
             "<body style='margin:0'><button id='b' onclick='window.__c=1' "
             "style='position:absolute;left:20px;top:20px;width:120px;height:40px'>"
             "click</button></body>",
             "about:blank");
  {
    std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // force layout for hit-testing
  }
  mbSendMouseClick(v, 80, 40);  // center of the button
  Expect(Eval(v, "String(window.__c||0)") == "1", "input: synthesized click");

  // 10. Keyboard: focus an input, type, verify its value.
  mbLoadHTML(v, "<body><input id='t'></body>", "about:blank");
  mbRunJS(v, "document.getElementById('t').focus();");
  mbSendText(v, "hi there");
  Expect(Eval(v, "document.getElementById('t').value") == "hi there",
         "input: typed text");

  // 10b. Keyboard with UTF-8: accented + CJK + a supplementary (emoji) char.
  // Verify .length (code-unit count) rather than echoing bytes through mbEvalJS.
  mbLoadHTML(v, "<body><input id='u'></body>", "about:blank");
  mbRunJS(v, "document.getElementById('u').focus();");
  mbSendText(v, "café日本😀");  // 4 + 0 ... = 'c','a','f','é','日','本', emoji(2 units)
  Expect(Eval(v, "document.getElementById('u').value.length") == "8" &&
             Eval(v, "document.getElementById('u').value.codePointAt(4)") ==
                 "26085",  // U+65E5 日
         "input: typed UTF-8 (accent/CJK/emoji)",
         Eval(v, "document.getElementById('u').value"));

  // 10b. UTF-8-safe buffer truncation: a getter into a too-small buffer must cut
  // at a character boundary, never mid-multibyte. "café" has a multi-byte é after
  // "caf"; a 5-byte buffer's naive cut at byte 4 would land inside é (invalid
  // UTF-8). The boundary-aware copy backs off so the result ends at a real char
  // boundary — encoding-independent check below (works whatever é encodes to).
  {
    mbLoadHTML(v, "<body><b id='t'>café</b></body>", "about:blank");
    char big[64] = {0};
    mbGetTextForSelector(v, "#t", big, sizeof(big));  // the full text
    const std::string full_s(big);
    char small[5] = {0};  // out_cap 5 -> at most 4 usable bytes
    mbGetTextForSelector(v, "#t", small, sizeof(small));
    const std::string got(small);
    const bool truncated = got.size() < full_s.size();  // buffer was too small
    const bool is_prefix = full_s.compare(0, got.size(), got) == 0;
    // The byte just past `got` in the full text must NOT be a continuation byte
    // (0b10xxxxxx) — i.e. `got` ended at a char boundary (naive cut would not).
    const bool boundary =
        got.size() == full_s.size() ||
        (static_cast<unsigned char>(full_s[got.size()]) & 0xC0) != 0x80;
    Expect(truncated && is_prefix && boundary && !got.empty(),
           "mbGetTextForSelector truncates at a UTF-8 boundary (no split char)",
           std::string("full=") + std::to_string((int)full_s.size()) + " got='" +
               got + "' (" + std::to_string((int)got.size()) + "B)");
  }

  // 11. Scroll: a tall page, synthesize a downward gesture scroll, verify scrollY.
  mbLoadHTML(v,
             "<body style='margin:0'><div style='height:5000px'></div></body>",
             "about:blank");
  {
    std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // force layout so it's scrollable
  }
  mbSendScroll(v, 200, 150, 0, 400);  // scroll down 400px
  {
    int sy = std::atoi(Eval(v, "String(Math.round(window.scrollY))").c_str());
    Expect(sy > 0, "input: gesture scroll (scrollY)",
           std::to_string(sy));
  }
  // 11b. mbScrollTo: absolute scroll to a known offset (vs the relative gesture).
  mbScrollTo(v, 0, 250);
  {
    int sy = std::atoi(Eval(v, "String(Math.round(window.scrollY))").c_str());
    Expect(sy == 250, "mbScrollTo moves the viewport to an absolute Y",
           std::to_string(sy));
  }

  // 12. Mouse move: hover over an element fires mouseover (and :hover applies).
  mbLoadHTML(v,
             "<body style='margin:0'>"
             "<div id='h' onmouseover='window.__h=1' "
             "style='position:absolute;left:10px;top:10px;width:100px;height:60px'>"
             "hover</div></body>",
             "about:blank");
  {
    std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // layout for hit-testing
  }
  mbSendMouseMove(v, 50, 40);  // over the div
  Expect(Eval(v, "String(window.__h||0)") == "1", "input: mouse move (hover)");

  // mbSendMouseDown/Up enable a DRAG that mbSendMouseClick can't: press, move
  // (carrying the held button so e.buttons==1), release. A pad tracks the drag
  // delta and the buttons mask; a same-point down+up still fires onclick.
  mbLoadHTML(v,
      "<body style='margin:0'><div id='pad' style='width:300px;height:100px'>"
      "</div><button id='b' style='position:absolute;left:0;top:120px;"
      "width:120px;height:40px' "
      "onclick='window.__clk=(window.__clk||0)+1'>b</button>"
      "<script>window.__dx=0;window.__btn=-1;window.__drag=0;window.__done=0;"
      "var p=document.getElementById('pad');"
      "p.addEventListener('mousedown',function(e){window.__drag=1;window.__sx=e.clientX;});"
      "document.addEventListener('mousemove',function(e){if(window.__drag){"
      "window.__dx=e.clientX-window.__sx;window.__btn=e.buttons;}});"
      "document.addEventListener('mouseup',function(){window.__drag=0;window.__done=1;});"
      "</script></body>", "about:blank");
  {
    std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // layout for hit-testing
  }
  mbSendMouseDown(v, 50, 40);
  mbSendMouseMove(v, 150, 40);   // drag right (button held)
  mbSendMouseMove(v, 200, 40);
  mbSendMouseUp(v, 200, 40);
  const bool dragged = Eval(v, "String(window.__dx)") == "150" &&
                       Eval(v, "String(window.__done)") == "1";
  const bool held = Eval(v, "String(window.__btn)") == "1";  // moves carried the button
  mbSendMouseDown(v, 60, 140);   // a same-point down+up is still a click (button center)
  mbSendMouseUp(v, 60, 140);
  const bool click_ok = Eval(v, "String(window.__clk||0)") == "1";
  Expect(dragged && held && click_ok,
         "mbSendMouseDown/Up drag (delta + e.buttons) and down+up clicks",
         std::string("dx=") + Eval(v, "String(window.__dx)") + " btn=" +
             Eval(v, "String(window.__btn)") + " click=" +
             Eval(v, "String(window.__clk||0)"));

  // 12b. mbDragSelector drags one element's center onto another's: #handle follows
  // the cursor during the drag and the drop lands at #target's center x (220).
  {
  mbLoadHTML(v,
      "<body style='margin:0'>"
      "<div id='handle' style='position:absolute;left:0;top:0;width:40px;height:40px'></div>"
      "<div id='target' style='position:absolute;left:200px;top:0;width:40px;height:40px'></div>"
      "<script>window.__moved=0;window.__dropx=-1;var drag=0;"
      "document.getElementById('handle').addEventListener('mousedown',function(){drag=1;});"
      "document.addEventListener('mousemove',function(e){if(drag){window.__moved=1;"
      "document.getElementById('handle').style.left=e.clientX+'px';}});"
      "document.addEventListener('mouseup',function(e){if(drag){drag=0;window.__dropx=e.clientX;}});"
      "</script></body>", "about:blank");
  {
    std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // layout for hit-testing
  }
  const bool drag_ok = mbDragSelector(v, "#handle", "#target") == 1;
  const bool dropped = Eval(v, "String(window.__dropx)") == "220" &&
                       Eval(v, "String(window.__moved)") == "1" &&
                       Eval(v, "document.getElementById('handle').style.left") == "220px";
  const bool nomatch = mbDragSelector(v, "#handle", "#none") == 0;
  Expect(drag_ok && dropped && nomatch,
         "mbDragSelector drags from-center to to-center (drop at 220)",
         std::string("ok=") + (drag_ok ? "1" : "0") + " dropx=" +
             Eval(v, "String(window.__dropx)") + " nomatch=" + (nomatch ? "1" : "0"));
  }

  // 12c. mbSendTouchTap fires real touch events (touchstart+touchend) that mouse
  // events don't — a touch-only handler runs and sees touches[0].clientX == tap x.
  {
    mbLoadHTML(v,
        "<body style='margin:0'><div id='b' style='width:200px;height:100px'></div>"
        "<script>window.__ts=0;window.__tx=-1;window.__te=0;"
        "var b=document.getElementById('b');"
        "b.addEventListener('touchstart',function(e){window.__ts=1;"
        "if(e.touches[0])window.__tx=Math.round(e.touches[0].clientX);});"
        "b.addEventListener('touchend',function(){window.__te=1;});"
        "</script></body>", "about:blank");
    {
      std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
      mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // layout for hit-testing
    }
    mbSendTouchTap(v, 50, 40);
    const bool start = Eval(v, "String(window.__ts)") == "1";
    const bool coord = Eval(v, "String(window.__tx)") == "50";
    const bool end = Eval(v, "String(window.__te)") == "1";
    Expect(start && coord && end,
           "mbSendTouchTap fires touchstart+touchend with touches[0].clientX",
           std::string("start=") + (start ? "1" : "0") + " x=" +
               Eval(v, "String(window.__tx)") + " end=" + (end ? "1" : "0"));
  }

  // 12d. mbSendTouchSwipe drives touchmove: a handler sees the moves and the final
  // touches[0].clientX equals the swipe end x.
  {
    mbLoadHTML(v,
        "<body style='margin:0'><div id='s' style='width:300px;height:100px'></div>"
        "<script>window.__mv=0;window.__mx=-1;window.__se=0;"
        "var s=document.getElementById('s');"
        "s.addEventListener('touchmove',function(e){window.__mv++;"
        "if(e.touches[0])window.__mx=Math.round(e.touches[0].clientX);});"
        "s.addEventListener('touchend',function(){window.__se=1;});"
        "</script></body>", "about:blank");
    {
      std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
      mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // layout for hit-testing
    }
    mbSendTouchSwipe(v, 50, 50, 200, 50);
    const bool moved = Eval(v, "String(window.__mv>0)") == "true";
    const bool endx = Eval(v, "String(window.__mx)") == "200";
    const bool ended = Eval(v, "String(window.__se)") == "1";
    Expect(moved && endx && ended,
           "mbSendTouchSwipe fires touchmoves ending at the swipe end x",
           std::string("moved=") + (moved ? "1" : "0") + " endx=" +
               Eval(v, "String(window.__mx)") + " end=" + (ended ? "1" : "0"));
  }

  // 13. Body with an embedded NUL byte must not truncate the document (the host
  // used to commit body.c_str(), losing everything after the first NUL). Load via
  // file:// (the length-preserving path) and verify content AFTER the NUL parsed.
  {
    const char doc[] =
        "<body><div id='a'>before</div>\0<div id='b'>afternul</div></body>";
    const size_t doc_len = sizeof(doc) - 1;  // includes the embedded NUL
    if (FILE* f = std::fopen("/tmp/mb_nul.html", "wb")) {
      std::fwrite(doc, 1, doc_len, f);
      std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_nul.html");
    Expect(Eval(v, "var e=document.getElementById('b');e?e.textContent:''") ==
               "afternul",
           "load: embedded NUL does not truncate document");
  }

  // 14. Full-page mechanism: after resizing the view taller, a re-render must
  // capture content below the original fold (this is what mb_shot --full relies on).
  // Blue 0..1000px, green 1000..1200px; resize to 1200 tall and read a pixel at y=1100.
  mbLoadHTML(v,
             "<body style='margin:0'>"
             "<div style='height:1000px;background:#0000ff'></div>"
             "<div style='height:200px;background:#00ff00'></div></body>",
             "about:blank");
  mbResize(v, W, 1200);
  {
    std::vector<uint8_t> tall(static_cast<size_t>(W) * 1200 * 4, 0);
    mbPaintToBitmap(v, tall.data(), W, 1200, W * 4);
    const size_t at = (static_cast<size_t>(1100) * W + 10) * 4;  // y=1100 (green band)
    Expect(tall[at + 2] == 0 && tall[at + 1] == 255 && tall[at + 0] == 0,
           "full-page: resize captures below-the-fold");
  }

  // 14b. mbGetViewSize reads back the viewport set via mbResize (window.inner*).
  {
    mbResize(v, 640, 480);
    mbLoadHTML(v, "<body>x</body>", "about:blank");
    int vw = 0, vh = 0;
    const bool got = mbGetViewSize(v, &vw, &vh) == 1 && vw == 640 && vh == 480;
    Expect(got, "mbGetViewSize reads the viewport (640x480)",
           std::string("vw=") + std::to_string(vw) + " vh=" + std::to_string(vh));
    mbResize(v, W, H);  // restore the shared viewport for later cases
  }

  // 15. HiDPI: setting device scale factor makes window.devicePixelRatio report it
  // and resolution media queries re-evaluate (without zooming layout).
  mbSetDeviceScaleFactor(v, 2.0f);
  mbLoadHTML(v,
             "<style>#x{color:rgb(0,0,0)}"
             "@media (min-resolution:1.5dppx){#x{color:rgb(1,2,3)}}</style>"
             "<body><b id='x'>x</b></body>",
             "about:blank");
  Expect(Eval(v, "String(window.devicePixelRatio)") == "2",
         "HiDPI: devicePixelRatio", Eval(v, "String(window.devicePixelRatio)"));
  Expect(Eval(v, "getComputedStyle(document.getElementById('x')).color") ==
             "rgb(1, 2, 3)",
         "HiDPI: min-resolution media query matches");

  // 18. User-Agent: default is a real (non-empty) UA, and the override is reflected
  // in navigator.userAgent. Set before load so it applies to the committed document.
  mbLoadHTML(v, "<body>x</body>", "about:blank");  // default UA
  Expect(Eval(v, "String((navigator.userAgent||'').includes('Mozilla'))") == "true",
         "user-agent: default is non-empty");
  {
    // mbGetUserAgent reports the SAME effective UA the page sees.
    char ua[1024] = {0};
    mbGetUserAgent(v, ua, sizeof(ua));
    Expect(std::string(ua) == Eval(v, "navigator.userAgent"),
           "user-agent: mbGetUserAgent matches navigator.userAgent (default)", ua);
  }
  mbSetUserAgent(v, "MiniblinkBot/9.9 (test)");
  mbLoadHTML(v, "<body>x</body>", "about:blank");  // re-navigate to pick up the UA
  Expect(Eval(v, "navigator.userAgent") == "MiniblinkBot/9.9 (test)",
         "user-agent: override reflected in navigator.userAgent",
         Eval(v, "navigator.userAgent"));
  {
    char ua[1024] = {0};
    mbGetUserAgent(v, ua, sizeof(ua));
    Expect(std::string(ua) == "MiniblinkBot/9.9 (test)",
           "user-agent: mbGetUserAgent returns the override", ua);
  }

  // 20. Clip capture: a green box at logical (50,60,100,40). Clipping exactly to it
  // must yield an all-green bitmap (proves the region offset lands at the origin).
  mbSetDeviceScaleFactor(v, 1.0f);  // undo case-15's 2x so clip math is 1:1
  mbLoadHTML(v,
             "<body style='margin:0'><div style='position:absolute;left:50px;"
             "top:60px;width:100px;height:40px;background:#00ff00'></div></body>",
             "about:blank");
  {
    const int cw = 100, chh = 40;
    std::vector<uint8_t> clip(static_cast<size_t>(cw) * chh * 4, 0);
    mbPaintRectToBitmap(v, clip.data(), 50, 60, cw, chh, cw * 4);
    const size_t mid = (static_cast<size_t>(20) * cw + 50) * 4;  // center-ish
    Expect(clip[mid + 2] == 0 && clip[mid + 1] == 255 && clip[mid + 0] == 0,
           "clip: region capture lands on the element");
  }

  // 21. Transparent background (omitBackground): a page with no opaque body bg and a
  // single opaque green box. Outside the box must be alpha 0; inside, opaque green.
  mbSetTransparentBackground(v, 1);
  mbLoadHTML(v,
             "<body style='margin:0;background:transparent'>"
             "<div style='position:absolute;left:0;top:0;width:30px;height:30px;"
             "background:#00ff00'></div></body>",
             "about:blank");
  {
    std::vector<uint8_t> tpx(static_cast<size_t>(W) * H * 4, 0xAB);
    mbPaintToBitmap(v, tpx.data(), W, H, W * 4);
    const size_t inside = (static_cast<size_t>(10) * W + 10) * 4;  // in the box
    const size_t outside = (static_cast<size_t>(200) * W + 300) * 4;  // empty area
    Expect(tpx[inside + 3] == 255 && tpx[inside + 1] == 255 &&
               tpx[outside + 3] == 0,
           "transparent background (omitBackground)");
  }
  mbSetTransparentBackground(v, 0);  // restore default for any later use

  // 22. Wait-for-selector: content injected by a setTimeout must be caught by
  // mbWaitForSelector (which advances real time so the timer fires), and a selector
  // that never appears must time out returning 0. (We don't assert the element is
  // absent immediately after load — the load's own pumping spans enough wall-clock
  // that a short timer may already have fired; that timing isn't a guarantee.)
  mbSetTransparentBackground(v, 0);
  mbLoadHTML(v,
             "<body><script>setTimeout(function(){var d=document.createElement('div');"
             "d.id='ready';d.textContent='late';document.body.appendChild(d);},300);"
             "</script></body>",
             "about:blank");
  Expect(mbWaitForSelector(v, "#ready", 4000) == 1 &&
             Eval(v, "document.getElementById('ready').textContent") == "late",
         "wait: mbWaitForSelector catches setTimeout content");
  Expect(mbWaitForSelector(v, "#never", 100) == 0,
         "wait: missing selector times out");

  // 23. DOM storage probe: SPAs rely on localStorage/sessionStorage. Load over a
  // file:// origin (opaque origins deny storage) and round-trip a value.
  {
    const char* html = "<body>x</body>";
    if (FILE* f = std::fopen("/tmp/mb_store.html", "wb")) {
      std::fwrite(html, 1, std::strlen(html), f);
      std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_store.html");
    Expect(Eval(v, "(function(){try{localStorage.setItem('k','v42');"
                   "return localStorage.getItem('k');}catch(e){return 'THROW:'+e.name;}})()")
               == "v42",
           "DOM localStorage round-trip");
    Expect(Eval(v, "(function(){try{sessionStorage.setItem('s','s7');"
                   "return sessionStorage.getItem('s');}catch(e){return 'THROW:'+e.name;}})()")
               == "s7",
           "DOM sessionStorage round-trip",
           Eval(v, "(function(){try{sessionStorage.setItem('s','s7');"
                   "return sessionStorage.getItem('s');}catch(e){return 'THROW:'+e.name;}})()"));
  }

  // 25. requestAnimationFrame must fire (no compositor drives it; the host services
  // the page animator). Register a rAF that mutates the DOM, pump, verify it ran.
  mbLoadHTML(v, "<body><b id='r'>0</b></body>", "about:blank");
  mbRunJS(v, "requestAnimationFrame(function(){"
             "document.getElementById('r').textContent='raf';});");
  mbWait(v, 50);
  Expect(Eval(v, "document.getElementById('r').textContent") == "raf",
         "requestAnimationFrame callback fires");
  // A rAF chain (two frames) also advances — proves repeated servicing, not a one-shot.
  mbRunJS(v, "window.__n=0;(function loop(){requestAnimationFrame(function(){"
             "if(++window.__n<2)loop();});})();");
  mbWait(v, 80);
  Expect(Eval(v, "String(window.__n)") == "2",
         "requestAnimationFrame chain advances", Eval(v, "String(window.__n)"));

  // 27. Observer delivery: MutationObserver must fire on a DOM change, and an
  // IntersectionObserver on an in-viewport element must deliver (the offscreen
  // frame reads as throttled, so IO is force-computed by the host).
  {
    const char* doc =
        "<body><div id='t'>0</div><div id='io' style='height:20px'></div>"
        "<script>"
        "window.__mo=0;new MutationObserver(function(){window.__mo=1;})"
        ".observe(document.getElementById('t'),{childList:true,subtree:true,characterData:true});"
        "document.getElementById('t').textContent='changed';"
        "window.__io=0;new IntersectionObserver(function(es){"
        "es.forEach(function(e){if(e.isIntersecting)window.__io=1;});})"
        ".observe(document.getElementById('io'));"
        "</script></body>";
    if (FILE* f = std::fopen("/tmp/mb_observers.html", "wb")) {
      std::fwrite(doc, 1, std::strlen(doc), f); std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_observers.html");
    mbWait(v, 80);
    Expect(Eval(v, "String(window.__mo)") == "1", "MutationObserver delivers");
    Expect(Eval(v, "String(window.__io)") == "1",
           "IntersectionObserver delivers (in-viewport)",
           Eval(v, "String(window.__io)"));
  }

  // 29. Time-based animation + networking-adjacent delivery (these guard the rAF /
  // animation-clock + observer servicing added in recent changes):
  //  - Web Animations API: a 100ms animation's finished promise resolves (clock advances).
  //  - ResizeObserver delivers its initial observation.
  //  - dynamic Image().onload fires; synchronous XHR to a data: URL returns the body.
  {
    mbLoadHTML(v,
        "<body><div id='b' style='width:50px;height:50px'></div><script>"
        "window.__waapi=0;document.getElementById('b').animate("
        "[{opacity:0},{opacity:1}],100).finished.then(function(){window.__waapi=1;});"
        "window.__ro=0;new ResizeObserver(function(){window.__ro=1;})"
        ".observe(document.getElementById('b'));"
        "window.__img=0;var im=new Image();im.onload=function(){window.__img=1;};"
        "im.src='data:image/svg+xml,<svg xmlns=%22http://www.w3.org/2000/svg%22 width=%225%22 height=%225%22></svg>';"
        "window.__xhr='';try{var x=new XMLHttpRequest();x.open('GET',"
        "'data:text/plain,hello',false);x.send();window.__xhr=x.responseText;}"
        "catch(e){window.__xhr='ERR:'+e.name;}"
        "</script></body>",
        "about:blank");
    mbWait(v, 250);
    Expect(Eval(v, "String(window.__waapi)") == "1",
           "Web Animations API finished promise resolves (clock advances)");
    Expect(Eval(v, "String(window.__ro)") == "1", "ResizeObserver delivers");
    Expect(Eval(v, "String(window.__img)") == "1" &&
               Eval(v, "window.__xhr") == "hello",
           "dynamic Image().onload + sync XHR(data:) work");
  }

  // 30. Console capture: page console.log/warn/error are captured and drainable.
  mbLoadHTML(v, "<body><script>console.log('hello');console.warn('careful');"
                "console.error('boom');</script></body>", "about:blank");
  mbWait(v, 20);
  {
    char cbuf[1024] = {0};
    mbDrainConsole(v, cbuf, sizeof(cbuf));
    std::string console(cbuf);
    Expect(console.find("log: hello") != std::string::npos &&
               console.find("warn: careful") != std::string::npos &&
               console.find("error: boom") != std::string::npos,
           "console capture (log/warn/error)", console);
    // Draining clears the buffer.
    char cbuf2[64] = {0};
    mbDrainConsole(v, cbuf2, sizeof(cbuf2));
    Expect(cbuf2[0] == '\0', "console buffer clears after drain");
  }

  // PROBE: IndexedDB (PWAs use it; may need a backend like DOM storage did).
  mbLoadHTML(v, "<body>x</body>", "about:blank");
  mbRunJS(v, "window.__idb='pending';var r=indexedDB.open('mbdb',1);"
             "r.onsuccess=function(){window.__idb='ok';};"
             "r.onerror=function(){window.__idb='err';};"
             "r.onblocked=function(){window.__idb='blocked';};");
  mbWait(v, 150);
  std::fprintf(stderr, "PROBE4 indexedDB=%s\n", Eval(v, "String(window.__idb)").c_str());

  // Network cases (31, 32) are OPT-IN via MB_NET_TESTS=1: a dead host costs ~45s
  // per load (connect-timeout x retries), which would make every default run crawl.
  // They still skip gracefully if enabled but httpbin is unreachable.
  // Network cases use an httpbin-shaped echo host: default httpbin.org, override
  // with MB_NET_HOST for a deterministic local run, e.g.:
  //   python3 src/miniblink_host/test/echo_server.py &   # serves 127.0.0.1:8899
  //   MB_NET_TESTS=1 MB_NET_HOST=http://127.0.0.1:8899 ./mb_smoke
  if (std::getenv("MB_NET_TESTS")) {
  const std::string host =
      std::getenv("MB_NET_HOST") ? std::getenv("MB_NET_HOST") : "https://httpbin.org";
  // httpbin is a flaky public host: probe its health once so the cases that
  // assert specific httpbin shapes (status/redirect) SKIP — not fail — when it
  // is degraded (e.g. returning 503 for everything) rather than misbehaving.
  mbLoadURL(v, (host + "/get").c_str());
  const bool hb_ok = mbGetHttpStatus(v) == 200;
  // 31. Cookie jar: set a cookie via a redirecting endpoint, then a SEPARATE request
  // must still send it — Set-Cookie survives the redirect and the jar is shared.
  mbLoadURL(v, (host + "/cookies/set?mbck=val99").c_str());  // 302 -> /cookies
  mbWait(v, 400);
  std::string ck1 = Eval(v, "document.body?document.body.innerText:''");
  if (ck1.find("cookies") != std::string::npos) {  // host responded
    bool survived_redirect = ck1.find("val99") != std::string::npos;
    mbLoadURL(v, (host + "/cookies").c_str());  // separate request, shared jar
    mbWait(v, 400);
    std::string ck2 = Eval(v, "document.body?document.body.innerText:''");
    bool jar_persists = ck2.find("val99") != std::string::npos;
    Expect(survived_redirect && jar_persists,
           "cookie jar: survives redirect + persists across requests");
  } else {
    std::fprintf(stderr, "  [SKIP] cookie jar (host unreachable)\n");
  }

  // 32. Request headers: a custom header and the default Accept-Language must reach
  // the server (the echo host returns the request headers).
  mbSetExtraHeaders(v, "X-Mb-Test: probe-42");
  mbLoadURL(v, (host + "/headers").c_str());
  mbWait(v, 400);
  {
    std::string h = Eval(v, "document.body?document.body.innerText:''");
    if (h.find("headers") != std::string::npos) {  // host responded
      Expect(h.find("probe-42") != std::string::npos &&
                 h.find("Accept-Language") != std::string::npos,
             "request headers: custom header + default Accept-Language sent");
    } else {
      std::fprintf(stderr, "  [SKIP] request headers (host unreachable)\n");
    }
  }
  mbSetExtraHeaders(v, "");  // reset

  // 33. Cookie bridge: a cookie set via document.cookie on an http origin must be
  // sent on a subsequent network request (JS jar -> HTTP jar).
  mbLoadURL(v, (host + "/").c_str());
  mbWait(v, 400);
  if (!Eval(v, "String(document.location.host)").empty() &&
      Eval(v, "String(document.location.host)") != "undefined") {
    mbRunJS(v, "document.cookie='mbjs=fromjs';");
    mbLoadURL(v, (host + "/cookies").c_str());
    mbWait(v, 400);
    std::string c = Eval(v, "document.body?document.body.innerText:''");
    if (c.find("cookies") != std::string::npos) {
      Expect(c.find("fromjs") != std::string::npos,
             "cookie bridge: document.cookie reaches the HTTP jar");
    } else {
      std::fprintf(stderr, "  [SKIP] cookie bridge (host unreachable)\n");
    }
  } else {
    std::fprintf(stderr, "  [SKIP] cookie bridge (host unreachable)\n");
  }

  // 34. Image loading toggle: a NETWORK <img> loads (naturalWidth>0) by default but
  // is skipped (naturalWidth==0) when image loading is disabled. (data: images are
  // inline and not gated by this setting, so the test uses a served image.)
  {
    const std::string page = "<body><img id='i' src='" + host + "/img'></body>";
    std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
    mbSetLoadImages(v, 1);
    mbLoadHTML(v, page.c_str(), (host + "/").c_str());
    mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // settle the fetch+decode
    std::string on_nw = Eval(v, "String(document.getElementById('i').naturalWidth)");
    if (std::atoi(on_nw.c_str()) > 0) {  // host served the image
      mbSetLoadImages(v, 0);
      mbLoadHTML(v, page.c_str(), (host + "/").c_str());
      mbPaintToBitmap(v, tmp.data(), W, H, W * 4);
      std::string off_nw =
          Eval(v, "String(document.getElementById('i').naturalWidth)");
      Expect(off_nw == "0", "no-images: network image skipped when disabled",
             "on=" + on_nw + " off=" + off_nw);
      mbSetLoadImages(v, 1);
    } else {
      std::fprintf(stderr, "  [SKIP] no-images (host image unreachable)\n");
    }
  }

  // 35. Cookie export: after the jar has a cookie (set above via /cookies/set),
  // mbGetCookies returns it for the host to extract/reuse.
  {
    mbLoadURL(v, (host + "/cookies/set?expk=expv").c_str());
    mbWait(v, 400);
    if (!Eval(v, "(document.body?document.body.innerText:'')").empty()) {
      char cb[512] = {0};
      mbGetCookies(v, (host + "/").c_str(), cb, sizeof(cb));
      Expect(std::string(cb).find("expk=expv") != std::string::npos,
             "mbGetCookies exports the jar", cb);
    } else {
      std::fprintf(stderr, "  [SKIP] cookie export (host unreachable)\n");
    }
  }
  // 36. POST form submission: a method=post form's body must reach the server.
  // The form auto-submits via JS (same BeginNavigation path as a click); the
  // echo host returns the posted fields as JSON, so ours must come back. This
  // exercises the POST path in DoCommit (extract WebHTTPBody) + MbFetchUrl POST.
  {
    const std::string page =
        "<body><form id='f' method='post' action='" + host + "/post'>"
        "<input name='user' value='mbpost'></form>"
        "<script>document.getElementById('f').submit();</script></body>";
    mbLoadHTML(v, page.c_str(), (host + "/").c_str());
    mbWait(v, 900);  // submit -> POST -> commit the echo response
    std::string r = Eval(v, "document.body?document.body.innerText:''");
    if (r.find("\"form\"") != std::string::npos ||
        r.find("\"user\"") != std::string::npos) {  // host echoed
      Expect(r.find("mbpost") != std::string::npos,
             "POST form submission: posted body reaches the server");
    } else {
      std::fprintf(stderr, "  [SKIP] POST form (host unreachable)\n");
    }
  }

  // 37. fetch()/XHR with a request body: the subresource loader (MbURLLoader)
  // must send the method + body, not a bodyless GET. Issue a fetch POST and read
  // the echoed field back. (The page is loaded from the host origin so the fetch
  // is same-origin.) Exercises the method/body extraction in MbURLLoader::Deliver.
  {
    mbLoadHTML(v, "<body>x</body>", (host + "/").c_str());
    mbRunJS(v,
        ("window.__fp='';fetch('" + host +
         "/post',{method:'POST',headers:{'Content-Type':"
         "'application/x-www-form-urlencoded'},body:'mk=fetchmk'})"
         ".then(function(r){return r.json();}).then(function(j){"
         "window.__fp=(j.form&&j.form.mk)||'nofield';}).catch(function(e){"
         "window.__fp='ERR:'+e.name;});").c_str());
    mbWait(v, 1500);  // async fetch round-trip
    std::string r = Eval(v, "String(window.__fp)");
    if (!r.empty() && r.rfind("ERR:", 0) != 0) {  // host responded
      Expect(r.find("fetchmk") != std::string::npos,
             "fetch() POST sends the request body", r);
    } else {
      std::fprintf(stderr, "  [SKIP] fetch POST (host unreachable: %s)\n",
                   r.c_str());
    }
  }

  // 38. fetch() per-request headers: a custom header set on the fetch (e.g. an
  // Authorization token or X-* header) must reach the server. The echo host
  // returns the request headers; ours must be present. Exercises forwarding of
  // request->headers in MbURLLoader::Deliver.
  {
    mbLoadHTML(v, "<body>x</body>", (host + "/").c_str());
    mbRunJS(v,
        ("window.__fh='';fetch('" + host +
         "/headers',{headers:{'X-Mb-Tok':'mbtok7'}})"
         ".then(function(r){return r.json();}).then(function(j){"
         // Match the value, not the key: echo hosts differ on header-name case
         // (httpbin Title-Cases, postman-echo lowercases).
         "window.__fh=JSON.stringify(j.headers||{});})"
         ".catch(function(e){window.__fh='ERR:'+e.name;});").c_str());
    mbWait(v, 1500);  // async fetch round-trip
    std::string r = Eval(v, "String(window.__fh)");
    if (!r.empty() && r.rfind("ERR:", 0) != 0) {  // host responded
      Expect(r.find("mbtok7") != std::string::npos,
             "fetch() forwards custom request headers", r);
    } else {
      std::fprintf(stderr, "  [SKIP] fetch headers (host unreachable: %s)\n",
                   r.c_str());
    }
  }

  // 39. Response status + headers: an HTTP error (404) must resolve as a real
  // Response (status 404, ok=false), NOT a rejected fetch — and a server
  // response header must be readable. Previously 4xx/5xx were turned into
  // network failures (TypeError) and only Content-Type was exposed. (httpbin
  // shapes: /status/404 and /response-headers?k=v.)
  {
    mbLoadHTML(v, "<body>x</body>", (host + "/").c_str());
    mbRunJS(v,
        ("window.__rs='';fetch('" + host +
         "/status/404').then(function(r){window.__rs=r.status+'/'+r.ok;})"
         ".catch(function(e){window.__rs='ERR:'+e.name;});"
         "window.__rh='';fetch('" + host +
         "/response-headers?X-Smk=sv1').then(function(r){"
         "window.__rh=r.headers.get('X-Smk')||'MISSING';})"
         ".catch(function(e){window.__rh='ERR:'+e.name;});").c_str());
    mbWait(v, 1800);  // two async round-trips
    std::string st = Eval(v, "String(window.__rs)");
    std::string hd = Eval(v, "String(window.__rh)");
    if (hb_ok && !st.empty() && st.rfind("ERR:", 0) != 0) {  // host healthy
      Expect(st == "404/false" && hd == "sv1",
             "fetch sees real HTTP status (404, !ok) + response headers",
             st + " hdr=" + hd);
    } else {
      std::fprintf(stderr, "  [SKIP] response status/headers (host unhealthy: %s)\n",
                   st.c_str());
    }
  }

  // 40. Navigation redirect: navigating to a URL that 302-redirects must commit
  // with the FINAL URL as the document URL (location.href), not the original.
  // curl follows the redirect; LoadURL now commits the effective URL as the base.
  {
    mbLoadURL(v,
              (host + "/redirect-to?url=" + host + "/get&status_code=302").c_str());
    mbWait(v, 700);
    std::string loc = Eval(v, "String(location.href)");
    if (hb_ok && (loc.find("/get") != std::string::npos ||
                  loc.find("/redirect-to") != std::string::npos)) {  // healthy
      Expect(loc.find("/get") != std::string::npos &&
                 loc.find("/redirect-to") == std::string::npos,
             "navigation redirect commits the final URL as location.href", loc);
    } else {
      std::fprintf(stderr, "  [SKIP] nav redirect (host unhealthy: %s)\n",
                   loc.c_str());
    }
  }

  // 41. fetch() redirect: a fetch that 302-redirects must resolve as a Response
  // whose url is the FINAL URL and whose .redirected is true. The loader follows
  // redirects manually and reports each hop via WillFollowRedirect so Blink's
  // url list (response.url / redirected) is correct.
  {
    mbLoadHTML(v, "<body>x</body>", (host + "/").c_str());
    mbRunJS(v,
        ("window.__rr='';fetch('" + host + "/redirect-to?url=" + host +
         "/get&status_code=302').then(function(r){"
         "window.__rr=r.url+'|'+r.redirected;}).catch(function(e){"
         "window.__rr='ERR:'+e.name;});").c_str());
    mbWait(v, 1500);
    std::string rr = Eval(v, "String(window.__rr)");
    if (hb_ok && !rr.empty() && rr.rfind("ERR:", 0) != 0) {  // host healthy
      Expect(rr.find("/get") != std::string::npos &&
                 rr.find("/redirect-to") == std::string::npos &&
                 rr.find("|true") != std::string::npos,
             "fetch() redirect exposes final url + redirected=true", rr);
    } else {
      std::fprintf(stderr, "  [SKIP] fetch redirect (host unhealthy: %s)\n",
                   rr.c_str());
    }
  }

  // 41 (net). mbGetHttpStatus reflects the last navigation's real HTTP status
  // (200 vs 404), and mbGetResponseHeaders exposes the server's response headers.
  {
    mbLoadURL(v, (host + "/html").c_str());
    mbWait(v, 600);
    const int ok_status = mbGetHttpStatus(v);
    if (ok_status != 0) {  // host reachable
      char hb[4096] = {0};
      mbGetResponseHeaders(v, hb, sizeof(hb));
      std::string headers(hb);
      for (char& c : headers) c = static_cast<char>(std::tolower((unsigned char)c));
      const bool has_ct = headers.find("content-type") != std::string::npos;
      mbLoadURL(v, (host + "/status/404").c_str());
      mbWait(v, 600);
      const int err_status = mbGetHttpStatus(v);
      Expect(ok_status == 200 && err_status == 404 && has_ct,
             "mbGetHttpStatus (200/404) + mbGetResponseHeaders exposes headers",
             "ok=" + std::to_string(ok_status) + " err=" +
                 std::to_string(err_status) + " ct=" + (has_ct ? "1" : "0"));
    } else {
      std::fprintf(stderr, "  [SKIP] http status/headers (host unreachable)\n");
    }
  }

  // 42 (net). mbSetFollowRedirects(0) stops at the redirect so the 30x status +
  // Location header are visible; re-enabling follows through to the final 200.
  {
    mbSetFollowRedirects(0);
    mbLoadURL(v, (host + "/redirect/1").c_str());  // 302 -> /get
    mbWait(v, 700);
    const int s_off = mbGetHttpStatus(v);
    char hb[4096] = {0};
    mbGetResponseHeaders(v, hb, sizeof(hb));
    std::string h(hb);
    for (char& c : h) c = static_cast<char>(std::tolower((unsigned char)c));
    const bool redirect_seen =
        s_off >= 300 && s_off < 400 && h.find("location:") != std::string::npos;
    mbSetFollowRedirects(1);  // restore the default (process-wide) before more loads
    mbLoadURL(v, (host + "/redirect/1").c_str());
    mbWait(v, 900);
    const int s_on = mbGetHttpStatus(v);
    if (s_off != 0 || s_on != 0) {  // host reachable
      Expect(redirect_seen && s_on == 200,
             "mbSetFollowRedirects: off exposes 30x+Location, on follows to 200",
             "off=" + std::to_string(s_off) + " on=" + std::to_string(s_on));
    } else {
      std::fprintf(stderr, "  [SKIP] follow-redirects (host unreachable)\n");
    }
  }

  // 43 (net). mbPostURL: host-driven POST navigation. httpbin/post echoes the
  // received form data into the response JSON, which becomes the document.
  {
    mbPostURL(v, (host + "/post").c_str(), "mbk=postval", nullptr);
    mbWait(v, 700);
    const int status = mbGetHttpStatus(v);
    if (status != 0) {  // host reachable
      std::string doc = Eval(v, "document.body?document.body.innerText:''");
      Expect(status == 200 && doc.find("mbk") != std::string::npos &&
                 doc.find("postval") != std::string::npos,
             "mbPostURL posts a body and commits the response",
             "status=" + std::to_string(status));
    } else {
      std::fprintf(stderr, "  [SKIP] mbPostURL (host unreachable)\n");
    }
  }

  // 44 (net). End-to-end integration on a REAL https page: one fetch over real
  // TLS exercises the whole stack together — load -> parse -> layout -> the recent
  // scraping readers (text/html/rect/style). example.com is a stable target whose
  // <h1> says "Example Domain". Skips if the host is unreachable.
  {
    mbLoadURL(v, "https://example.com");
    mbWaitForSelector(v, "h1", 4000);
    const int status = mbGetHttpStatus(v);
    if (status == 200) {
      char tb[256] = {0};
      mbGetTextForSelector(v, "h1", tb, sizeof(tb));
      const bool text_ok = std::string(tb).find("Example Domain") != std::string::npos;
      char hb[512] = {0};
      mbGetHtmlForSelector(v, "h1", hb, sizeof(hb));
      const std::string html(hb);
      const bool html_ok = html.find("<h1") != std::string::npos &&
                           html.find("Example Domain") != std::string::npos;
      int rw = 0, rh = 0;
      const bool rect_ok =
          mbGetElementRect(v, "h1", nullptr, nullptr, &rw, &rh) && rw > 0 && rh > 0;
      char sb[64] = {0};
      mbGetComputedStyle(v, "h1", "display", sb, sizeof(sb));
      const bool style_ok = std::string(sb) == "block";
      Expect(text_ok && html_ok && rect_ok && style_ok,
             "integration: real-TLS load + text/html/rect/style readers agree",
             std::string("text=") + (text_ok ? "1" : "0") + " html=" +
                 (html_ok ? "1" : "0") + " rect=" + (rect_ok ? "1" : "0") +
                 " style=" + (style_ok ? "1" : "0"));
    } else {
      std::fprintf(stderr, "  [SKIP] integration (example.com unreachable)\n");
    }
  }
  }  // MB_NET_TESTS

  // 33. document.cookie (JS): write then read round-trips through the in-process
  // RestrictedCookieManager wired into the frame's BrowserInterfaceBroker.
  {
    const char* doc = "<body>x</body>";
    if (FILE* f = std::fopen("/tmp/mb_jsck.html", "wb")) {
      std::fwrite(doc, 1, std::strlen(doc), f); std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_jsck.html");
    mbRunJS(v, "document.cookie='a=1';document.cookie='b=2';");
    mbWait(v, 20);
    std::string ck = Eval(v, "document.cookie");
    Expect(ck.find("a=1") != std::string::npos &&
               ck.find("b=2") != std::string::npos,
           "document.cookie read/write round-trip", ck);
  }

  // 33b. document.cookie READ as the first cookie op, from the page's own inline
  // script during load (no prior write, no pump in between). This is the common
  // "read existing cookies on load" pattern and it used to HANG: the synchronous
  // RestrictedCookieManager.GetCookiesString blocked the main thread before the
  // BrowserInterfaceBroker.GetInterface that binds the manager had been pumped.
  // The broker is now bound on the runtime service thread, so the [Sync] read is
  // serviced off-thread and returns immediately. The inline read records the jar
  // into the DOM; reaching this assertion at all proves it didn't hang. Same
  // file:// origin as case 33, so it reads back the a=1/b=2 set there.
  {
    const char* doc =
        "<body><div id=o>x</div><script>"
        "document.getElementById('o').textContent='ck['+document.cookie+']';"
        "</script></body>";
    if (FILE* f = std::fopen("/tmp/mb_jsck2.html", "wb")) {
      std::fwrite(doc, 1, std::strlen(doc), f); std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_jsck2.html");
    mbWait(v, 30);
    std::string ck2 = Eval(v, "document.getElementById('o').textContent");
    Expect(ck2.rfind("ck[", 0) == 0 && ck2.find("a=1") != std::string::npos &&
               ck2.find("b=2") != std::string::npos,
           "document.cookie read-first on load does not hang", ck2);
  }

  // 34. Init script (evaluateOnNewDocument): runs before the page's own scripts.
  // Set a global in the init script; the page's inline script must observe it.
  mbSetInitScript(v, "window.__early='injected';");
  mbLoadHTML(v,
             "<body><script>window.__pageSaw=window.__early||'no';</script></body>",
             "about:blank");
  Expect(Eval(v, "window.__pageSaw") == "injected",
         "init script runs before page scripts", Eval(v, "window.__pageSaw"));
  mbSetInitScript(v, "");  // clear so it doesn't affect any later case

  // 35. Isolated-world eval: separate JS globals from the main world, shared DOM.
  mbLoadHTML(v, "<body></body>", "about:blank");
  mbRunJS(v, "window.__main='mainval';");
  // In the isolated world: set its own global, touch the shared DOM, and report
  // whether it can see the main world's global (it must NOT).
  Expect(EvalIso(v, "window.__iso='isoval';"
                    "document.body.setAttribute('data-s','shared');"
                    "String(typeof window.__main);") == "undefined",
         "isolated world: cannot see main-world globals");
  Expect(Eval(v, "String(window.__iso)") == "undefined",
         "isolated world: does not leak globals into main world");
  Expect(Eval(v, "document.body.getAttribute('data-s')") == "shared",
         "isolated world: shares the DOM with main world");

  // 36. Dark mode: prefers-color-scheme media query + a responsive CSS rule flip
  // when dark mode is emulated.
  {
    const char* page =
        "<style>#d{color:rgb(1,1,1)}"
        "@media (prefers-color-scheme:dark){#d{color:rgb(2,2,2)}}</style>"
        "<body><b id='d'>x</b></body>";
    mbSetDarkMode(v, 1);
    mbLoadHTML(v, page, "about:blank");
    bool mm = Eval(v, "String(matchMedia('(prefers-color-scheme:dark)').matches)") ==
              "true";
    bool css = Eval(v, "getComputedStyle(document.getElementById('d')).color") ==
               "rgb(2, 2, 2)";
    Expect(mm && css, "dark mode: prefers-color-scheme dark applies");
    mbSetDarkMode(v, 0);  // restore light for any later case
  }

  // 37. Locale: navigator.language / navigator.languages reflect the set value.
  mbSetLocale(v, "fr-FR,fr,en");
  mbLoadHTML(v, "<body>x</body>", "about:blank");
  Expect(Eval(v, "navigator.language") == "fr-FR" &&
             Eval(v, "navigator.languages.join(',')") == "fr-FR,fr,en",
         "locale: navigator.language(s) set",
         Eval(v, "navigator.language") + " / " +
             Eval(v, "navigator.languages.join(',')"));
  mbSetLocale(v, "en-US");  // restore for any later case

  // 38. Timezone override: Date/Intl report the chosen zone deterministically.
  mbSetTimezone(v, "America/New_York");
  mbLoadHTML(v, "<body>x</body>", "about:blank");
  Expect(Eval(v, "Intl.DateTimeFormat().resolvedOptions().timeZone") ==
             "America/New_York",
         "timezone override (Intl resolvedOptions)",
         Eval(v, "Intl.DateTimeFormat().resolvedOptions().timeZone"));
  // A fixed UTC instant formats to a New-York wall-clock time (EST/EDT), proving
  // Date itself uses the zone: 2021-01-01T00:00:00Z -> 2020-12-31 19:00 EST.
  Expect(Eval(v, "new Date(1609459200000).getHours().toString()") == "19",
         "timezone override (Date local hours)",
         Eval(v, "new Date(1609459200000).getHours().toString()"));
  mbSetTimezone(v, "UTC");  // restore deterministic UTC for any later case

  // 39. PDF export: print a document to a PDF and confirm it's a real PDF file.
  mbLoadHTML(v, "<body style='font:30px sans-serif'><h1>PDF</h1><p>page content</p></body>",
             "about:blank");
  {
    const char* pdf_path = "/tmp/mb_smoke.pdf";
    bool ok = mbSavePdf(v, pdf_path) != 0;
    char hdr[6] = {0};
    long sz = 0;
    if (FILE* f = std::fopen(pdf_path, "rb")) {
      std::fread(hdr, 1, 5, f);
      std::fseek(f, 0, SEEK_END);
      sz = std::ftell(f);
      std::fclose(f);
    }
    Expect(ok && std::string(hdr) == "%PDF-" && sz > 500,
           "PDF export (valid %PDF, non-trivial)",
           std::string(hdr) + " sz=" + std::to_string(sz));
  }

  // 35. Cutting-edge modern CSS — the M150-vs-M47 selling points, none of which the
  // frozen ~2015 engine could do. Each rule colors an element only if the feature works.
  mbLoadHTML(v,
    "<style>"
    ".p:has(.kid){color:rgb(1,1,1)} "                    // :has() selector
    ".n{& .inner{color:rgb(3,3,3)}} "                    // native CSS nesting
    "@container (min-width:1px){.c{color:rgb(4,4,4)}} "  // container query
    ".mix{color:color-mix(in srgb,#000,#fff)} "          // color-mix()
    "</style>"
    "<div class='p'><span class='kid'>x</span><b id='has'>x</b></div>"
    "<div class='n'><b class='inner' id='nest'>x</b></div>"
    "<div style='container-type:inline-size'><b class='c' id='cont'>x</b></div>"
    "<b class='mix' id='mix'>x</b>",
    "about:blank");
  Expect(Eval(v, "getComputedStyle(document.getElementById('has')).color") == "rgb(1, 1, 1)" &&
             Eval(v, "getComputedStyle(document.getElementById('nest')).color") == "rgb(3, 3, 3)" &&
             Eval(v, "getComputedStyle(document.getElementById('cont')).color") == "rgb(4, 4, 4)" &&
             Eval(v, "getComputedStyle(document.getElementById('mix')).color")
                     .find("0.5") != std::string::npos,
         "modern CSS: :has(), nesting, @container, color-mix()");

  // 36. Web Components (Custom Elements v1 + Shadow DOM) — a major modern-platform
  // feature (M47 had only the v0 prototype). Define a custom element (its
  // connectedCallback upgrades it), and attach an encapsulated shadow tree.
  mbLoadHTML(v, "<body><div id='host'></div></body>", "about:blank");
  mbRunJS(v,
    "customElements.define('my-el',class extends HTMLElement{"
    "  connectedCallback(){this.textContent='upgraded';}});"
    "document.body.appendChild(document.createElement('my-el'));"
    "var sr=document.getElementById('host').attachShadow({mode:'open'});"
    "sr.innerHTML='<span id=s>shadow</span>';");
  mbWait(v, 40);
  Expect(Eval(v, "document.querySelector('my-el').textContent") == "upgraded" &&
             Eval(v, "document.getElementById('host').shadowRoot.querySelector('#s')"
                     ".textContent") == "shadow" &&
             Eval(v, "String(document.querySelector('#s'))") == "null",  // encapsulated
         "Web Components: custom element upgrade + shadow DOM encapsulation");

  // 37. Worker spawn must not crash the host. We have no worker-thread
  // infrastructure, so a dedicated Worker is INERT (never runs) — but a page
  // that does `new Worker(...)` must degrade gracefully, not SIGSEGV (it used
  // to: factory_client_ was null and DedicatedWorker::Start derefs it). The
  // guard: construct a Worker, pump, and confirm the host is still alive and
  // scripting after (a crash would never reach the assert). The worker itself
  // is expected to be inert, so we only assert survival + a live main frame.
  mbLoadHTML(v, "<body>worker-guard</body>", "about:blank");
  mbRunJS(v,
    "try{window.__w=new Worker('data:text/javascript,'+"
    "encodeURIComponent('onmessage=function(e){postMessage(e.data*2)}'));"
    "window.__w.postMessage(21);window.__wok=true;}"
    "catch(e){window.__wok=false;window.__werr=String(e);}");
  mbWait(v, 60);
  // Host survived the worker spawn (we got here at all) and JS still runs:
  Expect(Eval(v, "1+1") == "2" &&
             Eval(v, "document.body.textContent") == "worker-guard" &&
             (Eval(v, "String(typeof window.__w)") == "object" ||
              Eval(v, "String(window.__wok)") == "false"),
         "Worker spawn degrades gracefully (no crash; host still scriptable)");

  // 38. The rest of the Worker family must also be crash-safe. SharedWorker's
  // Connect() is a fire-and-forget mojo call our empty broker drops (inert, no
  // crash); navigator.serviceWorker.register() either rejects cleanly or, on a
  // real origin where we have no provider, is null-guarded (pending promise,
  // no crash). Neither must SIGSEGV — same hazard class as case 37. We assert
  // the host survives constructing both and stays scriptable afterward.
  // (about:blank is an opaque origin: SharedWorker may throw SecurityError and
  // navigator.serviceWorker may be absent — both fine. The invariant under test
  // is crash-safety, so we wrap each attempt in try/catch and set a sentinel at
  // the end: reaching it proves neither spawn took the host down, and the host
  // still evaluates JS afterward.)
  mbLoadHTML(v, "<body>family-guard</body>", "about:blank");
  mbRunJS(v,
    "window.__done=false;"
    "try{new SharedWorker('data:text/javascript,onconnect=function(){}');}catch(e){}"
    "try{if(navigator.serviceWorker)"
    "navigator.serviceWorker.register('data:text/javascript,').then(function(){},function(){});}"
    "catch(e){}"
    "window.__done=true;");
  mbWait(v, 60);
  Expect(Eval(v, "1+1") == "2" &&
             Eval(v, "document.body.textContent") == "family-guard" &&
             Eval(v, "String(window.__done)") == "true",
         "SharedWorker + ServiceWorker spawn are crash-safe (host scriptable)");

  // 39. IndexedDB fails GRACEFULLY (not a hang, not a crash). We bind no IDB
  // backend, so the frame broker drops network::...IDBFactory; the receiver
  // pipe closes, the remote disconnects, and Blink surfaces that to the open()
  // request as a clean `onerror` — async, deterministic, host stays live. (Use
  // a real http origin via the base URL: indexedDB is unavailable on the opaque
  // about:blank origin.) Asserts the error fired and the host is still
  // scriptable; corrects the old "open() hangs pending" note.
  mbLoadHTML(v, "<body>idb-guard</body>", "https://miniblink.test/");
  mbRunJS(v,
    "window.__idb='pending';"
    "try{var r=indexedDB.open('mb-probe',1);"
    "r.onerror=function(){window.__idb='error';};"
    "r.onsuccess=function(){window.__idb='success';};"
    "}catch(e){window.__idb='threw:'+e.name;}");
  mbWait(v, 200);
  Expect(Eval(v, "1+1") == "2" &&
             Eval(v, "document.body.textContent") == "idb-guard" &&
             Eval(v, "String(window.__idb)") == "error",
         "IndexedDB open() fails gracefully via onerror (no hang/crash)");

  // 40. WebSocket degrades gracefully. We have no network backend for the WS
  // mojo connector, so the handshake can't complete — but it must FAIL with the
  // spec's error/close events, not crash or hang the host. (A site's reconnect
  // logic then works normally.) Construct on a real origin, capture the close,
  // assert the socket reached a terminal state (CLOSING/CLOSED, readyState>=2)
  // and the host is still scriptable. Common API; clean event-based failure is
  // the strong invariant here.
  mbLoadHTML(v, "<body>ws-guard</body>", "https://miniblink.test/");
  mbRunJS(v,
    "window.__ws='pending';"
    "try{var s=new WebSocket('wss://miniblink.test/x');"
    "s.onerror=function(){window.__ws='error';};"
    "s.onclose=function(){window.__ws='closed';};"
    "}catch(e){window.__ws='threw:'+e.name;}");
  mbWait(v, 300);
  Expect(Eval(v, "1+1") == "2" &&
             Eval(v, "document.body.textContent") == "ws-guard" &&
             (Eval(v, "String(window.__ws)") == "closed" ||
              Eval(v, "String(window.__ws)") == "error"),
         "WebSocket degrades gracefully (error/close event, no hang/crash)");

  // 41. Canvas 2D full round-trip + WebGL graceful-null. Canvas is core for a
  // renderer and the backbone of chart/image libraries, so verify the complete
  // path works offline: get a 2D context, draw, read pixels back via
  // getImageData (exact color), and encode via toDataURL. Separately, WebGL has
  // no GPU backend here, so getContext('webgl') must return null (clean
  // feature-detection), not crash. Both are common; this locks in that 2D works
  // and WebGL degrades.
  mbLoadHTML(v, "<body><canvas id='c' width='20' height='20'></canvas></body>",
             "about:blank");
  mbRunJS(v,
    "var cv=document.getElementById('c'),x=cv.getContext('2d');"
    "x.fillStyle='#ff0000';x.fillRect(0,0,20,20);"
    "var d=x.getImageData(10,10,1,1).data;"
    "window.__rgba=d[0]+','+d[1]+','+d[2]+','+d[3];"
    "window.__png=cv.toDataURL().indexOf('data:image/png')===0;"
    "window.__gl=(document.createElement('canvas').getContext('webgl')===null);");
  Expect(Eval(v, "window.__rgba") == "255,0,0,255" &&
             Eval(v, "String(window.__png)") == "true" &&
             Eval(v, "String(window.__gl)") == "true",
         "Canvas 2D round-trip (draw/getImageData/toDataURL); WebGL null");

  // 42. Drawing to a canvas via mbEvalJS (not just mbRunJS) must also be
  // crash-safe. EvalToString/EvalIsolated used to run ExecuteScript
  // synchronously, so a draw inside an eval expression hit the same
  // CanvasPerformanceMonitor NOTREACHED as the old RunJS path. Both now run the
  // script inside a scheduler task. This eval draws green and reads the pixel
  // back in one expression: if the eval-draw path regressed it would SIGABRT
  // before returning; success returns the green channel (255).
  mbLoadHTML(v, "<body>eval-draw</body>", "about:blank");
  Expect(Eval(v,
              "(function(){var c=document.createElement('canvas');"
              "c.width=4;c.height=4;var x=c.getContext('2d');"
              "x.fillStyle='#00ff00';x.fillRect(0,0,4,4);"
              "return x.getImageData(1,1,1,1).data[1];})()") == "255" &&
             Eval(v, "document.body.textContent") == "eval-draw",
         "canvas draw via mbEvalJS is crash-safe (task-bracketed)");

  // 43. Same guard in the ISOLATED world (mbEvalJSIsolated shares the DOM but
  // has its own globals; it routes through the same task-bracketing fix).
  Expect(EvalIso(v,
                 "(function(){var c=document.createElement('canvas');"
                 "c.width=2;c.height=2;var x=c.getContext('2d');"
                 "x.fillStyle='#0000ff';x.fillRect(0,0,2,2);"
                 "return x.getImageData(0,0,1,1).data[2];})()") == "255",
         "canvas draw via mbEvalJSIsolated is crash-safe (task-bracketed)");

  // 44. JS dialogs must NOT hang the host. alert()/confirm()/prompt() are
  // [Sync] mojo calls to LocalFrameHost (RunModal*Dialog); with no browser
  // process to service them they deadlock the main thread forever — and pages
  // call them during load, so this is a severe common-case hazard. The
  // 0002-suppress-js-dialogs patch auto-dismisses them (headless semantics):
  // alert returns, confirm/prompt return their "Cancel" defaults (false/null).
  // This calls all three INLINE DURING LOAD (the realistic hang path); if the
  // suppression regressed, the whole smoke run would hang here (caught by the
  // bounded watchdog). Asserts completion + the documented default values.
  mbLoadHTML(v,
    "<body>dlg<script>"
    "window.__a=alert('hi');"
    "window.__c=confirm('ok?');"
    "window.__p=prompt('name?','def');"
    "window.__done=true;"
    "</script></body>", "about:blank");
  Expect(Eval(v, "String(window.__done)") == "true" &&
             Eval(v, "String(window.__a)") == "undefined" &&
             Eval(v, "String(window.__c)") == "false" &&
             Eval(v, "String(window.__p)") == "null",
         "JS dialogs auto-dismiss, no hang (alert/confirm/prompt)");

  // 45. Clipboard is crash/hang-safe. ClipboardHost has [Sync] read methods
  // (ReadText/IsFormatAvailable/...) — the same deadlock class as the JS
  // dialogs — but Blink gates them behind permission/gesture, so page JS never
  // reaches the sync call without a backend: execCommand('copy'/'paste') return
  // false, and navigator.clipboard read/write reject (NotAllowedError). Verify
  // none of it hangs and the host stays scriptable. (A regression that made a
  // clipboard op block would hang the suite, caught by the watchdog.)
  mbLoadHTML(v, "<body><textarea id='t'>x</textarea></body>", "about:blank");
  mbRunJS(v,
    "window.__done=false;var t=document.getElementById('t');t.select();"
    "try{window.__copy=document.execCommand('copy');}catch(e){window.__copy='threw';}"
    "try{window.__paste=document.execCommand('paste');}catch(e){window.__paste='threw';}"
    "window.__clip=(typeof navigator.clipboard);"
    "try{if(navigator.clipboard&&navigator.clipboard.writeText)"
    "navigator.clipboard.writeText('x').then(function(){},function(){});}catch(e){}"
    "window.__done=true;");
  mbWait(v, 60);
  // The invariant is hang-safety: __done==true proves the whole script — copy,
  // paste, and the clipboard-API call — ran to completion without blocking, and
  // the host still evaluates JS. (Specific return values and clipboard
  // availability vary by origin/secure-context, so we don't pin them; we only
  // require execCommand returned a real boolean rather than hanging/throwing.)
  Expect(Eval(v, "1+1") == "2" &&
             Eval(v, "String(window.__done)") == "true" &&
             (Eval(v, "String(window.__copy)") == "false" ||
              Eval(v, "String(window.__copy)") == "true"),
         "clipboard ops degrade gracefully, no hang (copy/paste/clipboard API)");

  // 46. URL.createObjectURL must not hang. It is the last [Sync]-mojo hazard:
  // PublicURLManager::RegisterURL makes the [Sync] BlobURLStore.Register call on
  // a store bound through the frame's navigation-associated channel, which no
  // browser process services — so any page calling createObjectURL() used to
  // deadlock the host forever (confirmed: mb_shot exit 137). The
  // 0003-skip-blob-url-register patch skips that registration: createObjectURL
  // returns a blob: URL without blocking (the URL won't resolve to data, but the
  // host survives). Blob data ops (size/text/arrayBuffer/FileReader) were always
  // fine. This calls createObjectURL DURING LOAD (the realistic hang path) and
  // also revokes; a regression would hang the whole suite (watchdog catches it).
  mbLoadHTML(v,
    "<body>blob<script>"
    "var b=new Blob(['hello'],{type:'text/plain'});"
    "window.__sz=b.size;"
    "window.__u=URL.createObjectURL(b);"           // used to hang here
    "window.__isblob=(window.__u.indexOf('blob:')===0);"
    "try{URL.revokeObjectURL(window.__u);window.__rev=true;}catch(e){window.__rev=false;}"
    "window.__done=true;"
    "</script></body>", "about:blank");
  Expect(Eval(v, "String(window.__done)") == "true" &&
             Eval(v, "String(window.__sz)") == "5" &&
             Eval(v, "String(window.__isblob)") == "true" &&
             Eval(v, "String(window.__rev)") == "true",
         "URL.createObjectURL/revokeObjectURL no longer hang (blob: URL returned)");

  // 47. Web Crypto works. crypto.subtle.* used to SIGSEGV: SubtleCrypto derefs
  // Platform::Current()->Crypto() unconditionally, and base Platform returns
  // null — so any crypto.subtle call crashed the host. MbPlatform now returns a
  // real BoringSSL-backed webcrypto::WebCryptoImpl. Verify the async digest
  // actually computes (SHA-256("abc") has a known value) and getRandomValues
  // fills bytes. (Secure origin: Web Crypto requires a secure context.)
  mbLoadHTML(v, "<body>crypto</body>", "https://miniblink.test/");
  mbRunJS(v,
    "window.__d='pending';var r=new Uint8Array(16);crypto.getRandomValues(r);"
    "window.__rnd=r.some(function(x){return x!==0;});"
    "crypto.subtle.digest('SHA-256',new TextEncoder().encode('abc')).then(function(h){"
    "var b=new Uint8Array(h),s='';for(var i=0;i<4;i++)s+=('0'+b[i].toString(16)).slice(-2);"
    "window.__d=s+':'+b.length;},function(e){window.__d='REJ:'+e.name;});");
  mbWait(v, 250);
  Expect(Eval(v, "String(window.__rnd)") == "true" &&
             Eval(v, "window.__d") == "ba7816bf:32",  // SHA-256("abc") prefix + len
         "Web Crypto: getRandomValues + subtle.digest(SHA-256) compute");

  // 48. Web Audio must not crash. `new AudioContext()` used to SIGSEGV: base
  // Platform::CreateAudioDevice returns null and AudioDestination's ctor derefs
  // it unguarded. MbPlatform now returns a silent stub device, so a realtime
  // AudioContext constructs and a graph can be wired (no sound, but no crash).
  // Also exercise OfflineAudioContext (always worked — renders to a buffer).
  // A regression would crash the suite before the assert.
  mbLoadHTML(v, "<body>audio</body>", "about:blank");
  mbRunJS(v,
    "window.__ok=false;"
    "var ac=new AudioContext();var o=ac.createOscillator();var g=ac.createGain();"
    "o.connect(g);g.connect(ac.destination);o.start();"
    "window.__sr=ac.sampleRate;window.__st=ac.state;"
    "var oc=new OfflineAudioContext(1,128,44100);window.__osr=oc.sampleRate;"
    "window.__ok=true;");
  mbWait(v, 40);
  Expect(Eval(v, "String(window.__ok)") == "true" &&
             Eval(v, "String(window.__sr)") == "48000" &&
             Eval(v, "String(window.__osr)") == "44100" &&
             Eval(v, "1+1") == "2",
         "Web Audio: AudioContext + OfflineAudioContext construct, no crash");

  // 49. Streams API actually moves data (not just constructs). A ReadableStream
  // whose source enqueues a chunk must deliver it through a reader — exercises
  // the async stream plumbing end to end. (ReadableStream/Transform/Compression/
  // TextDecoder streams + MessageChannel all construct without crashing too.)
  mbLoadHTML(v, "<body>streams</body>", "about:blank");
  mbRunJS(v,
    "window.__s='pending';"
    "var rs=new ReadableStream({start:function(c){c.enqueue('hello');c.close();}});"
    "rs.getReader().read().then(function(r){window.__s=r.value+':'+r.done;});");
  mbWait(v, 60);
  Expect(Eval(v, "window.__s") == "hello:false",
         "Streams: ReadableStream delivers an enqueued chunk via a reader");

  // 50. Native form controls paint (the WebThemeEngine path). Painting a
  // checkbox/button/range/progress is a distinct subsystem from text/box paint;
  // if the theme engine were missing it would crash or paint nothing. Render on
  // white and assert some non-white pixels exist (a control drew).
  mbLoadHTML(v,
    "<body style='margin:0;background:#fff'>"
    "<input type='checkbox' checked><button>OK</button>"
    "<input type='range'><progress value='0.6'></progress></body>",
    "about:blank");
  std::vector<uint8_t> fpx(static_cast<size_t>(W) * H * 4, 255);  // white
  mbPaintToBitmap(v, fpx.data(), W, H, W * 4);
  bool drew = false;
  for (size_t i = 0; i + 2 < fpx.size(); i += 4) {
    if (fpx[i] < 240 || fpx[i + 1] < 240 || fpx[i + 2] < 240) { drew = true; break; }
  }
  Expect(drew, "form controls paint via WebThemeEngine (non-blank output)");

  // 51. Paint correctness: a renderer must produce the RIGHT pixels, not just
  // non-blank ones. Verify exact layout (getBoundingClientRect) AND exact paint:
  //  (a) flexbox space-between positions children at 0 and 300 in a 400px row;
  //  (b) a solid #00ff00 fill rasterizes to pure green;
  //  (c) rgba(0,0,255,0.5) over white composites to ~(128,128,255).
  // px is BGRA: [0]=B [1]=G [2]=R. Sample interior points to avoid AA edges.
  {
    Expect(Eval(v,
      "(function(){document.body.style.margin='0';"
      "document.body.innerHTML="
      "\"<div style='display:flex;justify-content:space-between;width:400px'>\"+"
      "\"<i id=a style=\\\"width:100px;height:20px;display:block\\\"></i>\"+"
      "\"<i id=b style=\\\"width:100px;height:20px;display:block\\\"></i></div>\";"
      "var a=document.getElementById('a').getBoundingClientRect(),"
      "b=document.getElementById('b').getBoundingClientRect();"
      "return a.x+','+b.x;})()") == "0,300",
      "layout: flexbox space-between (children at 0 and 300)");

    mbLoadHTML(v,
      "<body style='margin:0;background:#fff'>"
      "<div style='width:80px;height:80px;background:#00ff00'></div></body>",
      "about:blank");
    std::vector<uint8_t> g(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, g.data(), W, H, W * 4);
    size_t i = (40u * W + 40u) * 4;  // inside the green box
    Expect(g[i + 2] == 0 && g[i + 1] == 255 && g[i] == 0,
           "paint: solid #00ff00 rasterizes to pure green");

    mbLoadHTML(v,
      "<body style='margin:0;background:#fff'>"
      "<div style='width:80px;height:80px;background:rgba(0,0,255,0.5)'></div></body>",
      "about:blank");
    std::vector<uint8_t> a2(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, a2.data(), W, H, W * 4);
    size_t j = (40u * W + 40u) * 4;
    int R = a2[j + 2], G = a2[j + 1], B = a2[j];
    auto near = [](int x, int t) { return x >= t - 4 && x <= t + 4; };
    Expect(near(R, 128) && near(G, 128) && B == 255,
           "paint: rgba(0,0,255,.5) over white composites to ~(128,128,255)");
  }

  // 52. Stacking + gradient paint. z-index/DOM order: a blue box painted over a
  // red box wins at the overlap. And a horizontal red->blue linear-gradient is
  // red-ish at the left edge and blue-ish at the right.
  {
    mbLoadHTML(v,
      "<body style='margin:0'>"
      "<div style='position:absolute;left:0;top:0;width:60px;height:60px;background:#ff0000'></div>"
      "<div style='position:absolute;left:0;top:0;width:60px;height:60px;background:#0000ff'></div>"
      "</body>", "about:blank");
    std::vector<uint8_t> s(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, s.data(), W, H, W * 4);
    size_t k = (30u * W + 30u) * 4;
    Expect(s[k] == 255 && s[k + 2] == 0,  // B=255, R=0 -> blue on top
           "paint: later box stacks over earlier (blue over red)");

    mbLoadHTML(v,
      "<body style='margin:0'>"
      "<div style='width:200px;height:40px;"
      "background:linear-gradient(to right,#ff0000,#0000ff)'></div></body>",
      "about:blank");
    std::vector<uint8_t> gr(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, gr.data(), W, H, W * 4);
    size_t L = (20u * W + 6u) * 4, Rt = (20u * W + 193u) * 4;
    Expect(gr[L + 2] > 200 && gr[L] < 60 &&        // left: red-ish
               gr[Rt] > 200 && gr[Rt + 2] < 60,    // right: blue-ish
           "paint: horizontal linear-gradient is red->blue across width");
  }

  // 53. CSS filter: grayscale(1) on a red box must desaturate it to gray
  // (R==G==B), exercising the filter paint pipeline (SkImageFilter). A
  // non-modern/!filter engine would leave it red.
  {
    mbLoadHTML(v,
      "<body style='margin:0;background:#fff'>"
      "<div style='width:80px;height:80px;background:#ff0000;filter:grayscale(1)'></div>"
      "</body>", "about:blank");
    std::vector<uint8_t> f(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, f.data(), W, H, W * 4);
    size_t i = (40u * W + 40u) * 4;
    int R = f[i + 2], G = f[i + 1], B = f[i];
    Expect(std::abs(R - G) <= 4 && std::abs(G - B) <= 4 && R > 20 && R < 200,
           "paint: filter:grayscale(1) desaturates red to gray (R==G==B)");
  }

  // 54. border-radius clipping: a 50% radius makes a circle; the corner is
  // clipped to the page background while the center is the box color. Proves
  // rounded-corner clipping actually rasterizes (not just a layout attribute).
  {
    mbLoadHTML(v,
      "<body style='margin:0;background:#ffffff'>"
      "<div style='width:80px;height:80px;border-radius:50%;background:#0000ff'></div>"
      "</body>", "about:blank");
    std::vector<uint8_t> c(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, c.data(), W, H, W * 4);
    size_t ctr = (40u * W + 40u) * 4, corner = (3u * W + 3u) * 4;
    Expect(c[ctr] == 255 && c[ctr + 2] == 0 &&          // center: blue
               c[corner] == 255 && c[corner + 1] == 255 && c[corner + 2] == 255,  // corner: white
           "paint: border-radius:50% clips corners to background (circle)");
  }

  // 55. box-shadow paints outside the border box. A solid 10px spread (no blur,
  // no offset) draws a black ring around a white box; a pixel in the ring is
  // black, a pixel inside the box is white.
  {
    mbLoadHTML(v,
      "<body style='margin:0;background:#fff'>"
      "<div style='margin:40px;width:60px;height:60px;background:#fff;"
      "box-shadow:0 0 0 10px #000'></div></body>", "about:blank");
    std::vector<uint8_t> sh(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, sh.data(), W, H, W * 4);
    size_t ring = (70u * W + 35u) * 4;   // x=35 in [30,40): inside the 10px ring
    size_t box = (70u * W + 70u) * 4;    // inside the white box
    Expect(sh[ring] < 40 && sh[ring + 1] < 40 && sh[ring + 2] < 40 &&  // ring: black
               sh[box] == 255 && sh[box + 1] == 255 && sh[box + 2] == 255,  // box: white
           "paint: box-shadow spread draws a ring outside the border box");
  }

  // 56. Text actually RASTERIZES to glyphs (fonts were a documented gap). Render
  // black text on white and scan the text band: correct glyph rendering yields
  // both dark pixels (the strokes) AND white pixels (gaps between/within glyphs).
  // All-white => no glyphs (tofu/blank/missing fonts); all-dark => a solid block,
  // not text. Requires real font data + shaping + rasterization.
  {
    mbLoadHTML(v,
      "<body style='margin:0;background:#fff'>"
      "<div style='font:30px monospace;color:#000;line-height:40px'>WWWWWWWW</div>"
      "</body>", "about:blank");
    std::vector<uint8_t> t(static_cast<size_t>(W) * H * 4, 255);
    mbPaintToBitmap(v, t.data(), W, H, W * 4);
    int dark = 0, light = 0;
    for (int y = 0; y < 40; ++y)
      for (int x = 0; x < 220; ++x) {
        int r = t[(static_cast<size_t>(y) * W + x) * 4 + 2];
        if (r < 60) ++dark; else if (r > 200) ++light;
      }
    Expect(dark > 50 && light > 50,
           "text: glyphs rasterize (dark strokes + white gaps present)");
  }

  // 57. Font metrics scale: canvas measureText must report a real, font-size-
  // proportional advance width (text shaping with metrics, not a stub). 40px
  // text is ~2x the width of the same string at 20px.
  Expect(Eval(v,
      "(function(){var x=document.createElement('canvas').getContext('2d');"
      "x.font='40px monospace';var w40=x.measureText('MMMM').width;"
      "x.font='20px monospace';var w20=x.measureText('MMMM').width;"
      "return (w20>0 && w40>w20*1.5 && w40<w20*2.5)?'ok':(w40+'/'+w20);})()") == "ok",
      "text: canvas measureText advance scales with font size");

  // 58. SVG renders (shapes + fills). Inline SVG is ubiquitous (icons, charts,
  // logos) and is a distinct rendering path from CSS boxes. Draw a green rect
  // and a red circle on white and sample: inside the rect -> green, inside the
  // circle -> red, an empty corner -> white. Proves SVG geometry + fill paint.
  {
    mbLoadHTML(v,
      "<body style='margin:0;background:#fff'>"
      "<svg width='100' height='100' xmlns='http://www.w3.org/2000/svg'>"
      "<rect x='0' y='0' width='50' height='50' fill='#00ff00'/>"
      "<circle cx='75' cy='25' r='18' fill='#ff0000'/></svg></body>",
      "about:blank");
    std::vector<uint8_t> s(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, s.data(), W, H, W * 4);
    size_t rect = (25u * W + 25u) * 4;     // inside green rect
    size_t circ = (25u * W + 75u) * 4;     // inside red circle
    size_t gap = (90u * W + 90u) * 4;      // empty -> white
    Expect(s[rect + 1] == 255 && s[rect + 2] == 0 &&            // green
               s[circ + 2] == 255 && s[circ] == 0 &&           // red
               s[gap] == 255 && s[gap + 1] == 255 && s[gap + 2] == 255,  // white
           "SVG: rect + circle render with correct fills");
  }

  // 59. CSS transition animates over time: a property must interpolate when the
  // clock advances (not jump or stay). Start width 0, transition to 100px; after
  // driving the engine it should be partway/complete, i.e. > 0.
  {
    mbLoadHTML(v,
      "<body><div id='an' style='width:0px;height:10px;background:#000;"
      "transition:width 0.1s linear'></div></body>", "about:blank");
    mbRunJS(v, "var e=document.getElementById('an');"
               "getComputedStyle(e).width;"          // flush start value
               "e.style.width='100px';");            // trigger transition
    mbWait(v, 200);                                  // drive past the 100ms duration
    Expect(Eval(v, "parseInt(getComputedStyle(document.getElementById('an')).width)") == "100",
           "CSS transition: width animates to its target when the clock advances");
  }

  // 60. Holistic integration: a realistic composed page exercising several
  // subsystems together (flex header + CSS grid body + inline SVG + text), to
  // catch cross-subsystem bugs that isolated unit checks miss. Assert exact grid
  // layout (geometry) AND that both the SVG icon and the text actually painted
  // (pixels) in one render.
  {
    mbLoadHTML(v,
      "<body style='margin:0;background:#fff;font:16px monospace;color:#000'>"
      "<header style='display:flex;align-items:center;height:40px'>"
      "<svg width='30' height='30' xmlns='http://www.w3.org/2000/svg'>"
      "<rect width='30' height='30' fill='#008000'/></svg><b>Title</b></header>"
      "<main style='display:grid;grid-template-columns:repeat(2,150px)'>"
      "<section id='c0'>Left</section><section id='c1'>Right</section></main>"
      "</body>", "about:blank");
    // Geometry: grid columns at x=0 and x=150.
    bool grid_ok = Eval(v,
      "(function(){var a=document.getElementById('c0').getBoundingClientRect(),"
      "b=document.getElementById('c1').getBoundingClientRect();"
      "return a.x+','+b.x;})()") == "0,150";
    // Paint: SVG icon green at ~(15,15); some dark text pixels in the header band.
    std::vector<uint8_t> p(static_cast<size_t>(W) * H * 4, 255);
    mbPaintToBitmap(v, p.data(), W, H, W * 4);
    size_t icon = (15u * W + 15u) * 4;
    bool icon_ok = p[icon + 1] >= 100 && p[icon + 2] < 80 && p[icon] < 80;  // greenish
    int darktext = 0;
    for (int y = 0; y < 40; ++y)
      for (int x = 40; x < 260; ++x)
        if (p[(static_cast<size_t>(y) * W + x) * 4 + 2] < 80) ++darktext;
    Expect(grid_ok && icon_ok && darktext > 20,
           "integration: flex+grid layout + SVG icon + text compose in one page");
  }

  // 61. mbClickSelector clicks an element by CSS selector (resolves its box,
  // clicks the center) — the Puppeteer-style page.click primitive. Place a
  // button, click it by selector, and confirm its handler ran. Also confirm a
  // non-matching selector returns 0 (failure) without clicking.
  {
    mbLoadHTML(v,
      "<body style='margin:0'><button id='go' "
      "style='position:absolute;left:30px;top:30px;width:90px;height:30px' "
      "onclick='window.__c=(window.__c||0)+1'>Go</button></body>", "about:blank");
    std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // force layout for hit-testing
    int hit = mbClickSelector(v, "#go");
    int miss = mbClickSelector(v, "#nope");
    Expect(hit == 1 && miss == 0 && Eval(v, "String(window.__c)") == "1",
           "mbClickSelector: clicks element by selector; 0 when no match");
  }

  // 62. mbFillSelector fills a field by selector and fires events (Playwright
  // fill). Fill an input, then confirm its .value updated AND an 'input' event
  // was observed (frameworks rely on the event, not just the value). Also a
  // non-matching selector returns 0.
  {
    mbLoadHTML(v,
      "<body><input id='name' value=''>"
      "<script>window.__ev=0;document.getElementById('name')"
      ".addEventListener('input',function(){window.__ev++;});</script></body>",
      "about:blank");
    int ok = mbFillSelector(v, "#name", "Ada Lovelace");
    int miss = mbFillSelector(v, "#missing", "x");
    Expect(ok == 1 && miss == 0 &&
               Eval(v, "document.getElementById('name').value") == "Ada Lovelace" &&
               Eval(v, "String(window.__ev>0)") == "true",
           "mbFillSelector: sets value + fires input event; 0 when no match");
  }

  // 62b. mbDispatchEvent fires arbitrary DOM events that click/fill don't — a
  // mouseover handler and a custom-event handler both run; no-match -> 0.
  {
    mbLoadHTML(v,
      "<body><div id='d'>x</div><script>window.__o=0;window.__c=0;"
      "document.getElementById('d').addEventListener('mouseover',function(){window.__o++;});"
      "document.getElementById('d').addEventListener('ping',function(){window.__c++;});"
      "</script></body>", "about:blank");
    const bool over = mbDispatchEvent(v, "#d", "mouseover") == 1 &&
                      Eval(v, "String(window.__o)") == "1";
    const bool custom = mbDispatchEvent(v, "#d", "ping") == 1 &&
                        Eval(v, "String(window.__c)") == "1";
    const bool none_ok = mbDispatchEvent(v, "#none", "click") == 0;
    Expect(over && custom && none_ok,
           "mbDispatchEvent fires mouseover + custom events; 0 on no match",
           std::string("over=") + (over ? "1" : "0") + " custom=" +
               (custom ? "1" : "0") + " none=" + (none_ok ? "1" : "0"));
  }

  // 63. mbWaitForFunction polls a JS predicate until truthy (general wait). A
  // setTimeout sets a flag after 50ms; waitForFunction must return 1 once it
  // flips. A condition that never holds must time out -> 0 (and not hang past
  // the timeout). Generalizes waitForSelector.
  {
    mbLoadHTML(v, "<body>wf<script>setTimeout(function(){window.__ready=1;},50);"
                  "</script></body>", "about:blank");
    int got = mbWaitForFunction(v, "window.__ready===1", 2000);
    int timedout = mbWaitForFunction(v, "window.__never", 60);
    Expect(got == 1 && timedout == 0 &&
               Eval(v, "String(window.__ready)") == "1",
           "mbWaitForFunction: resolves when predicate turns truthy; times out otherwise");
  }

  // 64. Blob DATA now resolves (in-process BlobRegistry/Blob on the service
  // thread). Previously blob reads stayed pending forever; now blob.text(),
  // arrayBuffer() and FileReader deliver the bytes. This exercises the [Sync]
  // BlobRegistry.Register serviced off the main thread + Blob.ReadAll over a
  // data pipe — the payoff of the service-host work.
  {
    mbLoadHTML(v, "<body>blob-data</body>", "about:blank");
    mbRunJS(v,
      "window.__t='pending';window.__ab=-1;window.__fr='pending';"
      "var b=new Blob(['hello'],{type:'text/plain'});"
      "b.text().then(function(s){window.__t=s;});"
      "b.arrayBuffer().then(function(a){window.__ab=a.byteLength;});"
      "var fr=new FileReader();fr.onload=function(){window.__fr=fr.result;};"
      "fr.readAsText(new Blob(['world']));");
    mbWait(v, 300);  // drive the async reads to completion
    Expect(Eval(v, "window.__t") == "hello" &&
               Eval(v, "String(window.__ab)") == "5" &&
               Eval(v, "window.__fr") == "world",
           "Blob data resolves: text()/arrayBuffer()/FileReader deliver bytes");
  }

  // 65. Blob data works at realistic sizes, not just a few bytes. A 100 KB blob
  // (still inline embedded_data, <=256 KB) must round-trip fully through
  // ReadAll's data-pipe write. Guards that the shipped path handles real content
  // (canvas exports, fetched bodies), not only tiny strings. (>256 KB needs the
  // BytesProvider path — a documented follow-up.)
  {
    mbLoadHTML(v, "<body>blob100k</body>", "about:blank");
    mbRunJS(v,
      "window.__n=-1;"
      "var b=new Blob(['z'.repeat(100000)]);"
      "b.text().then(function(s){window.__n=s.length;});");
    mbWait(v, 300);
    Expect(Eval(v, "String(window.__n)") == "100000",
           "Blob data round-trips at 100 KB (realistic size, not just bytes)");
  }

  // 66. Large blobs (>256 KB) resolve too: no inline embedded_data, so the bytes
  // come via the BytesProvider (fetched after Register replies), and the read
  // streams over the pipe in chunks (SimpleWatcher). Verifies the full data path
  // for any size. 500 KB exceeds both the 256 KB inline cap and the pipe buffer.
  {
    mbLoadHTML(v, "<body>bigblob</body>", "about:blank");
    mbRunJS(v,
      "window.__big=-1;window.__bab=-1;"
      "var b=new Blob(['q'.repeat(500000)]);"
      "b.text().then(function(s){window.__big=s.length;});"
      "b.arrayBuffer().then(function(a){window.__bab=a.byteLength;});");
    mbWait(v, 500);  // BytesProvider fetch + chunked write
    Expect(Eval(v, "String(window.__big)") == "500000" &&
               Eval(v, "String(window.__bab)") == "500000",
           "Blob >256KB resolves via BytesProvider + chunked pipe write");
  }

  // 67. canvas.toBlob() works end to end (canvas -> Skia encode -> blob -> read)
  // — headless image export. toBlob encodes on an IDLE task; idle tasks now run
  // in WaitMs (StartIdlePeriodForTesting), so the callback fires promptly instead
  // of via the ~1s fallback. Read the blob back and check the PNG signature.
  {
    mbLoadHTML(v, "<body><canvas id='c' width='16' height='16'></canvas></body>",
               "about:blank");
    mbRunJS(v,
      "window.__png='pending';"
      "var cv=document.getElementById('c'),x=cv.getContext('2d');"
      "x.fillStyle='#ff8800';x.fillRect(0,0,16,16);"
      "cv.toBlob(function(b){if(!b){window.__png='null';return;}"
      "b.arrayBuffer().then(function(ab){var u=new Uint8Array(ab);"
      "window.__png=(u[0]===0x89&&u[1]===0x50&&u[2]===0x4E&&u[3]===0x47&&u.length>0)"
      "?'PNG':'bad';});}, 'image/png');");
    mbWait(v, 400);  // idle encode + blob read (prompt now that idle tasks run)
    Expect(Eval(v, "window.__png") == "PNG",
           "canvas.toBlob() -> readable PNG blob (idle-task encode runs)");
  }

  // 68. Blobs that REFERENCE other blobs resolve (is_blob DataElements). A
  // blob composed of another blob registers an is_blob element holding a Blob
  // remote + offset/length; reading it must read through to the referenced
  // blob. Covers Response.blob() (wraps the body blob) and Blob.slice() (an
  // offset/length view). Previously these read empty (is_blob was ignored).
  {
    mbLoadHTML(v, "<body>blobref</body>", "about:blank");
    mbRunJS(v,
      "window.__resp='pending';window.__slice='pending';"
      "new Response('hello-resp').blob().then(function(b){return b.text();})"
      ".then(function(t){window.__resp=t;});"
      "new Blob(['0123456789']).slice(2,5).text().then(function(t){window.__slice=t;});");
    mbWait(v, 300);
    Expect(Eval(v, "window.__resp") == "hello-resp" &&
               Eval(v, "window.__slice") == "234",
           "Blob-of-blob resolves: Response.blob() + Blob.slice() read through");
  }

  // 69. fetch('data:...') resolves (loader decodes data: via net::DataURL::Parse)
  // for .text(), .arrayBuffer() AND .blob(). A fetched response's .blob() streams
  // the body through BlobRegistry.RegisterFromStream (fetch_data_loader.cc), which
  // we now service by draining the body into a blob.
  {
    mbLoadHTML(v, "<body>fetchdata</body>", "about:blank");
    mbRunJS(v,
      "window.__ft='pending';window.__fa=-1;window.__fb='pending';"
      "fetch('data:text/plain,hello-data').then(function(r){return r.text();})"
      ".then(function(t){window.__ft=t;},function(e){window.__ft='rej';});"
      "fetch('data:application/octet-stream,abcd').then(function(r){return r.arrayBuffer();})"
      ".then(function(a){window.__fa=a.byteLength;},function(e){window.__fa='rej';});"
      "fetch('data:text/plain,blob-data').then(function(r){return r.blob();})"
      ".then(function(b){return b.text();}).then(function(t){window.__fb=t;},"
      "function(e){window.__fb='rej';});");
    mbWait(v, 400);
    Expect(Eval(v, "window.__ft") == "hello-data" &&
               Eval(v, "String(window.__fa)") == "4" &&
               Eval(v, "window.__fb") == "blob-data",
           "fetch('data:...') resolves: .text(), .arrayBuffer(), .blob()");
  }

  // 70. CSS generated content (::before) renders. Ubiquitous (icons, badges,
  // quotes, numbering). Assert both the computed-style content and that the
  // generated box actually paints (a green ::before block at top-left).
  {
    mbLoadHTML(v,
      "<style>#g::before{content:'';display:block;width:50px;height:50px;"
      "background:#00ff00}</style><body style='margin:0;background:#fff'>"
      "<div id='g'>x</div></body>", "about:blank");
    std::vector<uint8_t> bpx(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, bpx.data(), W, H, W * 4);
    size_t bi = (10u * W + 10u) * 4;  // inside the ::before box
    bool painted = bpx[bi + 1] == 255 && bpx[bi + 2] == 0 && bpx[bi] == 0;  // green
    bool computed =
        Eval(v, "getComputedStyle(document.getElementById('g'),'::before')"
                ".backgroundColor") == "rgb(0, 255, 0)";
    Expect(painted && computed,
           "CSS ::before generated content paints + computed style reads it");
  }

  // 71. clip-path polygon clips paint to an arbitrary shape (distinct from
  // border-radius). A top-left triangle: inside it paints, the clipped corner
  // shows the page background.
  {
    mbLoadHTML(v,
      "<body style='margin:0;background:#ffffff'>"
      "<div style='width:80px;height:80px;background:#0000ff;"
      "clip-path:polygon(0 0,100% 0,0 100%)'></div></body>", "about:blank");
    std::vector<uint8_t> clp(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, clp.data(), W, H, W * 4);
    size_t tin = (8u * W + 8u) * 4;        // inside the triangle -> blue
    size_t tout = (70u * W + 70u) * 4;     // clipped corner -> white
    Expect(clp[tin] == 255 && clp[tin + 2] == 0 &&                         // blue
               clp[tout] == 255 && clp[tout + 1] == 255 && clp[tout + 2] == 255,
           "clip-path: polygon() clips paint to the shape");
  }

  // 72. CSS custom properties (var()), calc(), and clamp()/min()/max() — the
  // building blocks of modern stylesheets and design systems. All resolve via
  // computed style. (Custom properties also cascade + inherit; checked via a
  // child reading a property set on :root.)
  mbLoadHTML(v,
    "<style>:root{--accent:rgb(10,20,30);--gap:8px}"
    "#a{color:var(--accent)} "
    "#b{width:calc(100px + 50px)} "
    "#c{width:clamp(10px,40px,100px)} "
    "#d{margin-left:calc(var(--gap) * 2)}</style>"
    "<body><i id='a'>x</i><i id='b' style='display:block'>x</i>"
    "<i id='c' style='display:block'>x</i><i id='d' style='display:block'>x</i></body>",
    "about:blank");
  Expect(Eval(v, "getComputedStyle(document.getElementById('a')).color") == "rgb(10, 20, 30)" &&
             Eval(v, "getComputedStyle(document.getElementById('b')).width") == "150px" &&
             Eval(v, "getComputedStyle(document.getElementById('c')).width") == "40px" &&
             Eval(v, "getComputedStyle(document.getElementById('d')).marginLeft") == "16px",
         "CSS var()/calc()/clamp() resolve (custom properties + math)");

  // 73. Multiple concurrent views are independent. A real embedder (tabs)
  // creates several mbViews over one shared runtime; verify a second view has
  // its own DOM/JS world and does not disturb the first. (If the host assumed a
  // single view, this would crash or cross-contaminate.)
  {
    mbView* v2 = mbCreateView(W, H);
    Expect(v2 != nullptr, "multi-view: second mbCreateView succeeds");
    if (v2) {
      mbLoadHTML(v2, "<body><b id='x'>view2</b></body>", "about:blank");
      mbLoadHTML(v, "<body><b id='x'>view1</b></body>", "about:blank");
      mbRunJS(v2, "window.__who='two';");
      mbRunJS(v, "window.__who='one';");
      Expect(Eval(v2, "document.getElementById('x').textContent") == "view2" &&
                 Eval(v, "document.getElementById('x').textContent") == "view1" &&
                 Eval(v2, "window.__who") == "two" &&
                 Eval(v, "window.__who") == "one",
             "multi-view: two views keep independent DOM + JS globals");
      mbDestroyView(v2);
      // First view still usable after the second is destroyed.
      Expect(Eval(v, "1+1") == "2",
             "multi-view: first view survives second view's destruction");
    }
  }

  // 74. Stability across many sequential loads (a long-running scraper does
  // thousands). Load varied documents repeatedly on one view, evaluating and
  // painting each, and confirm every load renders correctly — no state leak or
  // accumulated breakage degrading later loads. A crash/leak would surface here
  // (and in the no-survivors check).
  {
    bool all_ok = true;
    std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
    for (int n = 0; n < 25 && all_ok; ++n) {
      char html[256];
      std::snprintf(html, sizeof(html),
                    "<body style='margin:0'><div id='n' "
                    "style='width:%dpx;height:10px;background:#%06x'>%d</div>"
                    "<script>window.__k=%d*2;</script></body>",
                    10 + n, (n * 9973) & 0xffffff, n, n);
      mbLoadHTML(v, html, "about:blank");
      if (Eval(v, "document.getElementById('n').textContent") != std::to_string(n) ||
          Eval(v, "String(window.__k)") != std::to_string(n * 2)) {
        all_ok = false;
      }
      if (n % 8 == 0)
        mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // exercise paint too
    }
    Expect(all_ok &&
               Eval(v, "document.getElementById('n').textContent") == "24",
           "stability: 25 sequential loads each render + script correctly");
  }

  // 75. file:// URLs with percent-encoded chars (spaces) decode correctly. The
  // loader used to pass url.path() (still %20-encoded) to the filesystem, so any
  // path with a space failed (common on macOS: "Application Support", fonts with
  // spaces — this broke @font-face). Now it goes through net::FileURLToFilePath.
  // Portable check (no system-font dependency): write a stylesheet whose name has
  // a space, link it via file:///...%20..., and confirm the style applies.
  {
    const char* css_path = "/tmp/mb url space.css";
    if (FILE* f = std::fopen(css_path, "wb")) {
      std::fputs("#z{color:rgb(7,8,9)}", f);
      std::fclose(f);
    }
    // file:// base so the file:// stylesheet is same-origin (isolates the decode
    // fix from opaque-origin subresource policy).
    mbLoadHTML(v,
      "<head><link rel='stylesheet' href='file:///tmp/mb%20url%20space.css'>"
      "</head><body><b id='z'>x</b></body>", "file:///tmp/mb_page.html");
    mbWaitForSelector(v, "#z", 1000);
    mbWait(v, 80);  // let the stylesheet fetch + apply
    Expect(Eval(v, "getComputedStyle(document.getElementById('z')).color") ==
               "rgb(7, 8, 9)",
           "file:// path with spaces decodes + loads (net::FileURLToFilePath)");
    std::remove(css_path);
  }

  // 75a2. mbScrollToBottom drives lazy loading: a scroll handler appends a tall
  // block on each scroll (up to 3), so the page only finishes growing if something
  // actually scrolls it. A small viewport (< one block) guarantees each scrollTo
  // moves. Auto-scrolling to the bottom must trigger all 3 (1 initial + 3 = 4).
  {
    mbResize(v, W, 300);  // viewport shorter than a 600px block -> scrolling moves
    mbLoadHTML(v,
        "<body style='margin:0'>"
        "<div class='blk' style='height:600px'>0</div>"
        "<script>window.__n=0;"
        "window.addEventListener('scroll',function(){"
        "if(window.__n<3){window.__n++;"
        "var d=document.createElement('div');d.className='blk';"
        "d.style.height='600px';d.textContent=window.__n;"
        "document.body.appendChild(d);}});</script></body>",
        "about:blank");
    const int grew = mbScrollToBottom(v, 10);
    const bool all_loaded = Eval(v, "String(document.querySelectorAll('.blk').length)")
                                == "4" &&
                            Eval(v, "String(window.__n)") == "3";
    Expect(grew > 0 && all_loaded,
           "mbScrollToBottom triggers lazy scroll-loaded content (4 blocks)",
           std::string("grew=") + std::to_string(grew) + " blocks=" +
               Eval(v, "String(document.querySelectorAll('.blk').length)"));
    mbResize(v, W, H);  // restore the shared viewport for later cases
  }

  // 75b. Request log: the loader records every subresource it fetches. Clear it,
  // load a page that links a file:// stylesheet, and confirm the log captured the
  // stylesheet URL; then clear and confirm it empties. (Offline — file:// flows
  // through the same loader chokepoint as network subresources.)
  {
    const char* rl_css = "/tmp/mb_reqlog.css";
    if (FILE* f = std::fopen(rl_css, "wb")) {
      std::fputs("#q{color:rgb(3,2,1)}", f);
      std::fclose(f);
    }
    mbClearRequestLog();
    mbLoadHTML(v,
        "<head><link rel='stylesheet' href='file:///tmp/mb_reqlog.css'></head>"
        "<body><b id='q'>x</b></body>", "file:///tmp/mb_rl_page.html");
    // Wait until the stylesheet applies — proves its fetch reached the loader.
    mbWaitForFunction(
        v, "getComputedStyle(document.getElementById('q')).color==='rgb(3, 2, 1)'",
        2000);
    char rb[4096] = {0};
    int rlen = mbGetRequestLog(rb, sizeof(rb));
    const bool logged =
        rlen > 0 && std::string(rb).find("mb_reqlog.css") != std::string::npos;
    mbClearRequestLog();
    const bool cleared = mbGetRequestLog(nullptr, 0) == 0;
    Expect(logged && cleared,
           "mbGetRequestLog records subresource fetches; mbClearRequestLog empties it",
           std::string("logged=") + (logged ? "1" : "0") + " cleared=" +
               (cleared ? "1" : "0"));
    std::remove(rl_css);
  }

  // 75c. Request blocking: a blocked subresource never loads. With "mb_block.css"
  // blocked, the file:// stylesheet's request fails -> #q keeps the default color;
  // after mbClearUrlBlocks a reload loads it -> #q turns rgb(5,5,5).
  {
    const char* bl_css = "/tmp/mb_block.css";
    if (FILE* f = std::fopen(bl_css, "wb")) {
      std::fputs("#q{color:rgb(5,5,5)}", f);
      std::fclose(f);
    }
    const char* doc =
        "<head><link rel='stylesheet' href='file:///tmp/mb_block.css'></head>"
        "<body><b id='q'>x</b></body>";
    mbBlockUrl("mb_block.css");
    mbLoadHTML(v, doc, "file:///tmp/mb_blk_page.html");
    mbWaitForSelector(v, "#q", 1000);
    mbWait(v, 80);  // give the (blocked) request time to resolve
    const bool blocked =
        Eval(v, "getComputedStyle(document.getElementById('q')).color") !=
        "rgb(5, 5, 5)";
    mbClearUrlBlocks();
    mbLoadHTML(v, doc, "file:///tmp/mb_blk_page.html");  // reload, now unblocked
    const bool applies = mbWaitForFunction(
        v, "getComputedStyle(document.getElementById('q')).color==='rgb(5, 5, 5)'",
        2000) == 1;
    Expect(blocked && applies,
           "mbBlockUrl blocks a subresource; mbClearUrlBlocks restores it",
           std::string("blocked=") + (blocked ? "1" : "0") + " applies=" +
               (applies ? "1" : "0"));
    std::remove(bl_css);
  }

  // 76. CSS background-image renders (data: SVG). Distinct from <img>: exercises
  // the CSS background paint path + a data: URL image + SVG-as-image. A 30x30 div
  // with a green-SVG background should paint green at its center.
  {
    mbLoadHTML(v,
      "<body style='margin:0;background:#fff'>"
      "<div style='width:30px;height:30px;background-image:url("
      "\"data:image/svg+xml,<svg xmlns=%22http://www.w3.org/2000/svg%22 "
      "width=%2230%22 height=%2230%22><rect width=%2230%22 height=%2230%22 "
      "fill=%22%2300ff00%22/></svg>\")'></div></body>",
      "about:blank");
    mbWait(v, 80);  // background image decode + paint
    std::vector<uint8_t> bg(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, bg.data(), W, H, W * 4);
    size_t mid = (15u * W + 15u) * 4;  // center of the div
    Expect(bg[mid] == 0 && bg[mid + 1] == 255 && bg[mid + 2] == 0,  // green
           "CSS background-image (data: SVG) paints");
  }

  // 77. iframes work end to end: the child frame is created (CreateChildFrame)
  // AND its srcdoc content commits (BeginNavigation fills the body + policy
  // container). frames.length==1, and the child document holds the srcdoc DOM —
  // contentDocument.body.textContent is the iframe's content. Parent unaffected.
  {
    mbLoadHTML(v,
      "<body><b id='p'>parent</b>"
      "<iframe id='f' srcdoc='<b>child-body</b>' width='80' height='40'></iframe>"
      "</body>", "about:blank");
    mbWait(v, 100);  // let the child commit + parse
    Expect(Eval(v, "1+1") == "2" &&
               Eval(v, "document.getElementById('p').textContent") == "parent" &&
               Eval(v, "String(window.frames.length)") == "1" &&
               Eval(v, "document.getElementById('f').contentDocument.body.textContent")
                   == "child-body",
           "iframe loads: child frame created + srcdoc content commits");
  }

  // 78. iframe src= loads too (not just srcdoc): the child's navigation fetches
  // the src body via the loader (MbFetchUrl) and commits it. Uses a data: src
  // (portable; file/http go through the same path). The parent is loaded from a
  // file:// base so the child data: document inherits that origin (same-origin),
  // letting the parent read contentDocument; with an opaque (about:blank) parent
  // the child would get a fresh opaque origin and the read would be cross-origin
  // blocked — that's correct browser behavior, not a commit failure.
  {
    mbLoadHTML(v,
      "<body>p<iframe id='f' src='data:text/html,<b>src-child</b>' "
      "width='80' height='40'></iframe></body>", "file:///tmp/p.html");
    mbWait(v, 250);  // child navigation: fetch + commit + parse
    Expect(Eval(v,
        "document.getElementById('f').contentDocument.body.textContent") ==
            "src-child",
        "iframe src= loads: child fetches + commits its document");
  }

  // 79. <iframe sandbox> is enforced: the owner's FramePolicy sandbox flags
  // reach the committed child document (CreateChildFrame -> BeginNavigation
  // applies them). The cleanest origin-independent signal is script blocking:
  // a sandboxed child (no allow-scripts) must NOT run its inline script, while
  // a non-sandboxed sibling does. Each child's script tags its own <body> with
  // data-ran; we read it back from the parent. (We avoid asserting via a
  // cross-origin read because the file:// parent has universal access here, so
  // it can read even an opaque-origin child — origin enforcement still happens,
  // but isn't observable that way.)
  {
    mbLoadHTML(v,
      "<body>"
      "<iframe id='sb' sandbox srcdoc=\"<body><script>"
      "document.body.setAttribute('data-ran','yes')</scr" "ipt></body>\">"
      "</iframe>"
      "<iframe id='op' srcdoc=\"<body><script>"
      "document.body.setAttribute('data-ran','yes')</scr" "ipt></body>\">"
      "</iframe>"
      "</body>", "file:///tmp/p.html");
    mbWait(v, 200);
    // Sandboxed (no allow-scripts): inline script must not run.
    Expect(Eval(v,
        "''+document.getElementById('sb').contentDocument.body"
        ".getAttribute('data-ran')") == "null",
        "iframe sandbox enforced: sandboxed child's script is blocked");
    // Non-sandboxed sibling: its script runs.
    Expect(Eval(v,
        "''+document.getElementById('op').contentDocument.body"
        ".getAttribute('data-ran')") == "yes",
        "iframe sandbox scoped: non-sandboxed sibling's script runs");
  }

  // 78. element.scrollIntoView() works (a common automation primitive: scroll a
  // target into view before clicking/capturing). Our non-compositing widget
  // handles scroll specially, so verify programmatic scroll-into-view actually
  // moves the viewport and lands the element on-screen.
  {
    mbLoadHTML(v,
      "<body style='margin:0'><div style='height:1500px'></div>"
      "<b id='t'>T</b><div style='height:400px'></div></body>", "about:blank");
    std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // force layout for scroll
    Expect(Eval(v,
        "(function(){window.scrollTo(0,0);var t=document.getElementById('t');"
        "var b=window.scrollY;t.scrollIntoView();var a=window.scrollY;"
        "var top=t.getBoundingClientRect().top;"
        "return (a>b && top>=0 && top<window.innerHeight)?'ok'"
        ":(a+'/'+b+'/'+Math.round(top));})()") == "ok",
        "scrollIntoView scrolls the target into the viewport");
  }

  // 80. Page-initiated main-frame navigation: a link click or location=
  // assignment must actually navigate the top frame. Previously BeginNavigation
  // early-returned for the main frame, so a page that navigated itself did
  // nothing. Now the main frame posts the commit. Drive it via a JS location
  // assignment (the anchor default-action path is the same BeginNavigation
  // hook) and confirm the new document is live.
  {
    const char* a = "<body><div id=o>navA-here</div></body>";
    const char* b = "<body><div id=o>navB-here</div></body>";
    if (FILE* f = std::fopen("/tmp/mb_navA.html", "wb")) {
      std::fwrite(a, 1, std::strlen(a), f); std::fclose(f);
    }
    if (FILE* f = std::fopen("/tmp/mb_navB.html", "wb")) {
      std::fwrite(b, 1, std::strlen(b), f); std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_navA.html");
    const bool on_a =
        Eval(v, "document.getElementById('o').textContent") == "navA-here";
    mbRunJS(v, "location.href='file:///tmp/mb_navB.html';");
    mbWait(v, 300);  // posted commit + fetch + parse
    const std::string after =
        Eval(v, "document.getElementById('o').textContent");
    Expect(on_a && after == "navB-here",
           "page-initiated main-frame navigation (location=) commits", after);
  }

  // 81. Character encoding: a non-UTF-8 page is decoded via <meta charset>.
  // CommitHtml used to force UTF-8 (authoritative), turning latin-1/Shift_JIS
  // bytes into mojibake. The encoding is now tentative so the parser honors the
  // declared charset. (Eval returns the JS string as UTF-8, so 'é' compares as
  // the 2-byte 0xC3 0xA9 sequence.)
  {
    // ISO-8859-1 source bytes: 0xE9='é', 0xE8='è'.
    const char* doc =
        "<meta charset=\"ISO-8859-1\"><body><div id=o>caf\xe9 cr\xe8me</div></body>";
    if (FILE* f = std::fopen("/tmp/mb_latin1.html", "wb")) {
      std::fwrite(doc, 1, std::strlen(doc), f); std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_latin1.html");
    mbWait(v, 50);
    Expect(Eval(v, "document.getElementById('o').textContent") ==
               "caf\xc3\xa9 cr\xc3\xa8me",
           "non-UTF-8 page decodes via <meta charset> (ISO-8859-1)");
  }

  // 82. UTF-8 without a <meta charset> still decodes (auto-detection) — guards
  // that the tentative-encoding change didn't regress the common UTF-8 default.
  {
    const char* doc = "<body><div id=o>caf\xc3\xa9</div></body>";  // UTF-8 'é'
    if (FILE* f = std::fopen("/tmp/mb_utf8nm.html", "wb")) {
      std::fwrite(doc, 1, std::strlen(doc), f); std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_utf8nm.html");
    mbWait(v, 50);
    Expect(Eval(v, "document.getElementById('o').textContent") == "caf\xc3\xa9",
           "UTF-8 page without <meta charset> decodes (auto-detect)");
  }

  // 83. mbGetURL / mbGetTitle: read the committed document's URL + title through
  // the C API (no JS), straight from the frame.
  {
    mbLoadHTML(v, "<head><title>My Title</title></head><body>x</body>",
               "file:///tmp/mb_titletest.html");
    char ub[512] = {0}, tb[256] = {0};
    mbGetURL(v, ub, sizeof(ub));
    mbGetTitle(v, tb, sizeof(tb));
    Expect(std::string(ub) == "file:///tmp/mb_titletest.html",
           "mbGetURL returns the document URL", ub);
    Expect(std::string(tb) == "My Title",
           "mbGetTitle returns the document title", tb);
  }

  // 84. mbGetText / mbGetHTML scraping accessors.
  {
    mbLoadHTML(v, "<body><p>Alpha</p><p>Beta</p></body>", "about:blank");
    char xb[1024] = {0};
    mbGetText(v, xb, sizeof(xb));
    Expect(std::string(xb).find("Alpha") != std::string::npos &&
               std::string(xb).find("Beta") != std::string::npos,
           "mbGetText returns visible text", xb);
    char hb[2048] = {0};
    int hlen = mbGetHTML(v, hb, sizeof(hb));
    Expect(hlen > 0 && std::string(hb).find("<p>Alpha</p>") != std::string::npos,
           "mbGetHTML returns serialized DOM", hb);
  }

  // 85. mbReload re-fetches the document: mutate the DOM, reload, the mutation is
  // gone (the file is re-read and re-committed).
  {
    const char* doc = "<body><div id=o>ORIGINAL</div></body>";
    if (FILE* f = std::fopen("/tmp/mb_reload.html", "wb")) {
      std::fwrite(doc, 1, std::strlen(doc), f); std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_reload.html");
    mbRunJS(v, "document.getElementById('o').textContent='MUTATED';");
    const bool mutated =
        Eval(v, "document.getElementById('o').textContent") == "MUTATED";
    mbReload(v);
    mbWait(v, 50);
    Expect(mutated && Eval(v, "document.getElementById('o').textContent") ==
                          "ORIGINAL",
           "mbReload re-fetches + re-commits the document");
  }

  // 86. Host-driven history (mbGoBack/mbGoForward) over a host load + a
  // page-initiated navigation: A --(location.href)--> B, back to A, forward to B.
  {
    const char* a = "<body><div id=o>histA</div></body>";
    const char* b = "<body><div id=o>histB</div></body>";
    if (FILE* f = std::fopen("/tmp/mb_hA.html", "wb")) {
      std::fwrite(a, 1, std::strlen(a), f); std::fclose(f);
    }
    if (FILE* f = std::fopen("/tmp/mb_hB.html", "wb")) {
      std::fwrite(b, 1, std::strlen(b), f); std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_hA.html");          // history: [A]
    mbRunJS(v, "location.href='file:///tmp/mb_hB.html';");  // page nav -> [A,B]
    mbWait(v, 300);
    const bool on_b =
        Eval(v, "document.getElementById('o').textContent") == "histB";
    const bool cgb = mbCanGoBack(v) == 1 && mbCanGoForward(v) == 0;
    mbGoBack(v);
    mbWait(v, 100);
    const bool back_a =
        Eval(v, "document.getElementById('o').textContent") == "histA" &&
        mbCanGoForward(v) == 1;
    mbGoForward(v);
    mbWait(v, 100);
    const bool fwd_b =
        Eval(v, "document.getElementById('o').textContent") == "histB";
    Expect(on_b && cgb && back_a && fwd_b,
           "mbGoBack/mbGoForward navigate the history stack",
           std::string("on_b=") + (on_b ? "1" : "0") + " cgb=" +
               (cgb ? "1" : "0") + " back_a=" + (back_a ? "1" : "0") +
               " fwd_b=" + (fwd_b ? "1" : "0"));
  }

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

  // 102h. mbWaitForNetworkIdle: a page fires a deferred fetch (150ms) that routes
  // through the loader; the wait must return idle (not timeout) only after that
  // request lands — so the log holds it afterward. A second quiet page confirms
  // it doesn't false-timeout. (file:// origin so the file:// fetch is same-scheme.)
  {
    mbClearRequestLog();
    mbLoadHTML(v,
        "<body><script>setTimeout(function(){var i=document.createElement('img');"
        "i.src='file:///tmp/mb_ni_probe.png';document.body.appendChild(i);},150);"
        "</script></body>", "file:///tmp/mb_ni_page.html");
    const bool idle = mbWaitForNetworkIdle(v, 300, 5000) == 1;
    char rb[4096] = {0};
    mbGetRequestLog(rb, sizeof(rb));
    const bool fetched = std::string(rb).find("mb_ni_probe.png") != std::string::npos;
    mbClearRequestLog();
    mbLoadHTML(v, "<body>quiet</body>", "about:blank");
    const bool quiet_ok = mbWaitForNetworkIdle(v, 150, 3000) == 1;  // no false timeout
    Expect(idle && fetched && quiet_ok,
           "mbWaitForNetworkIdle returns after deferred fetch; quiet page is idle",
           std::string("idle=") + (idle ? "1" : "0") + " fetched=" +
               (fetched ? "1" : "0") + " quiet=" + (quiet_ok ? "1" : "0"));
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

  // 107. Native function binding: a C function bound via mbJsBindFunction is
  // callable from JS synchronously — window[name](args) returns the C result
  // inline — receiving string args and the userdata pointer. Installed into each
  // new document (so it works after a navigation and from a page event handler).
  {
    int tag = 7;
    mbJsBindFunction(v, "mbEcho", SmokeEcho, &tag);
    mbJsBindFunction(v, "mbObj", SmokeJson, nullptr);
    mbLoadHTML(v, "<body>native</body>", "about:blank");
    const std::string defined = Eval(v, "typeof window.mbEcho");
    const std::string r = Eval(v, "window.mbEcho('hi')");  // -> "hi!7"
    const std::string in_expr =
        Eval(v, "(function(){return 'got:'+window.mbEcho('x');})()");
    // out_type 5 (json): the C return becomes a real JS object the page navigates.
    const std::string obj_type = Eval(v, "typeof window.mbObj()");
    const std::string obj_vals =
        Eval(v, "window.mbObj().a + ',' + window.mbObj().b[1]");  // -> "1,3"
    Expect(defined == "function" && r == "hi!7" && in_expr == "got:x!7" &&
               obj_type == "object" && obj_vals == "1,3",
           "mbJsBindFunction: synchronous C call; string + JSON-object returns",
           "typeof=" + defined + " r=" + r + " expr=" + in_expr +
               " objType=" + obj_type + " objVals=" + obj_vals);
  }

  mbDestroyView(v);
  mbShutdown();

  std::fprintf(stderr, "\nmb_smoke: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
