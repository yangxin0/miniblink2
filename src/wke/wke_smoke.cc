// wke_smoke — exercises the wke compatibility slice end to end in its own process:
// init -> create -> resize -> loadHTML -> poll loading state -> read title ->
// paint to a BGRA buffer and check the background pixel -> destroy -> finalize.

#include "wke/wke.h"

#include <algorithm>
#include <cstdio>
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

  wkeDestroyWebView(wv);
  wkeFinalize();

  std::printf("wke_smoke: %d passed, %d failed\n", pass, fail);
  return fail == 0 ? 0 : 1;
}
