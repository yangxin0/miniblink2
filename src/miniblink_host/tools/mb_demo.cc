// mb_demo — a runnable, end-to-end example of the pure-C `mb_capi` ABI (the
// project's primary embedding seam; the wke layer is built on top of this).
//
// It drives a small offline page the way a C/C++ embedder would: fill a field,
// read its live value, dispatch a custom DOM event that asynchronously appends a
// result element AND triggers a subresource fetch, wait for the network to go
// idle, scrape the result's text and HTML, inspect the request log, and capture
// just that element to a PNG. Each step asserts, so running it doubles as an
// integration check. Returns 0 on success, 1 on any failed step.

#include "miniblink_host/capi/mb_capi.h"

#include <cstdio>
#include <cstring>

namespace {
bool g_ok = true;
void step(bool cond, const char* what) {
  std::printf("  [%s] %s\n", cond ? " OK " : "FAIL", what);
  if (!cond)
    g_ok = false;
}
// Read a selector's text/html into a local buffer and return it (empty on miss).
}  // namespace

int main() {
  if (!mbInitialize()) {
    std::fprintf(stderr, "mb_demo: engine init failed\n");
    return 1;
  }
  mbView* v = mbCreateView(480, 240);

  // A tiny page: a 'refresh' event on #name appends a styled #out div AND injects
  // an <img> (a deferred subresource) — mimicking an SPA that fetches after an
  // interaction, which the embedder must wait out before scraping.
  mbLoadHTML(v,
      "<body style='font:16px sans-serif'>"
      "<input id='name'>"
      "<script>document.getElementById('name').addEventListener('refresh',"
      "function(){var d=document.createElement('div');d.id='out';"
      "d.style.color='rgb(0,128,0)';"
      "d.textContent='Value: '+document.getElementById('name').value;"
      "document.body.appendChild(d);"
      "var i=document.createElement('img');i.src='file:///tmp/mb_demo_probe.png';"
      "document.body.appendChild(i);});</script></body>",
      "file:///tmp/mb_demo.html");
  std::printf("mb_demo: driving the C ABI end-to-end\n");

  // 1. Fill a field and read its LIVE value back through the C ABI.
  step(mbFillSelector(v, "#name", "Bob") == 1, "fill #name = 'Bob'");
  char val[64] = {0};
  mbGetValueForSelector(v, "#name", val, sizeof(val));
  step(std::strcmp(val, "Bob") == 0, "mbGetValueForSelector(#name) == 'Bob'");

  // 2. Dispatch a custom DOM event that the page handles asynchronously, then
  //    wait for the network (the injected <img>) to go idle. Scope the log first.
  mbClearRequestLog();
  step(mbDispatchEvent(v, "#name", "refresh") == 1,
       "mbDispatchEvent(#name, 'refresh')");
  step(mbWaitForNetworkIdle(v, 400, 5000) == 1, "mbWaitForNetworkIdle settles");

  // 3. Scrape the asynchronously-appended result: its text and its outerHTML.
  char text[128] = {0};
  mbGetTextForSelector(v, "#out", text, sizeof(text));
  step(std::strcmp(text, "Value: Bob") == 0, "scrape #out text");
  std::printf("       #out text = \"%s\"\n", text);

  char html[256] = {0};
  mbGetHtmlForSelector(v, "#out", html, sizeof(html));
  step(std::strstr(html, "id=\"out\"") != nullptr, "mbGetHtmlForSelector(#out)");

  // 4. The deferred fetch shows up in the request log (network observability).
  char log[1024] = {0};
  mbGetRequestLog(log, sizeof(log));
  step(std::strstr(log, "mb_demo_probe.png") != nullptr,
       "request log holds the deferred <img> fetch");

  // 5. Capture just the result element to a PNG (Puppeteer elementHandle.screenshot).
  const char* shot = "/private/tmp/claude-501/mb_demo.png";
  step(mbSaveElementPng(v, "#out", shot) == 1, "mbSaveElementPng(#out)");
  std::remove(shot);  // demo cleanup; a real app would keep it

  // 6. Arbitrary JS eval returns a string result.
  char ev[16] = {0};
  mbEvalJS(v, "1 + 1", ev, sizeof(ev));
  step(std::strcmp(ev, "2") == 0, "mbEvalJS('1 + 1') == '2'");

  mbDestroyView(v);
  mbShutdown();

  std::printf("mb_demo: %s\n", g_ok ? "all steps OK" : "FAILED");
  return g_ok ? 0 : 1;
}
