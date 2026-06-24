// wke_smoke — exercises the wke compatibility slice end to end in its own process:
// init -> create -> resize -> loadHTML -> poll loading state -> read title ->
// paint to a BGRA buffer and check the background pixel -> destroy -> finalize.

#include "wke/wke.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// Capture state for the wkeOnJsBridge test (a C callback can't capture locals).
static int g_bridge_n = 0;
static char g_bridge_channel[256], g_bridge_message[1024];
static void OnBridge(wkeWebView, void*, const utf8* channel,
                     const utf8* message) {
  ++g_bridge_n;
  std::snprintf(g_bridge_channel, sizeof(g_bridge_channel), "%s",
                channel ? channel : "");
  std::snprintf(g_bridge_message, sizeof(g_bridge_message), "%s",
                message ? message : "");
}

// Native function bound via wkeJsBindFunction: sums its integer args + a base
// passed through `param`, returning the total as a jsValue.
static int g_bind_base = 100;
static jsValue WkeAdd(jsExecState es, void* param) {
  int sum = param ? *static_cast<int*>(param) : 0;
  for (int i = 0; i < jsArgCount(es); ++i)
    sum += jsToInt(es, jsArg(es, i));
  return jsInt(sum);
}
// Reports whether its first arg arrived as a JS number (arg-type fidelity).
static jsValue WkeArg0IsNumber(jsExecState es, void*) {
  return jsBoolean(jsIsNumber(jsArg(es, 0)));
}
// Returns its argument count (edge: 0 args, many args).
static jsValue WkeArgc(jsExecState es, void*) {
  return jsInt(jsArgCount(es));
}
// Returns nothing — JS should see undefined.
static jsValue WkeNothing(jsExecState, void*) {
  return jsUndefined();
}

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
  check(jsToInt(es, jsEval(es, "6*7")) == 42,
        "jsEval evaluates via the exec state (6*7==42)");
  {
    jsValue half = wkeRunJS(wv, "7/2");  // jsToFloat keeps the fraction; jsToInt truncates
    check(jsToFloat(es, half) == 3.5f && jsToInt(es, half) == 3,
          "jsToFloat returns 3.5 (vs jsToInt 3)");
  }
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

  // Uncaught exceptions surface through the console channel as error-level
  // messages — the embedder's way to observe a script error (Blink handles the
  // throw internally, so it's not visible via wkeRunJS's result).
  con.all.clear();
  con.saw_error = 0;
  wkeLoadHTML(wv, "<body><script>throw new Error('uncaughtX')</script></body>");
  check(con.all.find("uncaughtX") != std::string::npos && con.saw_error,
        "wkeOnConsole captures an uncaught JS exception (error level)");

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

  // jsCall / jsCallGlobal with constructed args (jsInt/jsString) + a bound `this`.
  {
    jsValue add = wkeRunJS(wv, "(function(a,b){return a+b})");
    jsValue iargs[2] = {jsInt(10), jsInt(32)};
    const bool sum_ok = jsToInt(es, jsCallGlobal(es, add, iargs, 2)) == 42;

    jsValue greet = wkeRunJS(wv, "(function(n){return 'hi '+n})");
    jsValue sargs[1] = {jsString(es, "Ada")};
    const bool str_ok = std::strcmp(
                            jsToTempString(es, jsCallGlobal(es, greet, sargs, 1)),
                            "hi Ada") == 0;

    jsValue obj = wkeRunJS(
        wv, "({base:100,add:function(x){return this.base+x}})");
    jsValue addfn = jsGet(es, obj, "add");
    jsValue cargs[1] = {jsInt(5)};
    const bool this_ok = jsToInt(es, jsCall(es, addfn, obj, cargs, 1)) == 105;

    check(sum_ok && str_ok && this_ok,
          "jsCall/jsCallGlobal invoke functions with constructed args + this");
  }

  // jsGetKeys enumerates own-enumerable property names in Object.keys order.
  {
    jsValue obj = wkeRunJS(wv, "({alpha:1, beta:2, gamma:3})");
    jsKeys* keys = jsGetKeys(es, obj);
    const bool keys_ok = keys && keys->length == 3 &&
                         std::strcmp(keys->keys[0], "alpha") == 0 &&
                         std::strcmp(keys->keys[1], "beta") == 0 &&
                         std::strcmp(keys->keys[2], "gamma") == 0;

    jsValue prim = wkeRunJS(wv, "42");
    jsKeys* empty = jsGetKeys(es, prim);
    const bool empty_ok = empty && empty->length == 0;

    check(keys_ok && empty_ok,
          "jsGetKeys enumerates object property names (empty for non-objects)");
  }

  // jsEmptyObject/jsEmptyArray + jsSet/jsSetAt/jsSetGlobal build values that the
  // read side (jsGet/jsGetAt/jsGetGlobal) and jsCall then observe.
  {
    jsValue obj = jsEmptyObject(es);
    jsSet(es, obj, "name", jsString(es, "Ada"));
    jsSet(es, obj, "n", jsInt(7));
    const bool obj_ok =
        std::strcmp(jsToTempString(es, jsGet(es, obj, "name")), "Ada") == 0 &&
        jsToInt(es, jsGet(es, obj, "n")) == 7;

    jsValue arr = jsEmptyArray(es);
    jsSetAt(es, arr, 0, jsInt(10));
    jsSetAt(es, arr, 1, jsInt(20));
    const bool arr_ok = jsGetLength(es, arr) == 2 &&
                        jsToInt(es, jsGetAt(es, arr, 0)) == 10 &&
                        jsToInt(es, jsGetAt(es, arr, 1)) == 20;

    jsSetGlobal(es, "mbBuilt", jsInt(99));
    const bool glob_ok = jsToInt(es, jsGetGlobal(es, "mbBuilt")) == 99;

    // The built object survives a round-trip through jsCall as an argument.
    jsValue fn = wkeRunJS(wv, "(function(o){return o.name+':'+o.n})");
    jsValue cargs[1] = {obj};
    const bool call_ok =
        std::strcmp(jsToTempString(es, jsCallGlobal(es, fn, cargs, 1)),
                    "Ada:7") == 0;

    check(obj_ok && arr_ok && glob_ok && call_ok,
          "jsEmptyObject/Array + jsSet/jsSetAt/jsSetGlobal build & pass values");
  }

  // Cookies (offline): a fresh view's current doc (about:blank) has no jar
  // cookies; the setter + clear-command exercise their paths without crashing.
  {
    const bool empty_ok = std::strcmp(wkeGetCookie(wv), "") == 0;
    wkeSetCookie(wv, "http://wke.test/", "wkesid=abc123");  // injects into jar
    wkePerformCookieCommand(wkeCookieCommandClearAllCookies);  // null-safe drive
    const bool still_empty = std::strcmp(wkeGetCookie(wv), "") == 0;  // not page url
    check(empty_ok && still_empty,
          "wkeGetCookie/wkeSetCookie/wkePerformCookieCommand are safe + consistent");
  }

  // wkeGetAllCookie (offline): the whole jar (Netscape format) lists an injected
  // cookie regardless of the current document URL (unlike wkeGetCookie).
  {
    wkeSetCookie(wv, "http://alljar.test/",
                 "ajk=allval123; expires=Fri, 31 Dec 2027 23:59:59 GMT");
    const char* all = wkeGetAllCookie(wv);
    const bool ok = std::strstr(all, "# Netscape") != nullptr &&
                    std::strstr(all, "alljar.test") != nullptr &&
                    std::strstr(all, "ajk") != nullptr &&
                    std::strstr(all, "allval123") != nullptr;
    wkePerformCookieCommand(wkeCookieCommandClearAllCookies);  // reset jar
    check(ok, "wkeGetAllCookie dumps the whole jar (Netscape format)");
  }

  // Cookie persistence (offline): inject a cookie, flush the jar to a file, and
  // inspect the file; then clear + reload and re-flush to prove it round-trips.
  {
    auto slurp = [](const char* path) -> std::string {
      std::string out;
      if (FILE* f = std::fopen(path, "rb")) {
        char b[4096];
        size_t n;
        while ((n = std::fread(b, 1, sizeof(b), f)) > 0)
          out.append(b, n);
        std::fclose(f);
      }
      return out;
    };
    const char* jar1 = "/private/tmp/claude-501/wke_jar1.txt";
    const char* jar2 = "/private/tmp/claude-501/wke_jar2.txt";
    std::remove(jar1);
    std::remove(jar2);

    wkeSetCookie(wv, "http://persist.test/",
                 "psid=jar987; expires=Fri, 31 Dec 2027 23:59:59 GMT");
    wkeSetCookieJarPath(wv, jar1);
    wkePerformCookieCommand(wkeCookieCommandFlushCookiesToFile);
    const bool flushed = slurp(jar1).find("jar987") != std::string::npos;

    wkePerformCookieCommand(wkeCookieCommandClearAllCookies);
    wkePerformCookieCommand(wkeCookieCommandReloadCookiesFromFile);
    wkeSetCookieJarPath(wv, jar2);
    wkePerformCookieCommand(wkeCookieCommandFlushCookiesToFile);
    const bool reloaded = slurp(jar2).find("jar987") != std::string::npos;

    std::remove(jar1);
    std::remove(jar2);
    check(flushed && reloaded,
          "wkeSetCookieJarPath + Flush/Reload persist the jar to/from a file");
  }

  // wkeSetProxy (offline): null / WKE_PROXY_NONE are safe and force a direct
  // connection, leaving local (non-network) loads working.
  {
    wkeSetProxy(nullptr);  // null-safe -> direct
    wkeProxy direct;
    std::memset(&direct, 0, sizeof(direct));
    direct.type = WKE_PROXY_NONE;
    wkeSetProxy(&direct);
    wkeLoadHTML(wv, "<title>ProxyOK</title><body>p</body>");
    check(wkeIsLoadingSucceeded(wv) &&
              std::strcmp(wkeGetTitle(wv), "ProxyOK") == 0,
          "wkeSetProxy(null/NONE) is safe and leaves local loads working");
  }

  // Pure view-state: transparent mirror, name, and the app key/value store.
  {
    // The mirror tracks both directions (prior tests may have left it set).
    wkeSetTransparent(wv, true);
    const bool now_transparent = wkeIsTransparent(wv);
    wkeSetTransparent(wv, false);
    const bool now_opaque = !wkeIsTransparent(wv);

    const bool default_name = std::strcmp(wkeGetName(wv), "") == 0;
    wkeSetName(wv, "miniwin");
    const bool name_ok = std::strcmp(wkeGetName(wv), "miniwin") == 0;

    int slot = 7;
    const bool unset_null = wkeGetUserKeyValue(wv, "ctx") == nullptr;
    wkeSetUserKeyValue(wv, "ctx", &slot);
    const bool kv_ok = wkeGetUserKeyValue(wv, "ctx") == &slot &&
                       *static_cast<int*>(wkeGetUserKeyValue(wv, "ctx")) == 7;

    check(now_transparent && now_opaque && default_name && name_ok &&
              unset_null && kv_ok,
          "wkeIsTransparent/wkeSetName/wkeSetUserKeyValue track view-state");
  }

  // wkeSetZoomFactor scales layout observably (getBoundingClientRect) and the
  // factor persists across a navigation.
  {
    const char* page =
        "<body style='margin:0'>"
        "<div id='d' style='width:100px;height:10px'></div></body>";
    auto divw = [&]() {
      return jsToInt(es, wkeRunJS(wv, "Math.round(document.getElementById('d')"
                                      ".getBoundingClientRect().width)"));
    };
    wkeSetZoomFactor(wv, 1.0f);
    wkeLoadHTML(wv, page);
    const bool base_ok = divw() == 100 && wkeGetZoomFactor(wv) == 1.0f;

    wkeSetZoomFactor(wv, 2.0f);
    const bool zoomed = divw() == 200 && wkeGetZoomFactor(wv) == 2.0f;

    wkeLoadHTML(wv, page);  // a fresh document — zoom must re-apply
    const bool persists = divw() == 200;

    wkeSetZoomFactor(wv, 1.0f);  // restore so later loads aren't zoomed
    check(base_ok && zoomed && persists,
          "wkeSetZoomFactor scales layout + persists across navigations");
  }

  // wkeSetEditable toggles whole-document editability (designMode) and persists.
  {
    auto designMode = [&]() {
      return jsToTempString(es, wkeRunJS(wv, "document.designMode"));
    };
    wkeLoadHTML(wv, "<body>edit me</body>");
    const bool off0 = std::strcmp(designMode(), "off") == 0;

    wkeSetEditable(wv, true);
    const bool on1 = std::strcmp(designMode(), "on") == 0;
    const bool dom_ok = jsToBoolean(
        es, wkeRunJS(wv, "document.body.isContentEditable"));

    wkeLoadHTML(wv, "<body>again</body>");  // fresh doc — must re-apply
    const bool on2 = std::strcmp(designMode(), "on") == 0;

    wkeSetEditable(wv, false);
    const bool off1 = std::strcmp(designMode(), "off") == 0;

    check(off0 && on1 && dom_ok && on2 && off1,
          "wkeSetEditable toggles document editability + persists across loads");
  }

  // wkeSetExtraHeaders (offline): set/clear/null are safe and leave local loads
  // working (the request-echo proof is network-gated below).
  {
    wkeSetExtraHeaders(wv, "X-Wke-A: 1\nX-Wke-B: 2");
    wkeSetExtraHeaders(wv, nullptr);  // clear
    wkeLoadHTML(wv, "<title>HdrOK</title><body>h</body>");
    check(wkeIsLoadingSucceeded(wv) &&
              std::strcmp(wkeGetTitle(wv), "HdrOK") == 0,
          "wkeSetExtraHeaders(set/clear/null) is safe; local loads still work");
  }

  // wkeSetDarkMode (offline): prefers-color-scheme flips a media query and the
  // computed style that depends on it. Set before loading to apply to that doc.
  {
    const char* page =
        "<style>#d{color:rgb(1,1,1)}"
        "@media (prefers-color-scheme:dark){#d{color:rgb(2,2,2)}}</style>"
        "<body><div id='d'>x</div></body>";
    auto matches = [&]() {
      return jsToBoolean(
          es, wkeRunJS(wv, "matchMedia('(prefers-color-scheme:dark)').matches"));
    };
    auto color = [&]() {
      return jsToTempString(
          es, wkeRunJS(wv, "getComputedStyle(document.getElementById('d')).color"));
    };
    wkeSetDarkMode(wv, false);
    wkeLoadHTML(wv, page);
    const bool light_ok = !matches() && std::strcmp(color(), "rgb(1, 1, 1)") == 0;

    wkeSetDarkMode(wv, true);
    wkeLoadHTML(wv, page);
    const bool dark_ok = matches() && std::strcmp(color(), "rgb(2, 2, 2)") == 0;

    wkeSetDarkMode(wv, false);  // restore for any later cases
    check(light_ok && dark_ok,
          "wkeSetDarkMode drives prefers-color-scheme (CSS flips dark/light)");
  }

  // wkeSetLocale + wkeSetTimezone (offline): navigator.language(s) and the
  // Date/Intl timezone reflect the emulated i18n environment.
  {
    wkeSetLocale(wv, "fr-FR,fr,en");
    wkeLoadHTML(wv, "<body>x</body>");
    const bool loc_ok =
        std::strcmp(jsToTempString(es, wkeRunJS(wv, "navigator.language")),
                    "fr-FR") == 0 &&
        std::strcmp(
            jsToTempString(es, wkeRunJS(wv, "navigator.languages.join(',')")),
            "fr-FR,fr,en") == 0;
    wkeSetLocale(wv, "en-US");  // restore

    wkeSetTimezone(wv, "America/New_York");
    wkeLoadHTML(wv, "<body>x</body>");
    const bool tz_ok =
        std::strcmp(
            jsToTempString(
                es, wkeRunJS(wv, "Intl.DateTimeFormat().resolvedOptions()"
                                 ".timeZone")),
            "America/New_York") == 0 &&
        // 2021-01-01T00:00:00Z -> 2020-12-31 19:00 EST proves Date uses the zone.
        jsToInt(es, wkeRunJS(wv, "new Date(1609459200000).getHours()")) == 19;
    wkeSetTimezone(wv, "UTC");  // restore process-global determinism

    check(loc_ok && tz_ok,
          "wkeSetLocale/wkeSetTimezone drive navigator.language(s) + Intl/Date");
  }

  // wkeSetInitScript (offline): runs before the page's own scripts, so the
  // page's inline script observes the injected global; clearing stops it.
  {
    const char* page =
        "<body><script>window.__pageSaw=window.__early||'no';</script></body>";
    wkeSetInitScript(wv, "window.__early='injected';");
    wkeLoadHTML(wv, page);
    const bool injected =
        std::strcmp(jsToTempString(es, wkeRunJS(wv, "window.__pageSaw")),
                    "injected") == 0;

    wkeSetInitScript(wv, nullptr);  // clear
    wkeLoadHTML(wv, page);
    const bool cleared =
        std::strcmp(jsToTempString(es, wkeRunJS(wv, "window.__pageSaw")),
                    "no") == 0;

    check(injected && cleared,
          "wkeSetInitScript runs before page scripts (evaluateOnNewDocument)");
  }

  // wkeSavePdf (offline): printing a document yields a real, non-trivial PDF.
  {
    const char* pdf = "/private/tmp/claude-501/wke_smoke.pdf";
    std::remove(pdf);
    wkeLoadHTML(wv, "<body style='font:30px sans-serif'>"
                    "<h1>PDF</h1><p>page content</p></body>");
    const bool wrote = wkeSavePdf(wv, pdf);
    char hdr[6] = {0};
    long sz = 0;
    if (FILE* f = std::fopen(pdf, "rb")) {
      std::fread(hdr, 1, 5, f);
      std::fseek(f, 0, SEEK_END);
      sz = std::ftell(f);
      std::fclose(f);
    }
    const bool nullsafe = !wkeSavePdf(wv, nullptr) && !wkeSavePdf(nullptr, pdf);
    std::remove(pdf);
    check(wrote && std::strcmp(hdr, "%PDF-") == 0 && sz > 500 && nullsafe,
          "wkeSavePdf prints a valid, non-trivial %PDF file");
  }

  // wkeSavePng (offline): saves a real PNG, and a .jpg extension yields JPEG.
  {
    const char* png = "/private/tmp/claude-501/wke_smoke.png";
    const char* jpg = "/private/tmp/claude-501/wke_smoke.jpg";
    std::remove(png);
    std::remove(jpg);
    wkeLoadHTML(wv, "<body style='background:#3050ff'>shot</body>");

    const bool wrote_png = wkeSavePng(wv, png, 200, 100);
    unsigned char sig[8] = {0};
    long psz = 0;
    if (FILE* f = std::fopen(png, "rb")) {
      std::fread(sig, 1, 8, f);
      std::fseek(f, 0, SEEK_END);
      psz = std::ftell(f);
      std::fclose(f);
    }
    const bool png_ok = wrote_png && sig[0] == 0x89 && sig[1] == 'P' &&
                        sig[2] == 'N' && sig[3] == 'G' && psz > 100;

    const bool wrote_jpg = wkeSavePng(wv, jpg, 200, 100);  // extension -> JPEG
    unsigned char jsig[2] = {0};
    if (FILE* f = std::fopen(jpg, "rb")) {
      std::fread(jsig, 1, 2, f);
      std::fclose(f);
    }
    const bool jpg_ok = wrote_jpg && jsig[0] == 0xFF && jsig[1] == 0xD8;

    const bool nullsafe =
        !wkeSavePng(wv, nullptr, 200, 100) && !wkeSavePng(nullptr, png, 200, 100);
    std::remove(png);
    std::remove(jpg);
    check(png_ok && jpg_ok && nullsafe,
          "wkeSavePng writes a PNG (and JPEG by extension)");
  }

  // wkeSavePngRect (offline): a rect capture yields a PNG whose IHDR dimensions
  // (big-endian at byte offsets 16/20) equal the requested w x h (dsf=1).
  {
    const char* png = "/private/tmp/claude-501/wke_rect.png";
    std::remove(png);
    wkeLoadHTML(wv, "<body style='margin:0;background:#10c040'>rect</body>");
    const bool wrote = wkeSavePngRect(wv, png, 5, 5, 120, 80);
    unsigned char d[24] = {0};
    size_t n = 0;
    if (FILE* f = std::fopen(png, "rb")) {
      n = std::fread(d, 1, 24, f);
      std::fclose(f);
    }
    const bool magic = n >= 24 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' &&
                       d[3] == 'G';
    int iw = 0, ih = 0;
    if (magic) {
      iw = (d[16] << 24) | (d[17] << 16) | (d[18] << 8) | d[19];
      ih = (d[20] << 24) | (d[21] << 16) | (d[22] << 8) | d[23];
    }
    const bool nullsafe = !wkeSavePngRect(wv, nullptr, 0, 0, 10, 10) &&
                          !wkeSavePngRect(nullptr, png, 0, 0, 10, 10);
    std::remove(png);
    check(wrote && magic && iw == 120 && ih == 80 && nullsafe,
          "wkeSavePngRect captures a logical rect at the requested size");
  }

  // wkeSetDeviceScaleFactor (offline): devicePixelRatio reports the scale and a
  // rect capture rasterizes at scale x (a 100x60 logical rect -> 200x120 px).
  {
    const char* png = "/private/tmp/claude-501/wke_hidpi.png";
    std::remove(png);
    wkeLoadHTML(wv, "<body style='margin:0;background:#204080'>hidpi</body>");
    wkeSetDeviceScaleFactor(wv, 2.0f);
    const bool dpr_ok =
        jsToInt(es, wkeRunJS(wv, "window.devicePixelRatio")) == 2;

    const bool wrote = wkeSavePngRect(wv, png, 0, 0, 100, 60);
    unsigned char d[24] = {0};
    size_t n = 0;
    if (FILE* f = std::fopen(png, "rb")) {
      n = std::fread(d, 1, 24, f);
      std::fclose(f);
    }
    const bool magic = n >= 24 && d[0] == 0x89 && d[1] == 'P';
    int iw = 0, ih = 0;
    if (magic) {
      iw = (d[16] << 24) | (d[17] << 16) | (d[18] << 8) | d[19];
      ih = (d[20] << 24) | (d[21] << 16) | (d[22] << 8) | d[23];
    }
    wkeSetDeviceScaleFactor(wv, 1.0f);  // restore 1:1 for any later capture
    std::remove(png);
    check(dpr_ok && wrote && iw == 200 && ih == 120,
          "wkeSetDeviceScaleFactor scales devicePixelRatio + raster output (2x)");
  }

  // wkeScrollTo (offline): an absolute scroll moves window.scrollY to the offset.
  {
    wkeLoadHTML(wv,
                "<body style='margin:0'><div style='height:3000px'></div></body>");
    auto scrollY = [&]() {
      return jsToInt(es, wkeRunJS(wv, "Math.round(window.scrollY)"));
    };
    wkeScrollTo(wv, 0, 250);
    const bool at_250 = scrollY() == 250;
    wkeScrollTo(wv, 0, 0);
    const bool at_0 = scrollY() == 0;
    check(at_250 && at_0,
          "wkeScrollTo moves the viewport to an absolute offset");
  }

  // wkeEncodePng (offline): render to in-memory PNG bytes (no temp file); verify
  // the signature and that the IHDR width/height match the requested size.
  {
    wkeLoadHTML(wv, "<body style='background:#fff'>encode</body>");
    const unsigned char* data = nullptr;
    const int len = wkeEncodePng(wv, 160, 90, &data);
    const bool magic = len > 24 && data && data[0] == 0x89 && data[1] == 'P' &&
                       data[2] == 'N' && data[3] == 'G' && data[4] == 0x0D &&
                       data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A;
    int iw = 0, ih = 0;
    if (magic) {
      iw = (data[16] << 24) | (data[17] << 16) | (data[18] << 8) | data[19];
      ih = (data[20] << 24) | (data[21] << 16) | (data[22] << 8) | data[23];
    }
    const unsigned char* unused = nullptr;
    const bool nullsafe = wkeEncodePng(nullptr, 160, 90, &unused) == 0 &&
                          wkeEncodePng(wv, 160, 90, nullptr) == 0;
    check(magic && iw == 160 && ih == 90 && nullsafe,
          "wkeEncodePng returns an in-memory PNG of the requested size");
  }

  // DOM query helpers (offline): count, per-element text (incl. :nth-of-type),
  // and attribute reads, plus "" on a miss.
  {
    wkeLoadHTML(
        wv,
        "<body><h1 class='t'>Hi</h1>"
        "<a id='lnk' href='https://ex.com/p'>L</a>"
        "<ul><li class='it'>a</li><li class='it'>b</li><li class='it'>c</li>"
        "</ul></body>");
    const bool count_ok =
        wkeCountSelector(wv, ".it") == 3 && wkeCountSelector(wv, ".none") == 0 &&
        wkeCountSelector(wv, nullptr) == -1;
    const bool text_ok =
        std::strcmp(wkeGetTextForSelector(wv, "h1.t"), "Hi") == 0;
    const bool nth_ok =
        std::strcmp(wkeGetTextForSelector(wv, ".it:nth-of-type(2)"), "b") == 0;
    const bool attr_ok = std::strcmp(wkeGetAttribute(wv, "#lnk", "href"),
                                     "https://ex.com/p") == 0;
    const bool miss_ok =
        std::strcmp(wkeGetTextForSelector(wv, ".none"), "") == 0 &&
        std::strcmp(wkeGetAttribute(wv, "#lnk", "nope"), "") == 0;
    check(count_ok && text_ok && nth_ok && attr_ok && miss_ok,
          "wkeCountSelector/wkeGetTextForSelector/wkeGetAttribute scrape the DOM");
  }

  // wkeGetElementRect (offline): the bbox of a positioned div matches its CSS,
  // NULL out-params are tolerated, and a non-match returns false.
  {
    wkeLoadHTML(wv, "<body style='margin:0'><div id='box' style='position:"
                    "absolute;left:30px;top:40px;width:120px;height:60px'>"
                    "</div></body>");
    int x = 0, y = 0, w = 0, h = 0;
    const bool rect_ok = wkeGetElementRect(wv, "#box", &x, &y, &w, &h) &&
                         x == 30 && y == 40 && w == 120 && h == 60;
    const bool partial = wkeGetElementRect(wv, "#box", nullptr, nullptr, &w,
                                           nullptr);  // NULLs tolerated
    const bool miss = !wkeGetElementRect(wv, "#none", &x, &y, &w, &h);
    check(rect_ok && partial && miss,
          "wkeGetElementRect returns the element's bounding box");
  }

  // wkeGetComputedStyle (offline): resolved color + display values; "" on miss.
  {
    wkeLoadHTML(wv, "<body><div id='d' style='color:rgb(1,2,3);display:none'>"
                    "x</div></body>");
    const bool color_ok =
        std::strcmp(wkeGetComputedStyle(wv, "#d", "color"), "rgb(1, 2, 3)") == 0;
    const bool disp_ok =
        std::strcmp(wkeGetComputedStyle(wv, "#d", "display"), "none") == 0;
    const bool miss_ok =
        std::strcmp(wkeGetComputedStyle(wv, "#none", "color"), "") == 0;
    check(color_ok && disp_ok && miss_ok,
          "wkeGetComputedStyle returns resolved CSS values");
  }

  // DOM actions (offline): click fires a handler, fill sets value + fires input,
  // select changes the <select> value; misses return false.
  {
    wkeLoadHTML(
        wv,
        "<body><button id='go' onclick='window.__c=(window.__c||0)+1'>go</button>"
        "<input id='name' oninput='window.__io=(window.__io||0)+1'>"
        "<select id='sel'><option value='x'>X</option>"
        "<option value='y'>Y</option></select></body>");

    const bool click_ok =
        wkeClickSelector(wv, "#go") && !wkeClickSelector(wv, "#none");
    const bool clicked = jsToInt(es, wkeRunJS(wv, "window.__c||0")) == 1;

    const bool fill_ok = wkeFillSelector(wv, "#name", "Ada Lovelace") &&
                         !wkeFillSelector(wv, "#none", "x");
    const bool filled =
        std::strcmp(jsToTempString(
                        es, wkeRunJS(wv, "document.getElementById('name').value")),
                    "Ada Lovelace") == 0 &&
        jsToInt(es, wkeRunJS(wv, "window.__io||0")) >= 1;

    const bool sel_ok =
        wkeSelectOption(wv, "#sel", "y") && !wkeSelectOption(wv, "#sel", "zzz");
    const bool selected = std::strcmp(
        jsToTempString(es, wkeRunJS(wv, "document.getElementById('sel').value")),
        "y") == 0;

    check(click_ok && clicked && fill_ok && filled && sel_ok && selected,
          "wkeClickSelector/wkeFillSelector/wkeSelectOption drive the page");
  }

  // wkeScrollIntoView (offline): a target far below the fold scrolls into the
  // viewport (scrollY rises from 0, and the element's box lands within height).
  {
    wkeLoadHTML(wv, "<body style='margin:0'><div style='height:3000px'></div>"
                    "<div id='t' style='height:50px'>target</div></body>");
    wkeScrollTo(wv, 0, 0);
    const int before = jsToInt(es, wkeRunJS(wv, "Math.round(window.scrollY)"));
    const bool acted = wkeScrollIntoView(wv, "#t");
    const int after = jsToInt(es, wkeRunJS(wv, "Math.round(window.scrollY)"));
    int ty = -1;
    wkeGetElementRect(wv, "#t", nullptr, &ty, nullptr, nullptr);
    const bool in_view = ty >= 0 && ty < 600;  // default view height
    const bool miss = !wkeScrollIntoView(wv, "#none");
    check(acted && before == 0 && after > 0 && in_view && miss,
          "wkeScrollIntoView brings an element into the viewport");
  }

  // Pointer/focus selector actions (offline): hover/dblclick/contextmenu fire
  // their handlers; focus/blur move document.activeElement; misses return false.
  {
    wkeLoadHTML(wv,
                "<body><div id='h' onmouseover='window.__h=1'>h</div>"
                "<div id='d' ondblclick='window.__d=1'>d</div>"
                "<div id='r' oncontextmenu='window.__r=1'>r</div>"
                "<input id='f'></body>");
    auto flag = [&](const char* g) {
      return jsToInt(es, wkeRunJS(wv, g)) == 1;
    };
    const bool hover = wkeHoverSelector(wv, "#h") && flag("window.__h||0");
    const bool dbl = wkeDoubleClickSelector(wv, "#d") && flag("window.__d||0");
    const bool rc = wkeRightClickSelector(wv, "#r") && flag("window.__r||0");
    const bool foc =
        wkeFocusSelector(wv, "#f") &&
        std::strcmp(jsToTempString(es, wkeRunJS(wv, "document.activeElement.id")),
                    "f") == 0;
    const bool blu =
        wkeBlurSelector(wv, "#f") &&
        std::strcmp(jsToTempString(es, wkeRunJS(wv, "document.activeElement.id")),
                    "f") != 0;
    const bool miss =
        !wkeHoverSelector(wv, "#none") && !wkeFocusSelector(wv, "#none");
    check(hover && dbl && rc && foc && blu && miss,
          "wkeHover/DoubleClick/RightClick/Focus/BlurSelector dispatch events");
  }

  // wkeGetText (offline): visible innerText — includes rendered text, excludes
  // markup and <script> contents.
  {
    wkeLoadHTML(wv, "<body><h1>Title</h1><p>Hello world</p>"
                    "<script>var x='SCRIPTTEXT'</script></body>");
    const char* t = wkeGetText(wv);
    const bool ok = std::strstr(t, "Title") != nullptr &&
                    std::strstr(t, "Hello world") != nullptr &&
                    std::strstr(t, "SCRIPTTEXT") == nullptr &&
                    std::strstr(t, "<h1>") == nullptr;
    check(ok, "wkeGetText returns the page's visible text (no markup/scripts)");
  }

  // jsIs* type predicates (offline): one value of each JS type.
  {
    wkeLoadHTML(wv, "<body>types</body>");
    jsValue n = wkeRunJS(wv, "42");
    jsValue s = wkeRunJS(wv, "'hi'");
    jsValue bt = wkeRunJS(wv, "true");
    jsValue bf = wkeRunJS(wv, "false");
    jsValue o = wkeRunJS(wv, "({a:1})");
    jsValue a = wkeRunJS(wv, "[1,2]");
    jsValue f = wkeRunJS(wv, "(function(){})");
    jsValue u = wkeRunJS(wv, "undefined");
    jsValue nl = wkeRunJS(wv, "null");
    const bool ok =
        jsIsNumber(n) && jsIsString(s) && jsIsBoolean(bt) && jsIsObject(o) &&
        jsIsArray(a) && jsIsFunction(f) && jsIsUndefined(u) && jsIsNull(nl) &&
        jsIsTrue(bt) && jsIsFalse(bf) &&
        // negatives: cross-type and array-vs-object distinction
        !jsIsNumber(s) && !jsIsObject(a) && !jsIsArray(o) && !jsIsTrue(bf) &&
        !jsIsFalse(bt) && !jsIsNull(u);
    check(ok, "jsIsNumber/String/Boolean/Object/Array/Function/Undefined/Null/"
              "True/False classify values");
  }

  // wkeSetFocus/wkeKillFocus (offline): window focus toggles document.hasFocus().
  {
    wkeLoadHTML(wv, "<body>focus</body>");
    wkeSetFocus(wv);
    const bool focused = jsToBoolean(es, wkeRunJS(wv, "document.hasFocus()"));
    wkeKillFocus(wv);
    const bool blurred = !jsToBoolean(es, wkeRunJS(wv, "document.hasFocus()"));
    wkeSetFocus(wv);  // restore (focus-dependent later cases expect it)
    const bool refocused = jsToBoolean(es, wkeRunJS(wv, "document.hasFocus()"));
    check(focused && blurred && refocused,
          "wkeSetFocus/wkeKillFocus toggle document.hasFocus()");
  }

  // wkeJsBindFunction (offline): a bound C function is callable from JS
  // synchronously — reads args via jsArg/jsArgCount, returns a jsValue inline.
  {
    wkeJsBindFunction(wv, "wkeAdd", WkeAdd, &g_bind_base);
    wkeJsBindFunction(wv, "wkeIsNum", WkeArg0IsNumber, nullptr);
    wkeJsBindFunction(wv, "wkeArgc", WkeArgc, nullptr);
    wkeJsBindFunction(wv, "wkeNothing", WkeNothing, nullptr);
    wkeLoadHTML(wv, "<body>bind</body>");  // installs the bound functions
    const bool defined =
        std::strcmp(jsToTempString(es, wkeRunJS(wv, "typeof window.wkeAdd")),
                    "function") == 0;
    // 2 + 3 + base(100) = 105, returned inline as a real NUMBER (===, not "105").
    const bool typed = jsToBoolean(
        es, wkeRunJS(wv, "window.wkeAdd(2,3)===105 && "
                         "typeof window.wkeAdd(2,3)==='number'"));
    const bool arith =
        jsToInt(es, wkeRunJS(wv, "window.wkeAdd(2,3)+1")) == 106;
    // Arg types flow through: jsArg(0) reflects the real JS type.
    const bool args_typed =
        jsToBoolean(es, wkeRunJS(wv, "window.wkeIsNum(5)")) &&
        !jsToBoolean(es, wkeRunJS(wv, "window.wkeIsNum('x')")) &&
        !jsToBoolean(es, wkeRunJS(wv, "window.wkeIsNum(true)"));
    // Edge cases: 0-arg and many-arg calls (jsArgCount), and an undefined return.
    const bool argc_ok = jsToInt(es, wkeRunJS(wv, "window.wkeArgc()")) == 0 &&
                         jsToInt(es, wkeRunJS(wv, "window.wkeArgc(1,2,3,4)")) == 4;
    const bool undef_ok = jsToBoolean(
        es, wkeRunJS(wv, "typeof window.wkeNothing()==='undefined'"));
    check(defined && typed && arith && args_typed && argc_ok && undef_ok,
          "wkeJsBindFunction: typed args/return + 0/N-arg + undefined edge cases");
  }

  // wkeOnJsBridge (offline): window.mbBridge(channel,message) is installed before
  // page scripts and delivers calls to the host callback (one-way page->host).
  {
    wkeOnJsBridge(wv, OnBridge, nullptr);
    wkeLoadHTML(wv, "<body>bridge</body>");  // init script defines window.mbBridge
    const bool defined =
        std::strcmp(jsToTempString(es, wkeRunJS(wv, "typeof window.mbBridge")),
                    "function") == 0;
    g_bridge_n = 0;
    wkeRunJS(wv, "window.mbBridge('greet', 'hello')");  // drained post-run
    const bool got = g_bridge_n == 1 &&
                     std::strcmp(g_bridge_channel, "greet") == 0 &&
                     std::strcmp(g_bridge_message, "hello") == 0;

    // Realistic path: a page event handler calls window.mbBridge; the selector
    // action drains it, so the host hears it with no manual wkeRunJS.
    wkeLoadHTML(wv, "<body><button id='b' onclick=\"window.mbBridge('click',"
                    "'fired')\">b</button></body>");
    g_bridge_n = 0;
    wkeClickSelector(wv, "#b");
    const bool from_handler = g_bridge_n == 1 &&
                              std::strcmp(g_bridge_channel, "click") == 0 &&
                              std::strcmp(g_bridge_message, "fired") == 0;

    wkeOnJsBridge(wv, nullptr, nullptr);  // unregister (removes the bootstrap)
    check(defined && got && from_handler,
          "wkeOnJsBridge delivers window.mbBridge from eval AND event handlers");
  }

  // jsToString (offline): JSON for objects/arrays, coerced value for primitives.
  {
    wkeLoadHTML(wv, "<body>str</body>");
    const bool obj_json = std::strcmp(jsToString(es, wkeRunJS(wv, "({a:1,b:'x'})")),
                                      "{\"a\":1,\"b\":\"x\"}") == 0;
    const bool arr_json =
        std::strcmp(jsToString(es, wkeRunJS(wv, "[1,2,3]")), "[1,2,3]") == 0;
    const bool num_ok =
        std::strcmp(jsToString(es, wkeRunJS(wv, "42")), "42") == 0;
    const bool str_ok =
        std::strcmp(jsToString(es, wkeRunJS(wv, "'hi'")), "hi") == 0;
    check(obj_json && arr_json && num_ok && str_ok,
          "jsToString JSON-serializes objects/arrays, coerces primitives");
  }

  // HTTP introspection (offline): a non-http load reports status 0 + no headers.
  {
    wkeLoadHTML(wv, "<body>local</body>");
    check(wkeGetHttpStatusCode(wv) == 0 &&
              std::strcmp(wkeGetResponseHeaders(wv), "") == 0,
          "wkeGetHttpStatusCode/wkeGetResponseHeaders are 0/empty for non-http");
  }

  // wkeSetFollowRedirects (offline): toggling it is safe and leaves local loads
  // working (the redirect-stops-at-30x proof is network-gated below).
  {
    wkeSetFollowRedirects(false);
    wkeSetFollowRedirects(true);  // restore default
    wkeLoadHTML(wv, "<title>RedirOK</title><body>r</body>");
    check(wkeIsLoadingSucceeded(wv) &&
              std::strcmp(wkeGetTitle(wv), "RedirOK") == 0,
          "wkeSetFollowRedirects toggle is safe; local loads still work");
  }

  // wkeSetLoadImages (offline): toggling is safe and leaves local loads working
  // (the network image-skip proof is network-gated below).
  {
    wkeSetLoadImages(wv, false);
    wkeSetLoadImages(wv, true);  // restore default
    wkeLoadHTML(wv, "<title>ImgOK</title><body>i</body>");
    check(wkeIsLoadingSucceeded(wv) &&
              std::strcmp(wkeGetTitle(wv), "ImgOK") == 0,
          "wkeSetLoadImages toggle is safe; local loads still work");
  }

  // wkeSetIgnoreCertErrors (offline): toggling is safe and leaves local loads
  // working (the self-signed-cert proof is network-gated below).
  {
    wkeSetIgnoreCertErrors(true);
    wkeSetIgnoreCertErrors(false);  // restore secure default
    wkeLoadHTML(wv, "<title>CertOK</title><body>c</body>");
    check(wkeIsLoadingSucceeded(wv) &&
              std::strcmp(wkeGetTitle(wv), "CertOK") == 0,
          "wkeSetIgnoreCertErrors toggle is safe; local loads still work");
  }

  // Waits (offline): a setTimeout adds a delayed element / flag; the wait pumps
  // until it appears, and times out on a condition that never holds.
  {
    wkeLoadHTML(wv,
                "<body><script>setTimeout(function(){"
                "var d=document.createElement('div');d.id='ready';"
                "document.body.appendChild(d);},50);</script></body>");
    const bool sel_ok = wkeWaitForSelector(wv, "#ready", 4000) &&
                        !wkeWaitForSelector(wv, "#never", 100);

    wkeLoadHTML(wv, "<body><script>setTimeout(function(){"
                    "window.__ready2=1;},50);</script></body>");
    const bool fn_ok = wkeWaitForFunction(wv, "window.__ready2===1", 2000) &&
                       !wkeWaitForFunction(wv, "window.__never", 60);

    check(sel_ok && fn_ok,
          "wkeWaitForSelector/wkeWaitForFunction resolve on condition + time out");
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

    // Cookie round-trip: httpbin sets a cookie, then wkeGetCookie reads it back
    // from the jar for the (now httpbin) current URL; the clear command drops it.
    wkeLoadURL(wv, "http://httpbin.org/cookies/set?wkeck=ok42");
    if (wkeIsLoadingSucceeded(wv)) {
      const bool set_ok = std::strstr(wkeGetCookie(wv), "wkeck=ok42") != nullptr;
      wkePerformCookieCommand(wkeCookieCommandClearAllCookies);
      const bool cleared = std::strstr(wkeGetCookie(wv), "wkeck") == nullptr;
      check(set_ok && cleared,
            "wkeGetCookie reads a server-set cookie; clear command drops it");
    } else {
      std::printf("  [SKIP] wkeGetCookie round-trip (host unreachable)\n");
    }

    // Proxy routing: a bogus (unresolvable) proxy must make an http load fail;
    // clearing it restores direct connectivity. Proves the proxy is applied.
    wkeProxy bad;
    std::memset(&bad, 0, sizeof(bad));
    bad.type = WKE_PROXY_HTTP;
    std::strcpy(bad.hostname, "no-such-proxy.invalid");
    bad.port = 8080;
    wkeSetProxy(&bad);
    wkeLoadURL(wv, "http://example.com/");
    const bool failed_via_proxy = !wkeIsLoadingSucceeded(wv);
    wkeSetProxy(nullptr);  // back to direct
    wkeLoadURL(wv, "http://example.com/");
    const bool ok_direct = wkeIsLoadingSucceeded(wv);
    if (ok_direct) {
      check(failed_via_proxy,
            "wkeSetProxy routes via the proxy (bogus proxy fails the load)");
    } else {
      std::printf("  [SKIP] wkeSetProxy routing (direct host unreachable)\n");
    }

    // Extra headers: httpbin echoes the request headers in its JSON body.
    wkeSetExtraHeaders(wv, "X-Wke-Test: zzz9");
    wkeLoadURL(wv, "http://httpbin.org/headers");
    if (wkeIsLoadingSucceeded(wv)) {
      const char* hbody = jsToTempString(
          es, wkeRunJS(wv, "document.body?document.body.innerText:''"));
      check(std::strstr(hbody, "zzz9") != nullptr,
            "wkeSetExtraHeaders injects a request header (httpbin echoes it)");
    } else {
      std::printf("  [SKIP] wkeSetExtraHeaders echo (host unreachable)\n");
    }
    wkeSetExtraHeaders(wv, nullptr);  // clear for any later requests

    // HTTP introspection over the network: a real 200 + non-empty headers.
    wkeLoadURL(wv, "http://httpbin.org/get");
    if (wkeIsLoadingSucceeded(wv)) {
      const int code = wkeGetHttpStatusCode(wv);
      const char* hdrs = wkeGetResponseHeaders(wv);
      check(code == 200 && hdrs[0] != '\0',
            "wkeGetHttpStatusCode==200 + wkeGetResponseHeaders non-empty (httpbin)");
    } else {
      std::printf("  [SKIP] wkeGetHttpStatusCode/Headers (host unreachable)\n");
    }

    // Follow-redirects toggle: with following on, /redirect/1 lands on /get
    // (200); with it off, the load stops at the 30x response.
    wkeSetFollowRedirects(true);
    wkeLoadURL(wv, "http://httpbin.org/redirect/1");
    if (wkeIsLoadingSucceeded(wv) && wkeGetHttpStatusCode(wv) == 200) {
      wkeSetFollowRedirects(false);
      wkeLoadURL(wv, "http://httpbin.org/redirect/1");
      const int code = wkeGetHttpStatusCode(wv);
      check(code >= 300 && code < 400,
            "wkeSetFollowRedirects(false) stops at the 30x response");
      wkeSetFollowRedirects(true);  // restore default for any later request
    } else {
      std::printf("  [SKIP] wkeSetFollowRedirects (redirect host unreachable)\n");
    }

    // Cert errors: with ignoring on, a self-signed HTTPS site loads; turning it
    // off rejects the same site. (Only asserted when the host is reachable.)
    wkeSetIgnoreCertErrors(true);
    wkeLoadURL(wv, "https://self-signed.badssl.com/");
    if (wkeIsLoadingSucceeded(wv)) {
      wkeSetIgnoreCertErrors(false);
      wkeLoadURL(wv, "https://self-signed.badssl.com/");
      check(!wkeIsLoadingSucceeded(wv),
            "wkeSetIgnoreCertErrors(false) rejects a self-signed certificate");
    } else {
      std::printf("  [SKIP] wkeSetIgnoreCertErrors (badssl unreachable)\n");
    }
    wkeSetIgnoreCertErrors(false);  // restore secure default

    // Image-loading toggle: a network <img> loads (naturalWidth>0) with images
    // on, but is skipped (naturalWidth==0) when disabled.
    const char* img_page =
        "<body><img id='i' src='http://httpbin.org/image/png'></body>";
    wkeSetLoadImages(wv, true);
    wkeLoadHTML(wv, img_page);
    if (wkeWaitForFunction(
            wv, "document.getElementById('i').naturalWidth>0", 4000)) {
      wkeSetLoadImages(wv, false);
      wkeLoadHTML(wv, img_page);  // images disabled — must not fetch
      const bool off =
          jsToInt(es, wkeRunJS(
                          wv, "document.getElementById('i').naturalWidth")) == 0;
      check(off, "wkeSetLoadImages(false) skips a network image");
      wkeSetLoadImages(wv, true);  // restore default
    } else {
      std::printf("  [SKIP] wkeSetLoadImages (image host unreachable)\n");
    }
  }

  wkeDestroyWebView(wv);
  wkeFinalize();

  std::printf("wke_smoke: %d passed, %d failed\n", pass, fail);
  return fail == 0 ? 0 : 1;
}
