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

  int sinks = mbViewFrameSinkRequested(v);

  char body[256] = {0};
  mbEvalJS(v, "document.body.textContent", body, sizeof(body));
  bool live = std::strstr(body, "hello compositor") != nullptr;

  // Compositing view -> a live SoftwareCompositor (count >= 0); non-compositing -> -1.
  bool ok = (plain_sinks == -1) && (sinks >= 0) && live;
  printf("mb_compositor_widget_smoke: plain=%d compositing=%d body=[%s] live=%d\n",
         plain_sinks, sinks, body, live ? 1 : 0);

  mbDestroyView(v);
  mbShutdown();

  printf("mb_compositor_widget_smoke: %s (InitializeCompositing live + frame-sink hook wired)\n",
         ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
