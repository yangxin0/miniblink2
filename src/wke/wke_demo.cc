// wke_demo — a runnable, end-to-end example of the wke automation API.
//
// It drives a small offline form the way a real scraper/automation app would:
//   fill a field -> choose a <select> option -> click submit -> wait for the
//   JS-rendered result -> scrape its text and computed style -> screenshot.
// Each step uses the high-level selector helpers (no hand-written JS), and the
// program asserts the outcome so running it doubles as an integration check.
// Returns 0 on success, 1 on any failed step.

#include "wke/wke.h"

#include <cstdio>
#include <cstring>

namespace {
bool g_ok = true;
void step(bool cond, const char* what) {
  std::printf("  [%s] %s\n", cond ? " OK " : "FAIL", what);
  if (!cond)
    g_ok = false;
}

// The page signals the host via window.mbBridge(channel, message); this callback
// receives it. (A C callback can't capture locals, so the result lands here.)
char g_bridge_channel[128], g_bridge_message[256];
void OnBridge(wkeWebView, void*, const utf8* channel, const utf8* message) {
  std::snprintf(g_bridge_channel, sizeof(g_bridge_channel), "%s",
                channel ? channel : "");
  std::snprintf(g_bridge_message, sizeof(g_bridge_message), "%s",
                message ? message : "");
}
}  // namespace

int main() {
  wkeInitialize();
  wkeWebView wv = wkeCreateWebView();
  jsExecState es = wkeGlobalExec(wv);

  // Let the page signal us: window.mbBridge calls reach OnBridge. Register before
  // loading so the bridge is installed in the document.
  wkeOnJsBridge(wv, OnBridge, nullptr);

  // A tiny form: clicking #go signals the host via window.mbBridge, then appends
  // a styled #out result after a 30ms timer — mimicking an async (SPA-like)
  // response the automation must wait for.
  wkeLoadHTML(
      wv,
      "<body style='font:16px sans-serif'>"
      "<h1 id='title'>wke demo</h1>"
      "<input id='name' placeholder='name'>"
      "<select id='role'>"
      "<option value='user'>User</option>"
      "<option value='admin'>Admin</option></select>"
      "<button id='go' onclick=\""
      "window.mbBridge('submit', document.getElementById('name').value);"
      "setTimeout(function(){"
      "var d=document.createElement('div');d.id='out';"
      "d.style.color='rgb(0,128,0)';"
      "d.textContent='Hello '+document.getElementById('name').value+"
      "' ('+document.getElementById('role').value+')';"
      "document.body.appendChild(d);},30)\">Go</button>"
      "</body>");
  std::printf("wke_demo: driving an offline form end-to-end\n");

  // 1. Fill the text field and choose a dropdown option (events fire, so a
  //    framework would observe them).
  step(wkeFillSelector(wv, "#name", "Ada"), "fill #name = 'Ada'");
  step(wkeSelectOption(wv, "#role", "admin"), "select #role = 'admin'");

  // 2. Submit, then wait for the asynchronously-rendered result element.
  step(wkeClickSelector(wv, "#go"), "click #go (submit)");
  step(wkeWaitForSelector(wv, "#out", 4000), "wait for #out to appear");

  // 2b. The click handler signaled the host via window.mbBridge; the click's
  //     drain delivered it to OnBridge (page -> host messaging).
  step(std::strcmp(g_bridge_channel, "submit") == 0 &&
           std::strcmp(g_bridge_message, "Ada") == 0,
       "page -> host bridge: window.mbBridge('submit','Ada') received");

  // 3. Scrape the result: its text and a computed style property.
  const char* text = wkeGetTextForSelector(wv, "#out");
  step(std::strcmp(text, "Hello Ada (admin)") == 0, "scrape #out text");
  std::printf("       #out text  = \"%s\"\n", text);

  const char* color = wkeGetComputedStyle(wv, "#out", "color");
  step(std::strcmp(color, "rgb(0, 128, 0)") == 0, "read #out computed color");
  std::printf("       #out color = %s\n", color);

  // 4. A couple of read-side helpers an app commonly uses.
  step(wkeCountSelector(wv, "option") == 2, "count <option> elements (2)");
  step(jsToInt(es, wkeRunJS(wv, "document.querySelectorAll('div').length")) >= 1,
       "wkeRunJS sees the appended <div>");

  // 5. Capture a screenshot of the final state.
  const char* shot = "/private/tmp/claude-501/wke_demo.png";
  step(wkeSavePng(wv, shot, 400, 200), "screenshot -> wke_demo.png");
  std::remove(shot);  // demo cleanup; a real app would keep it

  wkeDestroyWebView(wv);
  wkeFinalize();

  std::printf("wke_demo: %s\n", g_ok ? "all steps OK" : "FAILED");
  return g_ok ? 0 : 1;
}
