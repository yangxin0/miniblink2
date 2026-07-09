// Sample 9 — Multi Window (Ultralight sample-set parity; macOS + Windows).
//
// Two windows, two views, one process: an EDITOR window whose <textarea>
// pushes every keystroke to native code (mbJsBindFunction), and a PREVIEW
// window the host re-renders from that text. Demonstrates three modern-API
// idioms working together:
//   - the JS -> native push (window.Preview(text) is a bound C function);
//   - mbDefer: the binding fires from INSIDE page JS, where starting another
//     view's load would re-enter the engine — defer schedules it for the
//     moment the engine is off the stack;
//   - mbLoadHTMLEx(add_to_history=0): the preview REPLACES its history entry
//     per keystroke instead of growing back/forward by one per character.
//
// Run:  ./sample9_multi_window
#include <string>

#include "compat/mb_window.h"

static mbView* g_preview = nullptr;
static std::string g_pending;  // latest editor text, awaiting the deferred load

// Escape editor text for embedding in the preview HTML.
static std::string HtmlEscape(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (char c : in) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      default:  out += c; break;
    }
  }
  return out;
}

// Runs when the engine is next OFF the stack (mbDefer): safe to load.
static void ApplyPreview(void*) {
  if (!g_preview) return;
  std::string html =
      "<!doctype html><style>"
      "body{margin:0;padding:2em;background:#fffdf5;color:#333;"
      "font-family:Georgia,serif;font-size:18px;line-height:1.6}"
      "</style><body>" + HtmlEscape(g_pending) + "</body>";
  // add_to_history=0: each keystroke REPLACES the preview's history entry.
  mbLoadHTMLEx(g_preview, html.c_str(), "about:blank", /*add_to_history=*/0);
}

// window.Preview(text) — called from the editor page on every input event.
static const char* OnPreview(void*, int argc, const char** argv, const int*,
                             int* out_type) {
  if (argc > 0 && argv[0]) {
    g_pending = argv[0];
    // We are INSIDE the page's JS here: don't drive another view's load from
    // this stack — defer it to the engine's next idle moment.
    mbDefer(ApplyPreview, nullptr);
  }
  *out_type = 4;  // undefined
  return "";
}

static const char kEditorPage[] = R"HTML(
<!doctype html>
<style>
  html, body { height: 100%; margin: 0; }
  body { display: flex; flex-direction: column; background: #191d24;
         font-family: system-ui, sans-serif; }
  h3 { color: #9ab; margin: .8em 1em .4em; font-weight: 500; }
  textarea { flex: 1; margin: 0 1em 1em; padding: 1em; border: 0;
             border-radius: 8px; resize: none; outline: none;
             background: #232833; color: #dde;
             font: 15px/1.5 ui-monospace, Menlo, Consolas, monospace; }
</style>
<body>
  <h3>Type here — the other window re-renders live</h3>
  <textarea autofocus
      oninput="Preview(this.value)">Hello from the editor window!</textarea>
  <script>Preview(document.querySelector('textarea').value);</script>
</body>
)HTML";

int main() {
  if (!mbInitialize()) return 1;

  MbWindow* editor = MbWindowCreate("Sample 9 — Editor", 560, 480);
  MbWindow* preview = MbWindowCreate("Sample 9 — Preview", 560, 480);
  if (!editor || !preview) return 1;
  g_preview = MbWindowView(preview);

  mbJsBindFunction(MbWindowView(editor), "Preview", OnPreview, nullptr);
  mbLoadHTML(MbWindowView(editor), kEditorPage, "about:blank");

  MbRunApp();
  return 0;
}
