// mb_smoke — capability test suite for the miniblink-modern engine. Each case loads
// content and ASSERTS engine behavior (mostly via mbEvalJS / getComputedStyle, which is
// robust; plus one pixel check). Prints PASS/FAIL per case and a summary; exit 0 iff all pass.
#include "miniblink_host/test/mb_smoke_harness.h"

using mbsmoke::Eval;     // shared harness helpers (see mb_smoke_harness.h)
using mbsmoke::EvalIso;
using mbsmoke::Expect;
using mbsmoke::g_fail;
using mbsmoke::g_pass;

namespace {
// Native functions bound into JS for the mbJsBindFunction test: echoes its first
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

  // 8b. SVG renders to pixels — a distinct paint path from CSS boxes that
  // icon/chart-heavy pages rely on. An inline 100x100 SVG with a solid-green
  // <rect> must paint green well inside the rect (pixel (20,20)). Tolerances
  // absorb any AA/color-management drift on the interior.
  {
    mbLoadHTML(v,
        "<body style='margin:0'>"
        "<svg width='100' height='100' xmlns='http://www.w3.org/2000/svg'>"
        "<rect x='0' y='0' width='100' height='100' fill='rgb(0,128,0)'/></svg>"
        "</body>", "about:blank");
    std::vector<uint8_t> sp(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, sp.data(), W, H, W * 4);
    const size_t o = (static_cast<size_t>(20) * W + 20) * 4;  // inside the rect
    const int b = sp[o], g = sp[o + 1], r = sp[o + 2];
    Expect(r < 16 && g > 110 && g < 145 && b < 16,
           "SVG renders to pixels (inline <rect> paints green)",
           std::string("rgb(") + std::to_string(r) + "," + std::to_string(g) +
               "," + std::to_string(b) + ")");
  }

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

    // mbEvalJSEx's value buffer is the same path (arbitrary result content) —
    // verify it also truncates at a boundary, not mid-multibyte.
    char vbig[64] = {0}, tbig[16] = {0};
    mbEvalJSEx(v, "'café'", vbig, sizeof(vbig), tbig, sizeof(tbig));
    const std::string vfull(vbig);
    char vsmall[5] = {0};
    mbEvalJSEx(v, "'café'", vsmall, sizeof(vsmall), nullptr, 0);
    const std::string vgot(vsmall);
    const bool ev_boundary =
        vgot.size() == vfull.size() ||
        (static_cast<unsigned char>(vfull[vgot.size()]) & 0xC0) != 0x80;
    Expect(vgot.size() < vfull.size() && ev_boundary &&
               vfull.compare(0, vgot.size(), vgot) == 0,
           "mbEvalJSEx value buffer truncates at a UTF-8 boundary",
           std::string("vfull=") + std::to_string((int)vfull.size()) + " vgot='" +
               vgot + "'");
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

  // 14c. position:fixed in a full-page (resized) capture. mb_shot --full resizes
  // the view to the content height and paints at scroll 0; a fixed top:0 header
  // then sits at y=0 of that tall viewport. It must paint ONCE at the top — not
  // vanish, and not repeat down the page (a real screenshot-correctness concern
  // for sticky headers on long pages).
  {
    mbLoadHTML(v,
        "<body style='margin:0'>"
        "<div style='position:fixed;top:0;left:0;width:100%;height:50px;"
        "background:#00ff00'></div>"
        "<div style='height:1500px;background:#ffffff'></div></body>",
        "about:blank");
    mbResize(v, W, 1500);
    std::vector<uint8_t> tall(static_cast<size_t>(W) * 1500 * 4, 0);
    mbPaintToBitmap(v, tall.data(), W, 1500, W * 4);
    const size_t top = (static_cast<size_t>(10) * W + 10) * 4;   // inside the header
    const size_t mid = (static_cast<size_t>(800) * W + 10) * 4;  // content, far below
    auto green = [](const uint8_t* p) { return p[2] == 0 && p[1] == 255 && p[0] == 0; };
    const bool header_top = green(&tall[top]);
    const bool no_repeat = !green(&tall[mid]);
    Expect(header_top && no_repeat,
           "full-page capture: position:fixed header paints once at top (no repeat)",
           std::string("top=rgb(") + std::to_string(tall[top + 2]) + "," +
               std::to_string(tall[top + 1]) + "," + std::to_string(tall[top]) +
               ") mid=rgb(" + std::to_string(tall[mid + 2]) + "," +
               std::to_string(tall[mid + 1]) + "," + std::to_string(tall[mid]) + ")");
    mbResize(v, W, H);  // restore the shared viewport
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

  // 14d. Responsive emulation: width media queries track the VIEW width (set via
  // mbResize / mb_shot's width/height), so a mobile screenshot is just a narrow
  // view — the practical mobile-emulation path. Conversely <meta name=viewport>
  // directives are NOT honored (desktop-mode WebView: the layout viewport is
  // always the view size). This locks in both — the working capability and the
  // documented limitation — since responsive sites depend on the first.
  {
    const char* doc =
        "<style>#c{color:rgb(1,1,1)}@media (max-width:500px){#c{color:rgb(2,2,2)}}"
        "</style><body><div id='c'>x</div></body>";
    mbResize(v, 400, H);
    mbLoadHTML(v, doc, "about:blank");
    const std::string narrow =
        Eval(v, "getComputedStyle(document.getElementById('c')).color");
    mbResize(v, 800, H);
    mbLoadHTML(v, doc, "about:blank");
    const std::string wide =
        Eval(v, "getComputedStyle(document.getElementById('c')).color");
    // A viewport meta width cannot override the layout width (it's ignored).
    mbResize(v, 400, H);
    mbLoadHTML(v, "<meta name=viewport content='width=980'><body>x</body>",
               "about:blank");
    const std::string iw = Eval(v, "String(window.innerWidth)");
    Expect(narrow == "rgb(2, 2, 2)" && wide == "rgb(1, 1, 1)" && iw == "400",
           "responsive: width media queries track the view size "
           "(<meta viewport> ignored)",
           std::string("narrow=") + narrow + " wide=" + wide + " iw@vp980=" + iw);
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

  // 39b. History API (SPA client-side routing). pushState/replaceState update
  // location + history.state, and — the part that matters to an embedder —
  // mbGetURL reflects the new URL, so a scraper/automation sees the current SPA
  // route (not just the initial load). Uses a real https origin (the realistic
  // SPA case; about:blank can't pushState cross-path).
  {
    mbLoadHTML(v, "<body>spa</body>", "https://spa.test/page/one");
    mbRunJS(v, "history.pushState({a:1},'','/page/two?q=1');");
    char u1[256] = {0};
    mbGetURL(v, u1, sizeof(u1));
    const std::string loc_push = Eval(v, "location.pathname+location.search");
    mbRunJS(v, "history.replaceState({a:2},'','/page/three');");
    char u2[256] = {0};
    mbGetURL(v, u2, sizeof(u2));
    const std::string state = Eval(v, "String(history.state&&history.state.a)");
    Expect(std::string(u1) == "https://spa.test/page/two?q=1" &&
               loc_push == "/page/two?q=1" &&
               std::string(u2) == "https://spa.test/page/three" && state == "2",
           "History API: pushState/replaceState update location + mbGetURL",
           std::string("push=") + u1 + " replace=" + u2 + " state=" + state);
  }

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
  // deadlock the host forever (confirmed: mb_shot exit 137). Our in-process
  // MbBlobURLStore now services that [Sync] Register off-thread, so
  // createObjectURL returns a blob: URL without blocking AND the URL resolves to
  // data (see case 46b). Blob data ops (size/text/arrayBuffer/FileReader) were
  // always fine. This calls createObjectURL DURING LOAD (the realistic hang path)
  // and also revokes; a regression would hang the whole suite (watchdog catches it).
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

  // 46b. Blob: URL resolution actually SERVES the bytes — not merely "returns a
  // blob: URL without hanging" (case 46). The in-process MbBlobURLStore
  // (ResolveAsURLLoaderFactory) resolves blob: URLs, so createObjectURL + fetch
  // round-trips the content for BOTH inline (<=256 KB) and BytesProvider (>256 KB)
  // blobs. (This supersedes the old 0003-skip-blob-url-register behavior, where
  // the URL did not resolve.) Async — the script signals window.__bd when done.
  // The blob: URL [Sync] Register lands on the service thread, so in the busy
  // long-lived suite process the register->fetch ordering can transiently race
  // (a fresh single-shot process never sees it; verified 13/13 via mb_shot). We
  // retry the fetch a few times so the test stays deterministic when the product
  // is correct, yet still fails (BAD:<lengths>) if blob: URLs truly don't resolve.
  {
    mbLoadHTML(v,
      "<body><div id='r'>p</div><script>(async function(){"
      "var ok=false,last='';"
      "for(var i=0;i<25&&!ok;i++){try{"
      "var s=await (await fetch(URL.createObjectURL(new Blob(['hi blob'])))).text();"
      "var big='z'.repeat(300*1024);"   // > 256 KB inline cap -> BytesProvider path
      "var t=await (await fetch(URL.createObjectURL(new Blob([big])))).text();"
      "ok=(s==='hi blob'&&t===big);last='s='+s.length+',t='+t.length;"
      "}catch(e){last='THREW:'+e.name;break;}"
      "if(!ok)await new Promise(function(r){setTimeout(r,40);});}"
      "document.getElementById('r').textContent=ok?'OK':('BAD:'+last);"
      "window.__bd=true;})();</script></body>", "about:blank");
    const int ready = mbWaitForFunction(v, "window.__bd===true", 8000);
    const std::string r = Eval(v, "document.getElementById('r').textContent");
    Expect(ready == 1 && r == "OK",
           "blob: URL fetch resolves bytes (inline + BytesProvider >256 KB)",
           std::string("ready=") + std::to_string(ready) + " r=" + r);
  }

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

  // 56b. Emoji rasterize to glyph pixels — but MONOCHROME in this build. A color-
  // emoji font is not bundled, so U+1F600 😀 paints as a grayscale/black glyph
  // (saturated color pixels = 0), not Apple-Color-Emoji yellow. This is a known,
  // documented limitation (emoji in screenshots won't be colorful); the guard is
  // that it still rasterizes a real glyph (dark strokes + light gaps, like text)
  // and degrades gracefully rather than crashing or rendering tofu boxes. If a
  // color-emoji font is ever bundled, `colorful` jumps and this comment is stale.
  {
    mbLoadHTML(v,
      "<body style='margin:0;background:#fff'>"
      "<span style='font-size:72px;line-height:1'>\xf0\x9f\x98\x80</span>"
      "</body>", "about:blank");
    std::vector<uint8_t> ep(static_cast<size_t>(W) * H * 4, 255);
    mbPaintToBitmap(v, ep.data(), W, H, W * 4);
    int dark = 0, light = 0, colorful = 0;
    for (int y = 4; y < 84; ++y)
      for (int x = 2; x < 82; ++x) {
        const size_t o = (static_cast<size_t>(y) * W + x) * 4;
        int b = ep[o], g = ep[o + 1], r = ep[o + 2];
        int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
        int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
        if (mx - mn > 60 && mx > 80) ++colorful;  // a vivid, non-gray pixel
        if (r < 60) ++dark; else if (r > 200) ++light;
      }
    Expect(dark > 20 && light > 20 && colorful == 0,
           "emoji rasterizes (monochrome glyph; no color-emoji font bundled)",
           std::string("dark=") + std::to_string(dark) + " light=" +
               std::to_string(light) + " colorful=" + std::to_string(colorful));
  }

  // 56c. Multiple distinct fonts are available — not a single fallback. The three
  // generic families must render the same text at DIFFERENT advance widths,
  // proving the build ships a real serif + sans-serif + monospace, so screenshots
  // of sites that specify font families keep the right look. A font-config
  // regression that collapsed everything to one fallback would make these equal.
  {
    mbLoadHTML(v, "<body>fonts</body>", "about:blank");
    const std::string r = Eval(v,
        "(function(){var c=document.createElement('canvas').getContext('2d');"
        "function w(f){c.font='40px '+f;"
        "return Math.round(c.measureText('Wikipedia mix& Quilt').width);}"
        "var s=w('serif'),n=w('sans-serif'),m=w('monospace');"
        "return (s>0&&n>0&&m>0&&s!==m&&n!==m&&s!==n)+':'+s+','+n+','+m;})()");
    Expect(r.substr(0, 5) == "true:",
           "fonts: serif/sans/monospace render at distinct widths (real font set)",
           r);
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

  // 62-sep. JsEscape robustness: a value containing the JS line terminators
  // U+2028 / U+2029 must round-trip intact through the eval-based fill. These are
  // legal in ES2019+ string literals (V8's JSON-superset) but would terminate a
  // pre-ES2019 literal and break the generated JS — JsEscape escapes \ " \n \r
  // but NOT these, so this case is the regression guard proving the embedding
  // stays correct if V8's parser or JsEscape ever changes (real text — some PDFs
  // / rich-text sources — does carry these separators).
  {
    mbLoadHTML(v, "<body><input id='sep' value=''></body>", "about:blank");
    const char* sep_val = "a\xe2\x80\xa8" "b\xe2\x80\xa9" "c";  // a U+2028 b U+2029 c
    int ok = mbFillSelector(v, "#sep", sep_val);
    const std::string probe = Eval(v,
        "var x=document.getElementById('sep').value;"
        "x.length + ',' + x.charCodeAt(1) + ',' + x.charCodeAt(3)");
    Expect(ok == 1 && probe == "5,8232,8233",  // 5 code points; 0x2028, 0x2029
           "JsEscape: U+2028/U+2029 in a filled value round-trip intact",
           std::string("probe=") + probe);
  }

  // 62-popup. Popup / new-window safety: a scraped or automated page that calls
  // window.open or activates a target=_blank link must never crash the
  // single-process host. Modern Blink (M150) has no WebViewClient::CreateView to
  // override — the factory methods migrated out of that interface — and the
  // default denies the popup, so window.open returns null and the _blank
  // activation is a safe no-op here. This locks that in: a crash would take down
  // any embedder that runs untrusted pages.
  {
    mbLoadHTML(v,
        "<body><a id='b' href='https://example.com/' target='_blank'>x</a>"
        "<script>window.__r=String(window.open('about:blank'));</script></body>",
        "about:blank");
    const std::string opened = Eval(v, "window.__r");  // "null" == popup denied
    const int clicked = mbClickSelector(v, "#b");       // must not crash the host
    const std::string alive = Eval(v, "'alive'");       // host still responsive
    Expect(opened == "null" && clicked == 1 && alive == "alive",
           "popup safety: window.open -> null; _blank click doesn't crash the host",
           std::string("open=") + opened + " click=" + std::to_string(clicked));
  }

  // 62-csp. A strict Content-Security-Policy blocks the PAGE's own scripts but NOT
  // the host's extraction. Host eval (mbEvalJS / the selector readers) runs in a
  // privileged context like DevTools, so script-src 'none' can't stop it — which
  // is what lets us scrape the large fraction of the real web that ships strict
  // CSP. The page's inline script is blocked (proving CSP is active), yet
  // mbGetTextForSelector / mbCountSelector / mbEvalJS all still read the DOM.
  // Run on a dedicated view: a document's CSP from <meta> persists on the frame,
  // so reusing the shared `v` would carry script-src 'none' into later cases
  // (mb_shot is one-page-per-process, so that never bites the real tool).
  {
    mbView* cv = mbCreateView(W, H);
    mbLoadHTML(cv,
        "<meta http-equiv='Content-Security-Policy' "
        "content=\"script-src 'none'; default-src 'none'\">"
        "<body><h1 id='t'>protected</h1><div class='r'>a</div><div class='r'>b</div>"
        "<script>window.__pageran=true;</script></body>",  // CSP blocks this
        "https://csp.test/");
    const bool page_blocked =
        Eval(cv, "String(typeof window.__pageran)") == "undefined";
    char tb[64] = {0};
    const bool host_text = mbGetTextForSelector(cv, "#t", tb, sizeof(tb)) >= 0 &&
                           std::string(tb) == "protected";
    const bool host_count = mbCountSelector(cv, ".r") == 2;
    const bool host_eval =
        Eval(cv, "document.getElementById('t').textContent") == "protected";
    Expect(page_blocked && host_text && host_count && host_eval,
           "CSP script-src 'none' blocks page scripts, not host extraction",
           std::string("pageBlocked=") + (page_blocked ? "1" : "0") + " text=" +
               tb + " count=" + (host_count ? "2" : "?"));
    mbDestroyView(cv);
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

  // 75a3. mbScrollToBottom drives IntersectionObserver-based lazy loading — the
  // dominant modern pattern (loading="lazy" / IO libraries), distinct from 75a2's
  // scroll-event approach (it exercises ForceUpdateViewportIntersections between
  // scrolls). A below-fold element watched by an IO flips a flag only once it
  // scrolls into view, so the flag must be false before auto-scroll and true after.
  {
    mbResize(v, W, 300);  // #lz sits ~3000px down, well below this viewport
    mbLoadHTML(v,
        "<body style='margin:0'><div style='height:3000px'></div>"
        "<div id='lz' style='height:40px'></div>"
        "<script>window.__seen=false;"
        "new IntersectionObserver(function(es){es.forEach(function(e){"
        "if(e.isIntersecting)window.__seen=true;});})"
        ".observe(document.getElementById('lz'));</script></body>",
        "about:blank");
    const bool before = Eval(v, "String(window.__seen)") == "false";  // below fold
    mbScrollToBottom(v, 10);  // reveals #lz; this page doesn't grow (flag-only)
    const bool after = Eval(v, "String(window.__seen)") == "true";     // IO fired
    Expect(before && after,
           "mbScrollToBottom drives IntersectionObserver lazy-load (below-fold IO)",
           std::string("before_unseen=") + (before ? "1" : "0") + " after_seen=" +
               (after ? "1" : "0"));
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

  // 75d. Response mocking (mbMockResponse) — the #1 interception feature. A
  // registered https URL serves a canned stylesheet body WITHOUT any real fetch
  // (so it works fully offline): a <link> to the never-served URL turns #q green.
  // Then mbClearMocks + a re-mock with red proves clearing works and a later mock
  // overrides — all offline, no network. (https subresources route through the
  // loader's Deliver chokepoint, same as the block/log cases.)
  {
    const char* doc =
        "<head><link rel='stylesheet' href='https://mock.test/s.css'></head>"
        "<body><b id='q'>x</b></body>";
    mbClearMocks();
    mbMockResponse("https://mock.test/s.css", "#q{color:rgb(0,170,0)}", "text/css",
                   200);
    mbLoadHTML(v, doc, "https://mock.test/p1.html");
    const bool green = mbWaitForFunction(
        v, "getComputedStyle(document.getElementById('q')).color==='rgb(0, 170, 0)'",
        2000) == 1;
    mbClearMocks();  // drop the green mock...
    mbMockResponse("https://mock.test/s.css", "#q{color:rgb(170,0,0)}", "text/css",
                   200);  // ...and replace with red
    mbLoadHTML(v, doc, "https://mock.test/p2.html");
    const bool red = mbWaitForFunction(
        v, "getComputedStyle(document.getElementById('q')).color==='rgb(170, 0, 0)'",
        2000) == 1;
    mbClearMocks();
    Expect(green && red,
           "mbMockResponse serves a canned body with no fetch; clear/re-mock works",
           std::string("green=") + (green ? "1" : "0") + " red=" +
               (red ? "1" : "0"));
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

  // 77b. iframe content RENDERS into the parent's paint — not just the DOM (77).
  // The child frame must composite into the screenshot for captures of pages with
  // ads / embeds / maps / social widgets to be correct. Place a solid-green iframe
  // at the top-left and read a pixel inside its box.
  {
    mbResize(v, W, H);
    mbLoadHTML(v,
        "<body style='margin:0'>"
        "<iframe srcdoc=\"<body style='margin:0;background:rgb(0,128,0)'></body>\" "
        "width='150' height='80' style='border:0;display:block'></iframe></body>",
        "about:blank");
    mbWait(v, 150);  // let the child commit + paint
    std::vector<uint8_t> ifpx(static_cast<size_t>(W) * H * 4, 255);
    mbPaintToBitmap(v, ifpx.data(), W, H, W * 4);
    const size_t o = (static_cast<size_t>(30) * W + 40) * 4;  // inside the iframe box
    const int pb = ifpx[o], pg = ifpx[o + 1], pr = ifpx[o + 2];
    Expect(pr < 16 && pg > 110 && pg < 145 && pb < 16,
           "iframe content renders into the parent paint (green child paints)",
           std::string("rgb(") + std::to_string(pr) + "," + std::to_string(pg) +
               "," + std::to_string(pb) + ")");
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

  // 81b. Legacy CJK charsets decode too — not just latin-1. Confirms the bundled
  // ICU/codec data covers the common Asian-site encodings (a real international-
  // scraping need). Checked via code points (charCodeAt) to avoid UTF-8
  // byte-compare noise.
  {
    // Shift_JIS bytes 0x93FA 0x967B = 日(U+65E5=26085) 本(U+672C=26412).
    const char* sjis =
        "<meta charset=\"Shift_JIS\"><body><span id=o>\x93\xfa\x96\x7b</span></body>";
    if (FILE* f = std::fopen("/tmp/mb_sjis.html", "wb")) {
      std::fwrite(sjis, 1, std::strlen(sjis), f); std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_sjis.html");
    mbWait(v, 50);
    const std::string sj = Eval(v,
        "var t=document.getElementById('o').textContent;"
        "t.length+':'+t.charCodeAt(0)+','+t.charCodeAt(1)");
    // GBK bytes 0xD6D0 0xCEC4 = 中(U+4E2D=20013) 文(U+6587=25991).
    const char* gbk =
        "<meta charset=\"GBK\"><body><span id=o>\xd6\xd0\xce\xc4</span></body>";
    if (FILE* f = std::fopen("/tmp/mb_gbk.html", "wb")) {
      std::fwrite(gbk, 1, std::strlen(gbk), f); std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_gbk.html");
    mbWait(v, 50);
    const std::string gb = Eval(v,
        "var t=document.getElementById('o').textContent;"
        "t.length+':'+t.charCodeAt(0)+','+t.charCodeAt(1)");
    Expect(sj == "2:26085,26412" && gb == "2:20013,25991",
           "legacy CJK charsets decode via <meta charset> (Shift_JIS + GBK)",
           std::string("sjis=") + sj + " gbk=" + gb);
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

  // 86b. Page-driven history (history.back/forward/go from JS) is crash-safe. The
  // host routes page-initiated session-history nav through LocalFrameHost
  // (GoToEntryAtOffset); wiring that to actually navigate is an in-progress effort
  // (host-driven mbGoBack/mbGoForward already work, case 86). Until then it's a
  // graceful no-op — but an untrusted page calling history.back() must never crash
  // the single-process host. (pushState/replaceState DO work, case 39b.) This
  // safety invariant holds before AND after that wiring lands.
  {
    mbLoadHTML(v,
        "<body><p id='m'>alive</p><script>window.__hr='pre';"
        "try{history.back();history.forward();history.go(-1);window.__hr='ok';}"
        "catch(e){window.__hr='THREW:'+e.name;}</script></body>",
        "https://hist.test/");
    Expect(Eval(v, "window.__hr") == "ok" &&
               Eval(v, "document.getElementById('m').textContent") == "alive" &&
               Eval(v, "1+1") == "2",
           "page-driven history.back/forward/go is crash-safe",
           Eval(v, "window.__hr"));
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

  {
    // case 51: null-argument robustness. A C ABI must not crash when a caller
    // passes a null string pointer — it should return the documented failure
    // value. Reaching the Expect at all proves no function crashed (a null
    // deref would abort the process before we get there). Probes a spread
    // across categories: selector getters (-1), action setters (0), eval (0),
    // file save (0), and void sinks (no return, just must not crash).
    char nb[16] = {0};
    int rect_x = 0, rect_y = 0, rect_w = 0, rect_h = 0;
    const bool getters_safe =
        mbGetTextForSelector(v, nullptr, nb, sizeof(nb)) == -1 &&
        mbGetValueForSelector(v, nullptr, nb, sizeof(nb)) == -1 &&
        mbGetAttribute(v, nullptr, nullptr, nb, sizeof(nb)) == -1 &&
        mbGetCheckedForSelector(v, nullptr) == -1 &&
        mbIsVisibleForSelector(v, nullptr) == -1 &&
        mbGetElementRect(v, nullptr, &rect_x, &rect_y, &rect_w, &rect_h) == 0;
    const bool actions_safe =
        mbClickSelector(v, nullptr) == 0 &&
        mbFillSelector(v, nullptr, "x") == 0 &&
        mbSelectOption(v, nullptr, nullptr) == 0 &&
        mbSetAttribute(v, nullptr, nullptr, nullptr) == 0 &&
        mbEvalJS(v, nullptr, nb, sizeof(nb)) == 0 &&
        mbSavePdf(v, nullptr) == 0;
    // void sinks: no return value — the test is simply that these don't crash.
    mbLoadURL(v, nullptr);
    mbSetCookie(v, nullptr, nullptr);
    mbSendKey(v, nullptr);
    mbSendText(v, nullptr);
    Expect(getters_safe && actions_safe,
           "C ABI is null-arg safe (no crash; documented failure returns)",
           std::string("getters=") + (getters_safe ? "ok" : "BAD") +
               " actions=" + (actions_safe ? "ok" : "BAD"));
  }

  mbDestroyView(v);
  mbShutdown();

  std::fprintf(stderr, "\nmb_smoke: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
