// Sample 2 — Basic App (macOS + Windows).
//
// A window showing an in-memory HTML page with hover/click interactivity.
// Everything windowing lives in the shared samples scaffold (mb_window.h,
// with Cocoa and Win32 backends) — the engine itself is windowless; this
// file is just content.
//
// Run:  ./sample2_basic_app
#include "compat/mb_window.h"

static const char kPage[] = R"HTML(
<!doctype html>
<style>
  html, body { height: 100%; margin: 0; }
  body { display: flex; align-items: center; justify-content: center;
         background: linear-gradient(135deg, #1a2033, #2d1f3a);
         font-family: system-ui, sans-serif; color: #eee; }
  .card { text-align: center; padding: 3em 4em; border-radius: 16px;
          background: rgba(255,255,255,.06); }
  h1 { font-weight: 600; margin: 0 0 .3em; }
  p  { color: #9aa; margin: 0 0 1.5em; }
  button { font-size: 1.05em; padding: .7em 1.6em; border: 0; cursor: pointer;
           border-radius: 8px; background: #5b8cff; color: #fff;
           transition: transform .08s ease, background .15s ease; }
  button:hover  { background: #729aff; }
  button:active { transform: scale(.96); }
</style>
<body>
  <div class="card">
    <h1>miniblink2</h1>
    <p>an offscreen Chromium engine, hosted in ~20 lines of app code</p>
    <button onclick="this.textContent = 'clicked ' + (++window.__n || (window.__n=1)) + '×'">
      Click me
    </button>
  </div>
</body>
)HTML";

int main() {
  if (!mbInitialize()) return 1;
  MbWindow* win = MbWindowCreate("Sample 2 — Basic App", 900, 600);
  if (!win) return 1;
  mbLoadHTML(MbWindowView(win), kPage, "about:blank");
  MbRunApp();
  return 0;
}
