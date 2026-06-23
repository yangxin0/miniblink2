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

  mbDestroyView(v);
  mbShutdown();

  std::fprintf(stderr, "\nmb_smoke: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
