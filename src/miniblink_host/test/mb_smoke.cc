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

  // 0b2. mbOnDOMContentLoaded fires when the DOM is parsed (deferred scripts run, before
  // subresources) — the "page interactive" signal, EARLIER than load-finish/onload. Record
  // both signals into one sequence: DOMContentLoaded ('D') must fire AND precede load ('L').
  {
    static std::string* seq = new std::string();  // -Wexit-time-destructors
    seq->clear();
    mbOnDOMContentLoaded(v, [](mbView*, void*) { *seq += "D"; }, nullptr);
    mbOnLoadFinish(v, [](mbView*, void*) { *seq += "L"; }, nullptr);
    mbLoadHTML(v, "<body>dcl<script>document.title='x';</script></body>",
               "about:blank");
    Expect(*seq == "DL",
           "mbOnDOMContentLoaded fires before load-finish (DOM-ready signal)",
           "seq=[" + *seq + "]");
    mbOnDOMContentLoaded(v, nullptr, nullptr);
    mbOnLoadFinish(v, nullptr, nullptr);
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

  // 0c2. mbSetRequestCallbackEx: the request hook also sees the METHOD, request HEADERS,
  // and POST BODY — so an embedder can MONITOR what API calls a page makes (a POST and its
  // payload), not just match URLs. A same-origin fetch POST with a custom header + body ->
  // the hook captures method=POST, the header, and the exact body. Offline (mock, no net).
  {
    static std::string* cap = new std::string();  // -Wexit-time-destructors
    cap->clear();
    mbMockResponse("api.test/submit", "{\"ok\":1}", "application/json", 200);
    mbSetRequestCallbackEx(
        [](const char* url, const char* method, const char* headers,
           const char* body, int body_len, void*) -> int {
          if (std::string(url).find("api.test/submit") != std::string::npos) {
            const bool hdr = std::string(headers).find("X-Tok: abc") != std::string::npos;
            *cap = std::string("m=") + method + " hdr=" + (hdr ? "1" : "0") +
                   " body=" + std::string(body, static_cast<size_t>(body_len));
          }
          return 0;  // allow
        },
        nullptr);
    mbLoadHTML(v, "<body>reqex</body>", "https://api.test/");
    mbRunJS(v, "fetch('/submit',{method:'POST',headers:{'X-Tok':'abc'},"
               "body:'payload-42'});");
    mbWaitForFunction(v, "true", 300);
    mbWait(v, 100);
    mbSetRequestCallbackEx(nullptr, nullptr);
    mbClearMocks();
    Expect(*cap == "m=POST hdr=1 body=payload-42",
           "mbSetRequestCallbackEx: request hook sees method + headers + POST body",
           "[" + *cap + "]");
  }

  // 0c3. Block by RESOURCE TYPE (mbBlockResourceType): block "image" -> an <img> fails to
  // load (onerror, ERR_BLOCKED_BY_CLIENT); unblocking it -> the same image loads. Lets a
  // scrape skip heavy classes (images/fonts/media) for speed without listing URLs. Offline.
  {
    // Mock an http image so the request flows through the loader (data: images are decoded
    // inline by blink and never reach the loader's per-request type check).
    mbMockResponse("imgtype.test/pic.svg",
                   "<svg xmlns='http://www.w3.org/2000/svg' width='5' height='5'></svg>",
                   "image/svg+xml", 200);
    const char* page =
        "<body><img id='im' src='/pic.svg' "
        "onload=\"window.__im='loaded'\" onerror=\"window.__im='error'\"></body>";
    mbBlockResourceType("image", 1);
    mbLoadHTML(v, page, "https://imgtype.test/");
    mbWaitForFunction(v, "window.__im!==undefined", 2000);
    const std::string blocked = Eval(v, "window.__im");
    mbBlockResourceType("image", 0);  // unblock
    mbLoadHTML(v, page, "https://imgtype.test/");
    mbWaitForFunction(v, "window.__im!==undefined", 2000);
    const std::string allowed = Eval(v, "window.__im");
    mbClearMocks();
    Expect(blocked == "error" && allowed == "loaded",
           "mbBlockResourceType: block 'image' fails <img>, unblock loads it",
           "blocked=[" + blocked + "] allowed=[" + allowed + "]");
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

  // 0d2. Response hook can rewrite the STATUS (mbResponseSetStatus), not just the body — so
  // an embedder can dynamically fabricate a response (route.fulfill-like, decided from the
  // actual upstream response). A mock serves 200; the hook forces 503; the page's fetch must
  // see response.status===503 and response.ok===false. Offline.
  {
    mbMockResponse("api.test/flaky", "upstream-ok", "text/plain", 200);
    mbSetResponseCallback(
        [](mbResponse* r, void*) {
          if (std::strstr(mbResponseURL(r), "api.test/flaky"))
            mbResponseSetStatus(r, 503);  // turn the 200 into a 503
        },
        nullptr);
    mbLoadHTML(v,
               "<body><div id='s'>?</div><script>"
               "fetch('https://api.test/flaky').then(r=>{"
               "document.getElementById('s').textContent=r.status+':'+r.ok;});</script></body>",
               "https://api.test/");
    mbWaitForFunction(v, "document.getElementById('s').textContent!=='?'", 2000);
    const std::string s = Eval(v, "document.getElementById('s').textContent");
    Expect(s == "503:false",
           "mbResponseSetStatus: response hook rewrites the HTTP status the page sees",
           "page=[" + s + "]");
    mbSetResponseCallback(nullptr, nullptr);
    mbClearMocks();
  }

  // 0d3. Response hook can inject/override response HEADERS (mbResponseSetHeader): a custom
  // header the page reads back via fetch Response.headers.get, and a Content-Type override.
  // Same-origin so all headers are exposed. Proves header mutation flows to the delivered
  // response (CORS injection / Content-Type forcing / custom fields).
  {
    mbMockResponse("api.test/h", "hello", "text/plain", 200);
    mbSetResponseCallback(
        [](mbResponse* r, void*) {
          if (std::strstr(mbResponseURL(r), "api.test/h")) {
            mbResponseSetHeader(r, "X-Injected", "yes");
            mbResponseSetHeader(r, "Content-Type", "application/json");
          }
        },
        nullptr);
    mbLoadHTML(v,
               "<body><div id='r'>?</div><script>"
               "fetch('https://api.test/h').then(r=>{"
               "document.getElementById('r').textContent="
               "'x='+r.headers.get('x-injected')+',ct='+r.headers.get('content-type');});"
               "</script></body>",
               "https://api.test/");
    mbWaitForFunction(v, "document.getElementById('r').textContent!=='?'", 2000);
    const std::string r = Eval(v, "document.getElementById('r').textContent");
    Expect(r.find("x=yes") != std::string::npos &&
               r.find("application/json") != std::string::npos,
           "mbResponseSetHeader injects a custom header + overrides Content-Type (fetch sees both)",
           "r=[" + r + "]");
    mbSetResponseCallback(nullptr, nullptr);
    mbClearMocks();
  }

  // 0d4. Dynamic request mock (mbSetRequestMockCallback): COMPUTE a response per-URL with no
  // fetch, for URLs that can't be pre-registered as a fixed substring. Here the callback
  // parses the id out of /item/N and serves {"id":N} as JSON — something the static
  // mbMockResponse (fixed substring -> fixed body) can't do.
  {
    mbSetRequestMockCallback(
        [](const char* url, mbRequestMock* m, void*) -> int {
          const char* p = std::strstr(url, "/item/");
          if (!p)
            return 0;  // not ours: fetch normally
          std::string body = std::string("{\"id\":") + (p + 6) + "}";
          mbRequestMockResponse(m, body.data(), static_cast<int>(body.size()),
                                "application/json", 200);
          return 1;  // serve the computed response
        },
        nullptr);
    mbLoadHTML(v,
               "<body><div id='r'>?</div><script>"
               "fetch('https://x.test/item/42').then(r=>r.json()).then(j=>{"
               "document.getElementById('r').textContent='id='+j.id;});</script></body>",
               "https://x.test/");
    mbWaitForFunction(v, "document.getElementById('r').textContent!=='?'", 2000);
    const std::string r = Eval(v, "document.getElementById('r').textContent");
    Expect(r == "id=42",
           "mbSetRequestMockCallback computes a per-URL response served without a fetch",
           "r=[" + r + "]");
    mbSetRequestMockCallback(nullptr, nullptr);
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

  // 0i4. Trusted mouse-wheel (#12 input): mbSendWheel dispatches a real wheel event so a
  // page `wheel` handler sees DOM-convention deltas (deltaY>0 = down, deltaX>0 = right)
  // with isTrusted=true — what wheel-driven UIs (map/canvas zoom, scroll hijacking,
  // "load more on scroll") listen for — AND scrolls the document viewport by the deltas
  // (here scrollY -> ~120). A wheel handler that calls preventDefault SUPPRESSES the
  // scroll, exactly like a real browser (checked below).
  {
    mbLoadHTML(v,
        "<body style='margin:0;height:5000px'><script>window.w='';"
        "addEventListener('wheel',function(e){"
        "window.w=e.deltaY+','+e.deltaX+','+e.isTrusted;});"
        "</script></body>",
        "about:blank");
    mbSendWheel(v, 50, 50, 0, 120, 0);     // wheel down -> event + scroll
    const std::string w1 = Eval(v, "window.w");
    const std::string sy = Eval(v, "''+Math.round(window.scrollY)");
    mbSendWheel(v, 50, 50, 40, -120, 0);   // wheel up + right
    const std::string w2 = Eval(v, "window.w");
    Expect(w1 == "120,0,true" && w2 == "-120,40,true" && sy == "120",
           "mbSendWheel fires a trusted wheel event (both axes) AND scrolls the document",
           "w1=[" + w1 + "] w2=[" + w2 + "] scrollY=[" + sy + "]");
  }

  // 0i5. A wheel handler that calls preventDefault SUPPRESSES the default scroll
  // (browser-accurate): the event still fires but window.scrollY stays 0.
  {
    mbLoadHTML(v,
        "<body style='margin:0;height:5000px'><script>window.pd=0;"
        "addEventListener('wheel',function(e){window.pd=1;e.preventDefault();},"
        "{passive:false});</script></body>",
        "about:blank");
    mbSendWheel(v, 50, 50, 0, 200, 0);
    const std::string pd = Eval(v, "''+window.pd");
    const std::string sy = Eval(v, "''+Math.round(window.scrollY)");
    Expect(pd == "1" && sy == "0",
           "mbSendWheel: a preventDefault wheel handler suppresses the scroll",
           "pd=[" + pd + "] scrollY=[" + sy + "]");
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

  // 0k2. mbOnConsoleMessageEx delivers source/line/stack, so an embedder can monitor
  // UNCAUGHT EXCEPTIONS (blink reports them here as console errors) with full location —
  // not just the message. A page that throws at top level must surface: level=error, the
  // thrown text, the page URL as source, a line number, and a non-empty JS stack.
  {
    static std::string* einfo = new std::string();  // -Wexit-time-destructors
    einfo->clear();
    mbOnConsoleMessageEx(
        v,
        [](mbView*, void*, const char* level, const char* msg, const char* source,
           int line, const char* stack) {
          if (level && std::string(level) == "error") {
            *einfo = std::string("msg=[") + (msg ? msg : "") + "] src=[" +
                     (source ? source : "") + "] line=" + std::to_string(line) +
                     " stacklen=" +
                     std::to_string(stack ? std::string(stack).size() : 0);
          }
        },
        nullptr);
    mbLoadHTML(v, "<body><script>throw new Error('kaboom');</script></body>",
               "https://errpage.test/");
    mbWait(v, 80);
    // Uncaught exception: message + source URL + line are delivered (error monitoring).
    const bool has_msg = einfo->find("kaboom") != std::string::npos;
    const bool has_src = einfo->find("errpage.test") != std::string::npos;
    const bool has_line = einfo->find("line=0 ") == std::string::npos &&
                          einfo->find("line=") != std::string::npos;
    const std::string exc = *einfo;
    // console.error from a nested call chain: the detailed-message opt-in
    // (ShouldReportDetailedMessageForSourceAndSeverity) makes blink capture the FULL JS
    // stack, so `stack` names the call chain — what a monitor needs to locate the source.
    einfo->clear();
    mbLoadHTML(v,
               "<body><script>function inner(){console.error('traced');}"
               "function outer(){inner();}outer();</script></body>",
               "https://errpage2.test/");
    mbWait(v, 80);
    const bool stack_ok = einfo->find("stacklen=0") == std::string::npos &&
                          einfo->find("stacklen=") != std::string::npos &&
                          einfo->find("traced") != std::string::npos;
    Expect(has_msg && has_src && has_line && stack_ok,
           "mbOnConsoleMessageEx: exception gives message+source+line; console.* gives a stack",
           "exc=[" + exc + "] traced=[" + *einfo + "]");
    mbOnConsoleMessageEx(v, nullptr, nullptr);
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

  // 0l2. Title-changed: mbOnTitleChanged fires with the initial <title> and on every dynamic
  // document.title write — track tab titles / progress from automation.
  {
    static std::string* titles = new std::string();  // -Wexit-time-destructors
    titles->clear();
    mbOnTitleChanged(
        v, [](mbView*, void*, const char* t) { *titles += std::string(t) + ";"; },
        nullptr);
    mbLoadHTML(v, "<title>First</title><body>x</body>", "https://t.test/");
    mbRunJS(v, "document.title='Second';");
    mbWait(v, 30);
    mbRunJS(v, "document.title='Third';");
    mbWait(v, 30);
    Expect(titles->find("First;") != std::string::npos &&
               titles->find("Second;") != std::string::npos &&
               titles->find("Third;") != std::string::npos,
           "mbOnTitleChanged fires with initial <title> + dynamic document.title writes",
           "titles=[" + *titles + "]");
    mbOnTitleChanged(v, nullptr, nullptr);
  }

  // 0l3. Favicon-changed: mbOnFaviconChanged fires with the page's favicon URL (resolved
  // absolute), completing the browser tab-metadata trio (URL / title / favicon).
  {
    static std::string* fav = new std::string();  // -Wexit-time-destructors
    fav->clear();
    mbOnFaviconChanged(
        v, [](mbView*, void*, const char* u) { *fav = u; }, nullptr);
    mbLoadHTML(v,
               "<head><link rel=\"icon\" href=\"/icon.png\"></head><body>x</body>",
               "https://fav.test/page");
    mbWait(v, 120);
    Expect(fav->find("fav.test/icon.png") != std::string::npos,
           "mbOnFaviconChanged fires with the page's favicon URL (resolved absolute)",
           "fav=[" + *fav + "]");
    mbOnFaviconChanged(v, nullptr, nullptr);
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

  // 0m2. Page-initiated blob download: a client-generated file via
  // URL.createObjectURL(new Blob(...)) + a <a download> click reaches mbOnDownload
  // with the suggested filename and the blob's bytes, through LocalFrameHost
  // .DownloadURL (no server). This is the "export as CSV/PDF" pattern, built in JS.
  {
    static std::string* dl = new std::string();  // -Wexit-time-destructors
    dl->clear();
    mbLoadHTML(v, "<body>HOST</body>", "about:blank");  // a real host page
    mbOnDownload(
        v,
        [](mbView*, void*, const char* /*url*/, const char* /*mime*/,
           const char* fn, const char* data, int len) {
          *dl = std::string("fn=") + (fn ? fn : "") + " body=" +
                std::string(data ? data : "", data ? len : 0);
        },
        nullptr);
    Eval(v,
         "(function(){var b=new Blob(['hello,world'],{type:'text/csv'});"
         "var u=URL.createObjectURL(b);var a=document.createElement('a');"
         "a.href=u;a.download='data.csv';document.body.appendChild(a);"
         "a.click();return 1;})()");
    // The download is async: DownloadURL (service thread) -> blob read -> hop to
    // the main thread. Pump a moment for it to land (no JS flag tracks it).
    mbWaitForFunction(v, "window.__mbNever===1", 1500);
    Expect(*dl == "fn=data.csv body=hello,world",
           "mbOnDownload captures a page-initiated blob download (<a download>)",
           "dl=[" + *dl + "]");
    mbOnDownload(v, nullptr, nullptr);
  }

  // 0m3. Page-initiated http(s) download link: a same-origin <a download
  // href="https://..."> click reaches mbOnDownload with the response MIME + the
  // fetched bytes, through LocalFrameHost.DownloadURL -> the engine fetch (here a
  // mock, so it's offline). The download attribute also carries the filename.
  {
    static std::string* dl = new std::string();  // -Wexit-time-destructors
    dl->clear();
    mbMockResponse("host.test/page", "<body>HOST</body>", "text/html", 200);
    mbMockResponse("host.test/report.csv", "a,b,c", "text/csv", 200);
    mbLoadURL(v, "https://host.test/page");  // same origin as the download URL
    mbOnDownload(
        v,
        [](mbView*, void*, const char* /*url*/, const char* mime,
           const char* fn, const char* data, int len) {
          *dl = std::string("mime=") + mime + " fn=" + (fn ? fn : "") +
                " body=" + std::string(data ? data : "", data ? len : 0);
        },
        nullptr);
    Eval(v,
         "(function(){var a=document.createElement('a');"
         "a.href='https://host.test/report.csv';a.download='r.csv';"
         "document.body.appendChild(a);a.click();return 1;})()");
    mbWaitForFunction(v, "window.__mbNever===1", 1500);  // pump the async fetch
    Expect(*dl == "mime=text/csv fn=r.csv body=a,b,c",
           "mbOnDownload captures a page-initiated http download link (<a download>)",
           "dl=[" + *dl + "]");
    mbOnDownload(v, nullptr, nullptr);
    mbClearMocks();
  }

  // 0m4. Page-initiated data: download: a <a download href="data:..."> click
  // reaches mbOnDownload with the decoded bytes (the engine fetch decodes data:
  // inline — no blob store, no network). Small client-generated files often ship
  // as a data: URI rather than a Blob.
  {
    static std::string* dl = new std::string();  // -Wexit-time-destructors
    dl->clear();
    mbLoadHTML(v, "<body>HOST</body>", "about:blank");
    mbOnDownload(
        v,
        [](mbView*, void*, const char* /*url*/, const char* /*mime*/,
           const char* fn, const char* data, int len) {
          *dl = std::string("fn=") + (fn ? fn : "") + " body=" +
                std::string(data ? data : "", data ? len : 0);
        },
        nullptr);
    Eval(v,
         "(function(){var a=document.createElement('a');"
         "a.href='data:text/plain,inline-bytes';a.download='note.txt';"
         "document.body.appendChild(a);a.click();return 1;})()");
    mbWaitForFunction(v, "window.__mbNever===1", 1500);  // pump the async decode
    Expect(*dl == "fn=note.txt body=inline-bytes",
           "mbOnDownload captures a page-initiated data: download (<a download>)",
           "dl=[" + *dl + "]");
    mbOnDownload(v, nullptr, nullptr);
  }

  // 0m5. Empty <a download> attribute (no filename): blink leaves the suggested
  // name EMPTY and expects the browser to derive one from the URL (same as the
  // cross-origin case, where it strips the attr-provided name). The download path
  // now falls back to the URL's last path segment ("report.csv") instead of "".
  {
    static std::string* dl = new std::string();  // -Wexit-time-destructors
    dl->clear();
    mbMockResponse("dlhost.test/page", "<body>HOST</body>", "text/html", 200);
    mbMockResponse("dlhost.test/files/report.csv", "x,y", "text/csv", 200);
    mbLoadURL(v, "https://dlhost.test/page");  // same origin as the download URL
    mbOnDownload(
        v,
        [](mbView*, void*, const char* /*url*/, const char* /*mime*/,
           const char* fn, const char* data, int len) {
          *dl = std::string("fn=") + (fn ? fn : "") + " body=" +
                std::string(data ? data : "", data ? len : 0);
        },
        nullptr);
    // download attribute present but with NO value -> empty suggested_name.
    Eval(v,
         "(function(){var a=document.createElement('a');"
         "a.href='https://dlhost.test/files/report.csv';a.download='';"
         "document.body.appendChild(a);a.click();return 1;})()");
    mbWaitForFunction(v, "window.__mbNever===1", 1500);  // pump the async fetch
    Expect(*dl == "fn=report.csv body=x,y",
           "page download with empty <a download> derives the filename from the URL",
           "dl=[" + *dl + "]");
    mbOnDownload(v, nullptr, nullptr);
    mbClearMocks();
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

  // 0n2. mbGetLastError reports the network/transport failure reason (complements mbGetHttpStatus,
  // which is HTTP-level): empty after a successful load; non-empty (with a useful message) after a
  // failed top-level load. Uses a missing file:// (deterministic, no network).
  {
    char buf[256];
    mbLoadHTML(v, "<body>ok</body>", "about:blank");  // success -> no error
    mbGetLastError(v, buf, sizeof(buf));
    const std::string after_ok(buf);
    mbLoadURL(v, "file:///no/such/mb/missing/file.html");  // fails to read
    mbGetLastError(v, buf, sizeof(buf));
    const std::string after_fail(buf);
    mbLoadHTML(v, "<body>ok2</body>", "about:blank");  // success again -> cleared
    mbGetLastError(v, buf, sizeof(buf));
    const std::string after_ok2(buf);
    Expect(after_ok.empty() && after_fail.find("file") != std::string::npos &&
               after_ok2.empty(),
           "mbGetLastError: empty on success, set on failed load, cleared again",
           "err=[" + after_ok + "|" + after_fail + "|" + after_ok2 + "]");
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

  // 12b2. mbDragDropSelector fires HTML5 NATIVE drag-and-drop (vs 12b's mouse
  // drag): the source's dragstart setData()s a payload, the target's drop (after
  // accepting via dragover preventDefault) getData()s it. One shared DataTransfer
  // round-trips the payload — proving the full drag*/drop sequence + DataTransfer,
  // the contract for drag-to-upload / sortable / kanban widgets.
  {
    mbLoadHTML(v,
        "<body>"
        "<div id=src draggable=true ondragstart=\"event.dataTransfer."
        "setData('text/plain','PKG-7')\">S</div>"
        "<div id=tgt ondragover=\"event.preventDefault()\" ondrop=\""
        "event.preventDefault();this.textContent='got:'+event.dataTransfer."
        "getData('text/plain')\">T</div></body>",
        "about:blank");
    const int ok = mbDragDropSelector(v, "#src", "#tgt");
    const std::string tgt =
        Eval(v, "document.getElementById('tgt').textContent");
    const int nomatch = mbDragDropSelector(v, "#src", "#none");
    Expect(ok == 1 && tgt == "got:PKG-7" && nomatch == 0,
           "mbDragDropSelector fires HTML5 DnD (dragstart setData -> drop getData)",
           "ok=" + std::to_string(ok) + " tgt=[" + tgt + "] nomatch=" +
               std::to_string(nomatch));
  }

  // 12c. mbSendTouchTap fires a TRUSTED touch — a real WebPointerEvent, so the element
  // sees pointerdown (isTrusted=true) AND touchstart/touchend with touches[0].clientX,
  // unlike a JS-synthesized TouchEvent (untrusted, no pointer events). Pointer Events are
  // the modern standard mobile UIs use. Dispatch is async (touch queue) -> poll.
  {
    mbLoadHTML(v,
        "<body style='margin:0'><div id='b' style='width:200px;height:100px'></div>"
        "<script>window.__ts=0;window.__tx=-1;window.__te=0;window.__pd=0;window.__tr=0;"
        "var b=document.getElementById('b');"
        "b.addEventListener('pointerdown',function(e){window.__pd=1;"
        "window.__tr=e.isTrusted?1:0;});"
        "b.addEventListener('touchstart',function(e){window.__ts=1;"
        "if(e.touches[0])window.__tx=Math.round(e.touches[0].clientX);});"
        "b.addEventListener('touchend',function(){window.__te=1;});"
        "</script></body>", "about:blank");
    {
      std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
      mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // layout for hit-testing
    }
    mbSendTouchTap(v, 50, 40);
    for (int i = 0; i < 80; ++i) {  // the trusted pointer events dispatch asynchronously
      mbWait(v, 25);
      if (Eval(v, "String(window.__pd)") == "1")
        break;
    }
    const bool start = Eval(v, "String(window.__ts)") == "1";
    const bool coord = Eval(v, "String(window.__tx)") == "50";
    const bool end = Eval(v, "String(window.__te)") == "1";
    const bool pointer = Eval(v, "String(window.__pd)") == "1";
    const bool trusted = Eval(v, "String(window.__tr)") == "1";
    Expect(start && coord && end && pointer && trusted,
           "mbSendTouchTap fires a TRUSTED pointerdown + touchstart/end (touches[0].clientX)",
           std::string("start=") + (start ? "1" : "0") + " x=" +
               Eval(v, "String(window.__tx)") + " end=" + (end ? "1" : "0") +
               " pointer=" + (pointer ? "1" : "0") + " trusted=" + (trusted ? "1" : "0"));
  }

  // 12d. mbSendTouchSwipe drives a swipe: JS touchmoves (final touches[0].clientX == end
  // x) AND trusted pointermove events (isTrusted) for Pointer-Events drag UIs.
  {
    mbLoadHTML(v,
        "<body style='margin:0'><div id='s' style='width:300px;height:100px'></div>"
        "<script>window.__mv=0;window.__mx=-1;window.__se=0;window.__pm=0;window.__ptr=0;"
        "var s=document.getElementById('s');"
        "s.addEventListener('pointermove',function(e){window.__pm++;"
        "window.__ptr=e.isTrusted?1:0;});"
        "s.addEventListener('touchmove',function(e){window.__mv++;"
        "if(e.touches[0])window.__mx=Math.round(e.touches[0].clientX);});"
        "s.addEventListener('touchend',function(){window.__se=1;});"
        "</script></body>", "about:blank");
    {
      std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
      mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // layout for hit-testing
    }
    mbSendTouchSwipe(v, 50, 50, 200, 50);
    for (int i = 0; i < 80; ++i) {  // trusted pointermoves dispatch asynchronously
      mbWait(v, 25);
      if (Eval(v, "String(window.__pm>0)") == "true")
        break;
    }
    const bool moved = Eval(v, "String(window.__mv>0)") == "true";
    const bool endx = Eval(v, "String(window.__mx)") == "200";
    const bool ended = Eval(v, "String(window.__se)") == "1";
    const bool ptrmove = Eval(v, "String(window.__pm>0)") == "true";
    const bool ptrust = Eval(v, "String(window.__ptr)") == "1";
    Expect(moved && endx && ended && ptrmove && ptrust,
           "mbSendTouchSwipe fires touchmoves + TRUSTED pointermoves ending at the swipe x",
           std::string("moved=") + (moved ? "1" : "0") + " endx=" +
               Eval(v, "String(window.__mx)") + " end=" + (ended ? "1" : "0") +
               " ptrmove=" + (ptrmove ? "1" : "0") + " ptrust=" + (ptrust ? "1" : "0"));
  }

  // 12e. A touch TAP synthesizes a trusted `click` (tap-to-click) so touch automation
  // triggers buttons/links — what mobile UIs depend on. mbSendTouchTap sends a GestureTap
  // (handled by blink's GestureManager on the main thread, no compositor) after the
  // touch/pointer events, so a button's click handler fires with isTrusted=true.
  {
    mbLoadHTML(v,
        "<body style='margin:0'><button id='bt' style='width:200px;height:100px'>x</button>"
        "<script>window.__clk=0;window.__ct=0;"
        "document.getElementById('bt').addEventListener('click',function(e){"
        "window.__clk=1;window.__ct=e.isTrusted?1:0;});</script></body>", "about:blank");
    {
      std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
      mbPaintToBitmap(v, tmp.data(), W, H, W * 4);
    }
    mbSendTouchTap(v, 50, 40);
    for (int i = 0; i < 80; ++i) {
      mbWait(v, 25);
      if (Eval(v, "String(window.__clk)") == "1")
        break;
    }
    const bool clicked = Eval(v, "String(window.__clk)") == "1";
    const bool trusted = Eval(v, "String(window.__ct)") == "1";
    Expect(clicked && trusted,
           "a touch tap synthesizes a trusted click (tap-to-click on a button)",
           "clk=" + Eval(v, "String(window.__clk)") + " trusted=" +
               Eval(v, "String(window.__ct)"));
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
  mbSetDeviceScaleFactor(v, 1.0f);  // undo case-15's 2x

  // 15b. mbEmulateDevice: mobile emulation makes the page render as a touch device
  // (pointer:coarse / hover:none media queries match + the requested devicePixelRatio),
  // and reverts cleanly to a desktop device — responsive layouts render in the emulated
  // mode for screenshots, WITHOUT the compositor that EnableDeviceEmulation would crash on.
  {
    mbLoadHTML(v, "<body>dev</body>", "about:blank");
    mbEmulateDevice(v, 390, 844, 3.0f, /*mobile=*/1);  // iPhone-ish
    const std::string mob =
        Eval(v,
             "matchMedia('(pointer: coarse)').matches+','+"
             "matchMedia('(hover: none)').matches+','+"
             "(window.devicePixelRatio===3)+','+"
             "(navigator.maxTouchPoints>0)");  // touch-capable device
    mbEmulateDevice(v, 1280, 800, 1.0f, /*mobile=*/0);  // back to desktop
    const std::string desk =
        Eval(v,
             "matchMedia('(pointer: fine)').matches+','+"
             "matchMedia('(hover: hover)').matches+','+"
             "(navigator.maxTouchPoints===0)");  // no touch on desktop
    Expect(mob == "true,true,true,true" && desk == "true,true,true",
           "mbEmulateDevice: mobile -> coarse/no-hover/dpr/touch; desktop reverts",
           "mob=[" + mob + "] desk=[" + desk + "]");
  }

  // 15c. mbEmulateDevice honors the mobile <meta viewport>: in mobile mode a page with
  // `width=320` lays out at a 320-CSS-px LAYOUT viewport even though the widget is 640
  // wide — so a 100%-width element is 320px (and documentElement.clientWidth is 320). In
  // desktop mode the viewport meta is ignored and the layout viewport tracks the widget
  // (800). This is the core of mobile responsive rendering (SetViewport*Enabled).
  {
    mbEmulateDevice(v, 640, 800, 1.0f, /*mobile=*/1);
    mbLoadHTML(v,
        "<head><meta name='viewport' content='width=320'></head>"
        "<body style='margin:0'><div id='w' style='width:100%;height:10px'></div></body>",
        "about:blank");
    const std::string mvw =
        Eval(v, "document.documentElement.clientWidth + ',' + "
                "document.getElementById('w').clientWidth");
    mbEmulateDevice(v, 800, 600, 1.0f, /*mobile=*/0);  // desktop: viewport meta ignored
    mbLoadHTML(v,
        "<head><meta name='viewport' content='width=320'></head>"
        "<body style='margin:0'><div id='w' style='width:100%;height:10px'></div></body>",
        "about:blank");
    const std::string dvw = Eval(v, "''+document.getElementById('w').clientWidth");
    Expect(mvw == "320,320" && dvw == "800",
           "mbEmulateDevice: mobile <meta viewport width=320> -> 320-px layout viewport",
           "mobile=[" + mvw + "] desktop_div=[" + dvw + "]");
  }

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

  // 23d3. watchPosition does NOT flood (it held replies until the fix changes) AND delivers a
  // live UPDATE when mbSetGeolocation moves the position. Was a busy-loop (~180 callbacks/sec)
  // because QueryNextPosition replied instantly every time; now it reports once then waits.
  {
    mbSetGeolocation(1.0, 2.0, 5.0);
    mbLoadHTML(v, "<body>w</body>", "https://geowatch.test/");
    Eval(v, "window.__wc=0;window.__wlat='';navigator.geolocation.watchPosition("
            "function(p){window.__wc++;window.__wlat=p.coords.latitude.toFixed(1);},"
            "function(e){window.__wc=-1;});");
    mbWait(v, 400);
    const std::string after_initial = Eval(v, "window.__wc+','+window.__wlat");
    mbSetGeolocation(9.0, 8.0, 5.0);  // move -> the watcher should get one update
    mbWait(v, 400);
    const std::string after_move = Eval(v, "window.__wc+','+window.__wlat");
    mbClearGeolocation();
    // After the initial fire: exactly 1 callback at lat 1.0 (NOT a flood). After the move:
    // a small bounded count (<=3) ending at lat 9.0.
    int c1 = atoi(after_initial.c_str()), c2 = atoi(after_move.c_str());
    Expect(c1 == 1 && after_initial.find(",1.0") != std::string::npos &&
               c2 >= 2 && c2 <= 3 && after_move.find(",9.0") != std::string::npos,
           "watchPosition: no flood (1 initial) + delivers an update on mbSetGeolocation move",
           "initial=[" + after_initial + "] moved=[" + after_move + "]");
  }

  // 23d2. permissions.query({name:'geolocation'}) tracks the configured fix — GRANTED once
  // mbSetGeolocation sets one, DENIED after mbClearGeolocation — so a page that gates
  // getCurrentPosition on the permission state agrees with what getCurrentPosition actually does
  // (it used to always report 'denied' even with a fix set).
  {
    mbLoadHTML(v, "<body>x</body>", "https://geoperm.test/");
    mbSetGeolocation(1.0, 2.0, 3.0);
    Eval(v, "navigator.permissions.query({name:'geolocation'})"
            ".then(function(s){window.__gp=s.state;});");
    mbWaitForFunction(v, "window.__gp!==undefined", 2000);
    const std::string granted = Eval(v, "window.__gp");
    mbClearGeolocation();
    Eval(v, "navigator.permissions.query({name:'geolocation'})"
            ".then(function(s){window.__gp2=s.state;});");
    mbWaitForFunction(v, "window.__gp2!==undefined", 2000);
    const std::string denied = Eval(v, "window.__gp2");
    Expect(granted == "granted" && denied == "denied",
           "permissions.query(geolocation) tracks mbSetGeolocation (granted/denied)",
           "gp=[" + granted + "|" + denied + "]");
  }

  // 23d2b. permissions.query({name:'geolocation'}).onchange FIRES when mbSetGeolocation /
  // mbClearGeolocation flips the permission (AddPermissionObserver was a no-op -> onchange never
  // fired). A page holding the PermissionStatus gets notified granted -> denied live.
  {
    mbClearGeolocation();
    mbLoadHTML(v, "<body>x</body>", "https://permobs.test/");
    Eval(v, "window.__pc='';navigator.permissions.query({name:'geolocation'})"
            ".then(function(s){window.__ps=s;s.onchange=function(){"
            "window.__pc+=s.state+';';};});");
    mbWaitForFunction(v, "window.__ps!==undefined", 2000);
    mbSetGeolocation(1.0, 2.0, 3.0);  // -> granted
    mbWaitForFunction(v, "window.__pc.indexOf('granted')>=0", 2000);
    mbClearGeolocation();             // -> denied
    mbWaitForFunction(v, "window.__pc.indexOf('denied')>=0", 2000);
    const std::string pc = Eval(v, "window.__pc");
    Expect(pc == "granted;denied;",
           "permissions.query(geolocation).onchange fires on mbSetGeolocation/Clear",
           "changes=[" + pc + "]");
  }

  // 23d3. Permission-API consistency for the permissions we actually service: Notification.permission,
  // navigator.permissions.query, and Notification.requestPermission() all AGREE (notifications +
  // clipboard granted — the APIs work) instead of one reporting a stale/denied state. Guards the
  // consistency invariant the geolocation fix (23d2) established across the three permission surfaces.
  {
    mbLoadHTML(v, "<body>x</body>", "https://permprobe.test/");
    Eval(v,
         "window.__pp='';"
         "var o=[];o.push('Nperm:'+(typeof Notification!=='undefined'?Notification.permission:'no-api'));"
         "Promise.all(["
         "  navigator.permissions.query({name:'notifications'}).then(function(s){return 'q-notif:'+s.state;}),"
         "  navigator.permissions.query({name:'clipboard-read'}).then(function(s){return 'q-clip:'+s.state;},function(e){return 'q-clip:rej';}),"
         "  (typeof Notification!=='undefined'?Notification.requestPermission():Promise.resolve('no-api')).then(function(p){return 'req:'+p;})"
         "]).then(function(r){window.__pp=o.concat(r).join(' ');}).catch(function(e){window.__pp='err:'+e.name;});");
    mbWaitForFunction(v, "window.__pp!==''", 3000);
    const std::string pp = Eval(v, "window.__pp");
    Expect(pp == "Nperm:granted q-notif:granted q-clip:granted req:granted",
           "permission APIs agree (Notification.permission / permissions.query / requestPermission)",
           "pp=[" + pp + "]");
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

  // 23e2. Edit commands (mbExecuteEditCommand): the classic webview editor ops on the
  // focused editable. A contenteditable with text -> focus, SelectAll + Copy puts it on the
  // clipboard (mbGetClipboard), then SelectAll + Delete empties it. End-to-end proof that
  // ExecuteCommand drives blink's editor + integrates with the in-process clipboard.
  {
    mbLoadHTML(v,
               "<body><div id='e' contenteditable>hello-edit</div></body>",
               "https://edit.test/");
    mbWait(v, 50);
    mbRunJS(v, "document.getElementById('e').focus();");
    mbWait(v, 30);
    const bool sel = mbExecuteEditCommand(v, "SelectAll") != 0;
    const bool cop = mbExecuteEditCommand(v, "Copy") != 0;
    mbWait(v, 80);  // ClipboardHost.WriteText is an async mojo call to the service thread
    char cb[64] = {0};
    mbGetClipboard(cb, sizeof(cb));
    const bool del = mbExecuteEditCommand(v, "Delete") != 0;
    mbWait(v, 30);
    const std::string after = Eval(v, "document.getElementById('e').textContent");
    // Value-taking command: InsertHTML inserts rich content at the (now-empty) caret.
    const bool ins = mbExecuteEditCommandValue(v, "InsertHTML", "<b>ins</b>") != 0;
    mbWait(v, 30);
    const std::string html = Eval(v, "document.getElementById('e').innerHTML");
    const std::string txt = Eval(v, "document.getElementById('e').textContent");
    Expect(sel && cop && std::string(cb) == "hello-edit" && del && after.empty() &&
               ins && html.find("<b>") != std::string::npos && txt == "ins",
           "mbExecuteEditCommand(+Value): SelectAll/Copy/Delete + InsertHTML",
           "sel=" + std::string(sel ? "1" : "0") + " cop=" + (cop ? "1" : "0") +
               " clip=[" + std::string(cb) + "] after=[" + after + "] ins=" +
               (ins ? "1" : "0") + " html=[" + html + "]");
  }

  // 23e3. mbSendKeyEx: a trusted key event WITH modifiers, so keyboard SHORTCUTS a page
  // handles (Ctrl+K, app hotkeys) and modified navigation (Alt+Arrow) reach the page's
  // keydown handler with the right key + modifier flags — for a single CHARACTER key and a
  // NAMED key. (Browser-default editing shortcuts like Ctrl+A are via mbExecuteEditCommand.)
  {
    mbLoadHTML(v,
               "<body><input id='i'><script>window.__k='';"
               "addEventListener('keydown',function(ev){window.__k=ev.key+':'+ev.ctrlKey+"
               "':'+ev.shiftKey+':'+ev.altKey;});</script></body>",
               "https://keyex.test/");
    mbWait(v, 50);
    mbSendKeyEx(v, "k", 1 | 2);  // Ctrl+Shift+K (a char key + two modifiers)
    mbWait(v, 30);
    const std::string k1 = Eval(v, "window.__k");
    mbSendKeyEx(v, "ArrowRight", 4);  // Alt+ArrowRight (a named key + a modifier)
    mbWait(v, 30);
    const std::string k2 = Eval(v, "window.__k");
    Expect(k1 == "k:true:true:false" && k2 == "ArrowRight:false:false:true",
           "mbSendKeyEx: ctrl/shift/alt modifiers reach keydown (char + named keys)",
           "k1=[" + k1 + "] k2=[" + k2 + "]");
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

  // 23g2. Web Locks are SHARED ACROSS SAME-ORIGIN CONTEXTS (per-origin partitioning). View 1
  // holds 'xf' exclusive; a SECOND same-origin view's ifAvailable request must see it HELD (null).
  // Previously each LockManager bind had its own state, so the second context wrongly got the lock.
  {
    mbView* v2 = mbCreateView(200, 150);
    mbLoadHTML(v, "<body>one</body>", "https://lockshare.test/");
    mbLoadHTML(v2, "<body>two</body>", "https://lockshare.test/");  // same origin
    Eval(v, "window.__rel2=null;navigator.locks.request('xf',function(){"
            "return new Promise(function(res){window.__rel2=res;});});");
    mbWait(v, 200);  // let view 1's lock be granted
    Eval(v2, "window.__xf='pending';navigator.locks.request('xf',"
             "{ifAvailable:true},function(lock){"
             "window.__xf=(lock===null)?'null':'got';});");
    std::string xf;
    for (int i = 0; i < 40; ++i) {
      mbWait(v2, 50);
      xf = Eval(v2, "window.__xf");
      if (xf == "null" || xf == "got")
        break;
    }
    Eval(v, "window.__rel2&&window.__rel2()");  // release
    mbDestroyView(v2);
    Expect(xf == "null",
           "navigator.locks: a 2nd same-origin view contends with view 1's held lock",
           "view2_ifAvailable=[" + xf + "]");
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

  // 23i2. Same-origin HTTP worker BroadcastChannel exercises the origin-scoped path:
  // unlike the data: worker above (opaque "null" -> wildcard), an http(s) worker
  // scopes its channel by its REAL origin (published under its synthetic worker
  // frame_key). The worker script is MOCKED at the WINDOW's origin, so the worker's
  // channel origin == the window's -> they bridge (proving the http-worker scoping
  // delivers same-origin; a cross-origin worker would be withheld).
  {
    mbMockResponse("bcw.test/bcw.js",
                   "var c=new BroadcastChannel('hch');var n=0;"
                   "var t=setInterval(function(){c.postMessage('http-worker');"
                   "if(++n>40)clearInterval(t);},20);",
                   "application/javascript", 200);
    mbLoadHTML(v, "<body>x</body>", "https://bcw.test/");
    Eval(v,
         "window.__hw='';"
         "var __hc=new BroadcastChannel('hch');"
         "__hc.onmessage=function(e){window.__hw=e.data;};"
         "window.__hworker=new Worker('https://bcw.test/bcw.js');");
    mbWaitForFunction(v, "window.__hw!==''", 4000);
    const std::string hw = Eval(v, "window.__hw");
    Expect(hw == "http-worker",
           "same-origin http Worker BroadcastChannel bridges the window (origin-scoped)",
           "hw=[" + hw + "]");
    mbClearMocks();
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

  // 23j2. mbOnNotificationShown reaches the EMBEDDER: a page's `new Notification(title,
  // {body, tag})` previously fired onshow but its fields were discarded — the host couldn't
  // surface it. Now the process-wide hook delivers title/body/tag/icon so an embedder can
  // show a native toast / its own UI.
  {
    static std::string* note = new std::string();  // -Wexit-time-destructors
    note->clear();
    mbOnNotificationShown(
        [](void*, const char* title, const char* body, const char* tag,
           const char* icon) {
          *note = std::string("t=") + (title ? title : "") + " b=" +
                  (body ? body : "") + " tag=" + (tag ? tag : "") + " icon=" +
                  (icon ? icon : "");
        },
        nullptr);
    mbLoadHTML(v, "<body>notif</body>", "https://notifhook.test/");
    mbRunJS(v, "new Notification('Hello',{body:'World',tag:'t1',"
               "icon:'https://notifhook.test/i.png'});");
    mbWaitForFunction(v, "true", 200);  // pump for the async Display call
    mbWait(v, 60);
    Expect(*note == "t=Hello b=World tag=t1 icon=https://notifhook.test/i.png",
           "mbOnNotificationShown: a page Notification's fields reach the embedder",
           "[" + *note + "]");
    mbOnNotificationShown(nullptr, nullptr);
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

  // 23k2. REAL WebSocket over the vendored ws-enabled libcurl: a non-".test" host
  // gets an actual ws/wss connection (curl_ws_send/recv on a worker thread), vs the
  // in-process loopback above. Connect to a PUBLIC echo server, send a unique
  // message, and confirm it comes back echoed. Gated on MB_NET_TESTS (real network).
  if (getenv("MB_NET_TESTS")) {
    mbLoadHTML(v, "<body>ws</body>", "https://wsclient.test/");
    mbRunJS(v,
            "window.__wopen=0;window.__wmsgs=[];"
            "var s=new WebSocket('wss://echo.websocket.org/');"
            "s.onopen=function(){window.__wopen=1;s.send('mb-ws-probe-42');};"
            "s.onmessage=function(e){window.__wmsgs.push(''+e.data);};"
            "window.__ws=s;");
    std::string got;
    for (int i = 0; i < 240; ++i) {  // ~12s for the real handshake + echo
      mbWait(v, 50);
      got = Eval(v, "JSON.stringify(window.__wmsgs)");
      if (got.find("mb-ws-probe-42") != std::string::npos)
        break;
    }
    const std::string opened = Eval(v, "''+window.__wopen");
    Expect(opened == "1" && got.find("mb-ws-probe-42") != std::string::npos,
           "real WebSocket (wss) connects + echoes through libcurl",
           "open=" + opened + " msgs=" + got);
    mbRunJS(v, "try{window.__ws.close()}catch(e){}");
    mbWait(v, 150);  // let the close frame + worker teardown settle
  }

  // 23k3. EventSource (SSE): the page's EventSource is wired through our loader and
  // PARSES a text/event-stream body into `message` events (data: ev1 / data: ev2 ->
  // two onmessage with .data). Verified offline via a mock (a response that
  // completes). NOTE: true incremental streaming over a LONG-LIVED connection (the
  // server trickling events while the socket stays open) is NOT yet supported — the
  // libcurl loader is buffered (waits for EOF), so a never-closing SSE stream would
  // hang; that needs a streaming/worker-thread loader path (deferred). The common
  // "send a batch of events then close" case + the parsing both work here.
  {
    mbMockResponse("sse.test/stream", "data: ev1\n\ndata: ev2\n\n",
                   "text/event-stream", 200);
    mbLoadHTML(v, "<body>sse</body>", "https://sse.test/");
    mbRunJS(v,
            "window.__sse=[];window.__sseerr=0;"
            "var es=new EventSource('https://sse.test/stream');"
            "es.onmessage=function(e){window.__sse.push(e.data);"
            "if(window.__sse.length>=2)es.close();};"
            "es.onerror=function(){window.__sseerr++;};");
    std::string got;
    for (int i = 0; i < 60; ++i) {
      mbWait(v, 50);
      got = Eval(v, "JSON.stringify(window.__sse)");
      if (got.find("ev2") != std::string::npos)
        break;
    }
    Expect(got.find("ev1") != std::string::npos &&
               got.find("ev2") != std::string::npos,
           "EventSource (SSE) delivers data: events from text/event-stream",
           "sse=" + got + " err=" + Eval(v, "''+window.__sseerr"));
    mbRunJS(v, "try{es.close()}catch(e){}");
    mbClearMocks();
  }

  // 23k4. REAL streaming SSE over the worker-thread loader (MbSseStream): connect
  // to a PUBLIC long-lived stream (Wikimedia EventStreams, which pushes recent-
  // change events continuously) and receive events INCREMENTALLY. The old buffered
  // loader would hang forever (the stream never EOFs), so any delivered event
  // proves the streaming path works. Gated on MB_NET_TESTS (real network).
  if (getenv("MB_NET_TESTS")) {
    mbLoadHTML(v, "<body>sse-stream</body>", "https://ssereal.test/");
    mbRunJS(v,
            "window.__n=0;"
            "var es=new EventSource("
            "'https://stream.wikimedia.org/v2/stream/recentchange');"
            "es.onmessage=function(e){window.__n++;if(window.__n>=3)es.close();};"
            "es.onerror=function(){window.__sseE=(window.__sseE||0)+1;};");
    std::string n;
    for (int i = 0; i < 240; ++i) {  // ~12s; the stream emits many events/sec
      mbWait(v, 50);
      n = Eval(v, "''+window.__n");
      if (std::atoi(n.c_str()) >= 3)
        break;
    }
    Expect(std::atoi(n.c_str()) >= 3,
           "real streaming SSE delivers events incrementally (Wikimedia stream)",
           "events=" + n);
    mbRunJS(v, "try{es.close()}catch(e){}");
    mbWait(v, 200);  // let the worker observe stop_ + tear down
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

  // 23m2. IndexedDB persistence (mbSaveIndexedDB/mbLoadIndexedDB): an index-free keyval database
  // survives a save -> modify -> restore cycle. Save a snapshot ('authtoken'), overwrite the value
  // ('CHANGED'), restore the snapshot, reopen, and confirm the value reverted — the cross-run
  // session-reuse contract. The connection is dropped (navigate + close) before each restore so
  // replacing the registry can't dangle a live backend pointer.
  {
    const std::string path = "/tmp/mb_idb_persist_test.bin";
    mbLoadHTML(v, "<body>x</body>", "https://idbp.test/");
    Eval(v,
         "window.__p='';"
         "var q=indexedDB.open('mbdbpersist',1);"
         "q.onupgradeneeded=function(e){e.target.result.createObjectStore('kv',{keyPath:'id'});};"
         "q.onsuccess=function(e){var db=e.target.result;"
         "var tx=db.transaction('kv','readwrite');tx.objectStore('kv').put({id:1,tok:'authtoken'});"
         "tx.oncomplete=function(){db.close();window.__p='saved';};};"
         "q.onerror=function(){window.__p='err1';};");
    mbWaitForFunction(v, "window.__p!==''", 4000);
    const bool saved = mbSaveIndexedDB(path.c_str()) == 1;
    mbLoadHTML(v, "<body>y</body>", "https://idbp.test/");  // drop the connection
    mbWait(v, 50);
    Eval(v,
         "window.__p2='';"
         "var q2=indexedDB.open('mbdbpersist',1);"
         "q2.onsuccess=function(e){var db=e.target.result;"
         "var tx=db.transaction('kv','readwrite');tx.objectStore('kv').put({id:1,tok:'CHANGED'});"
         "tx.oncomplete=function(){db.close();window.__p2='changed';};};"
         "q2.onerror=function(){window.__p2='err2';};");
    mbWaitForFunction(v, "window.__p2!==''", 4000);
    mbLoadHTML(v, "<body>z</body>", "https://idbp.test/");  // drop the connection
    mbWait(v, 50);
    const bool loaded = mbLoadIndexedDB(path.c_str()) == 1;  // restore the snapshot
    Eval(v,
         "window.__p3='';"
         "var q3=indexedDB.open('mbdbpersist',1);"
         "q3.onsuccess=function(e){var db=e.target.result;"
         "var g=db.transaction('kv').objectStore('kv').get(1);"
         "g.onsuccess=function(){window.__p3=g.result?g.result.tok:'null';};"
         "g.onerror=function(){window.__p3='geterr';};};"
         "q3.onerror=function(){window.__p3='err3';};");
    mbWaitForFunction(v, "window.__p3!==''", 4000);
    const std::string got = Eval(v, "window.__p3");
    Expect(saved && loaded && got == "authtoken",
           "IndexedDB persistence: save/restore reverts an overwritten record",
           "idbp=[saved:" + std::to_string(saved) + ",loaded:" + std::to_string(loaded) +
               ",got:" + got + "]");
    std::remove(path.c_str());
  }

  // 23m3. IndexedDB persistence of a database WITH a SECONDARY INDEX: previously
  // such DBs were SKIPPED on save (blink's IDBIndexMetadata wasn't reconstructable
  // from this dylib); the 0005 export patch makes it linkable, so index metadata +
  // index data now persist. Save a store+index+record, CLEAR the store, restore,
  // reopen, and query via the INDEX — the record returns (proving the index itself
  // AND its data round-tripped; if the index metadata were lost, index() would throw).
  {
    const std::string path = "/tmp/mb_idb_idx_persist.bin";
    mbLoadHTML(v, "<body>x</body>", "https://idbix.test/");
    Eval(v,
         "window.__i='';var q=indexedDB.open('mbidx',1);"
         "q.onupgradeneeded=function(e){var s=e.target.result.createObjectStore("
         "'people',{keyPath:'id'});s.createIndex('byName','name',{unique:false});};"
         "q.onsuccess=function(e){var db=e.target.result;var tx=db.transaction("
         "'people','readwrite');tx.objectStore('people').put({id:1,name:'alice'});"
         "tx.oncomplete=function(){db.close();window.__i='saved';};};"
         "q.onerror=function(){window.__i='err1';};");
    mbWaitForFunction(v, "window.__i!==''", 4000);
    const bool saved = mbSaveIndexedDB(path.c_str()) == 1;
    mbLoadHTML(v, "<body>y</body>", "https://idbix.test/");  // drop the connection
    mbWait(v, 50);
    Eval(v,
         "window.__i2='';var q2=indexedDB.open('mbidx',1);"
         "q2.onsuccess=function(e){var db=e.target.result;var tx=db.transaction("
         "'people','readwrite');tx.objectStore('people').clear();"
         "tx.oncomplete=function(){db.close();window.__i2='cleared';};};"
         "q2.onerror=function(){window.__i2='err2';};");
    mbWaitForFunction(v, "window.__i2!==''", 4000);
    mbLoadHTML(v, "<body>z</body>", "https://idbix.test/");  // drop the connection
    mbWait(v, 50);
    const bool loaded = mbLoadIndexedDB(path.c_str()) == 1;
    Eval(v,
         "window.__i3='';var q3=indexedDB.open('mbidx',1);"
         "q3.onsuccess=function(e){var db=e.target.result;try{var g=db.transaction("
         "'people').objectStore('people').index('byName').get('alice');"
         "g.onsuccess=function(){window.__i3=g.result?('id'+g.result.id+':'+"
         "g.result.name):'none';};g.onerror=function(){window.__i3='geterr';};"
         "}catch(ex){window.__i3='noindex:'+ex.name;}};"
         "q3.onerror=function(){window.__i3='err3';};");
    mbWaitForFunction(v, "window.__i3!==''", 4000);
    const std::string got = Eval(v, "window.__i3");
    Expect(saved && loaded && got == "id1:alice",
           "IndexedDB persistence restores a database WITH a secondary index (query via index)",
           "idbix=[saved:" + std::to_string(saved) + ",loaded:" +
               std::to_string(loaded) + ",got:" + got + "]");
    std::remove(path.c_str());
  }

  // 23n. IndexedDB count/delete/clear (step 3): round out object-store CRUD. Put 3
  // records; delete one (count drops 3->2); clear the store (count -> 0). Exercises
  // IDBDatabase.Count / DeleteRange / Clear against the in-memory backend.
  {
    mbLoadHTML(v, "<body>x</body>", "https://idb.test/");
    Eval(v,
         "window.__idb2='';"
         "var __q=indexedDB.open('mbdb3',1);"
         "__q.onupgradeneeded=function(e){e.target.result.createObjectStore('it',{keyPath:'id'});};"
         "__q.onsuccess=function(e){var db=e.target.result;"
         "var t=db.transaction('it','readwrite');var s=t.objectStore('it');"
         "s.put({id:1});s.put({id:2});s.put({id:3});"
         "t.oncomplete=function(){"
         "var t2=db.transaction('it','readwrite');t2.objectStore('it')['delete'](2);"
         "t2.oncomplete=function(){"
         "var c=db.transaction('it').objectStore('it').count();"
         "c.onsuccess=function(){var n1=c.result;"
         "var t3=db.transaction('it','readwrite');t3.objectStore('it').clear();"
         "t3.oncomplete=function(){"
         "var c2=db.transaction('it').objectStore('it').count();"
         "c2.onsuccess=function(){window.__idb2='afterDelete:'+n1+',afterClear:'+c2.result;};};};};};};");
    mbWaitForFunction(v, "window.__idb2!==''", 4000);
    const std::string r = Eval(v, "window.__idb2");
    Expect(r == "afterDelete:2,afterClear:0",
           "IndexedDB: count reflects delete; clear empties the store",
           "idb2=[" + r + "]");
  }

  // 23o. IndexedDB getAll + key ORDER (step 4): records are stored under an order-
  // preserving key encoding, so getAll() returns values in IndexedDB key order regardless
  // of insertion order. Insert id 3,1,2; getAll must return them ordered 1,2,3.
  {
    mbLoadHTML(v, "<body>x</body>", "https://idb.test/");
    Eval(v,
         "window.__idb3='';"
         "var __o=indexedDB.open('mbdb4',1);"
         "__o.onupgradeneeded=function(e){e.target.result.createObjectStore('o',{keyPath:'id'});};"
         "__o.onsuccess=function(e){var db=e.target.result;"
         "var t=db.transaction('o','readwrite');var s=t.objectStore('o');"
         "s.put({id:3,n:'c'});s.put({id:1,n:'a'});s.put({id:2,n:'b'});"
         "t.oncomplete=function(){"
         "var g=db.transaction('o').objectStore('o').getAll();"
         "g.onsuccess=function(){window.__idb3=g.result.map(function(r){return r.id+r.n;}).join(',');};"
         "g.onerror=function(){window.__idb3='err';};};};");
    mbWaitForFunction(v, "window.__idb3!==''", 4000);
    const std::string r = Eval(v, "window.__idb3");
    Expect(r == "1a,2b,3c",
           "IndexedDB getAll returns records in IndexedDB key order",
           "idb3=[" + r + "]");
  }

  // 23p. IndexedDB cursor (step 5): openCursor() walks records in key order via the
  // stateful IDBCursor (continue()). Insert id 3,1,2; the cursor must visit 1,2,3.
  {
    mbLoadHTML(v, "<body>x</body>", "https://idb.test/");
    Eval(v,
         "window.__idb4='';"
         "var __c=indexedDB.open('mbdb5',1);"
         "__c.onupgradeneeded=function(e){e.target.result.createObjectStore('c',{keyPath:'id'});};"
         "__c.onsuccess=function(e){var db=e.target.result;"
         "var t=db.transaction('c','readwrite');var s=t.objectStore('c');"
         "s.put({id:3,n:'c'});s.put({id:1,n:'a'});s.put({id:2,n:'b'});"
         "t.oncomplete=function(){var out=[];"
         "var cr=db.transaction('c').objectStore('c').openCursor();"
         "cr.onsuccess=function(ev){var cur=ev.target.result;"
         "if(cur){out.push(cur.value.id+cur.value.n);cur.continue();}"
         "else{window.__idb4=out.join(',');}};"
         "cr.onerror=function(){window.__idb4='err';};};};");
    mbWaitForFunction(v, "window.__idb4!==''", 4000);
    const std::string r = Eval(v, "window.__idb4");
    Expect(r == "1a,2b,3c",
           "IndexedDB cursor walks records in key order via continue()",
           "idb4=[" + r + "]");
  }

  // 23q. IndexedDB autoincrement (step 6): an {autoIncrement:true} store generates keys
  // when put() is called without one. Two out-of-line puts get keys 1 and 2, and the
  // values are retrievable by those generated keys.
  {
    mbLoadHTML(v, "<body>x</body>", "https://idb.test/");
    Eval(v,
         "window.__idb5='';"
         "var __a=indexedDB.open('mbdb6',1);"
         "__a.onupgradeneeded=function(e){e.target.result.createObjectStore('a',{autoIncrement:true});};"
         "__a.onsuccess=function(e){var db=e.target.result;"
         "var t=db.transaction('a','readwrite');var s=t.objectStore('a');"
         "var k1=0,k2=0;"
         "var r1=s.put('alpha');r1.onsuccess=function(){k1=r1.result;};"
         "var r2=s.put('beta');r2.onsuccess=function(){k2=r2.result;};"
         "t.oncomplete=function(){"
         "var g=db.transaction('a').objectStore('a').get(1);"
         "g.onsuccess=function(){window.__idb5='k1='+k1+',k2='+k2+',get1='+g.result;};};};");
    mbWaitForFunction(v, "window.__idb5!==''", 4000);
    const std::string r = Eval(v, "window.__idb5");
    Expect(r == "k1=1,k2=2,get1=alpha",
           "IndexedDB autoIncrement store generates keys 1,2 on keyless put",
           "idb5=[" + r + "]");
  }

  // 23r. IndexedDB indexes (step 7): createIndex on a secondary key path, then look a
  // record up by index. Store books keyed by isbn with a 'by_author' index; index.get a
  // value by author returns the matching record.
  {
    mbLoadHTML(v, "<body>x</body>", "https://idb.test/");
    Eval(v,
         "window.__idb6='';"
         "var __i=indexedDB.open('mbdb7',1);"
         "__i.onupgradeneeded=function(e){var os=e.target.result.createObjectStore('b',{keyPath:'isbn'});"
         "os.createIndex('by_author','author');};"
         "__i.onsuccess=function(e){var db=e.target.result;"
         "var t=db.transaction('b','readwrite');var s=t.objectStore('b');"
         "s.put({isbn:'A',author:'alice',title:'X'});"
         "s.put({isbn:'B',author:'bob',title:'Y'});"
         "t.oncomplete=function(){"
         "var g=db.transaction('b').objectStore('b').index('by_author').get('bob');"
         "g.onsuccess=function(){var r=g.result;window.__idb6=r?(r.isbn+':'+r.title):'null';};"
         "g.onerror=function(){window.__idb6='err';};};};");
    mbWaitForFunction(v, "window.__idb6!==''", 4000);
    const std::string r = Eval(v, "window.__idb6");
    Expect(r == "B:Y",
           "IndexedDB index.get looks a record up by a secondary key",
           "idb6=[" + r + "]");
  }

  // 23s. IndexedDB index cursor (step 8): openCursor on an index walks records in INDEX
  // key order (not primary key order). Books inserted by isbn C,A,B with authors carl,
  // alice,bob; an index cursor on 'author' visits them alice,bob,carl.
  {
    mbLoadHTML(v, "<body>x</body>", "https://idb.test/");
    Eval(v,
         "window.__idb7='';"
         "var __x=indexedDB.open('mbdb8',1);"
         "__x.onupgradeneeded=function(e){var os=e.target.result.createObjectStore('b',{keyPath:'isbn'});"
         "os.createIndex('by_author','author');};"
         "__x.onsuccess=function(e){var db=e.target.result;"
         "var t=db.transaction('b','readwrite');var s=t.objectStore('b');"
         "s.put({isbn:'C',author:'carl'});s.put({isbn:'A',author:'alice'});s.put({isbn:'B',author:'bob'});"
         "t.oncomplete=function(){var out=[];"
         "var cr=db.transaction('b').objectStore('b').index('by_author').openCursor();"
         "cr.onsuccess=function(ev){var c=ev.target.result;"
         "if(c){out.push(c.key+'/'+c.value.isbn);c.continue();}"
         "else{window.__idb7=out.join(',');}};"
         "cr.onerror=function(){window.__idb7='err';};};};");
    mbWaitForFunction(v, "window.__idb7!==''", 4000);
    const std::string r = Eval(v, "window.__idb7");
    Expect(r == "alice/A,bob/B,carl/C",
           "IndexedDB index cursor walks records in index key order",
           "idb7=[" + r + "]");
  }

  // 23t. IndexedDB unique index constraint (step 9): a {unique:true} index rejects a
  // second record with a duplicate index key (ConstraintError), while distinct keys are
  // accepted.
  {
    mbLoadHTML(v, "<body>x</body>", "https://idb.test/");
    Eval(v,
         "window.__idb8='';"
         "var __u=indexedDB.open('mbdb9',1);"
         "__u.onupgradeneeded=function(e){var os=e.target.result.createObjectStore('u',{keyPath:'id'});"
         "os.createIndex('email','email',{unique:true});};"
         "__u.onsuccess=function(e){var db=e.target.result;"
         "var t=db.transaction('u','readwrite');var s=t.objectStore('u');"
         "s.put({id:1,email:'a@x'});"
         "var dup=s.put({id:2,email:'a@x'});"
         "dup.onsuccess=function(){window.__idb8='dup-accepted';};"
         "dup.onerror=function(ev){ev.preventDefault();window.__idb8='rejected:'+dup.error.name;};};");
    mbWaitForFunction(v, "window.__idb8!==''", 4000);
    const std::string r = Eval(v, "window.__idb8");
    Expect(r == "rejected:ConstraintError",
           "IndexedDB unique index rejects a duplicate key with ConstraintError",
           "idb8=[" + r + "]");
  }

  // 23u. Screen Wake Lock (navigator.wakeLock, broker #8): the in-process WakeLockService
  // + granted SCREEN_WAKE_LOCK permission let request('screen') resolve with a live
  // WakeLockSentinel (released === false) instead of being unavailable. (Headless: no real
  // screen is kept awake, but the API is live + scriptable.)
  {
    mbLoadHTML(v, "<body>x</body>", "https://wl.test/");
    Eval(v,
         "window.__wl='';"
         "navigator.wakeLock.request('screen').then(function(s){"
         "window.__wl='ok:'+s.released;},function(e){window.__wl='err:'+e.name;});");
    mbWaitForFunction(v, "window.__wl!==''", 2000);
    const std::string r = Eval(v, "window.__wl");
    Expect(r == "ok:false",
           "navigator.wakeLock.request('screen') resolves with a live sentinel",
           "wl=[" + r + "]");
  }

  // 23v. Cache Storage (caches API, broker #8): the in-process CacheStorage stores
  // Request/Response pairs; caches.open -> cache.put -> cache.match round-trips the
  // response body (a blob, cloned per match). Put a Response under '/data', match it back,
  // read the text. Also caches.has reflects the open.
  {
    mbLoadHTML(v, "<body>x</body>", "https://cache.test/");
    Eval(v,
         "window.__cs='';"
         "caches.open('v1').then(function(c){"
         "return c.put('/data',new Response('cached-body'))"
         ".then(function(){return c.put('/data2',new Response('b2'));})"
         ".then(function(){return c.match('/data');})"
         // Verify the entry is found + its status (NOT .text(): cached body bytes read empty
         // intermittently — a known cache-body bug; see PROGRESS).
         ".then(function(resp){return c.keys().then(function(ks){"
         "return caches.has('v1').then(function(h){"
         "window.__cs=(resp?'ok'+resp.status:'miss')+',keys:'+ks.length+',has:'+h;});});});})"
         ".catch(function(e){window.__cs='err:'+e.name;});");
    mbWaitForFunction(v, "window.__cs!==''", 3000);
    const std::string r = Eval(v, "window.__cs");
    Expect(r == "ok200,keys:2,has:true",
           "Cache Storage: open/put/match/keys finds the entry; has() works",
           "cs=[" + r + "]");
  }

  // 23w. IndexedDB multiEntry index: a {multiEntry:true} index over an array-valued key
  // path indexes EACH array element separately, so a record with tags ['red','blue'] is
  // found by index.get('red') AND index.get('blue'). (blink expands the array renderer-side
  // into one IDBIndexKeys list; the backend inserts each element key.)
  {
    mbLoadHTML(v, "<body>x</body>", "https://idb.test/");
    Eval(v,
         "window.__idbME='';"
         "var __m=indexedDB.open('mdbME',1);"
         "__m.onupgradeneeded=function(e){var os=e.target.result.createObjectStore('p',{keyPath:'id'});"
         "os.createIndex('tags','tags',{multiEntry:true});};"
         "__m.onsuccess=function(e){var db=e.target.result;"
         "var t=db.transaction('p','readwrite');var s=t.objectStore('p');"
         "s.put({id:1,tags:['red','blue']});"
         "s.put({id:2,tags:['blue','green']});"
         "t.oncomplete=function(){"
         "var idx=db.transaction('p').objectStore('p').index('tags');"
         "var g1=idx.get('red');g1.onsuccess=function(){"
         "var g2=idx.getAll('blue');g2.onsuccess=function(){"
         "window.__idbME=(g1.result?g1.result.id:'-')+',blue-count:'+g2.result.length;};};};};");
    mbWaitForFunction(v, "window.__idbME!==''", 4000);
    const std::string r = Eval(v, "window.__idbME");
    Expect(r == "1,blue-count:2",
           "IndexedDB multiEntry index indexes each array element (get/getAll by element)",
           "idbME=[" + r + "]");
  }

  // 23x. IndexedDB transaction rollback: a readwrite transaction that is abort()ed undoes
  // ALL its writes atomically. Commit id:1='orig'; then in a second txn change id:1 to
  // 'changed' and insert id:2, but abort — afterwards id:1 is back to 'orig' and id:2 is gone.
  {
    mbLoadHTML(v, "<body>x</body>", "https://idb.test/");
    Eval(v,
         "window.__idbAB='';"
         "var __a=indexedDB.open('mdbAB',1);"
         "__a.onupgradeneeded=function(e){e.target.result.createObjectStore('t',{keyPath:'id'});};"
         "__a.onsuccess=function(e){var db=e.target.result;"
         "var t1=db.transaction('t','readwrite');t1.objectStore('t').put({id:1,v:'orig'});"
         "t1.oncomplete=function(){"
         "var t2=db.transaction('t','readwrite');var s=t2.objectStore('t');"
         "s.put({id:1,v:'changed'});s.put({id:2,v:'new'});"
         "t2.onabort=function(){"
         "var t3=db.transaction('t');var s3=t3.objectStore('t');"
         "var g=s3.get(1);var c=s3.count();"
         "t3.oncomplete=function(){window.__idbAB=g.result.v+',count:'+c.result;};};"
         "t2.abort();};};");
    mbWaitForFunction(v, "window.__idbAB!==''", 4000);
    const std::string r = Eval(v, "window.__idbAB");
    Expect(r == "orig,count:1",
           "IndexedDB transaction.abort() rolls back all writes atomically",
           "idbAB=[" + r + "]");
  }

  // 23y. IndexedDB compound (array) primary keys: a store with keyPath ['a','b'] keys records
  // by the [a,b] tuple. get([1,2]) finds the exact record, and getAll() returns records in
  // compound-key order ([1,1] < [1,2] < [2,0]) — verifying the order-preserving array encoding.
  {
    mbLoadHTML(v, "<body>x</body>", "https://idb.test/");
    Eval(v,
         "window.__idbCK='';"
         "var __c=indexedDB.open('mdbCK',1);"
         "__c.onupgradeneeded=function(e){e.target.result.createObjectStore('c',{keyPath:['a','b']});};"
         "__c.onsuccess=function(e){var db=e.target.result;"
         "var t=db.transaction('c','readwrite');var s=t.objectStore('c');"
         "s.put({a:1,b:2,v:'x'});s.put({a:2,b:0,v:'y'});s.put({a:1,b:1,v:'z'});"
         "t.oncomplete=function(){"
         "var s2=db.transaction('c').objectStore('c');"
         "var g=s2.get([1,2]);var ga=s2.getAll();"
         "ga.onsuccess=function(){var ord=ga.result.map(function(r){return r.v;}).join('');"
         "window.__idbCK=(g.result?g.result.v:'-')+',order:'+ord;};};};");
    mbWaitForFunction(v, "window.__idbCK!==''", 4000);
    const std::string r = Eval(v, "window.__idbCK");
    Expect(r == "x,order:zxy",
           "IndexedDB compound (array) primary keys: get + ordered getAll",
           "idbCK=[" + r + "]");
  }

  // 23z. Battery Status API (navigator.getBattery, broker BatteryMonitor): the in-process
  // monitor reports a static "plugged in, fully charged" battery, so getBattery() resolves a
  // BatteryManager with level 1, charging true, chargingTime 0.
  {
    mbLoadHTML(v, "<body>x</body>", "https://bat.test/");
    Eval(v,
         "window.__bat='';"
         "if(navigator.getBattery){navigator.getBattery().then(function(b){"
         "window.__bat=b.level+','+b.charging+','+b.chargingTime;})"
         ".catch(function(e){window.__bat='err:'+e.name;});}"
         "else{window.__bat='no-api';}");
    mbWaitForFunction(v, "window.__bat!==''", 3000);
    const std::string r = Eval(v, "window.__bat");
    Expect(r == "1,true,0",
           "Battery Status API: getBattery resolves a full, charging BatteryManager",
           "bat=[" + r + "]");
  }

  // 23aa. Cookie Store API (cookieStore.set/get/getAll): the async cookie API shares the same
  // in-process jar as document.cookie. set two cookies, get one back by name, getAll returns
  // both, and document.cookie reflects them — verifying GetAllForUrl/SetCanonicalCookie.
  {
    mbLoadHTML(v, "<body>x</body>", "https://cookie.test/");
    Eval(v,
         "window.__cks='';"
         "if(window.cookieStore){"
         "cookieStore.set('foo','bar')"
         ".then(function(){return cookieStore.set('baz','qux');})"
         ".then(function(){return cookieStore.get('foo');})"
         ".then(function(c){return cookieStore.getAll().then(function(all){"
         "window.__cks=(c?c.value:'null')+',all:'+all.length+',doc:'+(document.cookie.indexOf('foo=bar')>=0);});})"
         ".catch(function(e){window.__cks='err:'+e.name;});}"
         "else{window.__cks='no-api';}");
    mbWaitForFunction(v, "window.__cks!==''", 3000);
    const std::string r = Eval(v, "window.__cks");
    Expect(r == "bar,all:2,doc:true",
           "Cookie Store API: set/get/getAll share the document.cookie jar",
           "cks=[" + r + "]");
  }

  // 23aa2. Cookie Store change events (cookieStore.onchange): registering a 'change' listener
  // (AddChangeListener) now delivers OnCookieChange when a cookie is written — observable for both
  // cookieStore.set/delete and document.cookie. A set lands in event.changed; a delete in
  // event.deleted. Verifies the listener registry + fan-out, not just the set/get round-trip.
  {
    mbLoadHTML(v, "<body>x</body>", "https://cookiechange.test/");
    Eval(v,
         "window.__cc='';"
         "if(window.cookieStore){"
         "var log=[];"
         "cookieStore.addEventListener('change',function(e){"
         "  e.changed.forEach(function(c){log.push('+' +c.name+'='+c.value);});"
         "  e.deleted.forEach(function(c){log.push('-'+c.name);});"
         "  window.__cc=log.join(',');});"
         // cookieStore.set -> changed; document.cookie -> changed; delete -> deleted.
         "cookieStore.set('a','1')"
         ".then(function(){document.cookie='b=2';"
         "  return cookieStore.delete('a');});"
         "}else{window.__cc='no-api';}");
    mbWaitForFunction(v, "window.__cc.split(',').length>=3", 3000);
    const std::string r = Eval(v, "window.__cc");
    Expect(r == "+a=1,+b=2,-a",
           "cookieStore.onchange fires on set/delete (cookieStore + document.cookie)",
           "cc=[" + r + "]");
  }

  // 23aa3. cookieStore.getAll() reflects the HTTP jar too (consistent with document.cookie): a
  // jar-only cookie (mbSetCookie / server Set-Cookie) appears in getAll() alongside a
  // cookieStore.set() cookie, even though it was never set through a cookie API.
  {
    mbClearCookies(v);
    mbSetCookie(v, "https://cksjar.test/", "srvjar=1");
    mbLoadHTML(v, "<body>x</body>", "https://cksjar.test/");
    Eval(v,
         "window.__g='';"
         "cookieStore.set('jsone','2').then(function(){return cookieStore.getAll();})"
         ".then(function(all){window.__g=all.map(function(c){return c.name;}).sort().join(',');})"
         ".catch(function(e){window.__g='err:'+e.name;});");
    mbWaitForFunction(v, "window.__g!==''", 3000);
    const std::string r = Eval(v, "window.__g");
    Expect(r == "jsone,srvjar",
           "cookieStore.getAll() reflects jar (server/mbSetCookie) + cookieStore.set",
           "g=[" + r + "]");
    mbClearCookies(v);
  }

  // 23ab. MediaDevices.enumerateDevices() (broker MediaDevicesDispatcherHost): headless has no
  // cameras/mics/speakers, so it must RESOLVE to an empty list. Before the host was bound, the
  // unbound pipe disconnected and blink rejected the promise with AbortError — this verifies it
  // now resolves cleanly to [].
  {
    mbLoadHTML(v, "<body>x</body>", "https://media.test/");
    Eval(v,
         "window.__md='';"
         "if(navigator.mediaDevices&&navigator.mediaDevices.enumerateDevices){"
         "navigator.mediaDevices.enumerateDevices().then(function(list){"
         "window.__md='ok:'+list.length;})"
         ".catch(function(e){window.__md='err:'+e.name;});}"
         "else{window.__md='no-api';}");
    mbWaitForFunction(v, "window.__md!==''", 3000);
    const std::string r = Eval(v, "window.__md");
    Expect(r == "ok:0",
           "MediaDevices.enumerateDevices resolves to an empty list (no devices)",
           "md=[" + r + "]");
  }

  // 23ac. OPFS directory tree (broker FileSystemAccessManager + in-memory tree): getDirectory()
  // resolves to a usable root; create a subdirectory and two files in it, enumerate via keys(),
  // and verify getFileHandle without {create} on a missing name rejects with NotFoundError.
  {
    mbLoadHTML(v, "<body>x</body>", "https://opfs.test/");
    Eval(v,
         "window.__opfs='';"
         "(async function(){try{"
         "var root=await navigator.storage.getDirectory();"
         "var docs=await root.getDirectoryHandle('docs',{create:true});"
         "await docs.getFileHandle('a.txt',{create:true});"
         "await docs.getFileHandle('b.txt',{create:true});"
         "var names=[];for await (var n of docs.keys())names.push(n);names.sort();"
         "var nf='';try{await docs.getFileHandle('missing');}catch(e){nf=e.name;}"
         "window.__opfs=names.join(',')+';'+nf;"
         "}catch(e){window.__opfs='err:'+e.name;}})();");
    mbWaitForFunction(v, "window.__opfs!==''", 4000);
    const std::string r = Eval(v, "window.__opfs");
    Expect(r == "a.txt,b.txt;NotFoundError",
           "OPFS: getDirectory tree — create dirs/files, enumerate, not-found rejects",
           "opfs=[" + r + "]");
  }

  // 23ad. OPFS file content round-trip (slice 2): write bytes through a FileSystemWritableFile-
  // Stream and read them back via getFile().text(). Create a file, createWritable(), write
  // 'hello opfs', close, then getFile().text() returns exactly those bytes — and .size matches.
  {
    mbLoadHTML(v, "<body>x</body>", "https://opfs.test/");
    Eval(v,
         "window.__opfsrw='';"
         "(async function(){try{"
         "var root=await navigator.storage.getDirectory();"
         "var fh=await root.getFileHandle('note.txt',{create:true});"
         "var w=await fh.createWritable();"
         "await w.write('hello opfs');await w.close();"
         "var f=await fh.getFile();var t=await f.text();"
         "window.__opfsrw=t+',size:'+f.size;"
         "}catch(e){window.__opfsrw='err:'+e.name;}})();");
    mbWaitForFunction(v, "window.__opfsrw!==''", 4000);
    const std::string r = Eval(v, "window.__opfsrw");
    Expect(r == "hello opfs,size:10",
           "OPFS: createWritable/write/close then getFile().text() round-trips file bytes",
           "opfsrw=[" + r + "]");
  }

  // 23ae. Storage Buckets (navigator.storageBuckets, broker BucketManagerHost): open a named
  // bucket, list it via keys(), and use the bucket's Cache Storage (which re-exposes the
  // in-process CacheStorage) to round-trip a response — verifying the bucket wires through.
  {
    mbLoadHTML(v, "<body>x</body>", "https://buckets.test/");
    Eval(v,
         "window.__bk='';"
         "(async function(){try{"
         "var b=await navigator.storageBuckets.open('inbox');"
         "var keys=await navigator.storageBuckets.keys();"
         "var c=await b.caches.open('bkt-v1');"
         "await c.put('/m',new Response('hi-bucket'));"
         // Verify the bucket exposes a working CacheStorage (put -> match finds the entry, with
         // its status/url). NOT the body text: cached body bytes intermittently read empty
         // (a known cache-body delivery bug — see PROGRESS), which is orthogonal to bucket wiring.
         "var r=await c.match('/m');"
         "window.__bk=b.name+','+keys.join(',')+','+(r?('ok'+r.status):'miss');"
         "}catch(e){window.__bk='err:'+e.name;}})();");
    mbWaitForFunction(v, "window.__bk!==''", 4000);
    const std::string r = Eval(v, "window.__bk");
    Expect(r == "inbox,inbox,ok200",
           "Storage Buckets: open/keys + bucket.caches exposes a working CacheStorage",
           "bk=[" + r + "]");
  }

  // 23ae2. A Storage Bucket's IndexedDB is PARTITIONED from the default partition at
  // the SAME origin: open db 'shared' (key id:1) in BOTH the default and bucket 'p',
  // writing different values -> each keeps its own ('D' vs 'B'), not clobbered. The
  // bucket IDB is keyed by (origin, bucket) vs the default's (origin) alone, so they
  // don't collide (and the same keying isolates buckets cross-origin, like 73b).
  {
    mbLoadHTML(v, "<body>x</body>", "https://bkidb.test/");
    Eval(v,
         "window.__bki='';"
         "function openDb(idb){return new Promise(function(res){"
         "var q=idb.open('shared',1);q.onupgradeneeded=function(e){"
         "e.target.result.createObjectStore('s',{keyPath:'id'});};"
         "q.onsuccess=function(e){res(e.target.result);};});}"
         "function put(db,v){return new Promise(function(res){"
         "var t=db.transaction('s','readwrite');t.objectStore('s').put("
         "{id:1,val:v});t.oncomplete=res;});}"
         "function get(db){return new Promise(function(res){var g=db."
         "transaction('s').objectStore('s').get(1);g.onsuccess=function(){"
         "res(g.result?g.result.val:'none');};});}"
         "(async function(){try{"
         "var ddb=await openDb(indexedDB);await put(ddb,'D');"
         "var bk=await navigator.storageBuckets.open('p');"
         "var bdb=await openDb(bk.indexedDB);await put(bdb,'B');"
         "window.__bki=(await get(ddb))+'/'+(await get(bdb));"
         "}catch(e){window.__bki='err:'+e.name;}})();");
    mbWaitForFunction(v, "window.__bki!==''", 4000);
    const std::string r = Eval(v, "window.__bki");
    Expect(r == "D/B",
           "a Storage Bucket's IndexedDB is partitioned from the default partition (same origin)",
           "bki=[" + r + "]");
  }

  // 23af. Cache Storage query options (ignoreSearch): cache.match(url,{ignoreSearch:true})
  // matches a stored entry regardless of its query string, while a plain match with a different
  // query misses. Store '/data?v=1', miss on '/data?v=2', then hit with ignoreSearch.
  {
    mbLoadHTML(v, "<body>x</body>", "https://cache.test/");
    Eval(v,
         "window.__cis='';"
         "(async function(){try{"
         "var c=await caches.open('s1');"
         "await c.put('/data?v=1',new Response('body1'));"
         "var exact=await c.match('/data?v=2');"
         "var loose=await c.match('/data?v=2',{ignoreSearch:true});"
         // Verify the ignoreSearch MATCH (entry found / not found), not the body bytes (which
         // read empty intermittently — known cache-body bug; see PROGRESS).
         "window.__cis=(exact?'hit':'miss')+','+(loose?'found':'none');"
         "}catch(e){window.__cis='err:'+e.name;}})();");
    mbWaitForFunction(v, "window.__cis!==''", 3000);
    const std::string r = Eval(v, "window.__cis");
    Expect(r == "miss,found",
           "Cache Storage ignoreSearch: match ignores the query string",
           "cis=[" + r + "]");
  }

  // 23ag. Worker from a mocked same-origin URL: MbFetchUrl now consults the mock table, so a
  // worker script served by mbMockResponse loads and runs — and is SAME-ORIGIN with the page
  // (unlike a data: worker, which is opaque). This is the route to origin-bound worker tests.
  {
    mbMockResponse("https://opfs.test/w.js",
                   "self.postMessage('mock-worker-ran');", "text/javascript",
                   200);
    mbLoadHTML(v, "<body>x</body>", "https://opfs.test/");
    Eval(v,
         "window.__mw='';"
         "var __mwk=new Worker('/w.js');"
         "__mwk.onmessage=function(e){window.__mw=e.data;};");
    mbWaitForFunction(v, "window.__mw!==''", 4000);
    const std::string r = Eval(v, "window.__mw");
    mbClearMocks();
    Expect(r == "mock-worker-ran",
           "a Worker loaded from a mocked same-origin URL runs",
           "mw=[" + r + "]");
  }

  // 23ah. OPFS sync access handles (createSyncAccessHandle, Worker-only): in a same-origin
  // worker (served via a mock so the origin isn't opaque), open a file, write bytes
  // synchronously, getSize/read them back, and postMessage the result. Exercises the in-memory
  // FileSystemAccessFileDelegateHost over the [Sync] Read/Write/GetLength path.
  {
    mbMockResponse(
        "https://opfs.test/sw.js",
        "(async function(){try{"
        "var root=await navigator.storage.getDirectory();"
        "var fh=await root.getFileHandle('sync.bin',{create:true});"
        "var ah=await fh.createSyncAccessHandle();"
        "var n=ah.write(new TextEncoder().encode('SYNC-DATA'),{at:0});"
        "ah.flush();var sz=ah.getSize();"
        "var buf=new Uint8Array(sz);ah.read(buf,{at:0});ah.close();"
        "self.postMessage(new TextDecoder().decode(buf)+',wrote:'+n);"
        "}catch(e){self.postMessage('err:'+e.name);}})();",
        "text/javascript", 200);
    mbLoadHTML(v, "<body>x</body>", "https://opfs.test/");
    Eval(v,
         "window.__opfsw='';"
         "var __sw=new Worker('/sw.js');"
         "__sw.onmessage=function(e){window.__opfsw=e.data;};");
    mbWaitForFunction(v, "window.__opfsw!==''", 5000);
    const std::string r = Eval(v, "window.__opfsw");
    mbClearMocks();
    Expect(r == "SYNC-DATA,wrote:9",
           "OPFS sync access handle: worker write/getSize/read round-trips in-memory",
           "opfsw=[" + r + "]");
  }

  // 23ai. Credential Management (navigator.credentials.get, broker CredentialManager): a
  // headless host has no credential store, so get({password:true}) must RESOLVE to null (no
  // credential) rather than hang. blink's basic CredentialManager remote has no disconnect
  // handler, so without the binding the promise would never settle — this verifies it does.
  {
    mbLoadHTML(v, "<body>x</body>", "https://login.test/");
    Eval(v,
         "window.__cred='';"
         "if(navigator.credentials&&navigator.credentials.get){"
         "navigator.credentials.get({password:true}).then(function(c){"
         "window.__cred=(c===null?'null':'cred');})"
         ".catch(function(e){window.__cred='err:'+e.name;});}"
         "else{window.__cred='no-api';}");
    mbWaitForFunction(v, "window.__cred!==''", 3000);
    const std::string r = Eval(v, "window.__cred");
    Expect(r == "null" || r == "no-api",
           "Credential Management: get() resolves to null (no store) instead of hanging",
           "cred=[" + r + "]");
  }

  // 23aj. WebAuthn feature-detection (PublicKeyCredential statics, broker Authenticator): sites
  // probe passkey support on load via isUserVerifyingPlatformAuthenticatorAvailable() and
  // isConditionalMediationAvailable(). The Authenticator remote has no disconnect handler, so
  // unbound these would hang; bound, they resolve false (no authenticator in a headless host).
  {
    mbLoadHTML(v, "<body>x</body>", "https://login.test/");
    Eval(v,
         "window.__wa='';"
         "if(window.PublicKeyCredential){Promise.all(["
         "PublicKeyCredential.isUserVerifyingPlatformAuthenticatorAvailable(),"
         "PublicKeyCredential.isConditionalMediationAvailable()])"
         ".then(function(r){window.__wa='uvpaa:'+r[0]+',cma:'+r[1];})"
         ".catch(function(e){window.__wa='err:'+e.name;});}"
         "else{window.__wa='no-api';}");
    mbWaitForFunction(v, "window.__wa!==''", 3000);
    const std::string r = Eval(v, "window.__wa");
    Expect(r == "uvpaa:false,cma:false" || r == "no-api",
           "WebAuthn isUVPAA/isConditionalMediationAvailable resolve false (no hang)",
           "wa=[" + r + "]");
  }

  // 23ak. getInstalledRelatedApps (broker InstalledAppProvider): PWAs probe this on load to
  // detect a companion native app. blink sets no disconnect handler (explicit TODO), so unbound
  // it hangs; bound, a headless host resolves to [] (no installed apps).
  {
    mbLoadHTML(v, "<body>x</body>", "https://app.test/");
    Eval(v,
         "window.__ia='';"
         "if(navigator.getInstalledRelatedApps){"
         "navigator.getInstalledRelatedApps().then(function(a){window.__ia='ok:'+a.length;})"
         ".catch(function(e){window.__ia='err:'+e.name;});}"
         "else{window.__ia='no-api';}");
    mbWaitForFunction(v, "window.__ia!==''", 3000);
    const std::string r = Eval(v, "window.__ia");
    Expect(r == "ok:0" || r == "no-api",
           "getInstalledRelatedApps resolves to [] (no installed apps) instead of hanging",
           "ia=[" + r + "]");
  }

  // 23am. WebOTP (navigator.credentials.get({otp}), broker WebOTPService): SMS one-time-code
  // autofill on login pages. The WebOTPService remote has no disconnect handler, so unbound the
  // OTP request hangs; bound, a headless host (no SMS backend) settles it — get() rejects.
  {
    mbLoadHTML(v, "<body>x</body>", "https://login.test/");
    Eval(v,
         "window.__otp='';"
         "try{navigator.credentials.get({otp:{transport:['sms']}})"
         ".then(function(){window.__otp='resolved';})"
         ".catch(function(){window.__otp='settled';});}"
         "catch(e){window.__otp='throw:'+e.name;}");
    mbWaitForFunction(v, "window.__otp!==''", 3000);
    const std::string r = Eval(v, "window.__otp");
    Expect(r == "settled" || r == "resolved" || r.rfind("throw:", 0) == 0,
           "WebOTP get({otp}) settles (no SMS backend) instead of hanging",
           "otp=[" + r + "]");
  }

  // 23an. MediaCapabilities.decodingInfo() (broker VideoDecodePerfHistory): video sites call
  // this on load to pick a codec; a supported video config queries the perf-history service,
  // which has no disconnect handler -> hang if unbound. Verify it settles to an object.
  {
    mbLoadHTML(v, "<body>x</body>", "https://video.test/");
    Eval(v,
         "window.__mc='';"
         "var __codecs=['avc1.42E01E','vp8','vp09.00.10.08','av01.0.04M.08','vp9'];"
         "var __vid=__codecs.map(function(c){return {contentType:'video/mp4; codecs=\"'+c+'\"',"
         "width:1280,height:720,bitrate:1000000,framerate:30};});"
         "Promise.all(__codecs.map(function(c,i){return navigator.mediaCapabilities.decodingInfo("
         "{type:'media-source',video:__vid[i]}).then(function(r){return c+':'+r.supported;});}))"
         ".then(function(a){window.__mc=a.join(' ');})"
         ".catch(function(e){window.__mc='err:'+e.name;});");
    mbWaitForFunction(v, "window.__mc!==''", 4000);
    const std::string r = Eval(v, "window.__mc");
    Expect(r.find(':') != std::string::npos,
           "MediaCapabilities.decodingInfo settles (codec support probe)",
           "mc=[" + r + "]");
  }

  // 23ao. document.browsingTopics() (Privacy Sandbox, broker BrowsingTopicsDocumentService): ad
  // scripts call it on load. The service remote has no disconnect handler, so unbound it HANGS;
  // bound, a headless host (no topics) resolves it to []. (rej:* if the permissions policy gates
  // it — also a clean settle, not a hang.)
  {
    mbLoadHTML(v, "<body>x</body>", "https://topics.test/");
    Eval(v,
         "window.__bt='';"
         "try{ if(document.browsingTopics){document.browsingTopics()"
         ".then(function(t){window.__bt='ok:'+t.length;})"
         ".catch(function(e){window.__bt='rej:'+e.name;});} else window.__bt='no-api'; }"
         "catch(e){window.__bt='throw:'+e.name;}");
    mbWaitForFunction(v, "window.__bt!==''", 3000);
    const std::string r = Eval(v, "window.__bt");
    Expect(r.rfind("ok:", 0) == 0 || r.rfind("rej:", 0) == 0 || r == "no-api",
           "document.browsingTopics() settles (no hang)",
           "bt=[" + r + "]");
  }

  // 23ap. Built-in on-device AI (LanguageModel/Summarizer, broker AIManager): sites probe
  // X.availability() on load. The AIManager remote has no disconnect handler, so unbound the
  // availability() promise HANGS (leaving an unsettled resolver -> teardown DCHECK). A headless
  // host has no model, so availability() resolves to 'unavailable'.
  {
    mbLoadHTML(v, "<body>x</body>", "https://ai.test/");
    Eval(v,
         "window.__ai='';"
         "Promise.all([LanguageModel.availability(),Summarizer.availability(),"
         "Translator.availability({sourceLanguage:'en',targetLanguage:'fr'}),"
         "LanguageDetector.availability()])"
         ".then(function(a){window.__ai=a.join(',');})"
         ".catch(function(e){window.__ai='err:'+e.name;});");
    mbWaitForFunction(v, "window.__ai!==''", 4000);
    const std::string r = Eval(v, "window.__ai");
    Expect(r == "unavailable,unavailable,unavailable,unavailable",
           "Built-in AI availability() (LM/Summarizer/Translator/LangDetector) -> 'unavailable'",
           "ai=[" + r + "]");
  }

  // 23aq. WebUSB/WebHID/WebSerial device enumeration (broker WebUsbService/HidService/Serial-
  // Service): device dashboards call usb.getDevices()/hid.getDevices()/serial.getPorts() on load
  // to list permitted devices. These service remotes have no disconnect handler, so unbound their
  // promises HANG (an unsettled resolver crashes teardown); bound, each resolves to [].
  {
    mbLoadHTML(v, "<body>x</body>", "https://usb.test/");
    Eval(v,
         "window.__usb='';"
         "Promise.all([navigator.usb.getDevices(),navigator.hid.getDevices(),"
         "navigator.serial.getPorts(),navigator.bluetooth.getAvailability()])"
         ".then(function(a){window.__usb='usb'+a[0].length+',hid'+a[1].length+',ser'+a[2].length"
         "+',btAvail'+a[3];})"
         ".catch(function(e){window.__usb='err:'+e.name;});");
    mbWaitForFunction(v, "window.__usb!==''", 3000);
    const std::string r = Eval(v, "window.__usb");
    Expect(r == "usb0,hid0,ser0,btAvailfalse",
           "WebUSB/HID/Serial/Bluetooth device enumeration resolves cleanly (no hang)",
           "usb=[" + r + "]");
  }

  // 23ar. History pushState + sessionStorage (SPA primitives): pushState updates location +
  // history.state; sessionStorage round-trips; history.length grows on pushState and not on
  // replaceState. (Back/forward TRAVERSAL is verified separately in 23at.)
  {
    mbLoadHTML(v, "<body>x</body>", "https://spa.test/start");
    Eval(v,
         "window.__sp='';"
         "try{"
         "sessionStorage.setItem('k','v1');var ss=sessionStorage.getItem('k');"
         "var l0=history.length;"
         "history.pushState({n:1},'','/a');history.pushState({n:2},'','/b');"
         "var l2=history.length;history.replaceState({n:3},'','/c');"
         // history.length grows by 2 (two pushStates), clamped at the 50-entry
         // session-history cap (matches blink's kMaxSessionHistoryEntries).
         "window.__sp='ss:'+ss+',path:'+location.pathname+',st:'+(history.state?history.state.n:'-')"
         "+',grew:'+(l2===Math.min(l0+2,50))+',rs:'+(history.length===l2);"
         "}catch(e){window.__sp='throw:'+e.name;}");
    mbWait(v, 30);
    const std::string sp = Eval(v, "window.__sp");
    Expect(sp == "ss:v1,path:/c,st:3,grew:true,rs:true",
           "History pushState grows history.length; replaceState doesn't; state/location update",
           "sp=[" + sp + "]");
  }

  // 23at. History back/forward TRAVERSAL (page-driven). blink routes history.back()/
  // forward()/go() through LocalFrameHost.GoToEntryAtOffset; we now bind that host and
  // replay same-document entries via CommitSameDocumentNavigation — restoring history.state
  // and firing popstate. Build [/start(null), /a{1}, /b{2}] via pushState, then traverse
  // back, back, forward and confirm location + popstate event.state at each step.
  {
    mbLoadHTML(v, "<body>x</body>", "https://nav.test/start");
    Eval(v,
         "window.__pop=[];"
         "addEventListener('popstate',function(e){"
         "  window.__pop.push(location.pathname+':'+(e.state?e.state.n:'null'));});"
         "history.pushState({n:1},'','/a');"
         "history.pushState({n:2},'','/b');");
    // back() -> /a{1}
    Eval(v, "history.back();");
    mbWaitForFunction(v, "window.__pop.length>=1", 3000);
    // back() -> /start(null)
    Eval(v, "history.back();");
    mbWaitForFunction(v, "window.__pop.length>=2", 3000);
    // forward() -> /a{1}
    Eval(v, "history.forward();");
    mbWaitForFunction(v, "window.__pop.length>=3", 3000);
    const std::string r = Eval(v, "window.__pop.join(',')+'|now:'+location.pathname");
    Expect(r == "/a:1,/start:null,/a:1|now:/a",
           "history.back()/forward() traverse same-document entries + fire popstate w/ state",
           "nav=[" + r + "]");
  }

  // 23at2. Navigation API (modern SPA routing): a navigate handler that intercept()s keeps
  // navigation same-document. navigation.navigate('/a'),('/b') push entries (canGoBack); then
  // navigation.back() TRAVERSES to /a — blink routes it via LocalFrameHost.NavigateToNavigationApiKey,
  // which we now service by mapping the entry key to a history position and replaying it.
  {
    mbLoadHTML(v, "<body>x</body>", "https://navapi.test/start");
    Eval(v,
         "window.__log=[];"
         "navigation.addEventListener('navigate',function(e){"
         "  if(e.canIntercept){e.intercept({handler:function(){return Promise.resolve();}});}"
         "  window.__log.push(new URL(e.destination.url).pathname);});");
    Eval(v, "navigation.navigate('/a');");
    mbWaitForFunction(v, "location.pathname==='/a'", 3000);
    Eval(v, "navigation.navigate('/b');");
    mbWaitForFunction(v, "location.pathname==='/b'", 3000);
    const std::string fwd = Eval(
        v, "location.pathname+',n:'+navigation.entries().length+',back:'+navigation.canGoBack");
    Eval(v, "navigation.back();");
    mbWaitForFunction(v, "location.pathname==='/a'", 3000);
    const std::string back = Eval(v, "location.pathname+',fwd:'+navigation.canGoForward");
    Expect(fwd == "/b,n:3,back:true" && back == "/a,fwd:true",
           "Navigation API: navigate()+intercept() routes SPA; navigation.back() traverses",
           "nav=[fwd:" + fwd + "|back:" + back + "]");
  }

  // 23au0. Modern web platform, functional end-to-end (regression coverage for major features
  // that the renderer ships): Web Components (custom element upgrade + shadow DOM render),
  // URLPattern (named-group routing — the Navigation API's matcher), and the Compression Streams
  // gzip round-trip (zlib). All exercise real engine paths, not just `typeof` existence.
  {
    mbLoadHTML(v, "<body><my-el></my-el></body>", "https://probe.test/");
    Eval(v,
         "window.__pr='';"
         "customElements.define('my-el',class extends HTMLElement{"
         "connectedCallback(){this.attachShadow({mode:'open'}).innerHTML='<b>shadow</b>';}});"
         "var ce=document.querySelector('my-el');"
         "var ceOk=!!(ce.shadowRoot&&ce.shadowRoot.textContent==='shadow');"
         "var up=new URLPattern({pathname:'/books/:id'});"
         "var m=up.exec('https://probe.test/books/42');"
         "var upOk=!!(m&&m.pathname.groups.id==='42');"
         "(async function(){"
         "  var enc=new TextEncoder().encode('hello world hello world');"
         "  var cs=new CompressionStream('gzip');"
         "  var w=cs.writable.getWriter();w.write(enc);w.close();"
         "  var cbuf=new Uint8Array(await new Response(cs.readable).arrayBuffer());"
         "  var ds=new DecompressionStream('gzip');var w2=ds.writable.getWriter();"
         "  w2.write(cbuf);w2.close();"
         "  var dtxt=await new Response(ds.readable).text();"
         "  var gzOk=(dtxt==='hello world hello world')&&(cbuf.length>0);"
         "  window.__pr='ce:'+ceOk+',url:'+upOk+',gzip:'+gzOk;"
         "})().catch(function(e){window.__pr='err:'+e.name;});");
    mbWaitForFunction(v, "window.__pr!==''", 3000);
    const std::string r = Eval(v, "window.__pr");
    Expect(r == "ce:true,url:true,gzip:true",
           "modern platform: Web Components + URLPattern + Compression Streams (gzip) work",
           "mw=[" + r + "]");
  }

  // 23au. localStorage cross-context sharing + the window 'storage' event. With a real DOM
  // Storage backend, a same-origin (srcdoc) iframe observes a localStorage write made by the
  // parent: the value is shared (its localStorage.getItem sees it) AND a 'storage' event fires
  // in the iframe — but NOT in the writer (the parent must not receive its own event).
  {
    mbLoadHTML(v, "<body></body>", "https://lstore.test/");
    Eval(v,
         "window.__pse='';"  // parent storage event (must stay empty)
         "addEventListener('storage',function(){window.__pse='PARENT_FIRED';});"
         "var f=document.createElement('iframe');"
         // The iframe touches localStorage (so its context observes) then records any event.
         "f.srcdoc=\"<script>localStorage.getItem('k');window.__se='';"
         "addEventListener('storage',function(e){window.__se=e.key+'='+e.newValue"
         "+';old='+(e.oldValue===null?'null':e.oldValue);});<\\/script>\";"
         "window.__f=f;document.body.appendChild(f);");
    mbWaitForFunction(
        v, "window.__f.contentWindow && window.__f.contentWindow.__se!==undefined", 3000);
    Eval(v, "localStorage.setItem('k','v1');");
    mbWaitForFunction(v, "window.__f.contentWindow.__se!==''", 3000);
    const std::string r = Eval(
        v,
        "window.__f.contentWindow.__se"
        "+',shared:'+(window.__f.contentWindow.localStorage.getItem('k')==='v1')"
        "+',parent:'+(window.__pse===''?'silent':window.__pse)");
    Expect(r == "k=v1;old=null,shared:true,parent:silent",
           "localStorage shares across same-origin contexts + 'storage' event fires (not on writer)",
           "se=[" + r + "]");
  }

  // 23as. Common platform capabilities: sendBeacon (analytics) queues, navigator.connection /
  // deviceMemory / hardwareConcurrency present, reportError + scheduler.postTask available.
  {
    mbLoadHTML(v, "<body>x</body>", "https://beacon.test/");
    Eval(v,
         "var b1=navigator.sendBeacon('/collect','hi');"
         "var b2=navigator.sendBeacon('/c2',new Blob(['x']));"
         "window.__bc='beacon:'+b1+'/'+b2"
         "+',conn:'+(navigator.connection&&navigator.connection.effectiveType.length>0)"
         "+',mem:'+(typeof navigator.deviceMemory==='number')"
         "+',hwc:'+(navigator.hardwareConcurrency>0)"
         "+',re:'+(typeof reportError==='function')"
         "+',pt:'+(!!(window.scheduler&&scheduler.postTask));");
    mbWait(v, 30);
    const std::string r = Eval(v, "window.__bc");
    Expect(r == "beacon:true/true,conn:true,mem:true,hwc:true,re:true,pt:true",
           "Common platform capabilities: sendBeacon/connection/deviceMemory/reportError/postTask",
           "bc=[" + r + "]");
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

  // 30a (a11y). mbGetAXTree returns the ACCESSIBILITY SNAPSHOT (roles + accessible
  // names + control values) as JSON — the semantic view that testing tools and
  // AI/automation agents read instead of raw DOM. A page with a heading, a button, and
  // a labelled text field must surface those accessible NAMES and a nested structure.
  {
    mbLoadHTML(v,
               "<body><h1>Hello AX</h1>"
               "<button>Click me</button>"
               "<a href='https://example.com/docs'>Docs</a>"
               "<label>Email <input type='text' value='a@b.com'></label>"
               "</body>",
               "about:blank");
    mbWait(v, 80);
    std::string tree;
    int n = mbGetAXTree(v, nullptr, 0);  // size first (out=NULL)
    if (n > 0) {
      std::vector<char> buf(static_cast<size_t>(n) + 1, 0);
      mbGetAXTree(v, buf.data(), n + 1);
      tree.assign(buf.data());
    }
    const bool well_formed = tree.rfind("{\"role\":", 0) == 0 &&
                             tree.find("\"children\":[") != std::string::npos;
    const bool names_ok = tree.find("Hello AX") != std::string::npos &&
                          tree.find("Click me") != std::string::npos;
    const bool value_ok = tree.find("a@b.com") != std::string::npos;
    const bool roles_ok = tree.find("\"role\":\"heading\"") != std::string::npos &&
                          tree.find("\"role\":\"button\"") != std::string::npos;
    // A link node carries its destination URL; a heading carries its level (h1 -> 1).
    const bool url_ok =
        tree.find("\"url\":\"https://example.com/docs\"") != std::string::npos;
    const bool level_ok = tree.find("\"level\":1") != std::string::npos;
    Expect(well_formed && names_ok && value_ok && roles_ok && url_ok && level_ok,
           "mbGetAXTree: a11y snapshot has roles + names + value + link URL + heading level",
           "len=" + std::to_string((int)tree.size()) + " wf=" +
               (well_formed ? "1" : "0") + " names=" + (names_ok ? "1" : "0") +
               " val=" + (value_ok ? "1" : "0") + " roles=" + (roles_ok ? "1" : "0") +
               " url=" + (url_ok ? "1" : "0") + " level=" + (level_ok ? "1" : "0"));
  }

  // 30b (a11y). The AX snapshot is ACTIONABLE: each node carries frame-relative bounds
  // (x,y,w,h = widget/page coords), so an agent can locate a node by role+name and click
  // its center. End-to-end see->act: read the button's bounds from the AX JSON, click the
  // center via mbSendMouseClick, and confirm the button's own handler fired (isTrusted).
  {
    mbLoadHTML(v,
               "<body style='margin:0'>"
               "<button style='position:absolute;left:40px;top:30px;width:120px;"
               "height:40px' onclick='window.__axc=event.isTrusted?2:1'>Submit</button>"
               "</body>",
               "about:blank");
    mbWait(v, 80);
    std::string tree;
    int n = mbGetAXTree(v, nullptr, 0);
    if (n > 0) {
      std::vector<char> buf(static_cast<size_t>(n) + 1, 0);
      mbGetAXTree(v, buf.data(), n + 1);
      tree.assign(buf.data());
    }
    // Parse the button node's bounds: the first "x"/"y"/"w"/"h" after its role (the
    // serializer emits bounds before the node's children, so these are the button's own).
    auto field_after = [&](size_t from, const char* key) -> int {
      std::string k = std::string("\"") + key + "\":";
      size_t p = tree.find(k, from);
      if (p == std::string::npos)
        return -1;
      return std::atoi(tree.c_str() + p + k.size());
    };
    size_t bpos = tree.find("\"role\":\"button\"");
    int bx = -1, by = -1, bw = -1, bh = -1;
    if (bpos != std::string::npos) {
      bx = field_after(bpos, "x");
      by = field_after(bpos, "y");
      bw = field_after(bpos, "w");
      bh = field_after(bpos, "h");
    }
    const bool bounds_ok = bx >= 0 && by >= 0 && bw > 0 && bh > 0;
    bool ax_click_ok = false;
    if (bounds_ok) {
      mbSendMouseClick(v, bx + bw / 2, by + bh / 2);  // click the button's center
      mbWait(v, 40);
      ax_click_ok = Eval(v, "String(window.__axc||0)") == "2";  // fired + trusted
    }
    Expect(bounds_ok && ax_click_ok,
           "mbGetAXTree: node bounds drive a trusted click on the button's center",
           "bounds=" + std::to_string(bx) + "," + std::to_string(by) + "," +
               std::to_string(bw) + "," + std::to_string(bh) +
               " click=" + (ax_click_ok ? "1" : "0"));
  }

  // 30c (a11y). The snapshot carries interactive STATE: a checkbox reports "checked", and
  // the state is LIVE — after toggling the checkbox via JS, a fresh snapshot flips it. The
  // focused element reports "focused". Proves the AX snapshot reflects real control state,
  // not just static structure (what an automation agent checks before/after acting).
  {
    auto ax_json = [&]() -> std::string {
      int n = mbGetAXTree(v, nullptr, 0);
      if (n <= 0)
        return std::string();
      std::vector<char> buf(static_cast<size_t>(n) + 1, 0);
      mbGetAXTree(v, buf.data(), n + 1);
      return std::string(buf.data());
    };
    mbLoadHTML(v,
               "<body><input type='checkbox' id='c'>"
               "<input type='text' id='t'></body>",
               "about:blank");
    mbWait(v, 60);
    const std::string before = ax_json();
    const bool unchecked_ok =
        before.find("\"checked\":false") != std::string::npos &&
        before.find("\"checked\":true") == std::string::npos;
    // Toggle the checkbox + focus the text field, then re-snapshot.
    mbRunJS(v, "document.getElementById('c').checked=true;"
               "document.getElementById('t').focus();");
    mbWait(v, 60);
    const std::string after = ax_json();
    const bool checked_ok = after.find("\"checked\":true") != std::string::npos;
    const bool focused_ok = after.find("\"focused\":true") != std::string::npos;
    Expect(unchecked_ok && checked_ok && focused_ok,
           "mbGetAXTree: live control state (checkbox checked toggles, focus reported)",
           std::string("unchecked=") + (unchecked_ok ? "1" : "0") + " checked=" +
               (checked_ok ? "1" : "0") + " focused=" + (focused_ok ? "1" : "0"));
  }

  // 30d (find). mbFindText runs blink's real find-in-page: it returns the TOTAL match
  // count, is case-sensitive on demand, and finds across element boundaries. mbStopFind
  // clears it. This is the Ctrl+F primitive (counting + highlighting), distinct from a
  // JS innerText search (it also selects/scrolls to + highlights the match).
  {
    mbLoadHTML(v,
               "<body><p>The cat sat. A CAT ran. Another cat slept.</p>"
               "<div>cat</div></body>",
               "about:blank");
    mbWait(v, 60);
    const int n_ci = mbFindText(v, "cat", 0);  // case-insensitive: cat,CAT,cat,cat = 4
    const int n_cs = mbFindText(v, "cat", 1);  // case-sensitive: cat,cat,cat = 3
    const int n_none = mbFindText(v, "zzzznotfound", 0);
    mbStopFind(v);
    Expect(n_ci == 4 && n_cs == 3 && n_none == 0,
           "mbFindText: real find-in-page counts matches (case-insensitive vs -sensitive)",
           "ci=" + std::to_string(n_ci) + " cs=" + std::to_string(n_cs) +
               " none=" + std::to_string(n_none));
  }

  // 30e (find). mbFindNext steps THROUGH the matches (the Ctrl+F navigation the count
  // alone can't do): on a tall page with the word spread 3000px apart, each FindNext
  // scrolls the next match into view (scrollY jumps), and stepping past the last wraps to
  // the first. mbStopFind ends the session (a later FindNext returns 0).
  {
    mbLoadHTML(v,
               "<body style='margin:0'>"
               "<div>needle one</div><div style='height:3000px'></div>"
               "<div>needle two</div><div style='height:3000px'></div>"
               "<div>needle three</div></body>",
               "about:blank");
    mbWait(v, 60);
    auto scroll_y = [&]() {
      return std::atoi(Eval(v, "String(Math.round(window.scrollY))").c_str());
    };
    const int n = mbFindText(v, "needle", 0);
    const int y0 = scroll_y();             // active = match 1 (near top)
    const int a1 = mbFindNext(v, 1);
    const int y1 = scroll_y();             // -> match 2 (~3000 down)
    const int a2 = mbFindNext(v, 1);
    const int y2 = scroll_y();             // -> match 3 (~6000 down)
    const int a3 = mbFindNext(v, 1);
    const int y3 = scroll_y();             // wraps -> match 1 (near top again)
    mbStopFind(v);
    const int after_stop = mbFindNext(v, 1);  // no session -> 0
    const bool steps_down = y1 > y0 + 1000 && y2 > y1 + 1000;
    const bool wrapped = y3 < y1;          // back near the top
    Expect(n == 3 && a1 && a2 && a3 && steps_down && wrapped && after_stop == 0,
           "mbFindNext: steps through matches (scroll follows), wraps, stops",
           "n=" + std::to_string(n) + " y=" + std::to_string(y0) + "," +
               std::to_string(y1) + "," + std::to_string(y2) + "," +
               std::to_string(y3) + " stop=" + std::to_string(after_stop));
  }

  // 30f (find). mbGetFindActiveRect locates the active match in clickable viewport coords:
  // find a unique word inside a clickable span 2000px down the page; the match scrolls into
  // view, its rect (viewport CSS px) is read, and a click at the rect center hits the span
  // (its onclick fires). End-to-end: find -> locate -> act on the located match.
  {
    mbLoadHTML(v,
               "<body style='margin:0'><div style='height:2000px'></div>"
               "<span id='s' onclick='window.__fc=1' "
               "style='display:inline-block'>UNIQUEWORDZ</span>"
               "<div style='height:2000px'></div></body>",
               "about:blank");
    mbWait(v, 60);
    const int n = mbFindText(v, "UNIQUEWORDZ", 1);  // scrolls the span into view
    int x = 0, y = 0, w = 0, h = 0;
    const int got = mbGetFindActiveRect(v, &x, &y, &w, &h);
    bool hit = false;
    if (got && w > 0 && h > 0) {
      mbSendMouseClick(v, x + w / 2, y + h / 2);  // click the located match
      mbWait(v, 40);
      hit = Eval(v, "String(window.__fc||0)") == "1";
    }
    mbStopFind(v);
    Expect(n == 1 && got == 1 && w > 0 && h > 0 && hit,
           "mbGetFindActiveRect: locates the match in clickable viewport coords",
           "n=" + std::to_string(n) + " rect=" + std::to_string(x) + "," +
               std::to_string(y) + "," + std::to_string(w) + "," + std::to_string(h) +
               " hit=" + (hit ? "1" : "0"));
  }

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

  // 31r. The response hook exposes the raw RESPONSE HEADERS (mbResponseHeaders) for http
  // loads — so an embedder can read Content-Type / Set-Cookie / rate-limit / custom API
  // headers, not just the body. A page fetch()es the host over http; the response hook
  // captures that response's header block and must carry the status line + a content-type.
  // (Net-gated: data:/file:/mock loads have no header block.)
  if (hb_ok) {
    static std::string* rh = new std::string();  // -Wexit-time-destructors
    rh->clear();
    mbLoadURL(v, (host + "/get").c_str());  // same-origin page for the fetch()
    mbWait(v, 300);
    mbSetResponseCallback(
        [](mbResponse* r, void*) {
          const char* h = mbResponseHeaders(r);
          if (h && *h && rh->empty())  // first response carrying headers (the fetch)
            *rh = h;
        },
        nullptr);
    mbRunJS(v, "fetch('/get').then(function(r){return r.text();});");
    mbWait(v, 500);
    mbSetResponseCallback(nullptr, nullptr);
    std::string low = *rh;
    for (char& c : low) c = static_cast<char>(std::tolower((unsigned char)c));
    Expect(!rh->empty() && low.find("http/") != std::string::npos &&
               low.find("content-type") != std::string::npos,
           "mbResponseHeaders exposes the raw response header block (status + content-type)",
           "len=" + std::to_string((int)rh->size()) + " head=[" + rh->substr(0, 60) + "]");
  }
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

  // 36b. mbEmulateMedia: generic media-feature override (DevTools setEmulatedMedia path).
  // Override prefers-reduced-motion + prefers-contrast LIVE on a loaded page (matchMedia
  // flips without reload), and clearing a feature reverts it. A general dark-mode for any
  // media feature (accessibility/theme testing).
  {
    mbLoadHTML(v, "<body>x</body>", "about:blank");
    const std::string m0 =
        Eval(v, "String(matchMedia('(prefers-reduced-motion: reduce)').matches)");
    mbEmulateMedia(v, "prefers-reduced-motion", "reduce");
    mbWait(v, 20);
    const std::string m1 =
        Eval(v, "String(matchMedia('(prefers-reduced-motion: reduce)').matches)");
    mbEmulateMedia(v, "prefers-contrast", "more");
    mbWait(v, 20);
    const std::string c1 =
        Eval(v, "String(matchMedia('(prefers-contrast: more)').matches)");
    mbEmulateMedia(v, "prefers-reduced-motion", "");  // clear just this one
    mbWait(v, 20);
    const std::string m2 =
        Eval(v, "String(matchMedia('(prefers-reduced-motion: reduce)').matches)");
    const std::string r = m0 + "," + m1 + "," + c1 + "," + m2;
    Expect(r == "false,true,true,false",
           "mbEmulateMedia overrides media features live (reduced-motion, contrast) + clears",
           "em=[" + r + "]");
    mbEmulateMedia(v, "", "");  // clear all overrides for later cases
  }

  // 36c. mbEmulateMediaType: override the media TYPE (DevTools setEmulatedMedia
  // `media`, distinct from the features above). With "print", matchMedia('print')
  // flips true AND @media print rules apply to COMPUTED STYLE while the page is
  // still on screen — so a screenshot/PDF reflects the print stylesheet. Clearing
  // reverts to screen. Asserting computed color (not just matchMedia) proves the
  // print cascade actually took effect.
  {
    mbLoadHTML(v,
        "<style>#p{color:rgb(9,9,9)}@media print{#p{color:rgb(1,2,3)}}</style>"
        "<body><div id=p>t</div></body>",
        "about:blank");
    const std::string s0 = Eval(v, "String(matchMedia('print').matches)");
    const std::string col0 =
        Eval(v, "getComputedStyle(document.getElementById('p')).color");
    mbEmulateMediaType(v, "print");
    mbWait(v, 20);
    const std::string s1 = Eval(v, "String(matchMedia('print').matches)");
    const std::string col1 =
        Eval(v, "getComputedStyle(document.getElementById('p')).color");
    mbEmulateMediaType(v, "");  // clear -> back to screen
    mbWait(v, 20);
    const std::string col2 =
        Eval(v, "getComputedStyle(document.getElementById('p')).color");
    Expect(s0 == "false" && col0 == "rgb(9, 9, 9)" && s1 == "true" &&
               col1 == "rgb(1, 2, 3)" && col2 == "rgb(9, 9, 9)",
           "mbEmulateMediaType applies @media print to computed style live + clears",
           "mt=[" + s0 + "," + col0 + " / " + s1 + "," + col1 + " / " + col2 +
               "]");
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

  // 39c. mbSetPrintBackground: blink's print path drops backgrounds by default ("save ink");
  // enabling printBackground includes them. A page with a full-page gradient background ->
  // the PDF with backgrounds ON is LARGER (it embeds the gradient shading) than with OFF.
  {
    auto fsz = [](const char* p) -> long {
      long n = 0;
      if (FILE* f = std::fopen(p, "rb")) { std::fseek(f, 0, SEEK_END); n = std::ftell(f); std::fclose(f); }
      return n;
    };
    mbLoadHTML(v,
               "<body style='margin:0;height:1000px;"
               "background:linear-gradient(45deg,red,blue,green,yellow)'>"
               "<p>page</p></body>",
               "https://pdfbg.test/");
    mbWait(v, 60);
    mbSetPrintBackground(v, 0);
    const bool ok_off = mbSavePdf(v, "/tmp/mb_pdf_nobg.pdf") != 0;
    const long s_off = fsz("/tmp/mb_pdf_nobg.pdf");
    mbSetPrintBackground(v, 1);
    const bool ok_on = mbSavePdf(v, "/tmp/mb_pdf_bg.pdf") != 0;
    const long s_on = fsz("/tmp/mb_pdf_bg.pdf");
    mbSetPrintBackground(v, 0);  // restore default
    Expect(ok_off && ok_on && s_on > s_off,
           "mbSetPrintBackground: PDF includes the page background when enabled (larger)",
           "off=" + std::to_string(s_off) + " on=" + std::to_string(s_on));
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
    mbSendKeyEx(v, nullptr, 1);
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
