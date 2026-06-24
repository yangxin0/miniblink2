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
  bool print_headers = false;  // dump the response headers to stderr
  bool print_text = false;
  bool print_html = false;
  bool no_images = false;
  bool dark_mode = false;
  bool insecure = false;  // skip TLS cert verification
  bool no_follow = false;  // don't follow 3xx redirects
  std::string lang;  // navigator.language(s)
  std::string tz;    // IANA timezone for Date/Intl
  float scale = 1.0f;
  std::string clip;      // "x,y,w,h"
  std::string selector;  // CSS selector -> capture that element's box
  std::string headers;   // extra request headers, "Name: Value" per line
  std::string wait_selector;  // wait for this selector before capture
  std::string wait_visible;   // wait for this selector to be VISIBLE before capture
  std::string click_selector;  // click this selector before capture
  std::string fill_selector;   // fill this field before capture (with fill_text)
  std::string fill_text;       // value for --fill
  std::string eval_js;         // JS to run after load; result printed to stdout
  std::string value_selector;  // print this control's live .value to stdout
  std::string checked_selector;  // print this control's .checked (1/0) to stdout
  std::string visible_selector;  // print this selector's visibility (1/0/-1)
  std::string text_all_selector;  // print JSON array of all matches' innerText
  std::string attr_all_selector;  // print JSON array of all matches' attribute
  std::string attr_all_name;      // attribute name for --attr-all
  std::string proxy;           // libcurl proxy string for network fetches
  std::string load_cookies;    // cookie jar file to load before navigating
  std::string save_cookies;    // cookie jar file to write after the page settles
  int wait_ms = 0;            // fixed wait before capture
  int scroll_to_y = -1;       // absolute scroll Y before capture (-1 = none)
  std::string post_body;       // when set, POST this body to the URL (vs GET)
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
    } else if (a == "--wait-visible" && i + 1 < argc) {
      wait_visible = argv[++i];
    } else if (a == "--click" && i + 1 < argc) {
      click_selector = argv[++i];
    } else if (a == "--fill" && i + 2 < argc) {
      fill_selector = argv[++i];
      fill_text = argv[++i];
    } else if (a == "--eval" && i + 1 < argc) {
      eval_js = argv[++i];
    } else if (a == "--value" && i + 1 < argc) {
      value_selector = argv[++i];
    } else if (a == "--checked" && i + 1 < argc) {
      checked_selector = argv[++i];
    } else if (a == "--visible" && i + 1 < argc) {
      visible_selector = argv[++i];
    } else if (a == "--text-all" && i + 1 < argc) {
      text_all_selector = argv[++i];
    } else if (a == "--attr-all" && i + 2 < argc) {
      attr_all_selector = argv[++i];
      attr_all_name = argv[++i];
    } else if (a == "--proxy" && i + 1 < argc) {
      proxy = argv[++i];
    } else if (a == "--load-cookies" && i + 1 < argc) {
      load_cookies = argv[++i];
    } else if (a == "--save-cookies" && i + 1 < argc) {
      save_cookies = argv[++i];
    } else if (a == "--wait-ms" && i + 1 < argc) {
      wait_ms = std::atoi(argv[++i]);
    } else if (a == "--scroll-to" && i + 1 < argc) {
      scroll_to_y = std::atoi(argv[++i]);
    } else if (a == "--post" && i + 1 < argc) {
      post_body = argv[++i];
    } else if (a == "--console") {
      print_console = true;
    } else if (a == "--headers") {
      print_headers = true;
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
    } else if (a == "--no-follow") {
      no_follow = true;
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
        "[--transparent] [--text] [--html] [--eval JS] [--value CSS] "
        "[--checked CSS] [--visible CSS] [--text-all CSS] [--attr-all CSS NAME] "
        "[--fill CSS TEXT] "
        "[--click CSS] [--wait-selector CSS] [--wait-visible CSS] [--wait-ms N] "
        "[--scroll-to Y] "
        "[--post BODY] [--proxy URL] "
        "[--load-cookies FILE] [--save-cookies FILE] [--insecure] [--headers] "
        "[--no-follow] "
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
  if (no_follow)
    mbSetFollowRedirects(0);  // stop at the redirect (see status + Location)
  if (!load_cookies.empty()) {
    if (!mbLoadCookies(load_cookies.c_str()))  // restore a saved session
      std::fprintf(stderr, "mb_shot: WARNING — --load-cookies '%s' unreadable\n",
                   load_cookies.c_str());
  }

  const bool is_http = input.rfind("http", 0) == 0;
  if (is_http && !post_body.empty()) {
    mbPostURL(view, input.c_str(), post_body.c_str(), nullptr);  // POST navigation
  } else if (input.rfind("file://", 0) == 0 || is_http) {
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

  // For network loads, detect a failed navigation from the real HTTP status so we
  // don't silently emit a blank PNG and report success. Status 0 means no response
  // (DNS/TLS/connection error); >= 400 is an HTTP error page. A 2xx/3xx is treated
  // as success even when the body is small (this replaces an old body-length
  // heuristic that false-flagged tiny-but-valid pages).
  bool load_ok = true;
  if (is_http) {
    const int status = mbGetHttpStatus(view);
    if (status == 0) {
      std::fprintf(stderr,
                   "mb_shot: WARNING — %s got no response (network/DNS/TLS "
                   "error); the PNG will be blank.\n",
                   input.c_str());
      load_ok = false;
    } else if (status >= 400) {
      std::fprintf(stderr,
                   "mb_shot: WARNING — %s returned HTTP %d (error page).\n",
                   input.c_str(), status);
      load_ok = false;
    }
  }

  // --headers: dump the server's response headers (the last http(s) navigation)
  // to stderr — for inspecting Content-Type, caching, or custom/API headers.
  if (print_headers && is_http) {
    std::vector<char> hbuf(1 << 16, 0);  // 64 KiB
    if (mbGetResponseHeaders(view, hbuf.data(),
                             static_cast<int>(hbuf.size())) > 0)
      std::fprintf(stderr, "---- response headers ----\n%s--------------------------\n",
                   hbuf.data());
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
  // --wait-visible: block until the selector is actually shown (not just present)
  // — for content that mounts hidden then fades/toggles in.
  if (!wait_visible.empty()) {
    if (!mbWaitForVisibleSelector(view, wait_visible.c_str(),
                                  wait_ms > 0 ? wait_ms : 5000)) {
      std::fprintf(stderr,
                   "mb_shot: WARNING — --wait-visible '%s' never became visible\n",
                   wait_visible.c_str());
    }
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

  // Scroll to an absolute Y before extracting/capturing (so --eval and the shot
  // both observe the scrolled viewport). For position:fixed/sticky pages, where
  // --full's resize would render them wrong; best with a plain viewport capture.
  if (scroll_to_y >= 0)
    mbScrollTo(view, 0, scroll_to_y);

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

  // --value: print a control's LIVE .value (post-typing/selection) — pairs with
  // --fill to read back exactly what landed in the field. Empty line + warning on
  // no match (distinct from a genuinely empty value, which prints an empty line).
  if (!value_selector.empty()) {
    std::vector<char> vbuf(1 << 16, 0);  // 64 KiB
    int n = mbGetValueForSelector(view, value_selector.c_str(), vbuf.data(),
                                  static_cast<int>(vbuf.size()));
    if (n < 0)
      std::fprintf(stderr, "mb_shot: --value '%s' matched no element / no value\n",
                   value_selector.c_str());
    std::fwrite(vbuf.data(), 1, std::strlen(vbuf.data()), stdout);
    std::fputc('\n', stdout);
  }

  // --checked: print a checkbox/radio's .checked as 1/0 (or -1 on no match / a
  // non-checkable element) — pairs with --click, which toggles a checkbox.
  if (!checked_selector.empty()) {
    int c = mbGetCheckedForSelector(view, checked_selector.c_str());
    if (c < 0)
      std::fprintf(stderr, "mb_shot: --checked '%s' matched no checkable element\n",
                   checked_selector.c_str());
    std::fprintf(stdout, "%d\n", c);
  }

  // --visible: print whether a selector is actually shown — 1 visible, 0 hidden
  // (display:none / visibility:hidden / opacity:0), -1 on no match.
  if (!visible_selector.empty()) {
    int vis = mbIsVisibleForSelector(view, visible_selector.c_str());
    if (vis < 0)
      std::fprintf(stderr, "mb_shot: --visible '%s' matched no element\n",
                   visible_selector.c_str());
    std::fprintf(stdout, "%d\n", vis);
  }

  // --text-all: print a JSON array of every match's innerText (one-shot list
  // scraping). "[]" for no matches; a warning + empty line on an invalid selector.
  if (!text_all_selector.empty()) {
    std::vector<char> tab(1 << 20, 0);  // 1 MiB
    int n = mbGetAllTextForSelector(view, text_all_selector.c_str(), tab.data(),
                                    static_cast<int>(tab.size()));
    if (n < 0)
      std::fprintf(stderr, "mb_shot: --text-all '%s' invalid selector\n",
                   text_all_selector.c_str());
    std::fwrite(tab.data(), 1, std::strlen(tab.data()), stdout);
    std::fputc('\n', stdout);
  }

  // --attr-all CSS NAME: print a JSON array of attribute NAME across all matches
  // (absent -> null). "[]" for no matches; warning on an invalid selector.
  if (!attr_all_selector.empty()) {
    std::vector<char> aab(1 << 20, 0);  // 1 MiB
    int n = mbGetAllAttributeForSelector(view, attr_all_selector.c_str(),
                                         attr_all_name.c_str(), aab.data(),
                                         static_cast<int>(aab.size()));
    if (n < 0)
      std::fprintf(stderr, "mb_shot: --attr-all '%s' invalid selector\n",
                   attr_all_selector.c_str());
    std::fwrite(aab.data(), 1, std::strlen(aab.data()), stdout);
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
