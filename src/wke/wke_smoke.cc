// wke_smoke — exercises the wke compatibility slice end to end in its own process:
// init -> create -> resize -> loadHTML -> poll loading state -> read title ->
// paint to a BGRA buffer and check the background pixel -> destroy -> finalize.

#include "wke/wke.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main() {
  int pass = 0, fail = 0;
  auto check = [&](bool ok, const char* name) {
    if (ok) {
      ++pass;
      std::printf("  [PASS] %s\n", name);
    } else {
      ++fail;
      std::printf("  [FAIL] %s\n", name);
    }
  };

  wkeInitialize();

  wkeWebView wv = wkeCreateWebView();
  check(wv != nullptr, "wkeCreateWebView returns a view");
  if (!wv) {
    std::printf("wke_smoke: 0 passed, 1 failed\n");
    return 1;
  }

  wkeResize(wv, 200, 150);
  check(wkeGetWidth(wv) == 200 && wkeGetHeight(wv) == 150 &&
            wkeWidth(wv) == 200 && wkeHeight(wv) == 150,
        "wkeResize + wkeGetWidth/Height/wkeWidth/Height");

  // A page with a title and a solid blue background (rgb(0,128,255)).
  wkeLoadHTML(wv,
              "<title>WkeTitle</title>"
              "<body style='margin:0;background:rgb(0,128,255)'>hi</body>");
  check(!wkeIsLoading(wv) && wkeIsLoadingCompleted(wv) &&
            wkeIsLoadingSucceeded(wv) && !wkeIsLoadingFailed(wv) &&
            wkeIsDocumentReady(wv),
        "loading-state polling (completed + succeeded, not loading/failed)");
  check(std::strstr(wkeGetTitle(wv), "WkeTitle") != nullptr,
        "wkeGetTitle reads the document title");

  // Pull-model paint: render into a BGRA buffer and check the centre pixel is the
  // page's blue background (BGRA byte order: B high, G mid, R low).
  std::vector<unsigned char> buf(static_cast<size_t>(200) * 150 * 4, 0);
  wkePaint(wv, buf.data(), 200 * 4);
  const size_t c = (static_cast<size_t>(75) * 200 + 100) * 4;
  const bool blue = buf[c] > 200 && buf[c + 1] > 100 && buf[c + 1] < 160 &&
                    buf[c + 2] < 60;
  check(blue, "wkePaint renders the page (blue background pixel)");

  // Input: a left click flips the background to red. Load a page whose onclick
  // recolors the body, fire a down+up at the centre, repaint, and check the pixel
  // turned red (BGRA: R high, G/B low) — verifies wkeFireMouseEvent end to end
  // without needing wkeRunJS yet.
  wkeLoadHTML(wv,
              "<body style='margin:0;background:rgb(0,128,255)' "
              "onclick='document.body.style.background=\"rgb(255,0,0)\"'>x</body>");
  wkeFireMouseEvent(wv, WKE_MSG_LBUTTONDOWN, 100, 75, WKE_LBUTTON);
  wkeFireMouseEvent(wv, WKE_MSG_LBUTTONUP, 100, 75, 0);
  std::fill(buf.begin(), buf.end(), 0);
  wkePaint(wv, buf.data(), 200 * 4);
  const bool red = buf[c + 2] > 200 && buf[c + 1] < 60 && buf[c] < 60;
  check(red, "wkeFireMouseEvent click fires onclick (bg flips to red)");

  // Scripting: wkeRunJS + the jsToXxx readers (string-backed jsValue).
  wkeLoadHTML(wv, "<title>JSDoc</title><body>x</body>");
  jsExecState es = wkeGlobalExec(wv);
  check(jsToInt(es, wkeRunJS(wv, "1+2")) == 3, "wkeRunJS + jsToInt (1+2==3)");
  check(std::strcmp(jsToTempString(es, wkeRunJS(wv, "'hel'+'lo'")), "hello") == 0,
        "wkeRunJS + jsToTempString ('hello')");
  check(jsToDouble(es, wkeRunJS(wv, "7/2")) == 3.5, "wkeRunJS + jsToDouble (3.5)");
  check(jsToBoolean(es, wkeRunJS(wv, "1<2")) &&
            !jsToBoolean(es, wkeRunJS(wv, "1>2")),
        "wkeRunJS + jsToBoolean (1<2 true, 1>2 false)");
  check(std::strcmp(jsToTempString(es, wkeRunJS(wv, "document.title")),
                    "JSDoc") == 0,
        "wkeRunJS reads the DOM (document.title)");

  // Keyboard: type into a focused field, then submit with Enter — verified by
  // reading the field value and the form's submit flag through wkeRunJS.
  wkeLoadHTML(wv,
              "<form onsubmit='window.__sub=1;return false'>"
              "<input id='k'></form>");
  wkeRunJS(wv, "document.getElementById('k').focus()");
  wkeFireKeyPressEvent(wv, 'a', 0, false);
  wkeFireKeyPressEvent(wv, 'b', 0, false);
  check(std::strcmp(jsToTempString(
                        es, wkeRunJS(wv, "document.getElementById('k').value")),
                    "ab") == 0,
        "wkeFireKeyPressEvent types into the focused field ('ab')");
  wkeFireKeyDownEvent(wv, 0x0D, 0, false);  // VK_RETURN -> submit
  check(jsToInt(es, wkeRunJS(wv, "window.__sub|0")) == 1,
        "wkeFireKeyDownEvent Enter triggers the form submit handler");

  // Navigation history: load two distinct file pages, then go back/forward and
  // confirm the title follows. (file:// gives distinct URLs the history records.)
  if (FILE* fa = std::fopen("/tmp/wke_nav_a.html", "wb")) {
    std::fputs("<title>PageA</title><body>a</body>", fa);
    std::fclose(fa);
  }
  if (FILE* fb = std::fopen("/tmp/wke_nav_b.html", "wb")) {
    std::fputs("<title>PageB</title><body>b</body>", fb);
    std::fclose(fb);
  }
  wkeLoadURL(wv, "file:///tmp/wke_nav_a.html");
  wkeLoadURL(wv, "file:///tmp/wke_nav_b.html");
  const bool on_b = std::strcmp(wkeGetTitle(wv), "PageB") == 0;
  const bool can_back = wkeCanGoBack(wv);
  wkeGoBack(wv);
  const bool back_a = std::strcmp(wkeGetTitle(wv), "PageA") == 0;
  const bool can_fwd = wkeCanGoForward(wv);
  wkeGoForward(wv);
  const bool fwd_b = std::strcmp(wkeGetTitle(wv), "PageB") == 0;
  check(on_b && can_back && back_a && can_fwd && fwd_b,
        "wkeGoBack/GoForward navigate the history (A<->B)");

  // Content size: a tall/wide document reports its full scroll size, beyond the
  // 200x150 view.
  wkeResize(wv, 200, 150);
  wkeLoadHTML(wv,
              "<body style='margin:0'><div style='width:400px;height:2000px'>"
              "</div></body>");
  check(wkeGetContentHeight(wv) >= 2000 && wkeGetContentWidth(wv) >= 400,
        "wkeGetContentWidth/Height report the full document size");

  // Transparent background: an unpainted area keeps alpha 0 (BGRA byte 3).
  wkeSetTransparent(wv, true);
  wkeLoadHTML(wv,
              "<body style='margin:0'><div style='width:10px;height:10px;"
              "background:#000'></div></body>");
  std::vector<unsigned char> tb(static_cast<size_t>(200) * 150 * 4, 200);
  wkePaint(wv, tb.data(), 200 * 4);
  const size_t corner = (static_cast<size_t>(140) * 200 + 190) * 4;  // far from box
  check(tb[corner + 3] == 0,
        "wkeSetTransparent: unpainted area keeps alpha 0");

  // Async callbacks: wkeOnLoadingFinish + wkeOnTitleChanged fire after a load,
  // carrying the URL/title as wkeStrings. State travels through the void* param
  // (non-capturing lambdas, so they convert to the C function pointers).
  struct CbState {
    int load = 0;
    int title = 0;
    std::string title_text;
    int result = -1;
  };
  CbState cbs;
  wkeOnLoadingFinish(
      wv,
      [](wkeWebView, void* p, const wkeString /*url*/, wkeLoadingResult r,
         const wkeString) {
        auto* s = static_cast<CbState*>(p);
        s->load = 1;
        s->result = r;
      },
      &cbs);
  wkeOnTitleChanged(wv, [](wkeWebView, void* p, const wkeString title) {
    auto* s = static_cast<CbState*>(p);
    s->title = 1;
    s->title_text = wkeGetString(title);
  }, &cbs);
  wkeLoadHTML(wv, "<title>CbTitle</title><body>cb</body>");
  check(cbs.load == 1 && cbs.result == WKE_LOADING_SUCCEEDED && cbs.title == 1 &&
            cbs.title_text == "CbTitle",
        "wkeOnLoadingFinish + wkeOnTitleChanged fire with URL/title/result");

  // Console callback: capture the page's console.log/error and their levels.
  struct ConState {
    std::string all;
    int saw_error = 0;
  };
  ConState con;
  wkeOnConsole(wv, [](wkeWebView, void* p, wkeConsoleLevel level,
                      const wkeString message, const wkeString, unsigned,
                      const wkeString) {
    auto* s = static_cast<ConState*>(p);
    s->all += wkeGetString(message);
    s->all += "|";
    if (level == wkeLevelError)
      s->saw_error = 1;
  }, &con);
  wkeLoadHTML(wv,
              "<body><script>console.log('hello console');"
              "console.error('boom');</script></body>");
  check(con.all.find("hello console") != std::string::npos &&
            con.all.find("boom") != std::string::npos && con.saw_error,
        "wkeOnConsole captures console.log/error with levels");

  // Document-ready callback + wkeGetSource: the callback fires on load and the
  // source contains the page's (post-JS) markup.
  int doc_ready = 0;
  wkeOnDocumentReady(wv, [](wkeWebView, void* p) { *static_cast<int*>(p) = 1; },
                     &doc_ready);
  wkeLoadHTML(wv, "<body><p id='sg'>source-here</p></body>");
  const char* src = wkeGetSource(wv);
  check(doc_ready == 1 && src && std::strstr(src, "source-here") != nullptr &&
            std::strstr(src, "id=\"sg\"") != nullptr,
        "wkeOnDocumentReady fires + wkeGetSource returns the page HTML");

  // Mouse wheel: a tall page scrolls down (a negative delta) — scrollY increases.
  wkeResize(wv, 200, 150);
  wkeLoadHTML(wv,
              "<body style='margin:0'><div style='height:3000px'></div></body>");
  wkeFireMouseWheelEvent(wv, 100, 75, -300, 0);  // negative delta -> down
  check(jsToInt(es, wkeRunJS(wv, "Math.round(window.scrollY)")) > 0,
        "wkeFireMouseWheelEvent scrolls the page (scrollY > 0)");

  // jsTypeOf: the result type is captured during the single eval.
  check(jsTypeOf(wkeRunJS(wv, "1+2")) == JSTYPE_NUMBER &&
            jsTypeOf(wkeRunJS(wv, "'x'")) == JSTYPE_STRING &&
            jsTypeOf(wkeRunJS(wv, "1<2")) == JSTYPE_BOOLEAN &&
            jsTypeOf(wkeRunJS(wv, "[1,2,3]")) == JSTYPE_ARRAY &&
            jsTypeOf(wkeRunJS(wv, "({a:1})")) == JSTYPE_OBJECT &&
            jsTypeOf(wkeRunJS(wv, "null")) == JSTYPE_NULL &&
            jsTypeOf(wkeRunJS(wv, "undefined")) == JSTYPE_UNDEFINED &&
            jsTypeOf(wkeRunJS(wv, "(function(){})")) == JSTYPE_FUNCTION,
        "jsTypeOf reports number/string/boolean/array/object/null/undefined/function");

  // jsGetLength + jsGetAt: read array elements (incl. nested) via the JS-side
  // slot store — the object-model slice.
  {
    jsValue arr = wkeRunJS(wv, "['ant','bee','cat']");
    const int len = jsGetLength(es, arr);
    jsValue e1 = jsGetAt(es, arr, 1);                 // 'bee'
    jsValue nested = wkeRunJS(wv, "[[10,20],[30,40]]");
    jsValue inner = jsGetAt(es, nested, 1);           // [30,40]
    const int inner0 = jsToInt(es, jsGetAt(es, inner, 0));  // 30
    check(len == 3 && std::strcmp(jsToTempString(es, e1), "bee") == 0 &&
              jsGetLength(es, nested) == 2 &&
              jsTypeOf(inner) == JSTYPE_ARRAY && inner0 == 30,
          "jsGetLength/jsGetAt read array elements (incl. nested)");
  }

  // jsGet (object property by name, incl. nested) + jsGetGlobal (window prop).
  {
    jsValue obj = wkeRunJS(wv, "({name:'Ada',age:36,inner:{n:7}})");
    const bool name_ok =
        std::strcmp(jsToTempString(es, jsGet(es, obj, "name")), "Ada") == 0;
    const bool age_ok = jsToInt(es, jsGet(es, obj, "age")) == 36;
    const bool nested_ok =
        jsToInt(es, jsGet(es, jsGet(es, obj, "inner"), "n")) == 7;
    wkeRunJS(wv, "window.__gv=99");
    const bool global_ok = jsToInt(es, jsGetGlobal(es, "__gv")) == 99;
    check(name_ok && age_ok && nested_ok && global_ok,
          "jsGet reads object properties (nested) + jsGetGlobal reads a global");
  }

  // Network-gated (MB_NET_TESTS=1): wkePostURL posts a body; httpbin echoes the
  // form into the response document.
  if (std::getenv("MB_NET_TESTS")) {
    wkePostURL(wv, "http://httpbin.org/post", "wkk=wval", 8);
    const char* body =
        jsToTempString(es, wkeRunJS(wv, "document.body?document.body.innerText:''"));
    if (wkeIsLoadingSucceeded(wv)) {
      check(std::strstr(body, "wkk") != nullptr &&
                std::strstr(body, "wval") != nullptr,
            "wkePostURL posts a body (httpbin echoes it)");
    } else {
      std::printf("  [SKIP] wkePostURL (host unreachable)\n");
    }
  }

  wkeDestroyWebView(wv);
  wkeFinalize();

  std::printf("wke_smoke: %d passed, %d failed\n", pass, fail);
  return fail == 0 ? 0 : 1;
}
