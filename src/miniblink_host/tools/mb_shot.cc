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
//   --script-timeout MS  terminate a single page-JS task that runs longer than MS
//                      (guards against while(true){} / infinite microtask floods that
//                      would otherwise hang the load forever). Off by default.
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
#include <utility>
#include <vector>

#include "miniblink2/automation.h"

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
  bool print_title = false;     // print document.title to stdout
  bool print_url = false;       // print the current document URL (post-redirect)
  std::string cookies_url;      // print the jar's cookies for this origin to stdout
  std::string local_storage_key;    // print localStorage[key] for the doc origin
  std::string session_storage_key;  // print sessionStorage[key] for the doc origin
  bool print_requests = false;  // dump the subresource request log to stdout
  bool wait_idle = false;       // wait for network idle before capture (networkidle)
  bool auto_scroll = false;     // scroll through the page to load lazy content
  bool no_images = false;
  bool dark_mode = false;
  bool insecure = false;  // skip TLS cert verification
  bool no_follow = false;  // don't follow 3xx redirects
  std::string lang;  // navigator.language(s)
  std::string tz;    // IANA timezone for Date/Intl
  float scale = 1.0f;
  bool scale_set = false;  // was --scale given explicitly (vs the mobile default)
  bool mobile = false;     // --mobile: mobile UA + DPR 3 + 390x844 default viewport
  std::string clip;      // "x,y,w,h"
  std::string selector;  // CSS selector -> capture that element's box
  std::string require_selector;  // assert this selector matches; else exit 3
  std::string headers;   // extra request headers, "Name: Value" per line
  std::string wait_selector;  // wait for this selector before capture
  std::string wait_visible;   // wait for this selector to be VISIBLE before capture
  std::string wait_hidden;    // wait for this selector to be GONE/HIDDEN before capture
  std::string wait_eval;      // wait until this JS expression is truthy before capture
  std::string inject_css;      // inject this CSS (hide noise) before capture
  std::string click_selector;  // click this selector before capture
  std::string dispatch_sel;    // dispatch a DOM event on this selector...
  std::string dispatch_evt;    // ...of this type before capture (--dispatch CSS EVT)
  std::string drag_from;       // drag this selector...
  std::string drag_to;         // ...onto this one before capture (--drag FROM TO)
  std::string fill_selector;   // fill this field before capture (with fill_text)
  std::string fill_text;       // value for --fill
  std::string press_key;       // named key to press after interacting (--press KEY)
  std::string eval_js;         // JS to run after load; result printed to stdout
  std::string eval_json;       // JS expression, printed JSON.stringify'd (structured)
  int eval_frame = -1;         // --frame N: run --eval/--eval-json in child frame N
                               // (0-based; -1 = main frame). Reads cross-origin iframes.
  double pdf_w = 0, pdf_h = 0;  // --pdf-size: page size in points (0 = Letter)
  bool pdf_landscape = false;   // --landscape: swap PDF page width/height
  double pdf_scale = 1.0;       // --pdf-scale: PDF content scale (100% = 1.0)
  double pdf_margin = 0;        // --pdf-margin: uniform PDF margin in points
  std::string value_selector;  // print this control's live .value to stdout
  std::string html_for_selector;  // print the first match's outerHTML to stdout
  std::string checked_selector;  // print this control's .checked (1/0) to stdout
  std::string count_selector;    // print querySelectorAll length (>=0) to stdout
  std::string visible_selector;  // print this selector's visibility (1/0/-1)
  std::string rect_selector;     // print this selector's bounding box "x,y,w,h"
  std::string style_selector;    // print a computed style property of this selector
  std::string style_prop;        // the CSS property name for --style
  std::string text_all_selector;  // print JSON array of all matches' innerText
  std::string attr_selector;      // print first match's attribute value
  std::string attr_name;          // attribute name for --attr
  std::string attr_all_selector;  // print JSON array of all matches' attribute
  std::string attr_all_name;      // attribute name for --attr-all
  std::string proxy;           // libcurl proxy string for network fetches
  std::string load_cookies;    // cookie jar file to load before navigating
  std::string save_cookies;    // cookie jar file to write after the page settles
  int wait_ms = 0;            // fixed wait before capture
  int script_timeout_ms = 0;  // kill a single task whose JS runs >this (0=off)
  int scroll_to_y = -1;       // absolute scroll Y before capture (-1 = none)
  std::string scroll_to_sel;  // scroll this selector into view before capture
  std::string post_body;       // when set, POST this body to the URL (vs GET)
  std::string user_agent;      // override the User-Agent for the navigation
  std::vector<const char*> blocks;  // URL substrings to block (repeatable)
  std::vector<std::pair<const char*, const char*>> mocks;     // (url_substr, file)
  std::vector<std::pair<const char*, const char*>> rewrites;  // (from, to)
  std::vector<std::pair<const char*, const char*>> set_cookies;  // (url, cookie)
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
      scale_set = true;
    } else if (a == "--mobile") {
      mobile = true;
    } else if (a == "--clip" && i + 1 < argc) {
      clip = argv[++i];
    } else if (a == "--selector" && i + 1 < argc) {
      selector = argv[++i];
    } else if (a == "--require" && i + 1 < argc) {
      require_selector = argv[++i];
    } else if (a == "--wait-selector" && i + 1 < argc) {
      wait_selector = argv[++i];
    } else if (a == "--wait-visible" && i + 1 < argc) {
      wait_visible = argv[++i];
    } else if (a == "--wait-hidden" && i + 1 < argc) {
      wait_hidden = argv[++i];
    } else if (a == "--wait-eval" && i + 1 < argc) {
      wait_eval = argv[++i];
    } else if (a == "--wait-idle") {
      wait_idle = true;
    } else if (a == "--css" && i + 1 < argc) {
      inject_css = argv[++i];
    } else if (a == "--click" && i + 1 < argc) {
      click_selector = argv[++i];
    } else if (a == "--drag" && i + 2 < argc) {
      drag_from = argv[++i];
      drag_to = argv[++i];
    } else if (a == "--dispatch" && i + 2 < argc) {
      dispatch_sel = argv[++i];
      dispatch_evt = argv[++i];
    } else if (a == "--fill" && i + 2 < argc) {
      fill_selector = argv[++i];
      fill_text = argv[++i];
    } else if (a == "--press" && i + 1 < argc) {
      press_key = argv[++i];
    } else if (a == "--eval" && i + 1 < argc) {
      eval_js = argv[++i];
    } else if (a == "--eval-json" && i + 1 < argc) {
      eval_json = argv[++i];
    } else if (a == "--frame" && i + 1 < argc) {
      eval_frame = std::atoi(argv[++i]);
    } else if (a == "--pdf-size" && i + 1 < argc) {
      // A named page or "WxH" in points (letter/a4/legal/a3/tabloid).
      std::string s = argv[++i];
      for (char& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
      if (s == "letter") { pdf_w = 612; pdf_h = 792; }
      else if (s == "legal") { pdf_w = 612; pdf_h = 1008; }
      else if (s == "a4") { pdf_w = 595; pdf_h = 842; }
      else if (s == "a3") { pdf_w = 842; pdf_h = 1191; }
      else if (s == "tabloid") { pdf_w = 792; pdf_h = 1224; }
      else { double pw = 0, ph = 0;
             if (std::sscanf(s.c_str(), "%lfx%lf", &pw, &ph) == 2) { pdf_w = pw; pdf_h = ph; } }
    } else if (a == "--landscape") {
      pdf_landscape = true;
    } else if (a == "--pdf-scale" && i + 1 < argc) {
      pdf_scale = std::atof(argv[++i]);
    } else if (a == "--pdf-margin" && i + 1 < argc) {
      pdf_margin = std::atof(argv[++i]);
    } else if (a == "--value" && i + 1 < argc) {
      value_selector = argv[++i];
    } else if (a == "--html-for" && i + 1 < argc) {
      html_for_selector = argv[++i];
    } else if (a == "--checked" && i + 1 < argc) {
      checked_selector = argv[++i];
    } else if (a == "--count" && i + 1 < argc) {
      count_selector = argv[++i];
    } else if (a == "--visible" && i + 1 < argc) {
      visible_selector = argv[++i];
    } else if (a == "--rect" && i + 1 < argc) {
      rect_selector = argv[++i];
    } else if (a == "--style" && i + 2 < argc) {
      style_selector = argv[++i];
      style_prop = argv[++i];
    } else if (a == "--text-all" && i + 1 < argc) {
      text_all_selector = argv[++i];
    } else if (a == "--attr" && i + 2 < argc) {
      attr_selector = argv[++i];
      attr_name = argv[++i];
    } else if (a == "--attr-all" && i + 2 < argc) {
      attr_all_selector = argv[++i];
      attr_all_name = argv[++i];
    } else if (a == "--proxy" && i + 1 < argc) {
      proxy = argv[++i];
    } else if (a == "--set-cookie" && i + 2 < argc) {
      const char* u = argv[++i];
      set_cookies.emplace_back(u, argv[++i]);
    } else if (a == "--load-cookies" && i + 1 < argc) {
      load_cookies = argv[++i];
    } else if (a == "--save-cookies" && i + 1 < argc) {
      save_cookies = argv[++i];
    } else if (a == "--wait-ms" && i + 1 < argc) {
      wait_ms = std::atoi(argv[++i]);
    } else if (a == "--script-timeout" && i + 1 < argc) {
      script_timeout_ms = std::atoi(argv[++i]);
    } else if (a == "--scroll-to" && i + 1 < argc) {
      scroll_to_y = std::atoi(argv[++i]);
    } else if (a == "--scroll-to-selector" && i + 1 < argc) {
      scroll_to_sel = argv[++i];
    } else if (a == "--post" && i + 1 < argc) {
      post_body = argv[++i];
    } else if ((a == "--user-agent" || a == "--ua") && i + 1 < argc) {
      user_agent = argv[++i];
    } else if (a == "--console") {
      print_console = true;
    } else if (a == "--headers") {
      print_headers = true;
    } else if (a == "--text") {
      print_text = true;
    } else if (a == "--html") {
      print_html = true;
    } else if (a == "--title") {
      print_title = true;
    } else if (a == "--url") {
      print_url = true;
    } else if (a == "--cookies" && i + 1 < argc) {
      cookies_url = argv[++i];
    } else if (a == "--local-storage" && i + 1 < argc) {
      local_storage_key = argv[++i];
    } else if (a == "--session-storage" && i + 1 < argc) {
      session_storage_key = argv[++i];
    } else if (a == "--requests") {
      print_requests = true;
    } else if (a == "--auto-scroll") {
      auto_scroll = true;
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
    } else if (a == "--block" && i + 1 < argc) {
      blocks.push_back(argv[++i]);
    } else if (a == "--mock" && i + 2 < argc) {
      mocks.emplace_back(argv[i + 1], argv[i + 2]);
      i += 2;
    } else if (a == "--rewrite" && i + 2 < argc) {
      rewrites.emplace_back(argv[i + 1], argv[i + 2]);
      i += 2;
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
        "usage: %s [--full] [--scale N] [--mobile] [--clip x,y,w,h] [--selector CSS] "
        "[--transparent] [--title] [--url] [--cookies URL] "
        "[--local-storage KEY] [--session-storage KEY] [--text] [--html] [--html-for CSS] [--requests] [--eval JS] [--eval-json JS] [--frame N] [--value CSS] "
        "[--checked CSS] [--count CSS] [--visible CSS] [--rect CSS] [--style CSS PROP] "
        "[--text-all CSS] [--attr CSS NAME] [--attr-all CSS NAME] "
        "[--fill CSS TEXT] "
        "[--click CSS] [--drag FROM TO] [--dispatch CSS EVT] [--press KEY] "
        "[--wait-selector CSS] [--wait-visible CSS] "
        "[--wait-hidden CSS] [--wait-eval JS] [--wait-idle] [--css STYLES] [--auto-scroll] "
        "[--wait-ms N] [--script-timeout MS] "
        "[--scroll-to Y] [--scroll-to-selector CSS] "
        "[--post BODY] [--proxy URL] "
        "[--load-cookies FILE] [--save-cookies FILE] [--insecure] [--headers] "
        "[--no-follow] [--block SUBSTR] [--mock URL FILE] [--rewrite FROM TO] "
        "[--set-cookie URL COOKIE] [--user-agent UA] "
        "[--require CSS] "
        "[--pdf-size letter|a4|legal|a3|tabloid|WxH] [--landscape] [--pdf-scale N] [--pdf-margin PT] "
        "<input.html|file://URL|http(s)://URL> <out.(png|pdf)> [width height]\n",
        argv[0]);
    return 2;
  }
  const std::string input = pos[0];
  const char* out = pos[1];
  // --mobile presets a phone-ish viewport / DPR / UA; explicit width/height,
  // --scale, and --user-agent each still override their respective default.
  const int w = pos.size() > 2 ? std::atoi(pos[2]) : (mobile ? 390 : 1200);
  const int h = pos.size() > 3 ? std::atoi(pos[3]) : (mobile ? 844 : 800);
  if (mobile && !scale_set)
    scale = 3.0f;  // typical phone device-pixel-ratio (retina)

  // A non-positive width/height (e.g. a non-numeric positional reaching atoi,
  // the classic mistake of using a "--out path" flag that mb_shot doesn't have)
  // would allocate a zero-size bitmap and abort deep in Skia's PNG encoder.
  // Fail fast with a clear message instead.
  if (w <= 0 || h <= 0) {
    std::fprintf(stderr,
                 "mb_shot: width/height must be positive (got %dx%d). "
                 "The output path is a positional arg, not a flag: "
                 "mb_shot <input> <out.png> [width height]\n",
                 w, h);
    return 2;
  }

  if (!mbInitialize()) {
    std::fprintf(stderr, "mb_shot: engine init failed\n");
    return 1;
  }
  // Guard against a runaway page script (sync infinite loop / microtask flood) that
  // would otherwise hang the load forever. Process-global; off unless requested.
  if (script_timeout_ms > 0)
    mbSetScriptTimeout(script_timeout_ms);
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
  if (user_agent.empty() && mobile) {
    // Default mobile UA so sites that UA-sniff serve their mobile layout (paired
    // with the narrow view). An explicit --user-agent overrides this.
    user_agent =
        "Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) "
        "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Mobile/15E148 "
        "Safari/604.1";
  }
  if (!user_agent.empty())
    mbSetUserAgent(view, user_agent.c_str());  // before load so the navigation uses it
  if (!headers.empty())
    mbSetExtraHeaders(view, headers.c_str());  // before load so the navigation uses them
  if (!proxy.empty())
    mbSetProxy(proxy.c_str());  // route fetches through the proxy (process-wide)
  if (insecure)
    mbSetIgnoreCertErrors(1);  // skip TLS cert verification (self-signed/expired)
  if (no_follow)
    mbSetFollowRedirects(0);  // stop at the redirect (see status + Location)
  for (const char* b : blocks)
    mbBlockUrl(b);  // drop matching subresources (ads/trackers/images) before load
  // --rewrite FROM TO: redirect matching requests before fetch (host swap, CDN ->
  // local mock). Applied before --mock so a rewrite can land on a mocked URL.
  for (const auto& rw : rewrites)
    mbRewriteUrl(rw.first, rw.second);
  // --mock URL FILE: serve FILE's bytes for requests whose URL contains URL, with
  // no real fetch — substitute an API/resource response or run a page offline.
  // Content-Type is guessed from FILE's extension (css/js/json/svg/html/xml/png/
  // txt), defaulting to text/plain.
  for (const auto& mk : mocks) {
    std::ifstream mf(mk.second, std::ios::binary);
    if (!mf.is_open()) {
      std::fprintf(stderr, "mb_shot: WARNING — --mock body file '%s' unreadable\n",
                   mk.second);
      continue;
    }
    std::stringstream ms;
    ms << mf.rdbuf();
    const std::string body = ms.str();
    const std::string f(mk.second);
    const std::string ext =
        f.rfind('.') != std::string::npos ? f.substr(f.rfind('.') + 1) : "";
    const char* ct = "text/plain";
    if (ext == "css") ct = "text/css";
    else if (ext == "js") ct = "application/javascript";
    else if (ext == "json") ct = "application/json";
    else if (ext == "svg") ct = "image/svg+xml";
    else if (ext == "html" || ext == "htm") ct = "text/html";
    else if (ext == "xml") ct = "application/xml";
    else if (ext == "png") ct = "image/png";
    mbMockResponse(mk.first, body.c_str(), ct, 200);
  }
  if (!load_cookies.empty()) {
    if (!mbLoadCookies(load_cookies.c_str()))  // restore a saved session
      std::fprintf(stderr, "mb_shot: WARNING — --load-cookies '%s' unreadable\n",
                   load_cookies.c_str());
  }
  // --set-cookie URL COOKIE: inject a cookie into the jar before navigating (sent
  // on the navigation + subresource requests) — inline session injection without
  // crafting a cookie file. Applied after --load-cookies so it can override.
  for (const auto& sc : set_cookies)
    mbSetCookie(view, sc.first, sc.second);

  if (print_requests || wait_idle)
    mbClearRequestLog();  // scope the log to this navigation's subresources

  const bool is_http = input.rfind("http", 0) == 0;
  if (is_http && !post_body.empty()) {
    mbPostURL(view, input.c_str(), post_body.c_str(), nullptr);  // POST navigation
  } else if (input.rfind("file://", 0) == 0 || is_http) {
    if (!is_http) {
      // A file:// URL to a local path: verify it exists, consistent with the
      // bare-path branch below — a missing file must fail, not blank-PNG-succeed.
      // Only the file:///abs form (path starts with '/') and only when there's no
      // percent-encoding (we don't decode here, so skip to avoid a false failure).
      std::string path = input.substr(7);  // strip "file://"
#ifdef _WIN32
      // "file:///C:/x" -> path "/C:/x"; drop the leading '/' before a drive.
      if (path.size() >= 3 && path[0] == '/' && path[2] == ':')
        path.erase(0, 1);
      const bool looks_abs = path.size() >= 2 && path[1] == ':';
#else
      const bool looks_abs = !path.empty() && path[0] == '/';
#endif
      if (looks_abs && path.find('%') == std::string::npos) {
        std::ifstream probe(path, std::ios::binary);
        if (!probe.is_open()) {
          std::fprintf(stderr, "mb_shot: cannot open input file '%s'\n",
                       input.c_str());
          mbDestroyView(view);
          mbShutdown();
          return 1;
        }
      }
    }
    mbLoadURL(view, input.c_str());
  } else {
    // A local HTML file path: read it and commit (base URL = its file:// dir so that
    // relative subresources resolve through the loader).
    std::ifstream f(input, std::ios::binary);
    if (!f.is_open()) {
      // A missing/unreadable input file must fail, not silently commit empty HTML
      // and "succeed" with a blank PNG (a typo'd path is the common case). An
      // empty-but-existing file still opens, so it stays valid (renders blank).
      std::fprintf(stderr, "mb_shot: cannot open input file '%s'\n", input.c_str());
      mbDestroyView(view);
      mbShutdown();
      return 1;
    }
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
  // --wait-hidden: block until the selector is gone/hidden — the canonical "wait
  // for the loading spinner to disappear" before scraping/capturing.
  if (!wait_hidden.empty()) {
    if (!mbWaitForSelectorHidden(view, wait_hidden.c_str(),
                                 wait_ms > 0 ? wait_ms : 5000)) {
      std::fprintf(stderr,
                   "mb_shot: WARNING — --wait-hidden '%s' still visible at timeout\n",
                   wait_hidden.c_str());
    }
  }
  // --wait-eval JS: block until an arbitrary JS expression is truthy (Puppeteer's
  // waitForFunction) — the general condition wait, e.g. "window.appReady" or
  // "document.querySelectorAll('.row').length > 10", for state a selector can't
  // express.
  if (!wait_eval.empty()) {
    if (!mbWaitForFunction(view, wait_eval.c_str(), wait_ms > 0 ? wait_ms : 5000)) {
      std::fprintf(stderr,
                   "mb_shot: WARNING — --wait-eval '%s' never became truthy\n",
                   wait_eval.c_str());
    }
  }
  // --wait-idle: let the page's deferred fetches / lazy images settle (Puppeteer
  // networkidle) before scraping/capturing — for SPAs that fetch after load.
  if (wait_idle) {
    if (!mbWaitForNetworkIdleEx(view, 500, wait_ms > 0 ? wait_ms : 10000))
      std::fprintf(stderr,
                   "mb_shot: WARNING — --wait-idle still busy at timeout\n");
  }
  // --css: inject a stylesheet (hide cookie banners / ads / sticky headers, or
  // restyle) before capture. Applied after the waits so it lands on settled DOM.
  if (!inject_css.empty()) {
    if (!mbInsertCSS(view, inject_css.c_str()))
      std::fprintf(stderr, "mb_shot: WARNING — --css injection failed\n");
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

  // --drag FROM TO: mouse-drag one element's center onto another's (slider thumb,
  // sortable item, map pan) before capturing, then let it settle.
  if (!drag_from.empty()) {
    if (!mbDragSelector(view, drag_from.c_str(), drag_to.c_str())) {
      std::fprintf(stderr,
                   "mb_shot: WARNING — --drag '%s' -> '%s' matched no element\n",
                   drag_from.c_str(), drag_to.c_str());
    } else {
      mbWait(view, wait_ms > 0 ? wait_ms : 100);
    }
  }

  // --dispatch CSS EVT: fire a synthetic DOM event (mouseover hover menu, custom
  // framework event, …) that click/fill don't, before capturing, then settle.
  if (!dispatch_sel.empty()) {
    if (!mbDispatchEvent(view, dispatch_sel.c_str(), dispatch_evt.c_str())) {
      std::fprintf(stderr,
                   "mb_shot: WARNING — --dispatch '%s' matched no element\n",
                   dispatch_sel.c_str());
    } else {
      mbWait(view, wait_ms > 0 ? wait_ms : 100);
    }
  }

  // --press KEY: press a named non-text key as a trusted event so its default
  // action fires — "Enter" to submit a search/form, "Tab" to advance focus,
  // "Escape" to dismiss a modal, arrows, etc. Runs last in the interact phase so
  // it follows --fill/--click (the canonical "--fill q --press Enter" submit), then
  // settles. Unknown key names are a no-op (per mbSendKey).
  if (!press_key.empty()) {
    mbSendKey(view, press_key.c_str());
    mbWait(view, wait_ms > 0 ? wait_ms : 100);  // let the default action render
  }

  // --auto-scroll: scroll through the page to trigger lazy-load / infinite-scroll
  // so deferred images and content materialize before extract/capture (pairs with
  // --full). Runs before --scroll-to so a final fixed position can still be set.
  if (auto_scroll)
    mbScrollToBottom(view, 0);  // default step cap

  // --scroll-to-selector: bring a specific element into view before a viewport
  // capture (show it in context — distinct from --selector, which clips just its
  // box). An absolute --scroll-to, if also given, applies after and wins.
  if (!scroll_to_sel.empty()) {
    if (!mbScrollIntoView(view, scroll_to_sel.c_str()))
      std::fprintf(stderr,
                   "mb_shot: WARNING — --scroll-to-selector '%s' matched no element\n",
                   scroll_to_sel.c_str());
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

  // --title: print document.title to stdout (the most basic scrape field).
  if (print_title) {
    char tbuf[2048] = {0};
    mbGetTitle(view, tbuf, sizeof(tbuf));
    std::fwrite(tbuf, 1, std::strlen(tbuf), stdout);
    std::fputc('\n', stdout);
  }

  // --url: print the current document URL — the landing URL after any redirects,
  // distinct from the input (e.g. http->https or a login bounce).
  if (print_url) {
    char ubuf[4096] = {0};
    mbGetURL(view, ubuf, sizeof(ubuf));
    std::fwrite(ubuf, 1, std::strlen(ubuf), stdout);
    std::fputc('\n', stdout);
  }

  // --cookies URL: print the jar's cookies for URL's origin ("name=value;
  // name2=value2", request-header form) to stdout — the inspection peer of
  // --set-cookie/--save-cookies, e.g. read a session token after a login flow.
  // An explicit origin (vs the current page) so it works regardless of how the
  // page loaded; empty for a non-http(s) URL or an origin with no cookies.
  if (!cookies_url.empty()) {
    std::vector<char> cbuf(1 << 16, 0);  // 64 KiB
    mbGetCookies(view, cookies_url.c_str(), cbuf.data(),
                 static_cast<int>(cbuf.size()));
    std::fwrite(cbuf.data(), 1, std::strlen(cbuf.data()), stdout);
    std::fputc('\n', stdout);
  }

  // --local-storage KEY / --session-storage KEY: print a Web Storage value for
  // the document's origin to stdout (an SPA's auth token / app state). Empty line
  // + stderr warning when the key is absent (getter returns -1), distinct from a
  // genuinely empty stored value. Origin-scoped, like cookies.
  if (!local_storage_key.empty()) {
    std::vector<char> lbuf(1 << 16, 0);  // 64 KiB
    int n = mbGetLocalStorage(view, local_storage_key.c_str(), lbuf.data(),
                              static_cast<int>(lbuf.size()));
    if (n < 0)
      std::fprintf(stderr, "mb_shot: --local-storage '%s' not set\n",
                   local_storage_key.c_str());
    std::fwrite(lbuf.data(), 1, std::strlen(lbuf.data()), stdout);
    std::fputc('\n', stdout);
  }
  if (!session_storage_key.empty()) {
    std::vector<char> sbuf(1 << 16, 0);  // 64 KiB
    int n = mbGetSessionStorage(view, session_storage_key.c_str(), sbuf.data(),
                                static_cast<int>(sbuf.size()));
    if (n < 0)
      std::fprintf(stderr, "mb_shot: --session-storage '%s' not set\n",
                   session_storage_key.c_str());
    std::fwrite(sbuf.data(), 1, std::strlen(sbuf.data()), stdout);
    std::fputc('\n', stdout);
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

  // --html-for CSS: print the first match's outerHTML (the element + its markup)
  // — extract one fragment (article body, a table, a card) instead of the whole
  // document. Empty line + stderr warning on no match (mbGetHtmlForSelector -1).
  if (!html_for_selector.empty()) {
    std::vector<char> hfb(1 << 21, 0);  // 2 MiB
    int n = mbGetHtmlForSelector(view, html_for_selector.c_str(), hfb.data(),
                                 static_cast<int>(hfb.size()));
    if (n < 0)
      std::fprintf(stderr, "mb_shot: --html-for '%s' matched no element\n",
                   html_for_selector.c_str());
    std::fwrite(hfb.data(), 1, std::strlen(hfb.data()), stdout);
    std::fputc('\n', stdout);
  }

  // --requests: dump the subresource fetch log (one URL per line) — an asset/
  // tracker inventory of what the page actually requested.
  if (print_requests) {
    std::vector<char> rbuf(1 << 20, 0);  // 1 MiB
    mbGetRequestLog(rbuf.data(), static_cast<int>(rbuf.size()));
    std::fwrite(rbuf.data(), 1, std::strlen(rbuf.data()), stdout);
  }

  // --eval: run arbitrary JS after the page settles and print the string result
  // to stdout. Exposes the whole scripting/scraping surface from the CLI — element
  // counts (document.querySelectorAll('.x').length), computed styles, attribute
  // reads, any page state — without needing a dedicated flag per query.
  // --frame N (with --eval/--eval-json) targets the Nth child frame instead of the
  // main frame, running host-privileged in that frame's own world — so it scrapes
  // even a cross-origin iframe whose content the page itself can't read.
  auto eval_into = [&](const char* js, char* out, int cap) {
    if (eval_frame >= 0)
      mbEvalJSInFrame(view, eval_frame, js, out, cap);
    else
      mbEvalJS(view, js, out, cap);
  };
  if (!eval_js.empty()) {
    std::vector<char> ebuf(1 << 20, 0);  // 1 MiB
    eval_into(eval_js.c_str(), ebuf.data(), static_cast<int>(ebuf.size()));
    std::fwrite(ebuf.data(), 1, std::strlen(ebuf.data()), stdout);
    std::fputc('\n', stdout);
  }

  // --eval-json: like --eval but JSON.stringify's the expression, so an object or
  // array comes out as real JSON instead of "[object Object]" / a lossy comma-join
  // — the structured-scraping path (e.g. map result rows to {title,href}). A
  // value that doesn't serialize (undefined / a function) prints the empty string.
  if (!eval_json.empty()) {
    std::vector<char> jbuf(1 << 20, 0);  // 1 MiB
    const std::string wrapped = "JSON.stringify((" + eval_json + "))";
    eval_into(wrapped.c_str(), jbuf.data(), static_cast<int>(jbuf.size()));
    std::fwrite(jbuf.data(), 1, std::strlen(jbuf.data()), stdout);
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

  // --count: print the number of elements matching the selector
  // (querySelectorAll length; 0 is valid). -1 -> a null/invalid selector;
  // print 0 to stdout in that case but warn on stderr.
  if (!count_selector.empty()) {
    int n = mbCountSelector(view, count_selector.c_str());
    if (n < 0) {
      std::fprintf(stderr, "mb_shot: --count '%s' invalid selector\n",
                   count_selector.c_str());
      n = 0;
    }
    std::fprintf(stdout, "%d\n", n);
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

  // --rect: print the first match's viewport-relative bounding box as "x,y,w,h"
  // (logical px) — locate an element, compute a crop, or feed coordinates onward.
  if (!rect_selector.empty()) {
    int x = 0, y = 0, rw = 0, rh = 0;
    if (mbGetElementRect(view, rect_selector.c_str(), &x, &y, &rw, &rh))
      std::fprintf(stdout, "%d,%d,%d,%d\n", x, y, rw, rh);
    else
      std::fprintf(stderr, "mb_shot: --rect '%s' matched no element\n",
                   rect_selector.c_str());
  }

  // --style CSS PROP: print a resolved computed-style value (color -> "rgb(...)",
  // display:none -> "none") — CSS / visual assertions from the command line.
  if (!style_selector.empty()) {
    std::vector<char> sb(1 << 14, 0);  // 16 KiB
    int n = mbGetComputedStyle(view, style_selector.c_str(), style_prop.c_str(),
                               sb.data(), static_cast<int>(sb.size()));
    if (n < 0)
      std::fprintf(stderr, "mb_shot: --style '%s' matched no element\n",
                   style_selector.c_str());
    std::fwrite(sb.data(), 1, std::strlen(sb.data()), stdout);
    std::fputc('\n', stdout);
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

  // --attr CSS NAME: print the first match's NAME attribute value (e.g. an href
  // or src), then a newline. Empty line + a stderr warning when no element
  // matches or the attribute is absent (mbGetAttribute returns -1 for both),
  // distinct from a genuinely empty attribute value.
  if (!attr_selector.empty()) {
    std::vector<char> abuf(1 << 16, 0);  // 64 KiB
    int n = mbGetAttribute(view, attr_selector.c_str(), attr_name.c_str(),
                           abuf.data(), static_cast<int>(abuf.size()));
    if (n < 0)
      std::fprintf(stderr,
                   "mb_shot: --attr '%s' [%s] matched no element / no attribute\n",
                   attr_selector.c_str(), attr_name.c_str());
    std::fwrite(abuf.data(), 1, std::strlen(abuf.data()), stdout);
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

  // --require CSS: after all waits/interaction, assert the page actually contains
  // the scrape target — for scripting, so a pipeline can tell "the data is here"
  // from "the page didn't load / the element never appeared" (which the warn-only
  // waits and a successful-but-empty file load don't signal). The capture/extract
  // still run (useful for debugging the miss); only the exit code changes -> 3.
  int require_failed = 0;
  if (!require_selector.empty()) {
    int n = mbCountSelector(view, require_selector.c_str());
    if (n <= 0) {
      std::fprintf(stderr,
                   "mb_shot: --require '%s' matched no element (exit 3)\n",
                   require_selector.c_str());
      require_failed = 1;
    }
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
    return require_failed ? 3 : ((ok && load_ok) ? 0 : 1);
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
    // mbSavePdfEx with defaults (pdf_w/h=0 -> Letter, scale 1.0, no margin) matches the
    // old mbSavePdf; --pdf-size/--landscape/--pdf-scale/--pdf-margin override the geometry.
    ok = mbSavePdfEx(view, out, pdf_w, pdf_h, pdf_landscape ? 1 : 0, pdf_scale,
                     pdf_margin);
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
  return require_failed ? 3 : ((ok && load_ok) ? 0 : 1);
}
