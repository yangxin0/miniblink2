// Sample 1 — Render to PNG (headless; no window toolkit).
//
// The whole pipeline in five calls: init the engine, create an offscreen view,
// load a URL (the load pumps until the document's `load` event), let the
// network go quiet, save a PNG. For a production-grade version of this exact
// idea — clip rects, full-page, element shots, PDF — see the mb_shot tool.
//
// Run:  ./sample1_render_to_png [url] [out.png]
#include <cstdio>

#include "miniblink2/automation.h"

int main(int argc, const char** argv) {
  const char* url = argc > 1 ? argv[1] : "https://example.com";
  const char* out = argc > 2 ? argv[2] : "result.png";
  const int W = 1024, H = 768;

  if (!mbInitialize()) {
    std::fprintf(stderr, "engine init failed\n");
    return 1;
  }
  mbView* view = mbCreateView(W, H);

  std::printf("loading %s ...\n", url);
  mbLoadURL(view, url);                       // pumps until the `load` event
  mbWaitForNetworkIdle(view, 500, 10000);   // this view's late resources/fonts settle

  // One-shot capture (mbSavePng settles the lifecycle before painting; the
  // interactive path would use mbRepaintToBitmap instead — see webview.h).
  if (!mbSavePng(view, out, W, H)) {
    std::fprintf(stderr, "capture failed\n");
    return 1;
  }
  std::printf("saved %dx%d -> %s (status %d)\n", W, H, out, mbGetHttpStatus(view));

  mbDestroyView(view);
  mbShutdown();
  return 0;
}
