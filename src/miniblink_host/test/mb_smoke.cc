// mb_smoke — capability test suite for the miniblink-modern engine. Each case loads
// content and ASSERTS engine behavior (mostly via mbEvalJS / getComputedStyle, which is
// robust; plus one pixel check). Prints PASS/FAIL per case and a summary; exit 0 iff all pass.
#include "miniblink_host/test/mb_smoke_harness.h"

using mbsmoke::Eval;     // shared harness helpers (see mb_smoke_harness.h)
using mbsmoke::EvalIso;
using mbsmoke::Expect;
using mbsmoke::g_fail;
using mbsmoke::g_pass;

namespace {
// Native functions bound into JS for the mbJsBindFunction test: echoes its first
// argument with a "!" suffix and the userdata it was given.
const char* SmokeEcho(void* userdata, int argc, const char** argv,
                      const int* /*argtypes*/,
                      int* /*out_type*/) {  // default string return
  static char buf[256];
  std::snprintf(buf, sizeof(buf), "%s!%d", (argc > 0 && argv[0]) ? argv[0] : "",
                userdata ? *static_cast<int*>(userdata) : -1);
  return buf;
}

// Returns structured data as JSON (out_type 5) -> a real JS object in the page.
const char* SmokeJson(void*, int, const char**, const int*, int* out_type) {
  *out_type = 5;  // json
  return "{\"a\":1,\"b\":[2,3]}";
}
}  // namespace

int main() {
  if (!mbInitialize()) {
    std::fprintf(stderr, "init failed\n");
    return 1;
  }

  // 0. Lifecycle: mbShutdown is safe and the engine survives a shutdown -> re-init
  // cycle. Blink's process-global init is one-time, so the engine stays resident and
  // re-init reuses it (pre-fix this deleted the runtime, leaving dangling allocator
  // pointers and crashing on the 2nd init). Running it BEFORE the main view means
  // every test below also exercises the post-cycle engine.
  mbShutdown();
  Expect(mbInitialize() == 1, "engine survives mbShutdown + re-init (no crash/leak)");

  const int W = 400, H = 300;
  mbView* v = mbCreateView(W, H);
  if (!v)
    return 1;

  // 0b. Push callback: mbOnLoadFinish fires on the real Blink DidFinishLoad signal
  // (the document `load` event), not a poll or fixed timer; mbIsLoadFinished queries
  // the same state. A counting callback (state via userdata; non-capturing lambda ->
  // C function pointer) must fire once per load and leave the flag set.
  {
    int fin = 0;
    mbOnLoadFinish(
        v, [](mbView*, void* ud) { ++*static_cast<int*>(ud); }, &fin);
    mbLoadHTML(v, "<body>load-a</body>", "about:blank");
    const int after_a = fin;
    mbLoadHTML(v, "<body>load-b</body>", "about:blank");
    Expect(after_a >= 1 && fin > after_a && mbIsLoadFinished(v) == 1,
           "mbOnLoadFinish fires on each DidFinishLoad; mbIsLoadFinished true",
           std::string("after_a=") + std::to_string(after_a) + " fin=" +
               std::to_string(fin));
    mbOnLoadFinish(v, nullptr, nullptr);  // clear before `fin` leaves scope
  }

  // 0c. Dynamic per-request hook: mbSetRequestCallback is consulted for EVERY request
  // the loader handles and can block per-URL at runtime (vs the static substring
  // tables). Two same-origin fetch()es — one served by a mock (allowed), one whose URL
  // contains "blockme" (the hook blocks it) — prove both inspection (the hook records
  // every URL it sees) and per-request veto. Fully offline (mock + hook, no network).
  {
    // Heap-owned, never destroyed (-Wexit-time-destructors); statics aren't captured
    // so the lambda stays convertible to a C function pointer.
    static std::vector<std::string>* seen = new std::vector<std::string>();
    seen->clear();
    mbMockResponse("site.test/ok", "{\"v\":7}", "application/json", 200);
    mbSetRequestCallback(
        [](const char* url, void*) -> int {
          seen->push_back(url);
          return std::strstr(url, "blockme") ? 1 : 0;  // veto only the blockme URL
        },
        nullptr);
    mbLoadHTML(v,
               "<body><div id='r'>?</div><script>(async()=>{"
               "let a='aerr',b='?';"
               "try{a='ok:'+(await (await fetch('https://site.test/ok')).json()).v;}"
               "catch(e){a='aerr';}"
               "try{await fetch('https://site.test/blockme');b='got';}"
               "catch(e){b='blocked';}"
               "document.getElementById('r').textContent=a+','+b;})();</script></body>",
               "https://site.test/");
    mbWaitForFunction(v, "document.getElementById('r').textContent!=='?'", 2000);
    const std::string r = Eval(v, "document.getElementById('r').textContent");
    bool saw_blockme = false;
    for (const auto& u : *seen)
      if (u.find("blockme") != std::string::npos)
        saw_blockme = true;
    Expect(r.find("ok:7") != std::string::npos &&
               r.find("blocked") != std::string::npos && saw_blockme,
           "mbSetRequestCallback inspects + vetoes per request (mock ok, blockme blocked)",
           std::string("r=[") + r + "] seen=" + std::to_string(seen->size()));
    mbSetRequestCallback(nullptr, nullptr);
    mbClearMocks();
  }

  // 0d. Response hook: mbSetResponseCallback sees every response BEFORE the page and can
  // REPLACE the body. A mock serves {"v":1}; the hook inspects it (records the original)
  // and rewrites it to {"v":99}; the page's fetch() must observe the rewritten 99, and
  // the new (shorter/longer) length must be delivered. Fully offline.
  {
    static std::string* orig = new std::string();  // heap-owned (-Wexit-time-destructors)
    orig->clear();
    mbMockResponse("api.test/v", "{\"v\":1}", "application/json", 200);
    mbSetResponseCallback(
        [](mbResponse* r, void*) {
          int n = 0;
          const char* b = mbResponseBody(r, &n);
          if (std::strstr(mbResponseURL(r), "api.test/v")) {
            orig->assign(b, n);  // inspect: capture what the server/mock returned
            const char* rep = "{\"v\":99}";
            mbResponseSetBody(r, rep, static_cast<int>(std::strlen(rep)));  // modify
          }
        },
        nullptr);
    mbLoadHTML(v,
               "<body><div id='r'>?</div><script>"
               "fetch('https://api.test/v').then(r=>r.json()).then(j=>{"
               "document.getElementById('r').textContent='v='+j.v;});</script></body>",
               "https://api.test/");
    mbWaitForFunction(v, "document.getElementById('r').textContent!=='?'", 2000);
    const std::string r = Eval(v, "document.getElementById('r').textContent");
    Expect(r == "v=99" && *orig == "{\"v\":1}",
           "mbSetResponseCallback inspects + rewrites the response body before the page",
           std::string("page=[") + r + "] orig=[" + *orig + "]");
    mbSetResponseCallback(nullptr, nullptr);
    mbClearMocks();
  }

  // 0e. mbDownloadURL fetches a URL through the engine and writes the body to disk
  // WITHOUT rendering it. (a) a data: URL decodes to the file; (b) a mocked URL is
  // served from the interception layer (no network) AND the response hook can rewrite
  // the downloaded bytes — proving downloads honor the same interception as page loads.
  {
    auto slurp = [](const char* p) -> std::string {
      std::string s;
      if (FILE* f = std::fopen(p, "rb")) {
        char b[4096];
        size_t n;
        while ((n = std::fread(b, 1, sizeof(b), f)) > 0)
          s.append(b, n);
        std::fclose(f);
      }
      return s;
    };
    const int d1 = mbDownloadURL(v, "data:text/plain,DL-DATA-7", "/tmp/mb_dl1.bin");
    const std::string f1 = slurp("/tmp/mb_dl1.bin");
    mbMockResponse("dl.test/file", "MOCKED-BODY", "application/octet-stream", 200);
    mbSetResponseCallback(
        [](mbResponse* r, void*) {
          if (std::strstr(mbResponseURL(r), "dl.test/file"))
            mbResponseSetBody(r, "REWRITTEN", 9);
        },
        nullptr);
    const int d2 =
        mbDownloadURL(v, "https://dl.test/file", "/tmp/mb_dl2.bin");
    const std::string f2 = slurp("/tmp/mb_dl2.bin");
    mbSetResponseCallback(nullptr, nullptr);
    mbClearMocks();
    Expect(d1 == 1 && f1 == "DL-DATA-7" && d2 == 1 && f2 == "REWRITTEN",
           "mbDownloadURL writes to disk; honors mock + response-hook rewrite",
           std::string("d1=") + std::to_string(d1) + " f1=[" + f1 + "] d2=" +
               std::to_string(d2) + " f2=[" + f2 + "]");
  }

  // 0f. JS dialogs (alert/confirm/prompt) handled in-process via mbSetJsDialogCallback:
  // a registered callback captures each message and drives the result (accept confirm,
  // return prompt text); with NO callback the headless-safe defaults apply (confirm=
  // false, prompt=null). Implemented as a pre-page JS override — no browser/modal.
  {
    static std::string* dlg = new std::string();  // -Wexit-time-destructors
    dlg->clear();
    mbSetJsDialogCallback(
        v,
        [](int type, const char* msg, const char* /*def*/, char* out, int cap,
           void*) -> int {
          *dlg += std::to_string(type) + ":" + (msg ? msg : "") + ";";  // capture
          if (type == 2 && out && cap > 0)
            std::snprintf(out, static_cast<size_t>(cap), "REPLY");  // prompt text
          return 1;  // accept alert/confirm/prompt
        },
        nullptr);
    mbLoadHTML(v,
               "<body><script>window.__a=(alert('hi'),'ok');"
               "window.__c=confirm('go?');window.__p=prompt('name?','d');"
               "</script></body>",
               "about:blank");
    const std::string c = Eval(v, "''+window.__c");
    const std::string p = Eval(v, "''+window.__p");
    Expect(Eval(v, "window.__a") == "ok" && c == "true" && p == "REPLY" &&
               dlg->find("0:hi;") != std::string::npos &&
               dlg->find("1:go?;") != std::string::npos &&
               dlg->find("2:name?;") != std::string::npos,
           "mbSetJsDialogCallback handles alert/confirm/prompt (capture + accept + text)",
           "c=[" + c + "] p=[" + p + "] log=[" + *dlg + "]");
    // No callback -> headless-safe defaults.
    mbSetJsDialogCallback(v, nullptr, nullptr);
    mbLoadHTML(v,
               "<body><script>window.__c2=confirm('x');window.__p2=prompt('y');"
               "</script></body>",
               "about:blank");
    Expect(Eval(v, "''+window.__c2") == "false" && Eval(v, "''+window.__p2") == "null",
           "JS dialog default (no callback): confirm=false, prompt=null");
  }

  // 0g. Navigation policy: mbOnNavigation fires for each PAGE-initiated navigation and
  // can BLOCK it. A callback denies URLs containing "blocked", allows others. A
  // location.href to an allowed data: URL commits (body becomes GOOD); a later one to a
  // "blocked" URL is vetoed (body stays GOOD). The log proves the callback saw both.
  {
    static std::string* navlog = new std::string();  // -Wexit-time-destructors
    navlog->clear();
    mbOnNavigation(
        v,
        [](mbView*, void*, const char* url) -> int {
          *navlog += std::string(url ? url : "") + ";";
          return (url && std::strstr(url, "blocked")) ? 0 : 1;  // veto "blocked"
        },
        nullptr);
    // Mock the navigation targets so they commit offline (top-level data:/file: nav is
    // browser-blocked, so use http URLs served from the interception layer).
    mbMockResponse("nav.test/ok", "<body>GOOD</body>", "text/html", 200);
    mbMockResponse("nav.test/blocked", "<body>SHOULD-NOT-SHOW</body>", "text/html", 200);
    mbLoadHTML(v, "<body>START</body>", "https://nav.test/");
    Eval(v, "location.href='https://nav.test/ok'");  // allowed -> commits the mock
    mbWait(v, 300);
    const std::string a = Eval(v, "document.body.textContent");
    Eval(v, "location.href='https://nav.test/blocked'");  // vetoed -> stays
    mbWait(v, 300);
    const std::string b = Eval(v, "document.body.textContent");
    Expect(a == "GOOD" && b == "GOOD" &&
               navlog->find("nav.test/ok") != std::string::npos &&
               navlog->find("nav.test/blocked") != std::string::npos,
           "mbOnNavigation allows + blocks page-initiated navigations",
           "a=[" + a + "] b=[" + b + "] log=[" + *navlog + "]");
    mbOnNavigation(v, nullptr, nullptr);
    mbClearMocks();
  }

  // 0h. New-window notification: mbOnNewWindow fires when the page calls window.open
  // (or activates target=_blank) with the requested URL + name. The popup itself is
  // still denied (window.open returns null) — safe for untrusted pages — but the host
  // learns what was requested and can act on it.
  {
    static std::string* winlog = new std::string();  // -Wexit-time-destructors
    winlog->clear();
    mbOnNewWindow(
        v,
        [](mbView*, void*, const char* url, const char* name) {
          *winlog += std::string(url ? url : "") + "|" + (name ? name : "") + ";";
        },
        nullptr);
    mbLoadHTML(v,
               "<body><script>window.__o=String(window.open("
               "'https://popup.test/p','winname'));</script></body>",
               "about:blank");
    const std::string opened = Eval(v, "window.__o");
    Expect(opened == "null" &&
               winlog->find("https://popup.test/p|winname;") != std::string::npos,
           "mbOnNewWindow notifies window.open URL+name; popup still denied",
           "open=[" + opened + "] log=[" + *winlog + "]");
    mbOnNewWindow(v, nullptr, nullptr);
  }

  // 0i. Mouse-click fidelity (#12): mbSendMouseClickEx carries the button + modifier
  // keys. A shift+alt LEFT click fires `click` with button 0 + shiftKey + altKey; a
  // MIDDLE click fires `auxclick` with button 1; a RIGHT click fires `contextmenu`.
  // (Ctrl+click isn't used here — on macOS that is the secondary/context-menu click.)
  {
    mbLoadHTML(v,
        "<body style='margin:0'><script>window.c='';window.a='';window.x='';"
        "addEventListener('click',function(e){window.c=e.button+','+e.shiftKey+','"
        "+e.altKey;});"
        "addEventListener('auxclick',function(e){window.a=''+e.button;});"
        "addEventListener('contextmenu',function(e){window.x='ctx';"
        "e.preventDefault();});</script></body>",
        "about:blank");
    mbSendMouseClickEx(v, 50, 50, 0, 2 | 4);  // left + shift + alt
    const std::string c = Eval(v, "window.c");
    mbSendMouseClickEx(v, 50, 50, 1, 0);  // middle
    const std::string a = Eval(v, "window.a");
    mbSendMouseClickEx(v, 50, 50, 2, 0);  // right
    const std::string x = Eval(v, "window.x");
    Expect(c == "0,true,true" && a == "1" && x == "ctx",
           "mbSendMouseClickEx carries button + shift/alt (left/middle/right)",
           "c=[" + c + "] a=[" + a + "] x=[" + x + "]");
  }

  // 0i2. IME composition (#12 input): mbSendIme drives the focused input through a
  // composition preview + commit — the committed text lands and the composition events
  // fire (CJK / accented input via an input method).
  {
    mbLoadHTML(v,
        "<body><input id='ime'><script>window.__cs=0;window.__ce=0;"
        "var e=document.getElementById('ime');"
        "e.addEventListener('compositionstart',function(){window.__cs++;});"
        "e.addEventListener('compositionend',function(){window.__ce++;});"
        "</script></body>",
        "about:blank");
    mbFocusSelector(v, "#ime");
    mbSendIme(v, "\xE3\x81\xAB\xE3\x81\xBB", "\xE6\x97\xA5\xE6\x9C\xAC");  // "にほ" -> "日本"
    const std::string val = Eval(v, "document.getElementById('ime').value");
    Expect(val == "\xE6\x97\xA5\xE6\x9C\xAC" && Eval(v, "''+window.__cs") == "1" &&
               Eval(v, "''+window.__ce") == "1",
           "mbSendIme: IME compose+commit lands text + fires composition events",
           "val=[" + val + "] cs=" + Eval(v, "''+window.__cs") + " ce=" +
               Eval(v, "''+window.__ce"));
  }

  // 0k. Console push: mbOnConsoleMessage fires LIVE for each console message (vs polling
  // DrainConsole), with its level + text — react to errors/logs during a long script.
  {
    static std::string* clog = new std::string();  // -Wexit-time-destructors
    clog->clear();
    mbOnConsoleMessage(
        v,
        [](mbView*, void*, const char* level, const char* msg) {
          *clog += std::string(level ? level : "") + ":" + (msg ? msg : "") + ";";
        },
        nullptr);
    mbLoadHTML(v,
               "<body><script>console.log('hi');console.error('boom');</script></body>",
               "about:blank");
    Expect(clog->find("log:hi;") != std::string::npos &&
               clog->find("error:boom;") != std::string::npos,
           "mbOnConsoleMessage: live push of console.log/error with level+text",
           "log=[" + *clog + "]");
    mbOnConsoleMessage(v, nullptr, nullptr);
  }

  // 0l. URL-changed: mbOnUrlChanged fires on every main-frame commit with the new URL —
  // track where the view is (host loads, navigations, redirects).
  {
    static std::string* urls = new std::string();  // -Wexit-time-destructors
    urls->clear();
    mbOnUrlChanged(
        v, [](mbView*, void*, const char* url) { *urls += std::string(url) + ";"; },
        nullptr);
    mbLoadHTML(v, "<body>a</body>", "https://u.test/a");
    mbLoadHTML(v, "<body>b</body>", "https://u.test/b");
    Expect(urls->find("u.test/a") != std::string::npos &&
               urls->find("u.test/b") != std::string::npos,
           "mbOnUrlChanged fires per main-frame commit with the new URL",
           "urls=[" + *urls + "]");
    mbOnUrlChanged(v, nullptr, nullptr);
  }

  // 0m. Download diversion (#6): a top-level navigation to a non-renderable response (a
  // data: URL with application/octet-stream) is handed to mbOnDownload (mime + bytes)
  // instead of committed — so the current page stays and a download link saves a file.
  {
    static std::string* dl = new std::string();  // -Wexit-time-destructors
    dl->clear();
    mbLoadHTML(v, "<body>PAGE</body>", "about:blank");  // a real page first
    mbOnDownload(
        v,
        [](mbView*, void*, const char* /*url*/, const char* mime,
           const char* /*fn*/, const char* data, int len) {
          *dl = std::string("mime=") + mime + " body=" + std::string(data, len);
        },
        nullptr);
    mbLoadURL(v, "data:application/octet-stream,DLBYTES");
    const std::string body = Eval(v, "document.body.textContent");
    Expect(*dl == "mime=application/octet-stream body=DLBYTES" && body == "PAGE",
           "mbOnDownload diverts a non-renderable navigation to the callback",
           "dl=[" + *dl + "] body=[" + body + "]");
    mbOnDownload(v, nullptr, nullptr);
  }

  // 0n. Failed-load finish (#4 tail): a top-level load that never commits (a file that
  // can't be read) still ENDS — mbOnLoadFinish must fire and mbIsLoadFinished must read
  // true, so a caller awaiting completion isn't stuck on a 404/missing file forever.
  {
    static int* fin = new int(0);  // -Wexit-time-destructors
    *fin = 0;
    mbLoadHTML(v, "<body>OK</body>", "about:blank");  // a real page first
    mbOnLoadFinish(
        v, [](mbView*, void* ud) { ++*static_cast<int*>(ud); }, fin);
    const int before = *fin;
    mbLoadURL(v, "file:///no/such/mb/missing/file.html");  // read fails -> no commit
    Expect(*fin > before && mbIsLoadFinished(v) == 1,
           "a failed top-level load still fires mbOnLoadFinish / sets mbIsLoadFinished",
           "fin delta=" + std::to_string(*fin - before) +
               " finished=" + std::to_string(mbIsLoadFinished(v)));
    mbOnLoadFinish(v, nullptr, nullptr);  // clear before `fin` leaves scope
  }

  // 0j. CSP does NOT leak across navigations in a reused view (#15). Load a page whose
  // strict <meta> CSP (script-src 'none') blocks its own inline script, then load a
  // normal page in the SAME view: the second page's script MUST run — each commit now
  // gets a fresh, empty policy container so the prior document's CSP is shed.
  {
    mbLoadHTML(v,
        "<meta http-equiv='Content-Security-Policy' content=\"script-src 'none'\">"
        "<body><script>window.__csp1=1;</script>x</body>",
        "https://csp.test/");
    const bool blocked = Eval(v, "String(typeof window.__csp1)") == "undefined";
    mbLoadHTML(v, "<body><script>window.__csp2=1;</script>y</body>",
               "https://nocsp.test/");
    const bool ran = Eval(v, "String(window.__csp2|0)") == "1";
    Expect(blocked && ran,
           "CSP from a prior page is shed on the next navigation (reused view)",
           std::string("blocked=") + (blocked ? "1" : "0") +
               " ran=" + (ran ? "1" : "0"));
  }

  // 1. HTML parse + DOM.
  mbLoadHTML(v, "<body><div id='x'>hello</div></body>", "about:blank");
  Expect(Eval(v, "document.getElementById('x').textContent") == "hello",
         "HTML/DOM parse");

  // 2. JavaScript evaluation.
  Expect(Eval(v, "2 + 2 * 10") == "22", "JS eval");

  // 3. CSS cascade via computed style (inline style attr).
  mbLoadHTML(v, "<body><p id='p' style='color:#ff0000'>x</p></body>", "about:blank");
  Expect(Eval(v, "getComputedStyle(document.getElementById('p')).color") ==
             "rgb(255, 0, 0)",
         "CSS computed style");

  // 4. UA stylesheet loaded (h1 default font-weight = bold = 700).
  mbLoadHTML(v, "<body><h1 id='h'>x</h1></body>", "about:blank");
  Expect(Eval(v, "getComputedStyle(document.getElementById('h')).fontWeight") == "700",
         "UA stylesheet (h1 bold)");

  // 5. mbRunJS drives the page; mbEvalJS reads it back.
  mbLoadHTML(v, "<body><b id='b'>0</b></body>", "about:blank");
  mbRunJS(v, "document.getElementById('b').textContent = 'driven';");
  Expect(Eval(v, "document.getElementById('b').textContent") == "driven",
         "mbRunJS + mbEvalJS bridge");

  // 6. <canvas> 2D draws (read a pixel back via getImageData).
  mbLoadHTML(v,
             "<canvas id='c' width='10' height='10'></canvas><script>"
             "var x=document.getElementById('c').getContext('2d');"
             "x.fillStyle='#00ff00';x.fillRect(0,0,10,10);</script>",
             "about:blank");
  Expect(Eval(v, "(function(){var d=document.getElementById('c').getContext('2d')"
                 ".getImageData(5,5,1,1).data;return d[0]+','+d[1]+','+d[2];})()") ==
             "0,255,0",
         "canvas 2D getImageData");

  // 7. External <link> CSS via the subresource URLLoader (+ MimeRegistry).
  {
    const char* css = "#q{color:rgb(0,128,255)}";
    if (FILE* f = std::fopen("/tmp/mb_test.css", "wb")) {
      std::fwrite(css, 1, std::strlen(css), f);
      std::fclose(f);
    }
    const char* html =
        "<head><link rel='stylesheet' href='mb_test.css'></head>"
        "<body><i id='q'>x</i></body>";
    if (FILE* f = std::fopen("/tmp/mb_test.html", "wb")) {
      std::fwrite(html, 1, std::strlen(html), f);
      std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_test.html");
    Expect(Eval(v, "getComputedStyle(document.getElementById('q')).color") ==
               "rgb(0, 128, 255)",
           "external <link> CSS subresource");
  }

  // 8. Rendering produces pixels (red bg -> red top-left pixel).
  mbLoadHTML(v, "<body style='margin:0;background:#ff0000'></body>", "about:blank");
  std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 0);
  mbPaintToBitmap(v, px.data(), W, H, W * 4);
  Expect(px[2] == 255 && px[1] == 0 && px[0] == 0, "paint to bitmap (red bg)");

  // 8b. SVG renders to pixels — a distinct paint path from CSS boxes that
  // icon/chart-heavy pages rely on. An inline 100x100 SVG with a solid-green
  // <rect> must paint green well inside the rect (pixel (20,20)). Tolerances
  // absorb any AA/color-management drift on the interior.
  {
    mbLoadHTML(v,
        "<body style='margin:0'>"
        "<svg width='100' height='100' xmlns='http://www.w3.org/2000/svg'>"
        "<rect x='0' y='0' width='100' height='100' fill='rgb(0,128,0)'/></svg>"
        "</body>", "about:blank");
    std::vector<uint8_t> sp(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, sp.data(), W, H, W * 4);
    const size_t o = (static_cast<size_t>(20) * W + 20) * 4;  // inside the rect
    const int b = sp[o], g = sp[o + 1], r = sp[o + 2];
    Expect(r < 16 && g > 110 && g < 145 && b < 16,
           "SVG renders to pixels (inline <rect> paints green)",
           std::string("rgb(") + std::to_string(r) + "," + std::to_string(g) +
               "," + std::to_string(b) + ")");
  }

  // 9. Input: synthesize a click on a button and verify its handler ran.
  mbLoadHTML(v,
             "<body style='margin:0'><button id='b' onclick='window.__c=1' "
             "style='position:absolute;left:20px;top:20px;width:120px;height:40px'>"
             "click</button></body>",
             "about:blank");
  {
    std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // force layout for hit-testing
  }
  mbSendMouseClick(v, 80, 40);  // center of the button
  Expect(Eval(v, "String(window.__c||0)") == "1", "input: synthesized click");

  // 10. Keyboard: focus an input, type, verify its value.
  mbLoadHTML(v, "<body><input id='t'></body>", "about:blank");
  mbRunJS(v, "document.getElementById('t').focus();");
  mbSendText(v, "hi there");
  Expect(Eval(v, "document.getElementById('t').value") == "hi there",
         "input: typed text");

  // 10b. Keyboard with UTF-8: accented + CJK + a supplementary (emoji) char.
  // Verify .length (code-unit count) rather than echoing bytes through mbEvalJS.
  mbLoadHTML(v, "<body><input id='u'></body>", "about:blank");
  mbRunJS(v, "document.getElementById('u').focus();");
  mbSendText(v, "café日本😀");  // 4 + 0 ... = 'c','a','f','é','日','本', emoji(2 units)
  Expect(Eval(v, "document.getElementById('u').value.length") == "8" &&
             Eval(v, "document.getElementById('u').value.codePointAt(4)") ==
                 "26085",  // U+65E5 日
         "input: typed UTF-8 (accent/CJK/emoji)",
         Eval(v, "document.getElementById('u').value"));

  // 10b. UTF-8-safe buffer truncation: a getter into a too-small buffer must cut
  // at a character boundary, never mid-multibyte. "café" has a multi-byte é after
  // "caf"; a 5-byte buffer's naive cut at byte 4 would land inside é (invalid
  // UTF-8). The boundary-aware copy backs off so the result ends at a real char
  // boundary — encoding-independent check below (works whatever é encodes to).
  {
    mbLoadHTML(v, "<body><b id='t'>café</b></body>", "about:blank");
    char big[64] = {0};
    mbGetTextForSelector(v, "#t", big, sizeof(big));  // the full text
    const std::string full_s(big);
    char small[5] = {0};  // out_cap 5 -> at most 4 usable bytes
    mbGetTextForSelector(v, "#t", small, sizeof(small));
    const std::string got(small);
    const bool truncated = got.size() < full_s.size();  // buffer was too small
    const bool is_prefix = full_s.compare(0, got.size(), got) == 0;
    // The byte just past `got` in the full text must NOT be a continuation byte
    // (0b10xxxxxx) — i.e. `got` ended at a char boundary (naive cut would not).
    const bool boundary =
        got.size() == full_s.size() ||
        (static_cast<unsigned char>(full_s[got.size()]) & 0xC0) != 0x80;
    Expect(truncated && is_prefix && boundary && !got.empty(),
           "mbGetTextForSelector truncates at a UTF-8 boundary (no split char)",
           std::string("full=") + std::to_string((int)full_s.size()) + " got='" +
               got + "' (" + std::to_string((int)got.size()) + "B)");

    // mbEvalJSEx's value buffer is the same path (arbitrary result content) —
    // verify it also truncates at a boundary, not mid-multibyte.
    char vbig[64] = {0}, tbig[16] = {0};
    mbEvalJSEx(v, "'café'", vbig, sizeof(vbig), tbig, sizeof(tbig));
    const std::string vfull(vbig);
    char vsmall[5] = {0};
    mbEvalJSEx(v, "'café'", vsmall, sizeof(vsmall), nullptr, 0);
    const std::string vgot(vsmall);
    const bool ev_boundary =
        vgot.size() == vfull.size() ||
        (static_cast<unsigned char>(vfull[vgot.size()]) & 0xC0) != 0x80;
    Expect(vgot.size() < vfull.size() && ev_boundary &&
               vfull.compare(0, vgot.size(), vgot) == 0,
           "mbEvalJSEx value buffer truncates at a UTF-8 boundary",
           std::string("vfull=") + std::to_string((int)vfull.size()) + " vgot='" +
               vgot + "'");
  }

  // 11. Scroll: a tall page, synthesize a downward gesture scroll, verify scrollY.
  mbLoadHTML(v,
             "<body style='margin:0'><div style='height:5000px'></div></body>",
             "about:blank");
  {
    std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // force layout so it's scrollable
  }
  mbSendScroll(v, 200, 150, 0, 400);  // scroll down 400px
  {
    int sy = std::atoi(Eval(v, "String(Math.round(window.scrollY))").c_str());
    Expect(sy > 0, "input: gesture scroll (scrollY)",
           std::to_string(sy));
  }
  // 11b. mbScrollTo: absolute scroll to a known offset (vs the relative gesture).
  mbScrollTo(v, 0, 250);
  {
    int sy = std::atoi(Eval(v, "String(Math.round(window.scrollY))").c_str());
    Expect(sy == 250, "mbScrollTo moves the viewport to an absolute Y",
           std::to_string(sy));
  }

  // 12. Mouse move: hover over an element fires mouseover (and :hover applies).
  mbLoadHTML(v,
             "<body style='margin:0'>"
             "<div id='h' onmouseover='window.__h=1' "
             "style='position:absolute;left:10px;top:10px;width:100px;height:60px'>"
             "hover</div></body>",
             "about:blank");
  {
    std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // layout for hit-testing
  }
  mbSendMouseMove(v, 50, 40);  // over the div
  Expect(Eval(v, "String(window.__h||0)") == "1", "input: mouse move (hover)");

  // mbSendMouseDown/Up enable a DRAG that mbSendMouseClick can't: press, move
  // (carrying the held button so e.buttons==1), release. A pad tracks the drag
  // delta and the buttons mask; a same-point down+up still fires onclick.
  mbLoadHTML(v,
      "<body style='margin:0'><div id='pad' style='width:300px;height:100px'>"
      "</div><button id='b' style='position:absolute;left:0;top:120px;"
      "width:120px;height:40px' "
      "onclick='window.__clk=(window.__clk||0)+1'>b</button>"
      "<script>window.__dx=0;window.__btn=-1;window.__drag=0;window.__done=0;"
      "var p=document.getElementById('pad');"
      "p.addEventListener('mousedown',function(e){window.__drag=1;window.__sx=e.clientX;});"
      "document.addEventListener('mousemove',function(e){if(window.__drag){"
      "window.__dx=e.clientX-window.__sx;window.__btn=e.buttons;}});"
      "document.addEventListener('mouseup',function(){window.__drag=0;window.__done=1;});"
      "</script></body>", "about:blank");
  {
    std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // layout for hit-testing
  }
  mbSendMouseDown(v, 50, 40);
  mbSendMouseMove(v, 150, 40);   // drag right (button held)
  mbSendMouseMove(v, 200, 40);
  mbSendMouseUp(v, 200, 40);
  const bool dragged = Eval(v, "String(window.__dx)") == "150" &&
                       Eval(v, "String(window.__done)") == "1";
  const bool held = Eval(v, "String(window.__btn)") == "1";  // moves carried the button
  mbSendMouseDown(v, 60, 140);   // a same-point down+up is still a click (button center)
  mbSendMouseUp(v, 60, 140);
  const bool click_ok = Eval(v, "String(window.__clk||0)") == "1";
  Expect(dragged && held && click_ok,
         "mbSendMouseDown/Up drag (delta + e.buttons) and down+up clicks",
         std::string("dx=") + Eval(v, "String(window.__dx)") + " btn=" +
             Eval(v, "String(window.__btn)") + " click=" +
             Eval(v, "String(window.__clk||0)"));

  // 12b. mbDragSelector drags one element's center onto another's: #handle follows
  // the cursor during the drag and the drop lands at #target's center x (220).
  {
  mbLoadHTML(v,
      "<body style='margin:0'>"
      "<div id='handle' style='position:absolute;left:0;top:0;width:40px;height:40px'></div>"
      "<div id='target' style='position:absolute;left:200px;top:0;width:40px;height:40px'></div>"
      "<script>window.__moved=0;window.__dropx=-1;var drag=0;"
      "document.getElementById('handle').addEventListener('mousedown',function(){drag=1;});"
      "document.addEventListener('mousemove',function(e){if(drag){window.__moved=1;"
      "document.getElementById('handle').style.left=e.clientX+'px';}});"
      "document.addEventListener('mouseup',function(e){if(drag){drag=0;window.__dropx=e.clientX;}});"
      "</script></body>", "about:blank");
  {
    std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // layout for hit-testing
  }
  const bool drag_ok = mbDragSelector(v, "#handle", "#target") == 1;
  const bool dropped = Eval(v, "String(window.__dropx)") == "220" &&
                       Eval(v, "String(window.__moved)") == "1" &&
                       Eval(v, "document.getElementById('handle').style.left") == "220px";
  const bool nomatch = mbDragSelector(v, "#handle", "#none") == 0;
  Expect(drag_ok && dropped && nomatch,
         "mbDragSelector drags from-center to to-center (drop at 220)",
         std::string("ok=") + (drag_ok ? "1" : "0") + " dropx=" +
             Eval(v, "String(window.__dropx)") + " nomatch=" + (nomatch ? "1" : "0"));
  }

  // 12c. mbSendTouchTap fires real touch events (touchstart+touchend) that mouse
  // events don't — a touch-only handler runs and sees touches[0].clientX == tap x.
  {
    mbLoadHTML(v,
        "<body style='margin:0'><div id='b' style='width:200px;height:100px'></div>"
        "<script>window.__ts=0;window.__tx=-1;window.__te=0;"
        "var b=document.getElementById('b');"
        "b.addEventListener('touchstart',function(e){window.__ts=1;"
        "if(e.touches[0])window.__tx=Math.round(e.touches[0].clientX);});"
        "b.addEventListener('touchend',function(){window.__te=1;});"
        "</script></body>", "about:blank");
    {
      std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
      mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // layout for hit-testing
    }
    mbSendTouchTap(v, 50, 40);
    const bool start = Eval(v, "String(window.__ts)") == "1";
    const bool coord = Eval(v, "String(window.__tx)") == "50";
    const bool end = Eval(v, "String(window.__te)") == "1";
    Expect(start && coord && end,
           "mbSendTouchTap fires touchstart+touchend with touches[0].clientX",
           std::string("start=") + (start ? "1" : "0") + " x=" +
               Eval(v, "String(window.__tx)") + " end=" + (end ? "1" : "0"));
  }

  // 12d. mbSendTouchSwipe drives touchmove: a handler sees the moves and the final
  // touches[0].clientX equals the swipe end x.
  {
    mbLoadHTML(v,
        "<body style='margin:0'><div id='s' style='width:300px;height:100px'></div>"
        "<script>window.__mv=0;window.__mx=-1;window.__se=0;"
        "var s=document.getElementById('s');"
        "s.addEventListener('touchmove',function(e){window.__mv++;"
        "if(e.touches[0])window.__mx=Math.round(e.touches[0].clientX);});"
        "s.addEventListener('touchend',function(){window.__se=1;});"
        "</script></body>", "about:blank");
    {
      std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
      mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // layout for hit-testing
    }
    mbSendTouchSwipe(v, 50, 50, 200, 50);
    const bool moved = Eval(v, "String(window.__mv>0)") == "true";
    const bool endx = Eval(v, "String(window.__mx)") == "200";
    const bool ended = Eval(v, "String(window.__se)") == "1";
    Expect(moved && endx && ended,
           "mbSendTouchSwipe fires touchmoves ending at the swipe end x",
           std::string("moved=") + (moved ? "1" : "0") + " endx=" +
               Eval(v, "String(window.__mx)") + " end=" + (ended ? "1" : "0"));
  }

  // 13. Body with an embedded NUL byte must not truncate the document (the host
  // used to commit body.c_str(), losing everything after the first NUL). Load via
  // file:// (the length-preserving path) and verify content AFTER the NUL parsed.
  {
    const char doc[] =
        "<body><div id='a'>before</div>\0<div id='b'>afternul</div></body>";
    const size_t doc_len = sizeof(doc) - 1;  // includes the embedded NUL
    if (FILE* f = std::fopen("/tmp/mb_nul.html", "wb")) {
      std::fwrite(doc, 1, doc_len, f);
      std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_nul.html");
    Expect(Eval(v, "var e=document.getElementById('b');e?e.textContent:''") ==
               "afternul",
           "load: embedded NUL does not truncate document");
  }

  // 14. Full-page mechanism: after resizing the view taller, a re-render must
  // capture content below the original fold (this is what mb_shot --full relies on).
  // Blue 0..1000px, green 1000..1200px; resize to 1200 tall and read a pixel at y=1100.
  mbLoadHTML(v,
             "<body style='margin:0'>"
             "<div style='height:1000px;background:#0000ff'></div>"
             "<div style='height:200px;background:#00ff00'></div></body>",
             "about:blank");
  mbResize(v, W, 1200);
  {
    std::vector<uint8_t> tall(static_cast<size_t>(W) * 1200 * 4, 0);
    mbPaintToBitmap(v, tall.data(), W, 1200, W * 4);
    const size_t at = (static_cast<size_t>(1100) * W + 10) * 4;  // y=1100 (green band)
    Expect(tall[at + 2] == 0 && tall[at + 1] == 255 && tall[at + 0] == 0,
           "full-page: resize captures below-the-fold");
  }

  // 14c. position:fixed in a full-page (resized) capture. mb_shot --full resizes
  // the view to the content height and paints at scroll 0; a fixed top:0 header
  // then sits at y=0 of that tall viewport. It must paint ONCE at the top — not
  // vanish, and not repeat down the page (a real screenshot-correctness concern
  // for sticky headers on long pages).
  {
    mbLoadHTML(v,
        "<body style='margin:0'>"
        "<div style='position:fixed;top:0;left:0;width:100%;height:50px;"
        "background:#00ff00'></div>"
        "<div style='height:1500px;background:#ffffff'></div></body>",
        "about:blank");
    mbResize(v, W, 1500);
    std::vector<uint8_t> tall(static_cast<size_t>(W) * 1500 * 4, 0);
    mbPaintToBitmap(v, tall.data(), W, 1500, W * 4);
    const size_t top = (static_cast<size_t>(10) * W + 10) * 4;   // inside the header
    const size_t mid = (static_cast<size_t>(800) * W + 10) * 4;  // content, far below
    auto green = [](const uint8_t* p) { return p[2] == 0 && p[1] == 255 && p[0] == 0; };
    const bool header_top = green(&tall[top]);
    const bool no_repeat = !green(&tall[mid]);
    Expect(header_top && no_repeat,
           "full-page capture: position:fixed header paints once at top (no repeat)",
           std::string("top=rgb(") + std::to_string(tall[top + 2]) + "," +
               std::to_string(tall[top + 1]) + "," + std::to_string(tall[top]) +
               ") mid=rgb(" + std::to_string(tall[mid + 2]) + "," +
               std::to_string(tall[mid + 1]) + "," + std::to_string(tall[mid]) + ")");
    mbResize(v, W, H);  // restore the shared viewport
  }

  // 14b. mbGetViewSize reads back the viewport set via mbResize (window.inner*).
  {
    mbResize(v, 640, 480);
    mbLoadHTML(v, "<body>x</body>", "about:blank");
    int vw = 0, vh = 0;
    const bool got = mbGetViewSize(v, &vw, &vh) == 1 && vw == 640 && vh == 480;
    Expect(got, "mbGetViewSize reads the viewport (640x480)",
           std::string("vw=") + std::to_string(vw) + " vh=" + std::to_string(vh));
    mbResize(v, W, H);  // restore the shared viewport for later cases
  }

  // 14d. Responsive emulation: width media queries track the VIEW width (set via
  // mbResize / mb_shot's width/height), so a mobile screenshot is just a narrow
  // view — the practical mobile-emulation path. Conversely <meta name=viewport>
  // directives are NOT honored (desktop-mode WebView: the layout viewport is
  // always the view size). This locks in both — the working capability and the
  // documented limitation — since responsive sites depend on the first.
  {
    const char* doc =
        "<style>#c{color:rgb(1,1,1)}@media (max-width:500px){#c{color:rgb(2,2,2)}}"
        "</style><body><div id='c'>x</div></body>";
    mbResize(v, 400, H);
    mbLoadHTML(v, doc, "about:blank");
    const std::string narrow =
        Eval(v, "getComputedStyle(document.getElementById('c')).color");
    mbResize(v, 800, H);
    mbLoadHTML(v, doc, "about:blank");
    const std::string wide =
        Eval(v, "getComputedStyle(document.getElementById('c')).color");
    // A viewport meta width cannot override the layout width (it's ignored).
    mbResize(v, 400, H);
    mbLoadHTML(v, "<meta name=viewport content='width=980'><body>x</body>",
               "about:blank");
    const std::string iw = Eval(v, "String(window.innerWidth)");
    Expect(narrow == "rgb(2, 2, 2)" && wide == "rgb(1, 1, 1)" && iw == "400",
           "responsive: width media queries track the view size "
           "(<meta viewport> ignored)",
           std::string("narrow=") + narrow + " wide=" + wide + " iw@vp980=" + iw);
    mbResize(v, W, H);  // restore the shared viewport for later cases
  }

  // 15. HiDPI: setting device scale factor makes window.devicePixelRatio report it
  // and resolution media queries re-evaluate (without zooming layout).
  mbSetDeviceScaleFactor(v, 2.0f);
  mbLoadHTML(v,
             "<style>#x{color:rgb(0,0,0)}"
             "@media (min-resolution:1.5dppx){#x{color:rgb(1,2,3)}}</style>"
             "<body><b id='x'>x</b></body>",
             "about:blank");
  Expect(Eval(v, "String(window.devicePixelRatio)") == "2",
         "HiDPI: devicePixelRatio", Eval(v, "String(window.devicePixelRatio)"));
  Expect(Eval(v, "getComputedStyle(document.getElementById('x')).color") ==
             "rgb(1, 2, 3)",
         "HiDPI: min-resolution media query matches");

  // 18. User-Agent: default is a real (non-empty) UA, and the override is reflected
  // in navigator.userAgent. Set before load so it applies to the committed document.
  mbLoadHTML(v, "<body>x</body>", "about:blank");  // default UA
  Expect(Eval(v, "String((navigator.userAgent||'').includes('Mozilla'))") == "true",
         "user-agent: default is non-empty");
  {
    // mbGetUserAgent reports the SAME effective UA the page sees.
    char ua[1024] = {0};
    mbGetUserAgent(v, ua, sizeof(ua));
    Expect(std::string(ua) == Eval(v, "navigator.userAgent"),
           "user-agent: mbGetUserAgent matches navigator.userAgent (default)", ua);
  }
  mbSetUserAgent(v, "MiniblinkBot/9.9 (test)");
  mbLoadHTML(v, "<body>x</body>", "about:blank");  // re-navigate to pick up the UA
  Expect(Eval(v, "navigator.userAgent") == "MiniblinkBot/9.9 (test)",
         "user-agent: override reflected in navigator.userAgent",
         Eval(v, "navigator.userAgent"));
  {
    char ua[1024] = {0};
    mbGetUserAgent(v, ua, sizeof(ua));
    Expect(std::string(ua) == "MiniblinkBot/9.9 (test)",
           "user-agent: mbGetUserAgent returns the override", ua);
  }

  // 20. Clip capture: a green box at logical (50,60,100,40). Clipping exactly to it
  // must yield an all-green bitmap (proves the region offset lands at the origin).
  mbSetDeviceScaleFactor(v, 1.0f);  // undo case-15's 2x so clip math is 1:1
  mbLoadHTML(v,
             "<body style='margin:0'><div style='position:absolute;left:50px;"
             "top:60px;width:100px;height:40px;background:#00ff00'></div></body>",
             "about:blank");
  {
    const int cw = 100, chh = 40;
    std::vector<uint8_t> clip(static_cast<size_t>(cw) * chh * 4, 0);
    mbPaintRectToBitmap(v, clip.data(), 50, 60, cw, chh, cw * 4);
    const size_t mid = (static_cast<size_t>(20) * cw + 50) * 4;  // center-ish
    Expect(clip[mid + 2] == 0 && clip[mid + 1] == 255 && clip[mid + 0] == 0,
           "clip: region capture lands on the element");
  }

  // 21. Transparent background (omitBackground): a page with no opaque body bg and a
  // single opaque green box. Outside the box must be alpha 0; inside, opaque green.
  mbSetTransparentBackground(v, 1);
  mbLoadHTML(v,
             "<body style='margin:0;background:transparent'>"
             "<div style='position:absolute;left:0;top:0;width:30px;height:30px;"
             "background:#00ff00'></div></body>",
             "about:blank");
  {
    std::vector<uint8_t> tpx(static_cast<size_t>(W) * H * 4, 0xAB);
    mbPaintToBitmap(v, tpx.data(), W, H, W * 4);
    const size_t inside = (static_cast<size_t>(10) * W + 10) * 4;  // in the box
    const size_t outside = (static_cast<size_t>(200) * W + 300) * 4;  // empty area
    Expect(tpx[inside + 3] == 255 && tpx[inside + 1] == 255 &&
               tpx[outside + 3] == 0,
           "transparent background (omitBackground)");
  }
  mbSetTransparentBackground(v, 0);  // restore default for any later use

  // 22. Wait-for-selector: content injected by a setTimeout must be caught by
  // mbWaitForSelector (which advances real time so the timer fires), and a selector
  // that never appears must time out returning 0. (We don't assert the element is
  // absent immediately after load — the load's own pumping spans enough wall-clock
  // that a short timer may already have fired; that timing isn't a guarantee.)
  mbSetTransparentBackground(v, 0);
  mbLoadHTML(v,
             "<body><script>setTimeout(function(){var d=document.createElement('div');"
             "d.id='ready';d.textContent='late';document.body.appendChild(d);},300);"
             "</script></body>",
             "about:blank");
  Expect(mbWaitForSelector(v, "#ready", 4000) == 1 &&
             Eval(v, "document.getElementById('ready').textContent") == "late",
         "wait: mbWaitForSelector catches setTimeout content");
  Expect(mbWaitForSelector(v, "#never", 100) == 0,
         "wait: missing selector times out");

  // 23. DOM storage probe: SPAs rely on localStorage/sessionStorage. Load over a
  // file:// origin (opaque origins deny storage) and round-trip a value.
  {
    const char* html = "<body>x</body>";
    if (FILE* f = std::fopen("/tmp/mb_store.html", "wb")) {
      std::fwrite(html, 1, std::strlen(html), f);
      std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_store.html");
    Expect(Eval(v, "(function(){try{localStorage.setItem('k','v42');"
                   "return localStorage.getItem('k');}catch(e){return 'THROW:'+e.name;}})()")
               == "v42",
           "DOM localStorage round-trip");
    Expect(Eval(v, "(function(){try{sessionStorage.setItem('s','s7');"
                   "return sessionStorage.getItem('s');}catch(e){return 'THROW:'+e.name;}})()")
               == "s7",
           "DOM sessionStorage round-trip",
           Eval(v, "(function(){try{sessionStorage.setItem('s','s7');"
                   "return sessionStorage.getItem('s');}catch(e){return 'THROW:'+e.name;}})()"));

    // 23b. localStorage persistence across runs (#9): snapshot the whole store, clear it
    // (simulating a fresh process with empty localStorage), then restore — the keys come
    // back, incl. one with characters needing JSON/JS escaping. The embedder writes the
    // snapshot to disk after login and reloads it next run (the cookie-jar peer).
    mbSetLocalStorage(v, "tok", "abc\"123");  // value with a quote (escaping)
    mbSetLocalStorage(v, "u", "ada");
    char snap[1024] = {0};
    const int n = mbSaveLocalStorage(v, snap, sizeof(snap));
    mbClearStorage(v);  // fresh-run: localStorage now empty
    const int gone = mbGetLocalStorage(v, "tok", nullptr, 0);  // -1 absent
    mbLoadLocalStorage(v, snap);  // restore the saved session
    char t[64] = {0}, u[64] = {0};
    mbGetLocalStorage(v, "tok", t, sizeof(t));
    mbGetLocalStorage(v, "u", u, sizeof(u));
    Expect(n > 2 && gone == -1 && std::string(t) == "abc\"123" &&
               std::string(u) == "ada",
           "localStorage snapshot survives clear + restore (cross-run persistence)",
           std::string("n=") + std::to_string(n) + " gone=" + std::to_string(gone) +
               " tok=[" + t + "] u=[" + u + "]");
  }

  // 23c. navigator.permissions.query (broker #8): with no browser the request was
  // dropped and the promise NEVER resolved (a permission-gated page hangs). The
  // in-process PermissionService now answers it, so query() resolves to "denied"
  // (the headless reality) and the page proceeds. Async -> wait on the result.
  {
    mbLoadHTML(v, "<body>x</body>", "https://perm.test/");
    Eval(v,
         "navigator.permissions.query({name:'geolocation'})"
         ".then(function(s){window.__ps=s.state;},"
         "function(e){window.__ps='rej:'+(e&&e.name);})");
    mbWaitForFunction(v, "window.__ps!==undefined", 2000);
    const std::string ps = Eval(v, "window.__ps");
    Expect(ps == "denied",
           "navigator.permissions.query resolves (denied) instead of hanging",
           "state=[" + ps + "]");
  }

  // 23d. navigator.geolocation (broker #8): by default it errors PERMISSION_DENIED;
  // after mbSetGeolocation it resolves getCurrentPosition to the configured fix.
  {
    mbLoadHTML(v, "<body>x</body>", "https://geo.test/");
    Eval(v, "navigator.geolocation.getCurrentPosition("
            "function(p){window.__g='ok';},"
            "function(e){window.__g='err:'+e.code;},{timeout:1500})");
    mbWaitForFunction(v, "window.__g!==undefined", 2500);
    const std::string deflt = Eval(v, "window.__g");
    mbSetGeolocation(37.42, -122.08, 5.0);
    mbLoadHTML(v, "<body>y</body>", "https://geo.test/");  // re-query on a fresh doc
    Eval(v, "navigator.geolocation.getCurrentPosition("
            "function(p){window.__g2=p.coords.latitude.toFixed(2)+','+"
            "p.coords.longitude.toFixed(2)+'@'+p.coords.accuracy;},"
            "function(e){window.__g2='err:'+e.code;},{timeout:1500})");
    mbWaitForFunction(v, "window.__g2!==undefined", 2500);
    const std::string got = Eval(v, "window.__g2");
    mbClearGeolocation();
    Expect(deflt == "err:1" && got == "37.42,-122.08@5",
           "mbSetGeolocation: getCurrentPosition returns the configured fix",
           "default=[" + deflt + "] got=[" + got + "]");
  }

  // 23e. Clipboard (broker #8): navigator.clipboard read/write works against the
  // in-process clipboard (permission granted), and the host shares it via mbGet/Set-
  // Clipboard — a page's writeText is readable by the host; the host's set is readable
  // by the page. (Secure origin + document.hasFocus()==true, which the host reports.)
  {
    mbLoadHTML(v, "<body>x</body>", "https://clip.test/");
    Eval(v,
         "navigator.clipboard.writeText('copied-from-page').then("
         "function(){window.__w='ok';},function(e){window.__w='err:'+e.name;})");
    mbWaitForFunction(v, "window.__w!==undefined", 2000);
    const std::string w = Eval(v, "window.__w");
    char hb[128] = {0};
    mbGetClipboard(hb, sizeof(hb));
    mbSetClipboard("set-by-host");
    Eval(v,
         "navigator.clipboard.readText().then(function(t){window.__r=t;},"
         "function(e){window.__r='err:'+e.name;})");
    mbWaitForFunction(v, "window.__r!==undefined", 2000);
    const std::string r = Eval(v, "window.__r");
    Expect(w == "ok" && std::string(hb) == "copied-from-page" && r == "set-by-host",
           "clipboard: page writeText->host reads; host sets->page readText",
           "w=[" + w + "] host=[" + std::string(hb) + "] r=[" + r + "]");
  }

  // 23f. Web Locks (navigator.locks, broker #8): the in-process LockManager grants with
  // real EXCLUSIVE serialization. Two requests for the same name: the first holds the lock
  // across an async (timer) callback; the second must WAIT until the first's promise
  // settles (releasing the lock). Logging A on acquire, a on release, B on the second
  // acquire yields "AaB" iff the lock serialized them (concurrent grants would give "AB").
  {
    mbLoadHTML(v, "<body>x</body>", "https://locks.test/");
    Eval(v,
         "window.__lg='';"
         "navigator.locks.request('res',function(){window.__lg+='A';"
         "return new Promise(function(res){setTimeout(function(){window.__lg+='a';"
         "res();},50);});});"
         "navigator.locks.request('res',function(){window.__lg+='B';"
         "return Promise.resolve();});");
    mbWaitForFunction(v, "window.__lg.length>=3", 3000);
    const std::string lg = Eval(v, "window.__lg");
    Expect(lg == "AaB",
           "navigator.locks serializes an exclusive lock (2nd waits for 1st release)",
           "log=[" + lg + "]");
  }

  // 23g. Web Locks ifAvailable: a second request with {ifAvailable:true} for a held name
  // is rejected immediately (callback gets null) rather than queued — exercises NO_WAIT.
  {
    mbLoadHTML(v, "<body>x</body>", "https://locks2.test/");
    Eval(v,
         "window.__av='';"
         "navigator.locks.request('r2',function(){"
         "return new Promise(function(res){window.__rel=res;});});"  // held open
         "navigator.locks.request('r2',{ifAvailable:true},function(lock){"
         "window.__av=(lock===null)?'null':'got';});");
    mbWaitForFunction(v, "window.__av!==''", 3000);
    const std::string av = Eval(v, "window.__av");
    Eval(v, "window.__rel&&window.__rel()");  // release the held lock
    Expect(av == "null",
           "navigator.locks ifAvailable returns null when the lock is held (NO_WAIT)",
           "av=[" + av + "]");
  }

  // 23h. BroadcastChannel (window path, broker #8-adjacent): a window's BroadcastChannel
  // uses an ASSOCIATED provider from the frame's navigation-associated interfaces (not the
  // broker). The host serves it in-process: a message posted on one channel is delivered to
  // every OTHER same-name channel. Two channels 'ch' in one window — a.postMessage -> b
  // receives; the sender (a) does NOT receive its own message.
  {
    mbLoadHTML(v, "<body>x</body>", "https://bc.test/");
    Eval(v,
         "window.__bcB='';window.__bcA='self';"
         "var __a=new BroadcastChannel('ch');var __b=new BroadcastChannel('ch');"
         "__b.onmessage=function(e){window.__bcB=e.data;};"
         "__a.onmessage=function(e){window.__bcA='GOT:'+e.data;};"
         "__a.postMessage('ping');");
    mbWaitForFunction(v, "window.__bcB!==''", 2000);
    const std::string b = Eval(v, "window.__bcB");
    const std::string a = Eval(v, "window.__bcA");
    Expect(b == "ping" && a == "self",
           "BroadcastChannel delivers to other same-name channels, not the sender",
           "b=[" + b + "] a=[" + a + "]");
  }

  // 23i. BroadcastChannel across a window AND a Worker: the worker's channel uses the
  // broker path (its own thread) while the window's uses the nav-associated path; both bind
  // into ONE process-wide registry on the service thread, so they interoperate. A dedicated
  // worker posts on 'xch'; the window's same-name channel receives it. The worker reposts on
  // an interval so the window channel is certainly registered first (no buffering in BC).
  {
    mbLoadHTML(v, "<body>x</body>", "https://bcw.test/");
    Eval(v,
         "window.__xw='';"
         "var __xc=new BroadcastChannel('xch');"
         "__xc.onmessage=function(e){window.__xw=e.data;};"
         "window.__xworker=new Worker('data:text/javascript,'+encodeURIComponent("
         "'var c=new BroadcastChannel(\"xch\");var n=0;"
         "var t=setInterval(function(){c.postMessage(\"from-worker\");"
         "if(++n>40)clearInterval(t);},20);'));");
    mbWaitForFunction(v, "window.__xw!==''", 4000);
    const std::string xw = Eval(v, "window.__xw");
    Expect(xw == "from-worker",
           "BroadcastChannel bridges a Worker and the window (shared registry)",
           "xw=[" + xw + "]");
  }

  // 23j. Notification API (broker #8): the in-process NotificationService grants the
  // permission (Notification.permission == 'granted', a [Sync] call) and "shows" a
  // non-persistent notification by firing the listener's OnShow -> the page's
  // notification.onshow runs. (Headless: no OS toast, but the API is live + scriptable.)
  // Also Notification.requestPermission() resolves 'granted' via the permission service.
  {
    mbLoadHTML(v, "<body>x</body>", "https://notify.test/");
    const std::string perm = Eval(v, "Notification.permission");
    Eval(v,
         "window.__nshow='';window.__nperm='';"
         "Notification.requestPermission().then(function(p){window.__nperm=p;});"
         "var __n=new Notification('hi',{body:'there'});"
         "__n.onshow=function(){window.__nshow='shown:'+__n.title;};");
    mbWaitForFunction(v, "window.__nshow!==''&&window.__nperm!==''", 3000);
    const std::string shown = Eval(v, "window.__nshow");
    const std::string rp = Eval(v, "window.__nperm");
    Expect(perm == "granted" && shown == "shown:hi" && rp == "granted",
           "Notification: permission granted, new Notification fires onshow",
           "perm=[" + perm + "] show=[" + shown + "] requestPerm=[" + rp + "]");
  }

  // 23k. WebSocket (broker #8): the in-process WebSocketConnector establishes the
  // connection (onopen fires, readyState OPEN) and runs a loopback echo — a message the
  // page sends comes straight back via onmessage. Proves the whole WebSocket mojo data
  // plane (handshake + SendMessage framing over the writable pipe + OnDataFrame over the
  // readable pipe) works in-process, offline. Then ws.close() drives onclose.
  {
    mbLoadHTML(v, "<body>x</body>", "https://ws.test/");
    Eval(v,
         "window.__ws='';window.__wsmsg='';window.__wsclose='';"
         "var __s=new WebSocket('wss://echo.test/');"
         "__s.onopen=function(){window.__ws='open:'+__s.readyState;"
         "__s.send('hello-ws');};"
         "__s.onmessage=function(e){window.__wsmsg=e.data;__s.close();};"
         "__s.onclose=function(){window.__wsclose='closed';};");
    mbWaitForFunction(v, "window.__wsclose!==''", 3000);
    const std::string open = Eval(v, "window.__ws");
    const std::string msg = Eval(v, "window.__wsmsg");
    const std::string closed = Eval(v, "window.__wsclose");
    Expect(open == "open:1" && msg == "hello-ws" && closed == "closed",
           "WebSocket connects (onopen), echoes a message, and closes (onclose)",
           "open=[" + open + "] msg=[" + msg + "] close=[" + closed + "]");
  }

  // 23l. navigator.storage.estimate() (broker #8): the in-process QuotaManagerHost
  // reports a generous quota + zero usage, so storage.estimate() resolves with a usable
  // quota instead of hanging (the QuotaManagerHost was dropped before). Apps that gate
  // caching on available storage can proceed.
  {
    mbLoadHTML(v, "<body>x</body>", "https://quota.test/");
    Eval(v,
         "window.__q='';"
         "navigator.storage.estimate().then(function(e){"
         "window.__q=(e.quota>0?'quota:ok':'quota:zero')+',usage:'+e.usage;});");
    mbWaitForFunction(v, "window.__q!==''", 2000);
    const std::string q = Eval(v, "window.__q");
    Expect(q == "quota:ok,usage:0",
           "navigator.storage.estimate() resolves with a usable quota",
           "q=[" + q + "]");
  }

  // 23m. IndexedDB full round-trip (broker #8, step 1+2): open a database (onupgradeneeded
  // createObjectStore), put a record in a readwrite transaction, then get it back in a
  // separate transaction — the value (a structured-cloned object) survives the
  // serialize/store/deserialize round-trip through the in-memory backend.
  {
    mbLoadHTML(v, "<body>x</body>", "https://idb.test/");
    Eval(v,
         "window.__idb='';"
         "var __rq=indexedDB.open('mbdb2',1);"
         "__rq.onupgradeneeded=function(e){"
         "e.target.result.createObjectStore('items',{keyPath:'id'});};"
         "__rq.onsuccess=function(e){var db=e.target.result;"
         "var tx=db.transaction('items','readwrite');"
         "tx.objectStore('items').put({id:7,name:'widget',qty:3});"
         "tx.oncomplete=function(){"
         "var g=db.transaction('items').objectStore('items').get(7);"
         "g.onsuccess=function(){var r=g.result;"
         "window.__idb=r?('v'+db.version+',got:'+r.name+'x'+r.qty):'v'+db.version+',got:null';};"
         "g.onerror=function(){window.__idb='geterr';};};"
         "tx.onerror=function(){window.__idb='txerr';};};"
         "__rq.onerror=function(e){window.__idb='operr';};");
    mbWaitForFunction(v, "window.__idb!==''", 4000);
    const std::string r = Eval(v, "window.__idb");
    Expect(r == "v1,got:widgetx3",
           "IndexedDB: open + createObjectStore + put + get round-trips a record",
           "idb=[" + r + "]");
  }

  // 25. requestAnimationFrame must fire (no compositor drives it; the host services
  // the page animator). Register a rAF that mutates the DOM, pump, verify it ran.
  mbLoadHTML(v, "<body><b id='r'>0</b></body>", "about:blank");
  mbRunJS(v, "requestAnimationFrame(function(){"
             "document.getElementById('r').textContent='raf';});");
  mbWait(v, 50);
  Expect(Eval(v, "document.getElementById('r').textContent") == "raf",
         "requestAnimationFrame callback fires");
  // A rAF chain (two frames) also advances — proves repeated servicing, not a one-shot.
  mbRunJS(v, "window.__n=0;(function loop(){requestAnimationFrame(function(){"
             "if(++window.__n<2)loop();});})();");
  mbWait(v, 80);
  Expect(Eval(v, "String(window.__n)") == "2",
         "requestAnimationFrame chain advances", Eval(v, "String(window.__n)"));

  // 27. Observer delivery: MutationObserver must fire on a DOM change, and an
  // IntersectionObserver on an in-viewport element must deliver (the offscreen
  // frame reads as throttled, so IO is force-computed by the host).
  {
    const char* doc =
        "<body><div id='t'>0</div><div id='io' style='height:20px'></div>"
        "<script>"
        "window.__mo=0;new MutationObserver(function(){window.__mo=1;})"
        ".observe(document.getElementById('t'),{childList:true,subtree:true,characterData:true});"
        "document.getElementById('t').textContent='changed';"
        "window.__io=0;new IntersectionObserver(function(es){"
        "es.forEach(function(e){if(e.isIntersecting)window.__io=1;});})"
        ".observe(document.getElementById('io'));"
        "</script></body>";
    if (FILE* f = std::fopen("/tmp/mb_observers.html", "wb")) {
      std::fwrite(doc, 1, std::strlen(doc), f); std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_observers.html");
    mbWait(v, 80);
    Expect(Eval(v, "String(window.__mo)") == "1", "MutationObserver delivers");
    Expect(Eval(v, "String(window.__io)") == "1",
           "IntersectionObserver delivers (in-viewport)",
           Eval(v, "String(window.__io)"));
  }

  // 29. Time-based animation + networking-adjacent delivery (these guard the rAF /
  // animation-clock + observer servicing added in recent changes):
  //  - Web Animations API: a 100ms animation's finished promise resolves (clock advances).
  //  - ResizeObserver delivers its initial observation.
  //  - dynamic Image().onload fires; synchronous XHR to a data: URL returns the body.
  {
    mbLoadHTML(v,
        "<body><div id='b' style='width:50px;height:50px'></div><script>"
        "window.__waapi=0;document.getElementById('b').animate("
        "[{opacity:0},{opacity:1}],100).finished.then(function(){window.__waapi=1;});"
        "window.__ro=0;new ResizeObserver(function(){window.__ro=1;})"
        ".observe(document.getElementById('b'));"
        "window.__img=0;var im=new Image();im.onload=function(){window.__img=1;};"
        "im.src='data:image/svg+xml,<svg xmlns=%22http://www.w3.org/2000/svg%22 width=%225%22 height=%225%22></svg>';"
        "window.__xhr='';try{var x=new XMLHttpRequest();x.open('GET',"
        "'data:text/plain,hello',false);x.send();window.__xhr=x.responseText;}"
        "catch(e){window.__xhr='ERR:'+e.name;}"
        "</script></body>",
        "about:blank");
    mbWait(v, 250);
    Expect(Eval(v, "String(window.__waapi)") == "1",
           "Web Animations API finished promise resolves (clock advances)");
    Expect(Eval(v, "String(window.__ro)") == "1", "ResizeObserver delivers");
    Expect(Eval(v, "String(window.__img)") == "1" &&
               Eval(v, "window.__xhr") == "hello",
           "dynamic Image().onload + sync XHR(data:) work");
  }

  // 30. Console capture: page console.log/warn/error are captured and drainable.
  mbLoadHTML(v, "<body><script>console.log('hello');console.warn('careful');"
                "console.error('boom');</script></body>", "about:blank");
  mbWait(v, 20);
  {
    char cbuf[1024] = {0};
    mbDrainConsole(v, cbuf, sizeof(cbuf));
    std::string console(cbuf);
    Expect(console.find("log: hello") != std::string::npos &&
               console.find("warn: careful") != std::string::npos &&
               console.find("error: boom") != std::string::npos,
           "console capture (log/warn/error)", console);
    // Draining clears the buffer.
    char cbuf2[64] = {0};
    mbDrainConsole(v, cbuf2, sizeof(cbuf2));
    Expect(cbuf2[0] == '\0', "console buffer clears after drain");
  }

  // (IndexedDB is now covered by case 23m — the open+schema step-1 backend.)

  // Network cases (31, 32) are OPT-IN via MB_NET_TESTS=1: a dead host costs ~45s
  // per load (connect-timeout x retries), which would make every default run crawl.
  // They still skip gracefully if enabled but httpbin is unreachable.
  // Network cases use an httpbin-shaped echo host: default httpbin.org, override
  // with MB_NET_HOST for a deterministic local run, e.g.:
  //   python3 src/miniblink_host/test/echo_server.py &   # serves 127.0.0.1:8899
  //   MB_NET_TESTS=1 MB_NET_HOST=http://127.0.0.1:8899 ./mb_smoke
  if (std::getenv("MB_NET_TESTS")) {
  const std::string host =
      std::getenv("MB_NET_HOST") ? std::getenv("MB_NET_HOST") : "https://httpbin.org";
  // httpbin is a flaky public host: probe its health once so the cases that
  // assert specific httpbin shapes (status/redirect) SKIP — not fail — when it
  // is degraded (e.g. returning 503 for everything) rather than misbehaving.
  mbLoadURL(v, (host + "/get").c_str());
  const bool hb_ok = mbGetHttpStatus(v) == 200;
  // 31. Cookie jar: set a cookie via a redirecting endpoint, then a SEPARATE request
  // must still send it — Set-Cookie survives the redirect and the jar is shared.
  mbLoadURL(v, (host + "/cookies/set?mbck=val99").c_str());  // 302 -> /cookies
  mbWait(v, 400);
  std::string ck1 = Eval(v, "document.body?document.body.innerText:''");
  if (ck1.find("cookies") != std::string::npos) {  // host responded
    bool survived_redirect = ck1.find("val99") != std::string::npos;
    mbLoadURL(v, (host + "/cookies").c_str());  // separate request, shared jar
    mbWait(v, 400);
    std::string ck2 = Eval(v, "document.body?document.body.innerText:''");
    bool jar_persists = ck2.find("val99") != std::string::npos;
    Expect(survived_redirect && jar_persists,
           "cookie jar: survives redirect + persists across requests");
  } else {
    std::fprintf(stderr, "  [SKIP] cookie jar (host unreachable)\n");
  }

  // 32. Request headers: a custom header and the default Accept-Language must reach
  // the server (the echo host returns the request headers).
  mbSetExtraHeaders(v, "X-Mb-Test: probe-42");
  mbLoadURL(v, (host + "/headers").c_str());
  mbWait(v, 400);
  {
    std::string h = Eval(v, "document.body?document.body.innerText:''");
    if (h.find("headers") != std::string::npos) {  // host responded
      Expect(h.find("probe-42") != std::string::npos &&
                 h.find("Accept-Language") != std::string::npos,
             "request headers: custom header + default Accept-Language sent");
    } else {
      std::fprintf(stderr, "  [SKIP] request headers (host unreachable)\n");
    }
  }
  mbSetExtraHeaders(v, "");  // reset

  // 32b. Per-URL request header injection: a header registered for the "/headers" URL
  // is echoed; one registered for a non-matching host is NOT — proving it's conditional
  // on the URL (vs the global extra-headers above), e.g. an API key sent only to its host.
  mbSetRequestHeader("/headers", "X-Mb-Inject", "inj-77");
  mbSetRequestHeader("other.example", "X-Mb-Skip", "should-not-appear");
  mbLoadURL(v, (host + "/headers").c_str());
  mbWait(v, 400);
  {
    std::string h = Eval(v, "document.body?document.body.innerText:''");
    if (h.find("headers") != std::string::npos) {  // host responded
      Expect(h.find("inj-77") != std::string::npos &&
                 h.find("should-not-appear") == std::string::npos,
             "per-URL request header injected for the matching URL only");
    } else {
      std::fprintf(stderr, "  [SKIP] header injection (host unreachable)\n");
    }
  }
  mbClearRequestHeaders();  // reset

  // 33. Cookie bridge: a cookie set via document.cookie on an http origin must be
  // sent on a subsequent network request (JS jar -> HTTP jar).
  mbLoadURL(v, (host + "/").c_str());
  mbWait(v, 400);
  if (!Eval(v, "String(document.location.host)").empty() &&
      Eval(v, "String(document.location.host)") != "undefined") {
    mbRunJS(v, "document.cookie='mbjs=fromjs';");
    mbLoadURL(v, (host + "/cookies").c_str());
    mbWait(v, 400);
    std::string c = Eval(v, "document.body?document.body.innerText:''");
    if (c.find("cookies") != std::string::npos) {
      Expect(c.find("fromjs") != std::string::npos,
             "cookie bridge: document.cookie reaches the HTTP jar");
    } else {
      std::fprintf(stderr, "  [SKIP] cookie bridge (host unreachable)\n");
    }
  } else {
    std::fprintf(stderr, "  [SKIP] cookie bridge (host unreachable)\n");
  }

  // 34. Image loading toggle: a NETWORK <img> loads (naturalWidth>0) by default but
  // is skipped (naturalWidth==0) when image loading is disabled. (data: images are
  // inline and not gated by this setting, so the test uses a served image.)
  {
    const std::string page = "<body><img id='i' src='" + host + "/img'></body>";
    std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
    mbSetLoadImages(v, 1);
    mbLoadHTML(v, page.c_str(), (host + "/").c_str());
    mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // settle the fetch+decode
    std::string on_nw = Eval(v, "String(document.getElementById('i').naturalWidth)");
    if (std::atoi(on_nw.c_str()) > 0) {  // host served the image
      mbSetLoadImages(v, 0);
      mbLoadHTML(v, page.c_str(), (host + "/").c_str());
      mbPaintToBitmap(v, tmp.data(), W, H, W * 4);
      std::string off_nw =
          Eval(v, "String(document.getElementById('i').naturalWidth)");
      Expect(off_nw == "0", "no-images: network image skipped when disabled",
             "on=" + on_nw + " off=" + off_nw);
      mbSetLoadImages(v, 1);
    } else {
      std::fprintf(stderr, "  [SKIP] no-images (host image unreachable)\n");
    }
  }

  // 35. Cookie export: after the jar has a cookie (set above via /cookies/set),
  // mbGetCookies returns it for the host to extract/reuse.
  {
    mbLoadURL(v, (host + "/cookies/set?expk=expv").c_str());
    mbWait(v, 400);
    if (!Eval(v, "(document.body?document.body.innerText:'')").empty()) {
      char cb[512] = {0};
      mbGetCookies(v, (host + "/").c_str(), cb, sizeof(cb));
      Expect(std::string(cb).find("expk=expv") != std::string::npos,
             "mbGetCookies exports the jar", cb);
    } else {
      std::fprintf(stderr, "  [SKIP] cookie export (host unreachable)\n");
    }
  }
  // 36. POST form submission: a method=post form's body must reach the server.
  // The form auto-submits via JS (same BeginNavigation path as a click); the
  // echo host returns the posted fields as JSON, so ours must come back. This
  // exercises the POST path in DoCommit (extract WebHTTPBody) + MbFetchUrl POST.
  {
    const std::string page =
        "<body><form id='f' method='post' action='" + host + "/post'>"
        "<input name='user' value='mbpost'></form>"
        "<script>document.getElementById('f').submit();</script></body>";
    mbLoadHTML(v, page.c_str(), (host + "/").c_str());
    mbWait(v, 900);  // submit -> POST -> commit the echo response
    std::string r = Eval(v, "document.body?document.body.innerText:''");
    if (r.find("\"form\"") != std::string::npos ||
        r.find("\"user\"") != std::string::npos) {  // host echoed
      Expect(r.find("mbpost") != std::string::npos,
             "POST form submission: posted body reaches the server");
    } else {
      std::fprintf(stderr, "  [SKIP] POST form (host unreachable)\n");
    }
  }

  // 37. fetch()/XHR with a request body: the subresource loader (MbURLLoader)
  // must send the method + body, not a bodyless GET. Issue a fetch POST and read
  // the echoed field back. (The page is loaded from the host origin so the fetch
  // is same-origin.) Exercises the method/body extraction in MbURLLoader::Deliver.
  {
    mbLoadHTML(v, "<body>x</body>", (host + "/").c_str());
    mbRunJS(v,
        ("window.__fp='';fetch('" + host +
         "/post',{method:'POST',headers:{'Content-Type':"
         "'application/x-www-form-urlencoded'},body:'mk=fetchmk'})"
         ".then(function(r){return r.json();}).then(function(j){"
         "window.__fp=(j.form&&j.form.mk)||'nofield';}).catch(function(e){"
         "window.__fp='ERR:'+e.name;});").c_str());
    mbWait(v, 1500);  // async fetch round-trip
    std::string r = Eval(v, "String(window.__fp)");
    if (!r.empty() && r.rfind("ERR:", 0) != 0) {  // host responded
      Expect(r.find("fetchmk") != std::string::npos,
             "fetch() POST sends the request body", r);
    } else {
      std::fprintf(stderr, "  [SKIP] fetch POST (host unreachable: %s)\n",
                   r.c_str());
    }
  }

  // 38. fetch() per-request headers: a custom header set on the fetch (e.g. an
  // Authorization token or X-* header) must reach the server. The echo host
  // returns the request headers; ours must be present. Exercises forwarding of
  // request->headers in MbURLLoader::Deliver.
  {
    mbLoadHTML(v, "<body>x</body>", (host + "/").c_str());
    mbRunJS(v,
        ("window.__fh='';fetch('" + host +
         "/headers',{headers:{'X-Mb-Tok':'mbtok7'}})"
         ".then(function(r){return r.json();}).then(function(j){"
         // Match the value, not the key: echo hosts differ on header-name case
         // (httpbin Title-Cases, postman-echo lowercases).
         "window.__fh=JSON.stringify(j.headers||{});})"
         ".catch(function(e){window.__fh='ERR:'+e.name;});").c_str());
    mbWait(v, 1500);  // async fetch round-trip
    std::string r = Eval(v, "String(window.__fh)");
    if (!r.empty() && r.rfind("ERR:", 0) != 0) {  // host responded
      Expect(r.find("mbtok7") != std::string::npos,
             "fetch() forwards custom request headers", r);
    } else {
      std::fprintf(stderr, "  [SKIP] fetch headers (host unreachable: %s)\n",
                   r.c_str());
    }
  }

  // 39. Response status + headers: an HTTP error (404) must resolve as a real
  // Response (status 404, ok=false), NOT a rejected fetch — and a server
  // response header must be readable. Previously 4xx/5xx were turned into
  // network failures (TypeError) and only Content-Type was exposed. (httpbin
  // shapes: /status/404 and /response-headers?k=v.)
  {
    mbLoadHTML(v, "<body>x</body>", (host + "/").c_str());
    mbRunJS(v,
        ("window.__rs='';fetch('" + host +
         "/status/404').then(function(r){window.__rs=r.status+'/'+r.ok;})"
         ".catch(function(e){window.__rs='ERR:'+e.name;});"
         "window.__rh='';fetch('" + host +
         "/response-headers?X-Smk=sv1').then(function(r){"
         "window.__rh=r.headers.get('X-Smk')||'MISSING';})"
         ".catch(function(e){window.__rh='ERR:'+e.name;});").c_str());
    mbWait(v, 1800);  // two async round-trips
    std::string st = Eval(v, "String(window.__rs)");
    std::string hd = Eval(v, "String(window.__rh)");
    if (hb_ok && !st.empty() && st.rfind("ERR:", 0) != 0) {  // host healthy
      Expect(st == "404/false" && hd == "sv1",
             "fetch sees real HTTP status (404, !ok) + response headers",
             st + " hdr=" + hd);
    } else {
      std::fprintf(stderr, "  [SKIP] response status/headers (host unhealthy: %s)\n",
                   st.c_str());
    }
  }

  // 40. Navigation redirect: navigating to a URL that 302-redirects must commit
  // with the FINAL URL as the document URL (location.href), not the original.
  // curl follows the redirect; LoadURL now commits the effective URL as the base.
  {
    mbLoadURL(v,
              (host + "/redirect-to?url=" + host + "/get&status_code=302").c_str());
    mbWait(v, 700);
    std::string loc = Eval(v, "String(location.href)");
    if (hb_ok && (loc.find("/get") != std::string::npos ||
                  loc.find("/redirect-to") != std::string::npos)) {  // healthy
      Expect(loc.find("/get") != std::string::npos &&
                 loc.find("/redirect-to") == std::string::npos,
             "navigation redirect commits the final URL as location.href", loc);
    } else {
      std::fprintf(stderr, "  [SKIP] nav redirect (host unhealthy: %s)\n",
                   loc.c_str());
    }
  }

  // 41. fetch() redirect: a fetch that 302-redirects must resolve as a Response
  // whose url is the FINAL URL and whose .redirected is true. The loader follows
  // redirects manually and reports each hop via WillFollowRedirect so Blink's
  // url list (response.url / redirected) is correct.
  {
    mbLoadHTML(v, "<body>x</body>", (host + "/").c_str());
    mbRunJS(v,
        ("window.__rr='';fetch('" + host + "/redirect-to?url=" + host +
         "/get&status_code=302').then(function(r){"
         "window.__rr=r.url+'|'+r.redirected;}).catch(function(e){"
         "window.__rr='ERR:'+e.name;});").c_str());
    mbWait(v, 1500);
    std::string rr = Eval(v, "String(window.__rr)");
    if (hb_ok && !rr.empty() && rr.rfind("ERR:", 0) != 0) {  // host healthy
      Expect(rr.find("/get") != std::string::npos &&
                 rr.find("/redirect-to") == std::string::npos &&
                 rr.find("|true") != std::string::npos,
             "fetch() redirect exposes final url + redirected=true", rr);
    } else {
      std::fprintf(stderr, "  [SKIP] fetch redirect (host unhealthy: %s)\n",
                   rr.c_str());
    }
  }

  // 41 (net). mbGetHttpStatus reflects the last navigation's real HTTP status
  // (200 vs 404), and mbGetResponseHeaders exposes the server's response headers.
  {
    mbLoadURL(v, (host + "/html").c_str());
    mbWait(v, 600);
    const int ok_status = mbGetHttpStatus(v);
    if (ok_status != 0) {  // host reachable
      char hb[4096] = {0};
      mbGetResponseHeaders(v, hb, sizeof(hb));
      std::string headers(hb);
      for (char& c : headers) c = static_cast<char>(std::tolower((unsigned char)c));
      const bool has_ct = headers.find("content-type") != std::string::npos;
      mbLoadURL(v, (host + "/status/404").c_str());
      mbWait(v, 600);
      const int err_status = mbGetHttpStatus(v);
      Expect(ok_status == 200 && err_status == 404 && has_ct,
             "mbGetHttpStatus (200/404) + mbGetResponseHeaders exposes headers",
             "ok=" + std::to_string(ok_status) + " err=" +
                 std::to_string(err_status) + " ct=" + (has_ct ? "1" : "0"));
    } else {
      std::fprintf(stderr, "  [SKIP] http status/headers (host unreachable)\n");
    }
  }

  // 42 (net). mbSetFollowRedirects(0) stops at the redirect so the 30x status +
  // Location header are visible; re-enabling follows through to the final 200.
  {
    mbSetFollowRedirects(0);
    mbLoadURL(v, (host + "/redirect/1").c_str());  // 302 -> /get
    mbWait(v, 700);
    const int s_off = mbGetHttpStatus(v);
    char hb[4096] = {0};
    mbGetResponseHeaders(v, hb, sizeof(hb));
    std::string h(hb);
    for (char& c : h) c = static_cast<char>(std::tolower((unsigned char)c));
    const bool redirect_seen =
        s_off >= 300 && s_off < 400 && h.find("location:") != std::string::npos;
    mbSetFollowRedirects(1);  // restore the default (process-wide) before more loads
    mbLoadURL(v, (host + "/redirect/1").c_str());
    mbWait(v, 900);
    const int s_on = mbGetHttpStatus(v);
    if (s_off != 0 || s_on != 0) {  // host reachable
      Expect(redirect_seen && s_on == 200,
             "mbSetFollowRedirects: off exposes 30x+Location, on follows to 200",
             "off=" + std::to_string(s_off) + " on=" + std::to_string(s_on));
    } else {
      std::fprintf(stderr, "  [SKIP] follow-redirects (host unreachable)\n");
    }
  }

  // 43 (net). mbPostURL: host-driven POST navigation. httpbin/post echoes the
  // received form data into the response JSON, which becomes the document.
  {
    mbPostURL(v, (host + "/post").c_str(), "mbk=postval", nullptr);
    mbWait(v, 700);
    const int status = mbGetHttpStatus(v);
    if (status != 0) {  // host reachable
      std::string doc = Eval(v, "document.body?document.body.innerText:''");
      Expect(status == 200 && doc.find("mbk") != std::string::npos &&
                 doc.find("postval") != std::string::npos,
             "mbPostURL posts a body and commits the response",
             "status=" + std::to_string(status));
    } else {
      std::fprintf(stderr, "  [SKIP] mbPostURL (host unreachable)\n");
    }
  }

  // 44 (net). End-to-end integration on a REAL https page: one fetch over real
  // TLS exercises the whole stack together — load -> parse -> layout -> the recent
  // scraping readers (text/html/rect/style). example.com is a stable target whose
  // <h1> says "Example Domain". Skips if the host is unreachable.
  {
    mbLoadURL(v, "https://example.com");
    mbWaitForSelector(v, "h1", 4000);
    const int status = mbGetHttpStatus(v);
    if (status == 200) {
      char tb[256] = {0};
      mbGetTextForSelector(v, "h1", tb, sizeof(tb));
      const bool text_ok = std::string(tb).find("Example Domain") != std::string::npos;
      char hb[512] = {0};
      mbGetHtmlForSelector(v, "h1", hb, sizeof(hb));
      const std::string html(hb);
      const bool html_ok = html.find("<h1") != std::string::npos &&
                           html.find("Example Domain") != std::string::npos;
      int rw = 0, rh = 0;
      const bool rect_ok =
          mbGetElementRect(v, "h1", nullptr, nullptr, &rw, &rh) && rw > 0 && rh > 0;
      char sb[64] = {0};
      mbGetComputedStyle(v, "h1", "display", sb, sizeof(sb));
      const bool style_ok = std::string(sb) == "block";
      Expect(text_ok && html_ok && rect_ok && style_ok,
             "integration: real-TLS load + text/html/rect/style readers agree",
             std::string("text=") + (text_ok ? "1" : "0") + " html=" +
                 (html_ok ? "1" : "0") + " rect=" + (rect_ok ? "1" : "0") +
                 " style=" + (style_ok ? "1" : "0"));
    } else {
      std::fprintf(stderr, "  [SKIP] integration (example.com unreachable)\n");
    }
  }
  }  // MB_NET_TESTS

  // 33. document.cookie (JS): write then read round-trips through the in-process
  // RestrictedCookieManager wired into the frame's BrowserInterfaceBroker.
  {
    const char* doc = "<body>x</body>";
    if (FILE* f = std::fopen("/tmp/mb_jsck.html", "wb")) {
      std::fwrite(doc, 1, std::strlen(doc), f); std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_jsck.html");
    mbRunJS(v, "document.cookie='a=1';document.cookie='b=2';");
    mbWait(v, 20);
    std::string ck = Eval(v, "document.cookie");
    Expect(ck.find("a=1") != std::string::npos &&
               ck.find("b=2") != std::string::npos,
           "document.cookie read/write round-trip", ck);
  }

  // 33b. document.cookie READ as the first cookie op, from the page's own inline
  // script during load (no prior write, no pump in between). This is the common
  // "read existing cookies on load" pattern and it used to HANG: the synchronous
  // RestrictedCookieManager.GetCookiesString blocked the main thread before the
  // BrowserInterfaceBroker.GetInterface that binds the manager had been pumped.
  // The broker is now bound on the runtime service thread, so the [Sync] read is
  // serviced off-thread and returns immediately. The inline read records the jar
  // into the DOM; reaching this assertion at all proves it didn't hang. Same
  // file:// origin as case 33, so it reads back the a=1/b=2 set there.
  {
    const char* doc =
        "<body><div id=o>x</div><script>"
        "document.getElementById('o').textContent='ck['+document.cookie+']';"
        "</script></body>";
    if (FILE* f = std::fopen("/tmp/mb_jsck2.html", "wb")) {
      std::fwrite(doc, 1, std::strlen(doc), f); std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_jsck2.html");
    mbWait(v, 30);
    std::string ck2 = Eval(v, "document.getElementById('o').textContent");
    Expect(ck2.rfind("ck[", 0) == 0 && ck2.find("a=1") != std::string::npos &&
               ck2.find("b=2") != std::string::npos,
           "document.cookie read-first on load does not hang", ck2);
  }

  // 34. Init script (evaluateOnNewDocument): runs before the page's own scripts.
  // Set a global in the init script; the page's inline script must observe it.
  mbSetInitScript(v, "window.__early='injected';");
  mbLoadHTML(v,
             "<body><script>window.__pageSaw=window.__early||'no';</script></body>",
             "about:blank");
  Expect(Eval(v, "window.__pageSaw") == "injected",
         "init script runs before page scripts", Eval(v, "window.__pageSaw"));
  mbSetInitScript(v, "");  // clear so it doesn't affect any later case

  // 35. Isolated-world eval: separate JS globals from the main world, shared DOM.
  mbLoadHTML(v, "<body></body>", "about:blank");
  mbRunJS(v, "window.__main='mainval';");
  // In the isolated world: set its own global, touch the shared DOM, and report
  // whether it can see the main world's global (it must NOT).
  Expect(EvalIso(v, "window.__iso='isoval';"
                    "document.body.setAttribute('data-s','shared');"
                    "String(typeof window.__main);") == "undefined",
         "isolated world: cannot see main-world globals");
  Expect(Eval(v, "String(window.__iso)") == "undefined",
         "isolated world: does not leak globals into main world");
  Expect(Eval(v, "document.body.getAttribute('data-s')") == "shared",
         "isolated world: shares the DOM with main world");

  // 36. Dark mode: prefers-color-scheme media query + a responsive CSS rule flip
  // when dark mode is emulated.
  {
    const char* page =
        "<style>#d{color:rgb(1,1,1)}"
        "@media (prefers-color-scheme:dark){#d{color:rgb(2,2,2)}}</style>"
        "<body><b id='d'>x</b></body>";
    mbSetDarkMode(v, 1);
    mbLoadHTML(v, page, "about:blank");
    bool mm = Eval(v, "String(matchMedia('(prefers-color-scheme:dark)').matches)") ==
              "true";
    bool css = Eval(v, "getComputedStyle(document.getElementById('d')).color") ==
               "rgb(2, 2, 2)";
    Expect(mm && css, "dark mode: prefers-color-scheme dark applies");
    mbSetDarkMode(v, 0);  // restore light for any later case
  }

  // 37. Locale: navigator.language / navigator.languages reflect the set value.
  mbSetLocale(v, "fr-FR,fr,en");
  mbLoadHTML(v, "<body>x</body>", "about:blank");
  Expect(Eval(v, "navigator.language") == "fr-FR" &&
             Eval(v, "navigator.languages.join(',')") == "fr-FR,fr,en",
         "locale: navigator.language(s) set",
         Eval(v, "navigator.language") + " / " +
             Eval(v, "navigator.languages.join(',')"));
  mbSetLocale(v, "en-US");  // restore for any later case

  // 38. Timezone override: Date/Intl report the chosen zone deterministically.
  mbSetTimezone(v, "America/New_York");
  mbLoadHTML(v, "<body>x</body>", "about:blank");
  Expect(Eval(v, "Intl.DateTimeFormat().resolvedOptions().timeZone") ==
             "America/New_York",
         "timezone override (Intl resolvedOptions)",
         Eval(v, "Intl.DateTimeFormat().resolvedOptions().timeZone"));
  // A fixed UTC instant formats to a New-York wall-clock time (EST/EDT), proving
  // Date itself uses the zone: 2021-01-01T00:00:00Z -> 2020-12-31 19:00 EST.
  Expect(Eval(v, "new Date(1609459200000).getHours().toString()") == "19",
         "timezone override (Date local hours)",
         Eval(v, "new Date(1609459200000).getHours().toString()"));
  mbSetTimezone(v, "UTC");  // restore deterministic UTC for any later case

  // 39. PDF export: print a document to a PDF and confirm it's a real PDF file.
  mbLoadHTML(v, "<body style='font:30px sans-serif'><h1>PDF</h1><p>page content</p></body>",
             "about:blank");
  {
    const char* pdf_path = "/tmp/mb_smoke.pdf";
    bool ok = mbSavePdf(v, pdf_path) != 0;
    char hdr[6] = {0};
    long sz = 0;
    if (FILE* f = std::fopen(pdf_path, "rb")) {
      std::fread(hdr, 1, 5, f);
      std::fseek(f, 0, SEEK_END);
      sz = std::ftell(f);
      std::fclose(f);
    }
    Expect(ok && std::string(hdr) == "%PDF-" && sz > 500,
           "PDF export (valid %PDF, non-trivial)",
           std::string(hdr) + " sz=" + std::to_string(sz));
  }

  // 39b. mbSavePdfEx page geometry: an A4 page (595x842 pt) and its landscape (842x595)
  // must set the PDF MediaBox accordingly — proves custom size + landscape reach output.
  {
    auto slurp = [](const char* p) -> std::string {
      std::string s;
      if (FILE* f = std::fopen(p, "rb")) {
        char b[8192];
        size_t n;
        while ((n = std::fread(b, 1, sizeof(b), f)) > 0)
          s.append(b, n);
        std::fclose(f);
      }
      return s;
    };
    const bool a4 = mbSavePdfEx(v, "/tmp/mb_a4.pdf", 595, 842, 0, 1.0, 0) != 0;
    const std::string p1 = slurp("/tmp/mb_a4.pdf");
    const bool ls = mbSavePdfEx(v, "/tmp/mb_a4l.pdf", 595, 842, 1, 1.0, 0) != 0;
    const std::string p2 = slurp("/tmp/mb_a4l.pdf");
    const bool a4ok = p1.find("595 842") != std::string::npos;
    const bool lsok = p2.find("842 595") != std::string::npos;
    Expect(a4 && a4ok && ls && lsok,
           "mbSavePdfEx sets the PDF MediaBox (A4 portrait + landscape)",
           std::string("a4=") + std::to_string(a4ok) + " ls=" + std::to_string(lsok));
  }

  // 107. Native function binding: a C function bound via mbJsBindFunction is
  // callable from JS synchronously — window[name](args) returns the C result
  // inline — receiving string args and the userdata pointer. Installed into each
  // new document (so it works after a navigation and from a page event handler).
  {
    int tag = 7;
    mbJsBindFunction(v, "mbEcho", SmokeEcho, &tag);
    mbJsBindFunction(v, "mbObj", SmokeJson, nullptr);
    mbLoadHTML(v, "<body>native</body>", "about:blank");
    const std::string defined = Eval(v, "typeof window.mbEcho");
    const std::string r = Eval(v, "window.mbEcho('hi')");  // -> "hi!7"
    const std::string in_expr =
        Eval(v, "(function(){return 'got:'+window.mbEcho('x');})()");
    // out_type 5 (json): the C return becomes a real JS object the page navigates.
    const std::string obj_type = Eval(v, "typeof window.mbObj()");
    const std::string obj_vals =
        Eval(v, "window.mbObj().a + ',' + window.mbObj().b[1]");  // -> "1,3"
    Expect(defined == "function" && r == "hi!7" && in_expr == "got:x!7" &&
               obj_type == "object" && obj_vals == "1,3",
           "mbJsBindFunction: synchronous C call; string + JSON-object returns",
           "typeof=" + defined + " r=" + r + " expr=" + in_expr +
               " objType=" + obj_type + " objVals=" + obj_vals);
  }

  {
    // case 51: null-argument robustness. A C ABI must not crash when a caller
    // passes a null string pointer — it should return the documented failure
    // value. Reaching the Expect at all proves no function crashed (a null
    // deref would abort the process before we get there). Probes a spread
    // across categories: selector getters (-1), action setters (0), eval (0),
    // file save (0), and void sinks (no return, just must not crash).
    char nb[16] = {0};
    int rect_x = 0, rect_y = 0, rect_w = 0, rect_h = 0;
    const bool getters_safe =
        mbGetTextForSelector(v, nullptr, nb, sizeof(nb)) == -1 &&
        mbGetValueForSelector(v, nullptr, nb, sizeof(nb)) == -1 &&
        mbGetAttribute(v, nullptr, nullptr, nb, sizeof(nb)) == -1 &&
        mbGetCheckedForSelector(v, nullptr) == -1 &&
        mbIsVisibleForSelector(v, nullptr) == -1 &&
        mbGetElementRect(v, nullptr, &rect_x, &rect_y, &rect_w, &rect_h) == 0;
    const bool actions_safe =
        mbClickSelector(v, nullptr) == 0 &&
        mbFillSelector(v, nullptr, "x") == 0 &&
        mbSelectOption(v, nullptr, nullptr) == 0 &&
        mbSetAttribute(v, nullptr, nullptr, nullptr) == 0 &&
        mbEvalJS(v, nullptr, nb, sizeof(nb)) == 0 &&
        mbSavePdf(v, nullptr) == 0;
    // void sinks: no return value — the test is simply that these don't crash.
    mbLoadURL(v, nullptr);
    mbSetCookie(v, nullptr, nullptr);
    mbSendKey(v, nullptr);
    mbSendText(v, nullptr);
    Expect(getters_safe && actions_safe,
           "C ABI is null-arg safe (no crash; documented failure returns)",
           std::string("getters=") + (getters_safe ? "ok" : "BAD") +
               " actions=" + (actions_safe ? "ok" : "BAD"));
  }

  mbDestroyView(v);
  mbShutdown();

  std::fprintf(stderr, "\nmb_smoke: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
