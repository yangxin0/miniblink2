// mb_compositor_widget_smoke — SOFTWARE COMPOSITOR milestone D2b-2: the LIVE widget flip.
//
// Exercises the real, opt-in compositing path through the public mb_capi ABI: enable
// compositing, then create a view. Its MbWidget creates a mb::SoftwareCompositor, installs the
// patch-0012 frame-sink hook bound to it, and calls WebFrameWidget::InitializeCompositing with
// single-thread synchronous LayerTreeSettings (instead of InitializeNonCompositing). The WebView
// is created with does_composite=true so that path's DCHECK is satisfied.
//
// Verifies (a) none of that crashes, (b) the view is in compositing mode — mbViewFrameSinkRequested
// returns >= 0 (a live mb::SoftwareCompositor exists) rather than -1 (non-compositing), and (c) the
// page is still live (DOM evaluates) under the compositing widget. Driving an actual cc frame
// (raster -> the in-process Display) additionally needs an in-process software gpu::SharedImage
// Interface, which is the next sub-arc (D2b-3); this smoke verifies the init + frame-sink wiring.
//
// Bounded by the caller's watchdog; no leaked process.

#include <cstdio>
#include <cstring>
#include <vector>

#include "miniblink_host/capi/mb_capi.h"

int main() {
  if (!mbInitialize()) {
    fprintf(stderr, "mb_compositor_widget_smoke: mbInitialize FAILED\n");
    return 1;
  }

  // Sanity: a default (non-compositing) view reports -1 (no compositor).
  mbView* plain = mbCreateView(200, 150);
  int plain_sinks = plain ? mbViewFrameSinkRequested(plain) : -99;
  if (plain)
    mbDestroyView(plain);

  // Compositing view.
  mbSetCompositingEnabled(1);
  mbView* v = mbCreateView(400, 300);
  mbSetCompositingEnabled(0);  // leave the global off for any later views
  if (!v) {
    fprintf(stderr, "mb_compositor_widget_smoke: mbCreateView FAILED\n");
    return 1;
  }

  mbLoadHTML(v, "<body style='background:#ff0'>hello compositor</body>",
             "about:blank");
  for (int i = 0; i < 20; ++i) {
    mbWait(v, 16);
    mbPumpMessages();
  }

  // Drive real cc frames: raster through the in-process GPU service's SharedImageInterface ->
  // the Display -> the captured bitmap. The first composite lazily pulls the frame sink.
  for (int i = 0; i < 3; ++i) {
    mbViewComposite(v);
    mbPumpMessages();
  }

  int sinks = mbViewFrameSinkRequested(v);

  char body[256] = {0};
  mbEvalJS(v, "document.body.textContent", body, sizeof(body));
  bool live = std::strstr(body, "hello compositor") != nullptr;

  // The composited center pixel should be the page's yellow (#ffff00) if cc raster -> viz ->
  // bitmap works end to end.
  unsigned int c = mbViewCompositorPixel(v, 200, 150);
  unsigned int a = (c >> 24) & 0xff, r = (c >> 16) & 0xff, g = (c >> 8) & 0xff,
               b = c & 0xff;
  bool yellow = r > 200 && g > 200 && b < 80;

  // The SCREENSHOT path (mbPaintToBitmap) must reflect the composited frame for a
  // compositing view (not the software paint record). Paint the full 400x300 view
  // and check the center pixel is the page's yellow — proving cc -> viz::Display ->
  // bitmap now flows all the way into the user-facing screenshot API.
  const int kW = 400, kH = 300, kStride = kW * 4;
  std::vector<unsigned char> shot(static_cast<size_t>(kStride) * kH, 0);
  bool painted = mbPaintToBitmap(v, shot.data(), kW, kH, kStride) != 0;
  const unsigned char* px = &shot[(static_cast<size_t>(150) * kStride) + 200 * 4];
  unsigned int sb = px[0], sg = px[1], sr = px[2], sa = px[3];  // BGRA
  bool shot_yellow = painted && sr > 200 && sg > 200 && sb < 80;

  // PASS gate: the FULL live cc compositing path works — a non-compositing view reports -1, the
  // compositing view pulls our in-process frame sink (sinks>=1), the page is DOM-live, AND the page
  // RASTERS through cc -> viz::Display -> the captured bitmap so the center pixel is the page's
  // yellow (#ffff00).
  bool ok = (plain_sinks == -1) && (sinks >= 1) && live && yellow && shot_yellow;
  printf("mb_compositor_widget_smoke: plain=%d sinks=%d body=[%s] live=%d pixel=%08X "
         "(a%d r%d g%d b%d) yellow=%d shot=(r%d g%d b%d a%d) shot_yellow=%d\n",
         plain_sinks, sinks, body, live ? 1 : 0, c, a, r, g, b, yellow ? 1 : 0,
         sr, sg, sb, sa, shot_yellow ? 1 : 0);

  mbDestroyView(v);
  mbShutdown();

  printf("mb_compositor_widget_smoke: %s (live page rasters through cc -> viz::Display -> bitmap)\n",
         ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
