// Sample 3 — Resizable App (macOS + Windows).
//
// Window resizes flow into mbResize (logical CSS px; the scaffold's resize
// handler does it) and the page relays out live — the readout shows
// window.innerWidth/Height and devicePixelRatio updating as you drag.
//
// Run:  ./sample3_resizable_app
#include "compat/mb_window.h"

static const char kPage[] = R"HTML(
<!doctype html>
<style>
  html, body { height: 100%; margin: 0; }
  body { display: grid; place-items: center; background: #101418;
         font-family: ui-monospace, Menlo, Consolas, monospace; color: #7fd962; }
  #size { font-size: clamp(18px, 6vw, 64px); }   /* scales with the viewport */
  #dpr  { color: #557; font-size: 14px; margin-top: 1em; text-align: center; }
</style>
<body>
  <div>
    <div id="size"></div>
    <div id="dpr"></div>
  </div>
  <script>
    function show() {
      document.getElementById('size').textContent =
          innerWidth + ' × ' + innerHeight;
      document.getElementById('dpr').textContent =
          'devicePixelRatio ' + devicePixelRatio + ' — resize the window';
    }
    addEventListener('resize', show);
    show();
  </script>
</body>
)HTML";

int main() {
  if (!mbInitialize()) return 1;
  MbWindow* win = MbWindowCreate("Sample 3 — Resizable App", 800, 480);
  if (!win) return 1;
  mbLoadHTML(MbWindowView(win), kPage, "about:blank");
  MbRunApp();
  return 0;
}
