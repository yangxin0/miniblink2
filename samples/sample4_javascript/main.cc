// Sample 4 — JavaScript (macOS + Windows).
//
// The JS <-> native bridge, mb style:
//   - mbJsBindFunction installs window.GetMessage / window.GetStats into every
//     new document's main world — synchronous C calls from page JS. Strings
//     and JSON cross the ABI (out_type 5 JSON.parses into a real object); no
//     engine JS types leak across the boundary (mb deliberately does not
//     expose V8, for ABI stability).
//   - mbOnWindowObjectReady is the sanctioned moment for host-COMPUTED setup:
//     it fires before ANY page script, so the page's first line already sees
//     what the host injected (a session token here).
//
// Run:  ./sample4_javascript
#include <cstdio>

#include "compat/mb_window.h"

static int g_calls = 0;

// window.GetMessage() -> a string computed in C.
static const char* GetMessage(void*, int, const char**, const int*, int*) {
  static char buf[64];
  std::snprintf(buf, sizeof buf, "Hello from C! (call #%d)", ++g_calls);
  return buf;  // the engine copies it before the call returns (see webview.h)
}

// window.GetStats() -> a real JS object (out_type 5: the string is JSON.parsed).
static const char* GetStats(void*, int, const char**, const int*, int* out_type) {
  static char buf[64];
  *out_type = 5;
  std::snprintf(buf, sizeof buf, "{\"calls\":%d,\"engine\":\"miniblink2\"}",
                g_calls);
  return buf;
}

static const char kPage[] = R"HTML(
<!doctype html>
<style>
  body { display: grid; place-items: center; height: 100vh; margin: 0;
         background: #14181d; color: #dde; font-family: system-ui, sans-serif; }
  .box { text-align: center; }
  button { font-size: 1em; padding: .6em 1.4em; margin: .3em; cursor: pointer;
           border: 0; border-radius: 8px; background: #47c774; color: #fff; }
  #out { margin-top: 1.2em; font-family: ui-monospace, Menlo, Consolas, monospace;
         color: #8fd; min-height: 3em; white-space: pre; }
</style>
<body>
  <div class="box">
    <h2>JS ⇄ native</h2>
    <button onclick="document.getElementById('out').textContent = GetMessage()">
      call GetMessage()
    </button>
    <button onclick="document.getElementById('out').textContent =
        JSON.stringify(GetStats(), null, 1)">
      call GetStats()
    </button>
    <div id="out"></div>
    <script>
      // Runs AFTER the window-object-ready callback: the token is already there.
      document.getElementById('out').textContent =
          'token injected before page script: ' + window.__token;
    </script>
  </div>
</body>
)HTML";

int main() {
  if (!mbInitialize()) return 1;
  MbWindow* win = MbWindowCreate("Sample 4 — JavaScript", 820, 560);
  if (!win) return 1;
  mbView* view = MbWindowView(win);

  // Bindings install into each new document; register before loading.
  mbJsBindFunction(view, "GetMessage", GetMessage, nullptr);
  mbJsBindFunction(view, "GetStats", GetStats, nullptr);
  // Host-computed per-document setup, before any page script runs.
  mbOnWindowObjectReady(view, [](mbView* v, void*) {
      mbRunJS(v, "window.__token = 'tok-' + 42;");
  }, nullptr);
  mbLoadHTML(view, kPage, "about:blank");

  MbRunApp();
  return 0;
}
