// wke_smoke — exercises the wke compatibility slice end to end in its own process:
// init -> create -> resize -> loadHTML -> poll loading state -> read title ->
// paint to a BGRA buffer and check the background pixel -> destroy -> finalize.

#include "wke/wke.h"

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

  wkeDestroyWebView(wv);
  wkeFinalize();

  std::printf("wke_smoke: %d passed, %d failed\n", pass, fail);
  return fail == 0 ? 0 : 1;
}
