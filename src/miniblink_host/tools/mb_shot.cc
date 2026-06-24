// mb_shot — headless renderer CLI. Renders an HTML file (or file:// URL) to a PNG using
// modern Blink via the miniblink_host C ABI.
//
//   mb_shot [--full] [--scale N] <input.html | file://URL | http(s)://URL> <out.png> [width height]
//
//   --full     capture the entire document height, not just the viewport (resizes the
//              view to the page's scrollHeight before rendering — like Puppeteer fullPage).
//   --scale N  device pixel ratio: lay out at [width height] CSS px but render at Nx
//              (window.devicePixelRatio == N). The PNG is width*N x height*N — retina-crisp.
//   --clip x,y,w,h     capture only that logical rectangle of the page.
//   --selector CSS     capture only the bounding box of the first element matching CSS
//                      (an element screenshot). Overrides --clip.
//   --transparent      capture with a transparent background (omitBackground) — areas the
//                      page doesn't paint keep alpha 0 in the PNG.
//   --wait-selector S  before capturing, wait until an element matching S exists
//   --click CSS        before capturing, click the element matching CSS
//                      (timeout = --wait-ms or 5000ms). For JS-rendered content.
//   --wait-ms N        before capturing, drive the engine for N ms (settle timers/async).
//   --console          print the page's console output (console.log/warn/error) to stderr.
//   --header "N: V"    add an HTTP request header (repeatable) to the navigation + subresources.
//   --text             print the page's visible text (document.body.innerText) to stdout.
//   --html             print the rendered (post-JS) DOM as serialized HTML to stdout.
//   --no-images        disable image loading (faster text/HTML scraping).
//   --dark             emulate prefers-color-scheme: dark (capture dark themes).
//   --lang "L,L2,..."  set navigator.language(s) (e.g. "fr-FR,fr,en").
//   --tz "Area/City"   override the timezone for Date/Intl (e.g. "America/New_York").
//
// This is the "product" the host enables: a standalone, single-process, modern-Blink
// screenshot tool — no browser process, no CEF.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "miniblink_host/capi/mb_capi.h"

namespace {
// Cap full-page height so a runaway/infinite-scroll page can't request a
// multi-gigabyte bitmap.
constexpr int kMaxFullPageHeight = 20000;
}  // namespace

int main(int argc, char** argv) {
  bool full_page = false;
  bool transparent = false;
  bool print_console = false;
  bool print_text = false;
  bool print_html = false;
  bool no_images = false;
  bool dark_mode = false;
  bool insecure = false;  // skip TLS cert verification
  std::string lang;  // navigator.language(s)
  std::string tz;    // IANA timezone for Date/Intl
  float scale = 1.0f;
  std::string clip;      // "x,y,w,h"
  std::string selector;  // CSS selector -> capture that element's box
  std::string headers;   // extra request headers, "Name: Value" per line
  std::string wait_selector;  // wait for this selector before capture
  std::string click_selector;  // click this selector before capture
  std::string fill_selector;   // fill this field before capture (with fill_text)
  std::string fill_text;       // value for --fill
  std::string eval_js;         // JS to run after load; result printed to stdout
  std::string proxy;           // libcurl proxy string for network fetches
  std::string load_cookies;    // cookie jar file to load before navigating
  std::string save_cookies;    // cookie jar file to write after the page settles
  int wait_ms = 0;            // fixed wait before capture
  std::vector<const char*> pos;  // positional args, flags filtered out
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--full") {
      full_page = true;
    } else if (a == "--transparent") {
      transparent = true;
    } else if (a == "--scale" && i + 1 < argc) {
      scale = static_cast<float>(std::atof(argv[++i]));
      if (scale <= 0.0f)
        scale = 1.0f;
    } else if (a == "--clip" && i + 1 < argc) {
      clip = argv[++i];
    } else if (a == "--selector" && i + 1 < argc) {
      selector = argv[++i];
    } else if (a == "--wait-selector" && i + 1 < argc) {
      wait_selector = argv[++i];
    } else if (a == "--click" && i + 1 < argc) {
      click_selector = argv[++i];
    } else if (a == "--fill" && i + 2 < argc) {
      fill_selector = argv[++i];
      fill_text = argv[++i];
    } else if (a == "--eval" && i + 1 < argc) {
      eval_js = argv[++i];
    } else if (a == "--proxy" && i + 1 < argc) {
      proxy = argv[++i];
    } else if (a == "--load-cookies" && i + 1 < argc) {
      load_cookies = argv[++i];
    } else if (a == "--save-cookies" && i + 1 < argc) {
      save_cookies = argv[++i];
    } else if (a == "--wait-ms" && i + 1 < argc) {
      wait_ms = std::atoi(argv[++i]);
    } else if (a == "--console") {
      print_console = true;
    } else if (a == "--text") {
      print_text = true;
    } else if (a == "--html") {
      print_html = true;
    } else if (a == "--no-images") {
      no_images = true;
    } else if (a == "--dark") {
      dark_mode = true;
    } else if (a == "--insecure") {
      insecure = true;
    } else if (a == "--lang" && i + 1 < argc) {
      lang = argv[++i];
    } else if (a == "--tz" && i + 1 < argc) {
      tz = argv[++i];
    } else if (a == "--header" && i + 1 < argc) {
      if (!headers.empty())
        headers += "\n";
      headers += argv[++i];
    } else {
      pos.push_back(argv[i]);
    }
  }
  if (pos.size() < 2) {
    std::fprintf(
        stderr,
        "usage: %s [--full] [--scale N] [--clip x,y,w,h] [--selector CSS] "
        "[--transparent] [--text] [--html] [--eval JS] [--fill CSS TEXT] "
        "[--click CSS] [--wait-selector CSS] [--wait-ms N] [--proxy URL] "
        "[--load-cookies FILE] [--save-cookies FILE] [--insecure] "
        "<input.html|file://URL|http(s)://URL> <out.png> [width height]\n",
        argv[0]);
    return 2;
  }
  const std::string input = pos[0];
  const char* out = pos[1];
  const int w = pos.size() > 2 ? std::atoi(pos[2]) : 1200;
  const int h = pos.size() > 3 ? std::atoi(pos[3]) : 800;

  if (!mbInitialize()) {
    std::fprintf(stderr, "mb_shot: engine init failed\n");
    return 1;
  }
  mbView* view = mbCreateView(w, h);
  if (!view) {
    std::fprintf(stderr, "mb_shot: view creation failed\n");
    return 1;
  }
  if (scale != 1.0f)
    mbSetDeviceScaleFactor(view, scale);  // before load so DPR-aware content responds
  if (transparent)
    mbSetTransparentBackground(view, 1);
  if (no_images)
    mbSetLoadImages(view, 0);  // skip image fetch/decode for faster scraping
  if (dark_mode)
    mbSetDarkMode(view, 1);  // emulate prefers-color-scheme: dark
  if (!lang.empty())
    mbSetLocale(view, lang.c_str());  // navigator.language(s)
  if (!tz.empty())
    mbSetTimezone(view, tz.c_str());  // Date/Intl timezone
  if (!headers.empty())
    mbSetExtraHeaders(view, headers.c_str());  // before load so the navigation uses them
  if (!proxy.empty())
    mbSetProxy(proxy.c_str());  // route fetches through the proxy (process-wide)
  if (insecure)
    mbSetIgnoreCertErrors(1);  // skip TLS cert verification (self-signed/expired)
  if (!load_cookies.empty()) {
    if (!mbLoadCookies(load_cookies.c_str()))  // restore a saved session
      std::fprintf(stderr, "mb_shot: WARNING — --load-cookies '%s' unreadable\n",
                   load_cookies.c_str());
  }

  const bool is_http = input.rfind("http", 0) == 0;
  if (input.rfind("file://", 0) == 0 || is_http) {
    mbLoadURL(view, input.c_str());
  } else {
    // A local HTML file path: read it and commit (base URL = its file:// dir so that
    // relative subresources resolve through the loader).
    std::ifstream f(input, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string base = "file://" + input;
    mbLoadHTML(view, ss.str().c_str(), base.c_str());
  }

  // For network loads, detect a failed/empty navigation so we don't silently
  // emit a blank PNG and report success. A failed fetch (DNS/TLS/HTTP error,
  // or throttling) makes the loader call DidFail, and Blink commits a near-empty
  // document (~tens of chars of "<html><head></head><body></body></html>"); a
  // real page is thousands. Below the threshold we treat the load as failed.
  bool load_ok = true;
  if (is_http) {
    char buf[64] = {0};
    mbEvalJS(view, "String(document.documentElement.outerHTML.length)", buf,
             sizeof(buf));
    const long html_len = std::atol(buf);
    if (html_len < 512) {
      std::fprintf(stderr,
                   "mb_shot: WARNING — %s loaded an empty document (HTML %ld "
                   "bytes); the fetch likely failed (network error/throttling). "
                   "The PNG will be blank.\n",
                   input.c_str(), html_len);
      load_ok = false;
    }
  }

  // Wait for dynamic content before capturing: a selector to appear and/or a fixed
  // delay (lets JS-rendered / setTimeout content settle).
  if (!wait_selector.empty()) {
    if (!mbWaitForSelector(view, wait_selector.c_str(),
                           wait_ms > 0 ? wait_ms : 5000)) {
      std::fprintf(stderr,
                   "mb_shot: WARNING — --wait-selector '%s' never appeared\n",
                   wait_selector.c_str());
    }
  } else if (wait_ms > 0) {
    mbWait(view, wait_ms);
  }

  // Optionally fill a field before interacting/capturing (e.g. type a query, then
  // --click the submit button). Runs before --click so a fill+submit flow works.
  if (!fill_selector.empty()) {
    if (!mbFillSelector(view, fill_selector.c_str(), fill_text.c_str())) {
      std::fprintf(stderr, "mb_shot: WARNING — --fill '%s' matched no element\n",
                   fill_selector.c_str());
    }
  }

  // Optionally click an element (e.g. to expand a menu / dismiss a banner) before
  // capturing, then let the result settle.
  if (!click_selector.empty()) {
    if (!mbClickSelector(view, click_selector.c_str())) {
      std::fprintf(stderr, "mb_shot: WARNING — --click '%s' matched no element\n",
                   click_selector.c_str());
    } else {
      mbWait(view, wait_ms > 0 ? wait_ms : 100);  // let the click's effects render
    }
  }

  if (print_console) {
    char cbuf[8192] = {0};
    mbDrainConsole(view, cbuf, sizeof(cbuf));
    if (cbuf[0])
      std::fprintf(stderr, "---- page console ----\n%s----------------------\n", cbuf);
  }

  // --text: dump the page's visible text to stdout (a scraping mode). innerText
  // reflects post-JS rendered text; large pages are truncated to the buffer.
  if (print_text) {
    std::vector<char> tbuf(1 << 20, 0);  // 1 MiB
    mbEvalJS(view, "document.body ? document.body.innerText : ''", tbuf.data(),
             static_cast<int>(tbuf.size()));
    std::fwrite(tbuf.data(), 1, std::strlen(tbuf.data()), stdout);
    std::fputc('\n', stdout);
  }

  // --html: dump the rendered (post-JS) DOM as serialized HTML to stdout — useful
  // for scraping SPAs whose fetched source is near-empty but render content.
  if (print_html) {
    std::vector<char> hbuf(1 << 21, 0);  // 2 MiB
    mbEvalJS(view, "document.documentElement ? document.documentElement.outerHTML : ''",
             hbuf.data(), static_cast<int>(hbuf.size()));
    std::fwrite(hbuf.data(), 1, std::strlen(hbuf.data()), stdout);
    std::fputc('\n', stdout);
  }

  // --eval: run arbitrary JS after the page settles and print the string result
  // to stdout. Exposes the whole scripting/scraping surface from the CLI — element
  // counts (document.querySelectorAll('.x').length), computed styles, attribute
  // reads, any page state — without needing a dedicated flag per query.
  if (!eval_js.empty()) {
    std::vector<char> ebuf(1 << 20, 0);  // 1 MiB
    mbEvalJS(view, eval_js.c_str(), ebuf.data(), static_cast<int>(ebuf.size()));
    std::fwrite(ebuf.data(), 1, std::strlen(ebuf.data()), stdout);
    std::fputc('\n', stdout);
  }

  // Persist the cookie jar after the page has settled (so cookies set during the
  // load are captured) — for reuse on a later run via --load-cookies.
  if (!save_cookies.empty()) {
    if (!mbSaveCookies(save_cookies.c_str()))
      std::fprintf(stderr, "mb_shot: WARNING — --save-cookies '%s' unwritable\n",
                   save_cookies.c_str());
  }

  // Clip / element capture: resolve a logical rectangle and shoot just that. We
  // first grow the view to the full document height so the region is laid out and
  // painted even if it sits below the original fold.
  if (!clip.empty() || !selector.empty()) {
    char buf[64] = {0};
    mbEvalJS(view,
             "String(Math.max(document.documentElement.scrollHeight,"
             "document.body?document.body.scrollHeight:0))",
             buf, sizeof(buf));
    int page_h = std::atoi(buf);
    if (page_h > h)
      page_h = page_h < kMaxFullPageHeight ? page_h : kMaxFullPageHeight;
    mbResize(view, w, page_h > h ? page_h : h);

    int cx = 0, cy = 0, cw = 0, ch = 0;
    if (!selector.empty()) {
      // Element box in page coords (viewport rect + scroll offset).
      std::string js =
          "(function(){var e=document.querySelector(" "'" + selector +
          "'" ");if(!e)return '';var r=e.getBoundingClientRect();"
          "return [Math.round(r.left+scrollX),Math.round(r.top+scrollY),"
          "Math.round(r.width),Math.round(r.height)].join(',');})()";
      char rb[128] = {0};
      mbEvalJS(view, js.c_str(), rb, sizeof(rb));
      if (std::sscanf(rb, "%d,%d,%d,%d", &cx, &cy, &cw, &ch) != 4 || cw <= 0 ||
          ch <= 0) {
        std::fprintf(stderr, "mb_shot: --selector '%s' matched no element\n",
                     selector.c_str());
        mbDestroyView(view);
        mbShutdown();
        return 1;
      }
    } else {
      if (std::sscanf(clip.c_str(), "%d,%d,%d,%d", &cx, &cy, &cw, &ch) != 4) {
        std::fprintf(stderr, "mb_shot: --clip wants x,y,w,h (got '%s')\n",
                     clip.c_str());
        mbDestroyView(view);
        mbShutdown();
        return 2;
      }
    }
    const int ok = mbSavePngRect(view, out, cx, cy, cw, ch);
    std::fprintf(stderr, "mb_shot: %s -> %s (clip %d,%d %dx%d @%gx) %s\n",
                 input.c_str(), out, cx, cy, cw, ch, scale,
                 (ok && load_ok) ? "OK" : "FAILED");
    mbDestroyView(view);
    mbShutdown();
    return (ok && load_ok) ? 0 : 1;
  }

  // Full-page capture: grow the view to the document's content height so the
  // render covers everything below the fold, then shoot at that height. We query
  // after the load so layout reflects the real content.
  int shot_h = h;
  if (full_page) {
    char buf[64] = {0};
    mbEvalJS(view,
             "String(Math.max(document.documentElement.scrollHeight,"
             "document.body?document.body.scrollHeight:0))",
             buf, sizeof(buf));
    const int page_h = std::atoi(buf);
    if (page_h > h)
      shot_h = page_h < kMaxFullPageHeight ? page_h : kMaxFullPageHeight;
    mbResize(view, w, shot_h);  // reflow to the taller viewport; SavePng repaints
  }

  // A .pdf output prints the document to a paginated PDF instead of a raster image.
  const std::string out_s(out);
  const bool want_pdf = out_s.size() >= 4 &&
                        out_s.compare(out_s.size() - 4, 4, ".pdf") == 0;
  int ok;
  if (want_pdf) {
    ok = mbSavePdf(view, out);
    std::fprintf(stderr, "mb_shot: %s -> %s (PDF) %s\n", input.c_str(), out,
                 (ok && load_ok) ? "OK" : "FAILED");
  } else {
    // Physical output dimensions: logical size times the device pixel ratio. The
    // view stays sized in logical (CSS) px; PaintInto scales the canvas by the DSF.
    const int out_w = static_cast<int>(w * scale);
    const int out_h = static_cast<int>(shot_h * scale);
    ok = mbSavePng(view, out, out_w, out_h);
    std::fprintf(stderr, "mb_shot: %s -> %s (%dx%d) %s\n", input.c_str(), out, out_w,
                 out_h, (ok && load_ok) ? "OK" : "FAILED");
  }

  mbDestroyView(view);
  mbShutdown();
  return (ok && load_ok) ? 0 : 1;
}
