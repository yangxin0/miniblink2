// mb_smoke_render — split from the mb_smoke monolith: modern/cutting-edge CSS,
// web components, platform-API crash-safety (workers/IDB/WS/dialogs/clipboard/
// crypto/audio/streams), blob, paint correctness, fonts/SVG/transitions,
// selector automation, navigation, charset, reload, host-driven history.
#include "miniblink_host/test/mb_smoke_harness.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

using mbsmoke::Eval;
using mbsmoke::EvalIso;
using mbsmoke::Expect;

static void RunCases(mbView* v, int W, int H) {
  // 41z7. FORCE-COMPOSITED CSS layers render in a screenshot (mbPaintToBitmap). Same
  // root-cause class as the WebGL-canvas-blank bug (patch 0008: our non-compositing
  // software paint skips cc-composited layers); the existing screenshot tests cover only
  // WebGL/2D-canvas/video, NOT plain CSS compositing triggers. Each 40x40 div has a
  // distinct color and a different trigger (translateZ / will-change / translate3d /
  // filter / opacity); if a layer were skipped its pixel would be the white background.
  {
    mbLoadHTML(v,
      "<body style='margin:0;background:#fff'>"
      "<div style='position:absolute;left:10px;top:10px;width:40px;height:40px;background:#ff0000;transform:translateZ(0)'></div>"
      "<div style='position:absolute;left:70px;top:10px;width:40px;height:40px;background:#00ff00;will-change:transform'></div>"
      "<div style='position:absolute;left:130px;top:10px;width:40px;height:40px;background:#0000ff;transform:translate3d(0px,0px,0px)'></div>"
      "<div style='position:absolute;left:10px;top:70px;width:40px;height:40px;background:#ff1493;filter:blur(0.1px)'></div>"
      "<div style='position:absolute;left:70px;top:70px;width:40px;height:40px;background:#00ffff;opacity:0.99'></div>"
      "</body>", "https://comp.test/");
    mbWait(v, 250);
    std::vector<unsigned char> cv(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, cv.data(), W, H, W * 4);
    auto rgb = [&](int x, int y, int c) {
      return static_cast<int>(cv[(static_cast<size_t>(y) * W + x) * 4 + (2 - c)]);
    };
    const bool red = rgb(30, 30, 0) > 200 && rgb(30, 30, 1) < 70 && rgb(30, 30, 2) < 70;
    const bool grn = rgb(90, 30, 1) > 200 && rgb(90, 30, 0) < 70 && rgb(90, 30, 2) < 70;
    const bool blu = rgb(150, 30, 2) > 200 && rgb(150, 30, 0) < 70 && rgb(150, 30, 1) < 70;
    const bool pnk = rgb(30, 90, 0) > 200 && rgb(30, 90, 2) > 100 && rgb(30, 90, 1) < 90;
    const bool cyn = rgb(90, 90, 1) > 200 && rgb(90, 90, 2) > 200 && rgb(90, 90, 0) < 60;
    Expect(red && grn && blu && pnk && cyn,
           "force-composited CSS layers (translateZ/will-change/3d/filter/opacity) render "
           "in a screenshot",
           "red=" + std::to_string(red) + " grn=" + std::to_string(grn) + " blu=" +
               std::to_string(blu) + " pnk=" + std::to_string(pnk) + " cyn=" +
               std::to_string(cyn));
  }
  // 35z. ALL synthetic input modalities route INTO a sub-frame (iframe) and reach its
  // elements — instead of SIGSEGV. The bug this guards: on a mousedown a sub-frame takes
  // the press for, EventHandler::CaptureMouseEventsToWidget -> WebFrameWidgetImpl::
  // SetMouseCapture dereferenced a null widget_input_handler_manager() (no browser-side
  // input host in our single-process embedder); patches/0011 guards it (the same null check
  // SetPanAction et al. already use). Because that crash fired on ANY sub-frame-routed
  // mouse press, this exercises the whole input surface: click, mousemove, wheel, drag,
  // right-click(context menu), and keyboard text entry into a focused sub-frame <input>.
  // (Keyboard chars need mbSendKeyEx — mbSendKey is for NAMED keys like Enter/Tab.)
  {
    mbLoadHTML(v,
      "<body style='margin:0'>"
      "<iframe srcdoc=\"<body style='margin:0'>"
      "<input id='i' style='width:300px;height:30px'>"
      "<div id='d' style='width:300px;height:90px'>D</div>"
      "<script>window.__ev={};var d=document.getElementById('d');"
      "d.onclick=function(){window.__ev.ck=1;};"
      "d.onmousemove=function(){window.__ev.mm=1;};"
      "d.oncontextmenu=function(e){window.__ev.cm=1;e.preventDefault();};"
      "d.onwheel=function(){window.__ev.wh=1;};"
      "</script></body>\" style='border:0;width:300px;height:120px'></iframe>"
      "</body>", "https://frameinput.test/");
    mbWait(v, 300);
    mbSendMouseMove(v, 100, 80); mbSendMouseMove(v, 120, 85);   // hover the child div
    mbSendWheel(v, 120, 80, 0, -40, 0);                          // wheel over it
    mbSendMouseDown(v, 60, 80); mbSendMouseMove(v, 100, 80);     // drag across it
    mbSendMouseMove(v, 140, 85); mbSendMouseUp(v, 140, 85);
    mbSendMouseClickEx(v, 120, 80, 2 /*right*/, 0);              // context menu
    mbSendMouseClick(v, 120, 80);                                // left click the div
    mbSendMouseClick(v, 100, 15); mbWait(v, 60);                 // focus the child <input>
    mbSendKeyEx(v, "A", 0); mbWait(v, 60);                       // type into it
    char buf[200] = {0};
    mbEvalJSInFrame(v, 0,
      "JSON.stringify({ev:window.__ev||{},val:document.getElementById('i').value})",
      buf, sizeof(buf));
    const std::string st(buf);
    const bool ok = st.find("\"ck\":1") != std::string::npos &&
                    st.find("\"mm\":1") != std::string::npos &&
                    st.find("\"wh\":1") != std::string::npos &&
                    st.find("\"cm\":1") != std::string::npos &&
                    st.find("\"val\":\"A\"") != std::string::npos;
    Expect(ok,
           "all synthetic input (click/move/wheel/drag/contextmenu/keyboard) routes into "
           "a sub-frame (no crash)",
           "child=[" + st + "]");
  }

  // 35z2. Trusted TOUCH into a sub-frame: mbSendTouchTap routes a WebPointerEvent(kTouch) into an
  // iframe and fires its touchstart + pointerdown (pointerType 'touch', isTrusted) without crashing
  // — the touch peer of 35z (touch routing goes through the same sub-frame hit-test/capture path).
  {
    mbLoadHTML(v,
      "<body style='margin:0'>"
      "<iframe srcdoc=\"<body style='margin:0'>"
      "<div id='d' style='width:300px;height:120px'>D</div>"
      "<script>window.__t={};var d=document.getElementById('d');"
      "d.addEventListener('touchstart',function(e){window.__t.ts=e.isTrusted?1:0;});"
      "d.addEventListener('pointerdown',function(e){"
      "window.__t.pd=(e.pointerType==='touch'&&e.isTrusted)?1:0;});"
      "</script></body>\" style='border:0;width:300px;height:120px'></iframe>"
      "</body>", "https://frametouch.test/");
    mbWait(v, 300);
    mbSendTouchTap(v, 120, 60);  // tap the iframe's div
    mbWait(v, 200);
    char tbuf[200] = {0};
    mbEvalJSInFrame(v, 0, "JSON.stringify(window.__t||{})", tbuf, sizeof(tbuf));
    const std::string ts(tbuf);
    Expect(ts.find("\"pd\":1") != std::string::npos ||
               ts.find("\"ts\":1") != std::string::npos,
           "trusted touch (mbSendTouchTap) routes into a sub-frame (no crash)",
           "child=[" + ts + "]");
  }

  // 35. Cutting-edge modern CSS — the M150-vs-M47 selling points, none of which the
  // frozen ~2015 engine could do. Each rule colors an element only if the feature works.
  mbLoadHTML(v,
    "<style>"
    ".p:has(.kid){color:rgb(1,1,1)} "                    // :has() selector
    ".n{& .inner{color:rgb(3,3,3)}} "                    // native CSS nesting
    "@container (min-width:1px){.c{color:rgb(4,4,4)}} "  // container query
    ".mix{color:color-mix(in srgb,#000,#fff)} "          // color-mix()
    "</style>"
    "<div class='p'><span class='kid'>x</span><b id='has'>x</b></div>"
    "<div class='n'><b class='inner' id='nest'>x</b></div>"
    "<div style='container-type:inline-size'><b class='c' id='cont'>x</b></div>"
    "<b class='mix' id='mix'>x</b>",
    "about:blank");
  Expect(Eval(v, "getComputedStyle(document.getElementById('has')).color") == "rgb(1, 1, 1)" &&
             Eval(v, "getComputedStyle(document.getElementById('nest')).color") == "rgb(3, 3, 3)" &&
             Eval(v, "getComputedStyle(document.getElementById('cont')).color") == "rgb(4, 4, 4)" &&
             Eval(v, "getComputedStyle(document.getElementById('mix')).color")
                     .find("0.5") != std::string::npos,
         "modern CSS: :has(), nesting, @container, color-mix()");

  // 36. Web Components (Custom Elements v1 + Shadow DOM) — a major modern-platform
  // feature (M47 had only the v0 prototype). Define a custom element (its
  // connectedCallback upgrades it), and attach an encapsulated shadow tree.
  mbLoadHTML(v, "<body><div id='host'></div></body>", "about:blank");
  mbRunJS(v,
    "customElements.define('my-el',class extends HTMLElement{"
    "  connectedCallback(){this.textContent='upgraded';}});"
    "document.body.appendChild(document.createElement('my-el'));"
    "var sr=document.getElementById('host').attachShadow({mode:'open'});"
    "sr.innerHTML='<span id=s>shadow</span>';");
  mbWait(v, 40);
  Expect(Eval(v, "document.querySelector('my-el').textContent") == "upgraded" &&
             Eval(v, "document.getElementById('host').shadowRoot.querySelector('#s')"
                     ".textContent") == "shadow" &&
             Eval(v, "String(document.querySelector('#s'))") == "null",  // encapsulated
         "Web Components: custom element upgrade + shadow DOM encapsulation");

  // 37. Worker spawn must not crash the host. A page that does `new Worker(...)` must
  // not SIGSEGV (it once did: factory_client_ was null and DedicatedWorker::Start
  // derefs it). The guard: construct a Worker, pump, and confirm the host is still
  // alive and scripting after. (37b below proves the worker actually RUNS now; this
  // case is the narrower crash-safety invariant and also covers the throw path.)
  mbLoadHTML(v, "<body>worker-guard</body>", "about:blank");
  mbRunJS(v,
    "try{window.__w=new Worker('data:text/javascript,'+"
    "encodeURIComponent('onmessage=function(e){postMessage(e.data*2)}'));"
    "window.__w.postMessage(21);window.__wok=true;}"
    "catch(e){window.__wok=false;window.__werr=String(e);}");
  mbWait(v, 60);
  // Host survived the worker spawn (we got here at all) and JS still runs:
  Expect(Eval(v, "1+1") == "2" &&
             Eval(v, "document.body.textContent") == "worker-guard" &&
             (Eval(v, "String(typeof window.__w)") == "object" ||
              Eval(v, "String(window.__wok)") == "false"),
         "Worker spawn degrades gracefully (no crash; host still scriptable)");

  // 37b. A dedicated Worker actually RUNS (worker bring-up Step 2): the worker thread
  // executes its script, receives a message, and posts a reply the page observes. This
  // exercises the in-process worker host (worker/mb_dedicated_worker_host.cc) — the
  // OnWorkerHostCreated + OnScriptLoadStarted handshake streams the script over a data
  // pipe, the worker thread compiles+runs it, and postMessage round-trips both ways. A
  // reply of 42 (= 21*2) proves the worker is LIVE, not the old inert stub.
  mbLoadHTML(v, "<body>worker-run</body>", "about:blank");
  mbRunJS(v,
    "window.__reply='';"
    "window.__w2=new Worker('data:text/javascript,'+"
    "encodeURIComponent('onmessage=function(e){postMessage(e.data*2)}'));"
    "window.__w2.onmessage=function(e){window.__reply=String(e.data)};"
    "window.__w2.postMessage(21);");
  {
    // Poll for the async round-trip (thread start + script compile + two messages).
    std::string reply;
    for (int i = 0; i < 80; ++i) {
      mbWait(v, 25);
      reply = Eval(v, "window.__reply");
      if (!reply.empty())
        break;
    }
    Expect(reply == "42",
           "a dedicated Worker runs its script and postMessage round-trips (Step 2)",
           "reply=[" + reply + "]");
  }

  // 37c. Multiple concurrent Workers each run independently (no shared-state clobber,
  // no thread cross-talk). Spawn three workers, each doubling its input; the page sums
  // the replies. 1*2 + 2*2 + 3*2 = 12 across 3 replies proves they ran concurrently
  // and each delivered its own result.
  mbLoadHTML(v, "<body>worker-multi</body>", "about:blank");
  mbRunJS(v,
    "window.__sum=0;window.__cnt=0;"
    "function __mk(x){var w=new Worker('data:text/javascript,'+"
    "encodeURIComponent('onmessage=function(e){postMessage(e.data*2)}'));"
    "w.onmessage=function(e){window.__sum+=e.data;window.__cnt++};w.postMessage(x);}"
    "__mk(1);__mk(2);__mk(3);");
  {
    std::string cnt;
    for (int i = 0; i < 80; ++i) {
      mbWait(v, 25);
      cnt = Eval(v, "String(window.__cnt)");
      if (cnt == "3")
        break;
    }
    Expect(cnt == "3" && Eval(v, "String(window.__sum)") == "12",
           "three concurrent Workers each run and deliver their own reply",
           "cnt=[" + cnt + "] sum=[" + Eval(v, "String(window.__sum)") + "]");
  }

  // 37d. A MODULE worker (new Worker(url, {type:'module'})) runs its top-level script.
  // Module workers take a different evaluation path than classic ones AND enforce strict
  // JavaScript MIME checking, so the synthesized script response must carry a Content-Type
  // header (see mb_dedicated_worker_host.cc). Modules are strict mode, so the handler binds
  // via self.onmessage (a bare `onmessage=` would throw). Adds 100; 5 -> 105.
  mbLoadHTML(v, "<body>worker-module</body>", "about:blank");
  mbRunJS(v,
    "window.__mreply='';"
    "window.__mw=new Worker('data:text/javascript,'+"
    "encodeURIComponent('self.onmessage=function(e){self.postMessage(e.data+100)}'),"
    "{type:'module'});"
    "window.__mw.onmessage=function(e){window.__mreply=String(e.data)};"
    "window.__mw.postMessage(5);");
  {
    std::string r;
    for (int i = 0; i < 80; ++i) {
      mbWait(v, 25);
      r = Eval(v, "window.__mreply");
      if (!r.empty())
        break;
    }
    Expect(r == "105", "a module-type Worker runs its script (postMessage round-trip)",
           "reply=[" + r + "]");
  }

  // 37e. importScripts() inside a Worker loads a subresource through the worker's fetch
  // context (worker/mb_worker_fetch_context.cc) ON the worker thread — the end-to-end
  // exercise of Step 1. The worker imports a data: script defining self.K=7, then replies
  // e.data + K; posting 1 yields 8. Proves worker-thread subresource loading works.
  mbLoadHTML(v, "<body>worker-import</body>", "about:blank");
  mbRunJS(v,
    "window.__ireply='';"
    "window.__iw=new Worker('data:text/javascript,'+encodeURIComponent("
    "'importScripts(\"data:text/javascript,self.K=7\");"
    "onmessage=function(e){postMessage(e.data+self.K)}'));"
    "window.__iw.onmessage=function(e){window.__ireply=String(e.data)};"
    "window.__iw.onerror=function(e){window.__ireply='ERR:'+e.message};"
    "window.__iw.postMessage(1);");
  {
    std::string r;
    for (int i = 0; i < 80; ++i) {
      mbWait(v, 25);
      r = Eval(v, "window.__ireply");
      if (!r.empty())
        break;
    }
    Expect(r == "8",
           "importScripts() inside a Worker loads via the worker fetch context (Step 1)",
           "reply=[" + r + "]");
  }

  // 37f. NESTED worker: a Worker that spawns a sub-Worker (and relays through it). The
  // sub-worker is created ON the outer worker's thread, so its fetch context comes from
  // CloneWorkerFetchContext (not the frame) — returning null there used to FATAL on the
  // sub-worker's script load (no resource-load observer). Now the outer context is cloned.
  // Outer relays 10 -> inner doubles to 20 -> outer replies "inner:20".
  mbLoadHTML(v, "<body>worker-nested</body>", "about:blank");
  mbRunJS(v,
    "window.__nreply='';"
    "var __o=new Worker('data:text/javascript,'+encodeURIComponent("
    "\"var inner=new Worker('data:text/javascript,'+encodeURIComponent('onmessage=function(e){postMessage(e.data*2)}'));\""
    "+\"inner.onmessage=function(e){postMessage('inner:'+e.data)};\""
    "+\"onmessage=function(e){inner.postMessage(e.data)}\""
    "));"
    "__o.onmessage=function(e){window.__nreply=String(e.data)};"
    "__o.postMessage(10);");
  {
    std::string r;
    for (int i = 0; i < 80; ++i) {
      mbWait(v, 25);
      r = Eval(v, "window.__nreply");
      if (!r.empty())
        break;
    }
    Expect(r == "inner:20",
           "a nested Worker (sub-Worker spawned on the worker thread) runs and relays",
           "reply=[" + r + "]");
  }

  // 37g. SharedWorker runs in-process. `new SharedWorker(url)` reaches the SharedWorker-
  // Connector we bind in the frame broker (routed to the main thread); it synthesizes the
  // browser-fetched script + drives WebSharedWorker::CreateAndStart, then delivers the
  // page's MessagePort to the worker's `onconnect`. From a non-opaque page origin (opaque
  // origins disallow SharedWorker). The worker echoes via the port: page posts 7 -> 14.
  mbLoadHTML(v, "<body>shared-worker</body>", "https://shared.test/");
  mbRunJS(v,
    "window.__sreply='';"
    "var __sw=new SharedWorker('data:text/javascript,'+encodeURIComponent("
    "'onconnect=function(e){var p=e.ports[0];p.onmessage=function(ev){p.postMessage(ev.data*2)};p.start&&p.start();}'"
    "));"
    "__sw.port.onmessage=function(ev){window.__sreply=String(ev.data)};"
    "__sw.port.start&&__sw.port.start();"
    "__sw.port.postMessage(7);");
  {
    std::string r;
    for (int i = 0; i < 120; ++i) {
      mbWait(v, 25);
      r = Eval(v, "window.__sreply");
      if (!r.empty())
        break;
    }
    Expect(r == "14",
           "a SharedWorker runs and round-trips through its connect MessagePort",
           "reply=[" + r + "]");
  }

  // 37h. SharedWorker SHARING: two SharedWorker handles to the SAME url share ONE worker
  // (shared state), which is the entire point of the API. The worker keeps a per-instance
  // counter incremented on each message; if the two handles hit the same instance, the
  // second connection sees the first's increment (replies 1 then 2). A fresh-worker-per-
  // connect impl would reply 1 and 1.
  mbLoadHTML(v, "<body>shared-state</body>", "https://shared.test/");
  mbRunJS(v,
    "window.__a='';window.__b='';"
    "var __u='data:text/javascript,'+encodeURIComponent("
    "'var n=0;onconnect=function(e){var p=e.ports[0];p.onmessage=function(){p.postMessage(++n)};p.start&&p.start();}');"
    "var __w1=new SharedWorker(__u);__w1.port.onmessage=function(ev){window.__a=String(ev.data)};"
    "__w1.port.start&&__w1.port.start();__w1.port.postMessage(0);");
  // Wait for the first reply, THEN open the second handle so ordering is deterministic.
  for (int i = 0; i < 120; ++i) { mbWait(v, 25); if (!Eval(v,"window.__a").empty()) break; }
  mbRunJS(v,
    "var __w2=new SharedWorker(__u);__w2.port.onmessage=function(ev){window.__b=String(ev.data)};"
    "__w2.port.start&&__w2.port.start();__w2.port.postMessage(0);");
  {
    std::string b;
    for (int i = 0; i < 120; ++i) {
      mbWait(v, 25);
      b = Eval(v, "window.__b");
      if (!b.empty())
        break;
    }
    Expect(Eval(v, "window.__a") == "1" && b == "2",
           "two SharedWorker handles to one url share state (counter 1 then 2)",
           "a=[" + Eval(v, "window.__a") + "] b=[" + b + "]");
  }

  // 37i. MODULE SharedWorker: new SharedWorker(url,{type:'module'}) runs its
  // top-level module script. Module workers MIME-check the script (HttpContentType),
  // which the shared script-param builder sets — this verifies that holds for the
  // SHARED path too (module dedicated workers needed the same fix in 37d). onconnect
  // in module scope is self.onconnect; echo e.data+100 -> 5 becomes 105.
  mbLoadHTML(v, "<body>module-shared-worker</body>", "https://shared.test/");
  mbRunJS(v,
    "window.__msreply='';"
    "var __mw=new SharedWorker('data:text/javascript,'+encodeURIComponent("
    "'self.onconnect=function(e){var p=e.ports[0];p.onmessage=function(ev){p.postMessage(ev.data+100)};p.start&&p.start();}'"
    "),{type:'module'});"
    "__mw.port.onmessage=function(ev){window.__msreply=String(ev.data)};"
    "__mw.port.start&&__mw.port.start();__mw.port.postMessage(5);");
  {
    std::string r;
    for (int i = 0; i < 120; ++i) {
      mbWait(v, 25);
      r = Eval(v, "window.__msreply");
      if (!r.empty())
        break;
    }
    Expect(r == "105",
           "a MODULE SharedWorker runs its module script and round-trips",
           "reply=[" + r + "]");
  }

  // 37j. A dedicated Worker loads its script over HTTP(S) (not just data:/blob:):
  // new Worker('https://host/w.js') fetches the script through MbFetchUrl (here a
  // MOCK, so it's offline) and runs it. Proves the worker fetch path handles real
  // http(s) script URLs, the common case for a deployed site's worker.
  mbMockResponse("workerhost.test/w.js",
                 "onmessage=function(e){postMessage(e.data*3)}",
                 "application/javascript", 200);
  mbLoadHTML(v, "<body>http-worker</body>", "https://workerhost.test/");
  mbRunJS(v,
    "window.__hwreply='';"
    "var __hw=new Worker('https://workerhost.test/w.js');"
    "__hw.onmessage=function(e){window.__hwreply=String(e.data)};"
    "__hw.postMessage(4);");
  {
    std::string r;
    for (int i = 0; i < 120; ++i) {
      mbWait(v, 25);
      r = Eval(v, "window.__hwreply");
      if (!r.empty())
        break;
    }
    Expect(r == "12",
           "a dedicated Worker loads its script over http(s) and runs (mocked)",
           "reply=[" + r + "]");
    mbClearMocks();
  }

  // 37j2. PER-VIEW request mock serves a dedicated Worker's http(s) script
  // (IMPROVEMENT.md item 4 closure): the worker main-script fetch carries the
  // creating document's view, so mbOnRequestMock — not just the process-wide
  // hook — intercepts it. Before the fix this fetch had no view context and
  // silently bypassed the per-view hook (the documented residual).
  {
    static bool saw_worker_url;  // set by the capture-less hook below
    saw_worker_url = false;
    mbOnRequestMock(
        v,
        [](const char* url, mbRequestMock* m, void*) -> int {
          if (!std::strstr(url, "pvworker.test/w.js"))
            return 0;  // not ours: fall through
          saw_worker_url = true;
          const char* body = "onmessage=function(e){postMessage(e.data*7)}";
          mbRequestMockResponse(m, body, static_cast<int>(std::strlen(body)),
                                "application/javascript", 200);
          return 1;
        },
        nullptr);
    mbLoadHTML(v, "<body>pv-worker</body>", "https://pvworker.test/");
    mbRunJS(v,
            "window.__pvreply='';"
            "var __pv=new Worker('https://pvworker.test/w.js');"
            "__pv.onmessage=function(e){window.__pvreply=String(e.data)};"
            "__pv.postMessage(3);");
    std::string r;
    for (int i = 0; i < 120; ++i) {
      mbWait(v, 25);
      r = Eval(v, "window.__pvreply");
      if (!r.empty())
        break;
    }
    Expect(r == "21" && saw_worker_url,
           "the PER-VIEW request mock serves a dedicated Worker's script",
           "reply=[" + r + "] hook_saw_url=" + (saw_worker_url ? "1" : "0"));
    mbOnRequestMock(v, nullptr, nullptr);
  }

  // 37k. SharedWorker EVICTION: a SharedWorker lives only while a client is
  // connected. When the last page disconnects (navigates away), the worker is
  // terminated, so a later new SharedWorker(sameUrl) starts FRESH — its instance
  // counter resets to 1. Without eviction the old instance persists and replies 2
  // (as the in-page sharing test 37h shows). The worker script is identical across
  // both sessions, so the registry key matches: only eviction makes session 2 fresh.
  {
    const std::string mk =
        "var __u='data:text/javascript,'+encodeURIComponent('var n=0;"
        "onconnect=function(e){var p=e.ports[0];p.onmessage=function(){"
        "p.postMessage(++n)};p.start&&p.start();}');";
    mbLoadHTML(v, "<body>evict-1</body>", "https://evict.test/");
    mbRunJS(v, (std::string("window.__r1='';") + mk +
                "var w=new SharedWorker(__u);"
                "w.port.onmessage=function(ev){window.__r1=String(ev.data)};"
                "w.port.start&&w.port.start();w.port.postMessage(0);")
                   .c_str());
    std::string r1;
    for (int i = 0; i < 120; ++i) {
      mbWait(v, 25);
      r1 = Eval(v, "window.__r1");
      if (!r1.empty())
        break;
    }
    // Navigate away (same origin): the only client disconnects -> worker evicts.
    mbLoadHTML(v, "<body>evict-2</body>", "https://evict.test/");
    mbWait(v, 500);  // let disconnect + TerminateWorkerContext + deregister settle
    mbRunJS(v, (std::string("window.__r2='';") + mk +
                "var w2=new SharedWorker(__u);"
                "w2.port.onmessage=function(ev){window.__r2=String(ev.data)};"
                "w2.port.start&&w2.port.start();w2.port.postMessage(0);")
                   .c_str());
    std::string r2;
    for (int i = 0; i < 120; ++i) {
      mbWait(v, 25);
      r2 = Eval(v, "window.__r2");
      if (!r2.empty())
        break;
    }
    Expect(r1 == "1" && r2 == "1",
           "a SharedWorker is evicted when its last client disconnects (counter resets)",
           "r1=[" + r1 + "] r2=[" + r2 + "]");
  }

  // 37l. Window<->worker IndexedDB SHARING (same origin): a worker's IDB is its
  // ORIGIN's, so a same-origin worker opens the SAME backend the window did and
  // sees its data. The per-origin isolation (73b) must NOT break this — the worker
  // scopes by its script origin, which equals the window's origin here. The worker
  // script is MOCKED at the window's origin (offline + same-origin). If the worker
  // were unscoped ("",name) it'd get a fresh empty db -> 'nostore'.
  mbMockResponse(
      "idbw.test/w.js",
      "onmessage=function(){var q=indexedDB.open('wshare',1);"
      "q.onsuccess=function(e){try{var g=e.target.result.transaction('s')."
      "objectStore('s').get(1);g.onsuccess=function(){postMessage(g.result?"
      "g.result.val:'none');};}catch(ex){postMessage('nostore');}};};",
      "application/javascript", 200);
  mbLoadHTML(v, "<body>idbw</body>", "https://idbw.test/");
  mbRunJS(v,
    "window.__wr='';var q=indexedDB.open('wshare',1);"
    "q.onupgradeneeded=function(e){e.target.result.createObjectStore('s',"
    "{keyPath:'id'});};q.onsuccess=function(e){var db=e.target.result;"
    "var t=db.transaction('s','readwrite');t.objectStore('s').put("
    "{id:1,val:'fromwin'});t.oncomplete=function(){var w=new Worker("
    "'https://idbw.test/w.js');w.onmessage=function(ev){window.__wr="
    "String(ev.data);};w.postMessage(0);};};");
  {
    std::string wr;
    for (int i = 0; i < 200; ++i) {
      mbWait(v, 25);
      wr = Eval(v, "window.__wr");
      if (!wr.empty())
        break;
    }
    Expect(wr == "fromwin",
           "a same-origin Worker shares its window's IndexedDB (per-origin, not per-frame)",
           "wr=[" + wr + "]");
    mbClearMocks();
  }

  // 37n. A Blob/File value stored in IndexedDB reads back intact within the session.
  // The record's structured-serialized bytes reference attached blobs by index; the
  // backend must RETAIN the WebBlobInfo handles (mb_indexeddb MbRecord::blob_info) so
  // the in-process MbBlob stays alive, then re-attach them on get() — mojo serializes
  // the blob handle back to the renderer. Without retention the value's blob ref is
  // dangling and rec.f.text() never resolves (or 'norec'). (IDB needs a non-opaque
  // origin — an opaque about:blank/data: context gets SecurityError.)
  mbLoadHTML(v, "<body>blobidb</body>", "https://blobidb.test/");
  mbRunJS(v,
    "window.__br='';var blob=new Blob(['hello-blob'],{type:'text/plain'});"
    "var q=indexedDB.open('blobidb',1);q.onupgradeneeded=function(e){"
    "e.target.result.createObjectStore('s',{keyPath:'id'});};"
    "q.onsuccess=function(e){var db=e.target.result;"
    "var t=db.transaction('s','readwrite');t.objectStore('s').put({id:1,f:blob});"
    "t.oncomplete=function(){var g=db.transaction('s').objectStore('s').get(1);"
    "g.onsuccess=function(){var rec=g.result;"
    "if(!rec||!rec.f){window.__br='norec';return;}"
    "rec.f.text().then(function(txt){window.__br='got:'+txt;});};};};");
  {
    std::string br;
    for (int i = 0; i < 200; ++i) {
      mbWait(v, 25);
      br = Eval(v, "window.__br");
      if (!br.empty())
        break;
    }
    Expect(br == "got:hello-blob",
           "a Blob value stored in IndexedDB reads back intact (get -> blob.text())",
           "br=[" + br + "]");
  }

  // 37n2. A Blob value stored in IndexedDB PERSISTS TO DISK across a save/reload. The
  // serializer reads each record blob's bytes asynchronously (MbReadBlobRemoteBytes — a
  // blob is only readable via its remote, and its stored UUID != the serving MbBlob's) and
  // writes them; the loader re-mints a fresh inline blob. We put a blob, mbSaveIndexedDB,
  // DELETE the db (dropping the in-memory record + its blob), mbLoadIndexedDB, reopen, and
  // read the blob back — proving the bytes came from DISK, not the in-session handle.
  {
    const char* idb_path = "/tmp/mb_blobidb_persist.bin";
    mbLoadHTML(v, "<body>blobpersist</body>", "https://blobpersist.test/");
    mbRunJS(v,
      "window.__s='';var blob=new Blob(['persist-blob'],{type:'text/plain'});"
      "var q=indexedDB.open('bp',1);q.onupgradeneeded=function(e){"
      "e.target.result.createObjectStore('s',{keyPath:'id'});};"
      "q.onsuccess=function(e){window.__db=e.target.result;"
      "var t=window.__db.transaction('s','readwrite');t.objectStore('s').put({id:1,f:blob});"
      "t.oncomplete=function(){window.__s='put';};};");
    std::string s;
    for (int i = 0; i < 200 && s != "put"; ++i) { mbWait(v, 25); s = Eval(v, "window.__s"); }
    const bool put_ok = (s == "put");
    mbSaveIndexedDB(idb_path);
    mbRunJS(v,
      "window.__s='';window.__db.close();"
      "var d=indexedDB.deleteDatabase('bp');"
      "d.onsuccess=function(){window.__s='del';};d.onerror=function(){window.__s='delerr';};");
    s.clear();
    for (int i = 0; i < 200 && s != "del"; ++i) { mbWait(v, 25); s = Eval(v, "window.__s"); }
    const bool del_ok = (s == "del");
    mbLoadIndexedDB(idb_path);  // restore from disk
    mbRunJS(v,
      "window.__s='';var q2=indexedDB.open('bp',1);"
      "q2.onupgradeneeded=function(e){window.__s='upgrade';};"  // should NOT fire if loaded
      "q2.onsuccess=function(e){var db=e.target.result;"
      "var g=db.transaction('s').objectStore('s').get(1);"
      "g.onsuccess=function(){var rec=g.result;if(!rec||!rec.f){window.__s='norec';return;}"
      "rec.f.text().then(function(t){window.__s='got:'+t;});};"
      "g.onerror=function(){window.__s='geterr';};};"
      "q2.onerror=function(){window.__s='openerr';};");
    s.clear();
    for (int i = 0; i < 240 && s.rfind("got:", 0) != 0 && s != "norec" &&
                    s != "upgrade" && s != "openerr"; ++i) {
      mbWait(v, 25);
      s = Eval(v, "window.__s");
    }
    Expect(put_ok && del_ok && s == "got:persist-blob",
           "a Blob value in IndexedDB persists to disk (save/delete/load round-trip)",
           "put=" + std::string(put_ok ? "1" : "0") + " del=" + (del_ok ? "1" : "0") +
               " final=[" + s + "]");
  }

  // 37n3. The two UNTESTED paths of blob-IDB persistence: a FILE (name + lastModified +
  // type, the is_file serialization branch) AND a LARGE >256KB payload (a BytesProvider
  // blob, the async-read path at scale — the same size class as the cache-body bug). Store
  // a 300000-byte File, persist, DELETE the db, reload, and verify name + lastModified +
  // type + full byte length + content all survive a disk round-trip.
  {
    const char* idb_path = "/tmp/mb_fileidb_persist.bin";
    mbLoadHTML(v, "<body>filepersist</body>", "https://filepersist.test/");
    mbRunJS(v,
      "window.__s='';"
      "var big='Q'.repeat(300000);"
      "var f=new File([big],'doc.txt',{type:'text/plain',lastModified:1700000000000});"
      "var q=indexedDB.open('fp',1);q.onupgradeneeded=function(e){"
      "e.target.result.createObjectStore('s',{keyPath:'id'});};"
      "q.onsuccess=function(e){window.__db=e.target.result;"
      "var t=window.__db.transaction('s','readwrite');t.objectStore('s').put({id:1,f:f});"
      "t.oncomplete=function(){window.__s='put';};};");
    std::string s;
    for (int i = 0; i < 200 && s != "put"; ++i) { mbWait(v, 25); s = Eval(v, "window.__s"); }
    const bool put_ok = (s == "put");
    mbSaveIndexedDB(idb_path);
    mbRunJS(v,
      "window.__s='';window.__db.close();"
      "var d=indexedDB.deleteDatabase('fp');d.onsuccess=function(){window.__s='del';};");
    s.clear();
    for (int i = 0; i < 200 && s != "del"; ++i) { mbWait(v, 25); s = Eval(v, "window.__s"); }
    const bool del_ok = (s == "del");
    mbLoadIndexedDB(idb_path);
    mbRunJS(v,
      "window.__s='';var q2=indexedDB.open('fp',1);"
      "q2.onsuccess=function(e){var db=e.target.result;"
      "var g=db.transaction('s').objectStore('s').get(1);"
      "g.onsuccess=function(){var r=g.result;"
      "if(!r||!r.f){window.__s='norec';return;}var f=r.f;"
      // Report file metadata; then verify content length + a sentinel char from the body.
      "f.text().then(function(t){"
      "window.__s='isFile='+(f instanceof File)+' name='+f.name+' lm='+f.lastModified"
      "+' type='+f.type+' len='+t.length+' ok='+(t.length===300000&&t[299999]==='Q');});};};"
      "q2.onerror=function(){window.__s='openerr';};");
    s.clear();
    for (int i = 0; i < 400 && s.rfind("isFile=", 0) != 0 && s != "norec" &&
                    s != "openerr"; ++i) {
      mbWait(v, 25);
      s = Eval(v, "window.__s");
    }
    const bool all_ok =
        put_ok && del_ok &&
        s.find("isFile=true") != std::string::npos &&
        s.find("name=doc.txt") != std::string::npos &&
        s.find("lm=1700000000000") != std::string::npos &&
        s.find("type=text/plain") != std::string::npos &&
        s.find("len=300000") != std::string::npos &&
        s.find("ok=true") != std::string::npos;
    Expect(all_ok,
           "a large File (300KB, name+lastModified) persists to disk intact",
           "put=" + std::string(put_ok ? "1" : "0") + " del=" + (del_ok ? "1" : "0") +
               " final=[" + s + "]");
  }

  // 38. ServiceWorker spawn must be crash-safe. (SharedWorker now runs — see 37g.)
  // navigator.serviceWorker.register() either rejects cleanly or, on a
  // real origin where we have no provider, is null-guarded (pending promise,
  // no crash). Neither must SIGSEGV — same hazard class as case 37. We assert
  // the host survives constructing both and stays scriptable afterward.
  // (about:blank is an opaque origin: SharedWorker may throw SecurityError and
  // navigator.serviceWorker may be absent — both fine. The invariant under test
  // is crash-safety, so we wrap each attempt in try/catch and set a sentinel at
  // the end: reaching it proves neither spawn took the host down, and the host
  // still evaluates JS afterward.)
  mbLoadHTML(v, "<body>family-guard</body>", "about:blank");
  mbRunJS(v,
    "window.__done=false;"
    "try{new SharedWorker('data:text/javascript,onconnect=function(){}');}catch(e){}"
    "try{if(navigator.serviceWorker)"
    "navigator.serviceWorker.register('data:text/javascript,').then(function(){},function(){});}"
    "catch(e){}"
    "window.__done=true;");
  mbWait(v, 60);
  Expect(Eval(v, "1+1") == "2" &&
             Eval(v, "document.body.textContent") == "family-guard" &&
             Eval(v, "String(window.__done)") == "true",
         "SharedWorker + ServiceWorker spawn are crash-safe (host scriptable)");

  // 39. IndexedDB opens (the in-process IDBFactory backend, frame/mb_indexeddb.cc).
  // open() fires onsuccess with a usable database; createObjectStore in onupgradeneeded
  // is reflected in objectStoreNames. (mb_smoke 23m covers this against the library ABI;
  // this is the render-suite guard.) Reads/writes are step 2. (Real http origin: IDB is
  // unavailable on the opaque about:blank origin.)
  mbLoadHTML(v, "<body>idb-guard</body>", "https://miniblink.test/");
  mbRunJS(v,
    "window.__idb='pending';"
    "try{var r=indexedDB.open('mb-probe',1);"
    "r.onupgradeneeded=function(e){e.target.result.createObjectStore('s');};"
    "r.onerror=function(){window.__idb='error';};"
    "r.onsuccess=function(e){var db=e.target.result;"
    "window.__idb='v'+db.version+':'+Array.from(db.objectStoreNames).join(',');};"
    "}catch(e){window.__idb='threw:'+e.name;}");
  for (int i = 0; i < 80; ++i) {
    mbWait(v, 25);
    if (Eval(v, "String(window.__idb)") != "pending")
      break;
  }
  Expect(Eval(v, "1+1") == "2" &&
             Eval(v, "document.body.textContent") == "idb-guard" &&
             Eval(v, "String(window.__idb)") == "v1:s",
         "IndexedDB open() succeeds with a created object store (host scriptable)");

  // 39b. History API (SPA client-side routing). pushState/replaceState update
  // location + history.state, and — the part that matters to an embedder —
  // mbGetURL reflects the new URL, so a scraper/automation sees the current SPA
  // route (not just the initial load). Uses a real https origin (the realistic
  // SPA case; about:blank can't pushState cross-path).
  {
    mbLoadHTML(v, "<body>spa</body>", "https://spa.test/page/one");
    mbRunJS(v, "history.pushState({a:1},'','/page/two?q=1');");
    char u1[256] = {0};
    mbGetURL(v, u1, sizeof(u1));
    const std::string loc_push = Eval(v, "location.pathname+location.search");
    mbRunJS(v, "history.replaceState({a:2},'','/page/three');");
    char u2[256] = {0};
    mbGetURL(v, u2, sizeof(u2));
    const std::string state = Eval(v, "String(history.state&&history.state.a)");
    Expect(std::string(u1) == "https://spa.test/page/two?q=1" &&
               loc_push == "/page/two?q=1" &&
               std::string(u2) == "https://spa.test/page/three" && state == "2",
           "History API: pushState/replaceState update location + mbGetURL",
           std::string("push=") + u1 + " replace=" + u2 + " state=" + state);
  }

  // 40. WebSocket connects and round-trips. The in-process WebSocketConnector
  // (frame/mb_websocket.cc) establishes the connection (onopen) and runs a loopback
  // echo, so a sent message returns via onmessage; ws.close() then drives onclose.
  // (mb_smoke 23k covers the same path against the library ABI; this is the render-
  // suite guard that the WS API is live and the host stays scriptable.)
  mbLoadHTML(v, "<body>ws-guard</body>", "https://miniblink.test/");
  mbRunJS(v,
    "window.__ws='pending';"
    "try{var s=new WebSocket('wss://miniblink.test/x');"
    "s.onopen=function(){s.send('ping-render');};"
    "s.onmessage=function(e){window.__ws='msg:'+e.data;s.close();};"
    "s.onerror=function(){window.__ws='error';};"
    "}catch(e){window.__ws='threw:'+e.name;}");
  for (int i = 0; i < 80; ++i) {
    mbWait(v, 25);
    if (Eval(v, "String(window.__ws)") != "pending")
      break;
  }
  Expect(Eval(v, "1+1") == "2" &&
             Eval(v, "document.body.textContent") == "ws-guard" &&
             Eval(v, "String(window.__ws)") == "msg:ping-render",
         "WebSocket connects + echoes (onopen/onmessage); host scriptable");

  // 41. Canvas 2D full round-trip. Canvas is core for a renderer and the backbone of
  // chart/image libraries, so verify the complete path works offline: get a 2D context,
  // draw, read pixels back via getImageData (exact color), and encode via toDataURL.
  mbLoadHTML(v, "<body><canvas id='c' width='20' height='20'></canvas></body>",
             "about:blank");
  mbRunJS(v,
    "var cv=document.getElementById('c'),x=cv.getContext('2d');"
    "x.fillStyle='#ff0000';x.fillRect(0,0,20,20);"
    "var d=x.getImageData(10,10,1,1).data;"
    "window.__rgba=d[0]+','+d[1]+','+d[2]+','+d[3];"
    "window.__png=cv.toDataURL().indexOf('data:image/png')===0;");
  Expect(Eval(v, "window.__rgba") == "255,0,0,255" &&
             Eval(v, "String(window.__png)") == "true",
         "Canvas 2D round-trip (draw/getImageData/toDataURL)");

  // 41z. WebGL RENDERS (bring-up milestone C): getContext('webgl') returns a real
  // context backed by our in-process ANGLE/SwiftShader GLES2 command buffer
  // (platform/mb_webgl.cc), and a clear + readPixels round-trips the cleared color.
  // Proves WebGL works headlessly (no GPU) — previously getContext('webgl') was null.
  mbLoadHTML(v, "<body><canvas id='g' width='32' height='32'></canvas></body>",
             "about:blank");
  mbRunJS(v,
    "window.__wg='';try{"
    "var gl=document.getElementById('g').getContext('webgl');"
    "if(!gl){window.__wg='null';}else{"
    "gl.clearColor(0,1,0,1);gl.clear(gl.COLOR_BUFFER_BIT);"
    "var p=new Uint8Array(4);gl.readPixels(0,0,1,1,gl.RGBA,gl.UNSIGNED_BYTE,p);"
    "window.__wg=p[0]+','+p[1]+','+p[2]+','+p[3];"
    "window.__wgver=''+gl.getParameter(gl.VERSION);}}"
    "catch(e){window.__wg='err:'+e;}");
  {
    std::string wg;
    for (int i = 0; i < 200; ++i) {
      mbWait(v, 25);
      wg = Eval(v, "window.__wg");
      if (!wg.empty())
        break;
    }
    Expect(wg == "0,255,0,255",
           "WebGL renders: getContext('webgl') + clear + readPixels (in-process ANGLE/SwiftShader)",
           "wg=[" + wg + "] ver=[" + Eval(v, "window.__wgver") + "]");
  }

  // 41z2. WebGL 2 renders: getContext('webgl2') returns an ES3-backed context (the
  // provider requests an OpenGL ES 3 command buffer for WebGL 2 — patches/0007 lets
  // GLInProcessContext::Initialize pick the context type), clear(blue)+readPixels
  // round-trips, and GL_VERSION reports WebGL 2.0.
  mbLoadHTML(v, "<body><canvas id='g2' width='32' height='32'></canvas></body>",
             "about:blank");
  mbRunJS(v,
    "window.__w2='';try{"
    "var gl=document.getElementById('g2').getContext('webgl2');"
    "if(!gl){window.__w2='null';}else{"
    "gl.clearColor(0,0,1,1);gl.clear(gl.COLOR_BUFFER_BIT);"
    "var p=new Uint8Array(4);gl.readPixels(0,0,1,1,gl.RGBA,gl.UNSIGNED_BYTE,p);"
    "window.__w2=p[0]+','+p[1]+','+p[2]+','+p[3];"
    "window.__w2ver=''+gl.getParameter(gl.VERSION);}}"
    "catch(e){window.__w2='err:'+e;}");
  {
    std::string w2;
    for (int i = 0; i < 200; ++i) {
      mbWait(v, 25);
      w2 = Eval(v, "window.__w2");
      if (!w2.empty())
        break;
    }
    const std::string ver = Eval(v, "window.__w2ver");
    Expect(w2 == "0,0,255,255" && ver.find("WebGL 2.0") != std::string::npos,
           "WebGL 2 renders: getContext('webgl2') ES3 context + clear + readPixels",
           "w2=[" + w2 + "] ver=[" + ver + "]");
  }

  // 41z3. A WebGL canvas COMPOSITES into the page paint (screenshot): a headless
  // renderer's WebGL output must reach mbPaintToBitmap, not just gl.readPixels — else
  // screenshots of WebGL visualizations are blank. Clear a visible WebGL canvas to
  // magenta, render the page, and read the canvas region from the page bitmap (the
  // 41b proof, for WebGL). The GPU drawing buffer must be read back + rasterized into
  // the software page paint.
  {
    mbLoadHTML(v,
      "<body style='margin:0;background:#fff'>"
      "<canvas id='gp' width='80' height='80'></canvas>"
      "<script>window.__gp='';try{"
      "var gl=document.getElementById('gp').getContext('webgl');"
      "gl.clearColor(1,0,1,1);gl.clear(gl.COLOR_BUFFER_BIT);gl.finish();"
      "window.__gp='ok';}catch(e){window.__gp='err:'+e;}</script></body>",
      "about:blank");
    // Let the WebGL draw + canvas compositing settle before capturing.
    for (int i = 0; i < 40; ++i)
      mbWait(v, 25);
    std::vector<uint8_t> cv(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, cv.data(), W, H, W * 4);
    size_t ci = (40u * W + 40u) * 4;  // inside the 80x80 canvas box
    // magenta = R255 G0 B255 (BGRA byte order: [0]=B, [1]=G, [2]=R).
    const bool magenta = cv[ci + 2] == 255 && cv[ci + 1] == 0 && cv[ci] == 255;
    Expect(magenta,
           "a WebGL canvas composites into the page paint (screenshot capture)",
           "R=" + std::to_string(cv[ci + 2]) + " G=" + std::to_string(cv[ci + 1]) +
               " B=" + std::to_string(cv[ci]) + " gp=[" + Eval(v, "window.__gp") + "]");
  }

  // 41z4. WebGL does REAL shader rendering, not just clear: compile a vertex+fragment
  // shader, link a program, upload a vertex buffer, and drawArrays a viewport-covering
  // triangle in orange — then readPixels the center. Exercises the full WebGL pipeline
  // (shader compile/link, attributes, buffers, draw) through the in-process command
  // buffer — the actual use case behind charts/3D/shadertoy, beyond clearColor.
  mbLoadHTML(v, "<body><canvas id='gs' width='32' height='32'></canvas></body>",
             "about:blank");
  mbRunJS(v,
    "window.__gs='';try{"
    "var gl=document.getElementById('gs').getContext('webgl');"
    "var vs=gl.createShader(gl.VERTEX_SHADER);"
    "gl.shaderSource(vs,'attribute vec2 p;void main(){gl_Position=vec4(p,0.0,1.0);}');"
    "gl.compileShader(vs);"
    "var fs=gl.createShader(gl.FRAGMENT_SHADER);"
    "gl.shaderSource(fs,'void main(){gl_FragColor=vec4(1.0,0.5,0.0,1.0);}');"
    "gl.compileShader(fs);"
    "var ok=gl.getShaderParameter(vs,gl.COMPILE_STATUS)&&"
    "gl.getShaderParameter(fs,gl.COMPILE_STATUS);"
    "var pr=gl.createProgram();gl.attachShader(pr,vs);gl.attachShader(pr,fs);"
    "gl.linkProgram(pr);ok=ok&&gl.getProgramParameter(pr,gl.LINK_STATUS);"
    "gl.useProgram(pr);"
    "var b=gl.createBuffer();gl.bindBuffer(gl.ARRAY_BUFFER,b);"
    "gl.bufferData(gl.ARRAY_BUFFER,new Float32Array([-1,-1,3,-1,-1,3]),gl.STATIC_DRAW);"
    "var l=gl.getAttribLocation(pr,'p');gl.enableVertexAttribArray(l);"
    "gl.vertexAttribPointer(l,2,gl.FLOAT,false,0,0);"
    "gl.clearColor(0,0,0,1);gl.clear(gl.COLOR_BUFFER_BIT);"
    "gl.drawArrays(gl.TRIANGLES,0,3);gl.finish();"
    "var p=new Uint8Array(4);gl.readPixels(16,16,1,1,gl.RGBA,gl.UNSIGNED_BYTE,p);"
    "window.__gs=(ok?'ok':'shaderfail')+':'+p[0]+','+p[1]+','+p[2]+','+p[3];"
    "}catch(e){window.__gs='err:'+e;}");
  {
    std::string gs;
    for (int i = 0; i < 200; ++i) {
      mbWait(v, 25);
      gs = Eval(v, "window.__gs");
      if (!gs.empty())
        break;
    }
    // Orange (1.0,0.5,0.0) -> R255 G~128 B0; allow a tolerance on G for rounding.
    bool ok = gs.rfind("ok:", 0) == 0;
    int r = 0, g = 0, bl = 0, a = 0;
    if (ok)
      sscanf(gs.c_str(), "ok:%d,%d,%d,%d", &r, &g, &bl, &a);
    Expect(ok && r == 255 && g >= 120 && g <= 136 && bl == 0 && a == 255,
           "WebGL shader pipeline: compile+link+drawArrays renders a triangle",
           "gs=[" + gs + "]");
  }

  // 41z5. WebGL on an OffscreenCanvas (no DOM <canvas>): the same in-process provider
  // serves OffscreenCanvas.getContext('webgl'), so off-DOM/worker-style GPU rendering
  // works — a major modern WebGL use (render off the main thread / without a visible
  // canvas). Clear to cyan, readPixels.
  mbLoadHTML(v, "<body>oc</body>", "about:blank");
  mbRunJS(v,
    "window.__oc='';try{"
    "var oc=new OffscreenCanvas(32,32);var gl=oc.getContext('webgl');"
    "if(!gl){window.__oc='null';}else{"
    "gl.clearColor(0,1,1,1);gl.clear(gl.COLOR_BUFFER_BIT);"
    "var p=new Uint8Array(4);gl.readPixels(0,0,1,1,gl.RGBA,gl.UNSIGNED_BYTE,p);"
    "window.__oc=p[0]+','+p[1]+','+p[2]+','+p[3];}}"
    "catch(e){window.__oc='err:'+e;}");
  {
    std::string oc;
    for (int i = 0; i < 200; ++i) {
      mbWait(v, 25);
      oc = Eval(v, "window.__oc");
      if (!oc.empty())
        break;
    }
    Expect(oc == "0,255,255,255",
           "WebGL on an OffscreenCanvas renders (off-DOM GPU rendering)",
           "oc=[" + oc + "]");
  }

  // 41z6. WebGL in a WORKER via transferControlToOffscreen: a <canvas>'s control is
  // transferred to a dedicated worker, which creates the WebGL context and renders —
  // true off-MAIN-THREAD GPU rendering (the headline OffscreenCanvas use). Exercises
  // Platform::CreateWebGLGraphicsContextProvider on a worker thread + the process-wide
  // in-process GPU thread shared across threads. Worker clears to yellow + readPixels.
  // (The provider posts its context teardown back to the worker's creation sequence, so
  // worker shutdown no longer aborts on the GPU sequence_checker DCHECK — the reason this
  // had been deferred. The whole render suite exiting 0 is part of the proof.)
  mbLoadHTML(v, "<body><canvas id='ocw' width='32' height='32'></canvas></body>",
             "about:blank");
  mbRunJS(v,
    "window.__ocw='';try{"
    "var w=new Worker('data:text/javascript,'+encodeURIComponent("
    "'onmessage=function(e){try{var gl=e.data.getContext(\"webgl\");"
    "if(!gl){postMessage(\"null\");return;}"
    "gl.clearColor(1,1,0,1);gl.clear(gl.COLOR_BUFFER_BIT);"
    "var p=new Uint8Array(4);gl.readPixels(0,0,1,1,gl.RGBA,gl.UNSIGNED_BYTE,p);"
    "postMessage(p[0]+\",\"+p[1]+\",\"+p[2]+\",\"+p[3]);}"
    "catch(err){postMessage(\"werr:\"+err);}}'));"
    "w.onmessage=function(e){window.__ocw=String(e.data);};"
    "var oc=document.getElementById('ocw').transferControlToOffscreen();"
    "w.postMessage(oc,[oc]);}catch(e){window.__ocw='err:'+e;}");
  {
    std::string ocw;
    for (int i = 0; i < 200; ++i) {
      mbWait(v, 25);
      ocw = Eval(v, "window.__ocw");
      if (!ocw.empty())
        break;
    }
    Expect(ocw == "255,255,0,255",
           "WebGL renders in a Worker via transferControlToOffscreen (off-main-thread)",
           "ocw=[" + ocw + "]");
  }

  // 41b. A 2D canvas COMPOSITES into the page paint (not just its in-memory backing
  // store): draw a red square, render the page, and read the canvas region from the
  // page bitmap. Proves canvas-drawn content rasterizes into the rendered output —
  // so screenshots of canvas charts/visualizations capture the drawing.
  {
    mbLoadHTML(v,
      "<body style='margin:0;background:#fff'>"
      "<canvas id='cp' width='80' height='80'></canvas>"
      "<script>var x=document.getElementById('cp').getContext('2d');"
      "x.fillStyle='#ff0000';x.fillRect(0,0,80,80);</script></body>",
      "about:blank");
    std::vector<uint8_t> cv(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, cv.data(), W, H, W * 4);
    size_t ci = (40u * W + 40u) * 4;  // inside the canvas box
    Expect(cv[ci + 2] == 255 && cv[ci + 1] == 0 && cv[ci] == 0,
           "a 2D canvas composites into the page paint (red square rasterizes)",
           "R=" + std::to_string(cv[ci + 2]) + " G=" +
               std::to_string(cv[ci + 1]) + " B=" + std::to_string(cv[ci]));
  }

  // 41c. Web Audio PROCESSING works (OfflineAudioContext): unlike <audio>/<video>
  // playback (which need a media pipeline + audio device), OfflineAudioContext
  // renders to a BUFFER by pure computation — no hardware. An oscillator rendered
  // offline must produce non-zero samples. Proves DSP/synthesis/analysis (the
  // headless-relevant Web Audio use) works, narrowing the "media" gap to playback.
  {
    mbLoadHTML(v, "<body>wa</body>", "about:blank");
    mbRunJS(v,
      "window.__wa='';try{"
      "var ctx=new OfflineAudioContext(1,256,44100);"
      "var osc=ctx.createOscillator();osc.frequency.value=440;"
      "osc.connect(ctx.destination);osc.start(0);"
      "ctx.startRendering().then(function(buf){var d=buf.getChannelData(0);"
      "var nz=0;for(var i=0;i<d.length;i++)if(d[i]!==0)nz++;"
      "window.__wa='len:'+d.length+',nonzero:'+(nz>0);"
      "}).catch(function(e){window.__wa='err:'+e.name;});"
      "}catch(e){window.__wa='throw:'+e.name;}");
    std::string wa;
    for (int i = 0; i < 80; ++i) {
      mbWait(v, 25);
      wa = Eval(v, "window.__wa");
      if (!wa.empty())
        break;
    }
    Expect(wa == "len:256,nonzero:true",
           "Web Audio: OfflineAudioContext renders an oscillator to a buffer (DSP works)",
           "wa=[" + wa + "]");
  }

  // 41d. Web Audio decodeAudioData() decodes a real audio FILE into an AudioBuffer —
  // exercises the FFmpeg media-decode stack (media::AudioFileReader) the embedder links
  // but had never been verified through a web API. Synthesize a 16-bit PCM mono WAV
  // (800 samples @ 8000 Hz) in JS and decode it; the AudioBuffer must report the file's
  // rate/length with non-zero samples. This is the decode foundation under <audio>/
  // <video> playback, and a real feature on its own (sound effects, music, analysis).
  {
    mbLoadHTML(v, "<body>dec</body>", "about:blank");
    mbRunJS(v,
      "window.__dec='';try{"
      "var sr=8000,n=800,buf=new ArrayBuffer(44+n*2),dv=new DataView(buf);"
      "function S(o,t){for(var i=0;i<t.length;i++)dv.setUint8(o+i,t.charCodeAt(i));}"
      "S(0,'RIFF');dv.setUint32(4,36+n*2,true);S(8,'WAVE');S(12,'fmt ');"
      "dv.setUint32(16,16,true);dv.setUint16(20,1,true);dv.setUint16(22,1,true);"
      "dv.setUint32(24,sr,true);dv.setUint32(28,sr*2,true);dv.setUint16(32,2,true);"
      "dv.setUint16(34,16,true);S(36,'data');dv.setUint32(40,n*2,true);"
      "for(var i=0;i<n;i++)dv.setInt16(44+i*2,Math.round(Math.sin(i/10)*10000),true);"
      "var ctx=new OfflineAudioContext(1,n,sr);"
      "ctx.decodeAudioData(buf).then(function(b){var d=b.getChannelData(0);var nz=0;"
      "for(var i=0;i<d.length;i++)if(d[i]!==0)nz++;"
      "window.__dec='sr:'+b.sampleRate+',ch:'+b.numberOfChannels+',len:'+b.length+"
      "',nonzero:'+(nz>0);}).catch(function(e){window.__dec='err:'+e.name;});"
      "}catch(e){window.__dec='throw:'+e.name;}");
    std::string dec;
    for (int i = 0; i < 200; ++i) {
      mbWait(v, 25);
      dec = Eval(v, "window.__dec");
      if (!dec.empty())
        break;
    }
    Expect(dec == "sr:8000,ch:1,len:800,nonzero:true",
           "Web Audio decodeAudioData decodes a WAV file (FFmpeg media-decode stack)",
           "dec=[" + dec + "]");
  }

  // 41e. An <audio> element LOADS + reports metadata (media playback bring-up step 1):
  // a custom WebMediaPlayer (media/mb_audio_player.cc) fetches the src + decodes it with
  // FFmpeg and reaches HAVE_ENOUGH_DATA, so loadedmetadata fires and audio.duration is
  // the file's duration (0.1s WAV). Previously <audio> had no player (loadedmetadata
  // never fired, duration NaN). Playback timeline is a follow-on; metadata is step 1.
  {
    mbLoadHTML(v, "<body>au</body>", "about:blank");
    mbRunJS(v,
      "window.__au='';try{"
      "var sr=8000,n=800,buf=new ArrayBuffer(44+n*2),dv=new DataView(buf);"
      "function S(o,t){for(var i=0;i<t.length;i++)dv.setUint8(o+i,t.charCodeAt(i));}"
      "S(0,'RIFF');dv.setUint32(4,36+n*2,true);S(8,'WAVE');S(12,'fmt ');"
      "dv.setUint32(16,16,true);dv.setUint16(20,1,true);dv.setUint16(22,1,true);"
      "dv.setUint32(24,sr,true);dv.setUint32(28,sr*2,true);dv.setUint16(32,2,true);"
      "dv.setUint16(34,16,true);S(36,'data');dv.setUint32(40,n*2,true);"
      "for(var i=0;i<n;i++)dv.setInt16(44+i*2,Math.round(Math.sin(i/10)*10000),true);"
      "var u8=new Uint8Array(buf),bin='';"
      "for(var i=0;i<u8.length;i++)bin+=String.fromCharCode(u8[i]);"
      "var a=document.createElement('audio');"
      "a.addEventListener('loadedmetadata',function(){"
      "window.__au='dur:'+a.duration.toFixed(2)+',ready:'+(a.readyState>=1);});"
      "a.addEventListener('error',function(){window.__au='err:'+(a.error?a.error.code:'?');});"
      "a.src='data:audio/wav;base64,'+btoa(bin);a.load();"
      "}catch(e){window.__au='throw:'+e;}");
    std::string au;
    for (int i = 0; i < 200; ++i) {
      mbWait(v, 25);
      au = Eval(v, "window.__au");
      if (!au.empty())
        break;
    }
    Expect(au == "dur:0.10,ready:true",
           "an <audio> element loads + reports duration/metadata (custom WebMediaPlayer)",
           "au=[" + au + "]");
  }

  // 41f. <audio> PLAYBACK TIMELINE (media bring-up step 2): play() advances currentTime
  // in real time (the player's clock), fires timeupdate while playing, and fires `ended`
  // (paused, currentTime==duration) when it reaches the end. A 0.3s WAV is played to
  // completion. (No real audio output — the device is silent — but the timeline is real:
  // what timers/seek-bars/"play next" logic depend on.) muted so autoplay isn't blocked.
  {
    mbLoadHTML(v, "<body>pb</body>", "about:blank");
    mbRunJS(v,
      "window.__pb='';try{var sr=8000,n=2400,buf=new ArrayBuffer(44+n*2),"
      "dv=new DataView(buf);"
      "function S(o,t){for(var i=0;i<t.length;i++)dv.setUint8(o+i,t.charCodeAt(i));}"
      "S(0,'RIFF');dv.setUint32(4,36+n*2,true);S(8,'WAVE');S(12,'fmt ');"
      "dv.setUint32(16,16,true);dv.setUint16(20,1,true);dv.setUint16(22,1,true);"
      "dv.setUint32(24,sr,true);dv.setUint32(28,sr*2,true);dv.setUint16(32,2,true);"
      "dv.setUint16(34,16,true);S(36,'data');dv.setUint32(40,n*2,true);"
      "for(var i=0;i<n;i++)dv.setInt16(44+i*2,Math.round(Math.sin(i/10)*10000),true);"
      "var u8=new Uint8Array(buf),bin='';"
      "for(var i=0;i<u8.length;i++)bin+=String.fromCharCode(u8[i]);"
      "var a=document.createElement('audio');a.muted=true;var tu=0;"
      "a.addEventListener('timeupdate',function(){tu++;});"
      "a.addEventListener('ended',function(){window.__pb='ended:'+"
      "a.currentTime.toFixed(1)+',paused:'+a.paused+',tu:'+(tu>0);});"
      "a.addEventListener('canplaythrough',function(){"
      "var p=a.play();if(p&&p.catch)p.catch(function(e){window.__pb='playerr:'+e.name;});});"
      "a.src='data:audio/wav;base64,'+btoa(bin);a.load();"
      "}catch(e){window.__pb='throw:'+e;}");
    std::string pb;
    for (int i = 0; i < 400; ++i) {  // up to 10s; the 0.3s clip ends well before
      mbWait(v, 25);
      pb = Eval(v, "window.__pb");
      if (!pb.empty())
        break;
    }
    Expect(pb == "ended:0.3,paused:true,tu:true",
           "an <audio> element plays back: currentTime advances, timeupdate + ended fire",
           "pb=[" + pb + "]");
  }

  // 41g. <audio> error handling: an undecodable source must fire `error` (not hang) with
  // a media error set, so apps can fall back. The player reports networkState=FormatError
  // when AudioFileReader can't open the bytes; the element surfaces MediaError.
  {
    mbLoadHTML(v, "<body>aerr</body>", "about:blank");
    mbRunJS(v,
      "window.__ae='';var a=document.createElement('audio');"
      "a.addEventListener('error',function(){window.__ae='err:'+(a.error?a.error.code>0:false);});"
      "a.addEventListener('canplay',function(){window.__ae='unexpected-ok';});"
      "a.src='data:audio/wav;base64,'+btoa('not a real wav file at all');a.load();");
    std::string ae;
    for (int i = 0; i < 200; ++i) {
      mbWait(v, 25);
      ae = Eval(v, "window.__ae");
      if (!ae.empty())
        break;
    }
    Expect(ae == "err:true",
           "an <audio> element fires `error` with a MediaError on an undecodable source",
           "ae=[" + ae + "]");
  }

  // 41h. <audio> seek: currentTime can be set; the player re-anchors and the element
  // fires `seeked`, with currentTime reflecting the new position. (Paused seek — no
  // playback needed.)
  {
    mbLoadHTML(v, "<body>ask</body>", "about:blank");
    mbRunJS(v,
      "window.__sk='';try{var sr=8000,n=4000,buf=new ArrayBuffer(44+n*2),"
      "dv=new DataView(buf);"
      "function S(o,t){for(var i=0;i<t.length;i++)dv.setUint8(o+i,t.charCodeAt(i));}"
      "S(0,'RIFF');dv.setUint32(4,36+n*2,true);S(8,'WAVE');S(12,'fmt ');"
      "dv.setUint32(16,16,true);dv.setUint16(20,1,true);dv.setUint16(22,1,true);"
      "dv.setUint32(24,sr,true);dv.setUint32(28,sr*2,true);dv.setUint16(32,2,true);"
      "dv.setUint16(34,16,true);S(36,'data');dv.setUint32(40,n*2,true);"
      "var u8=new Uint8Array(buf),bin='';"
      "for(var i=0;i<u8.length;i++)bin+=String.fromCharCode(u8[i]);"
      "var a=document.createElement('audio');"
      "a.addEventListener('seeked',function(){"
      "window.__sk='seeked:'+a.currentTime.toFixed(2);});"
      "a.addEventListener('canplaythrough',function(){a.currentTime=0.25;});"
      "a.src='data:audio/wav;base64,'+btoa(bin);a.load();"
      "}catch(e){window.__sk='throw:'+e;}");
    std::string sk;
    for (int i = 0; i < 200; ++i) {
      mbWait(v, 25);
      sk = Eval(v, "window.__sk");
      if (!sk.empty())
        break;
    }
    Expect(sk == "seeked:0.25",
           "an <audio> element seeks: currentTime set -> seeked fires at the new position",
           "sk=[" + sk + "]");
  }

  // 41i. A <video> element reports video METADATA (dimensions + duration) via FFmpeg
  // container parsing (no frame decode): a tiny VP8 webm -> loadedmetadata fires,
  // videoWidth/videoHeight > 0 and duration > 0. The custom player reads the video
  // stream coded width/height + container duration (FFmpegGlue + avformat_find_stream_
  // info). Frame painting is a follow-on; this is the metadata brick (the element loads
  // + sizes correctly instead of erroring on a video-only file).
  {
    mbLoadHTML(v, "<body>vid</body>", "about:blank");
    mbRunJS(v,
      "window.__vd='';var vd=document.createElement('video');"
      "vd.addEventListener('loadedmetadata',function(){window.__vd='w:'+"
      "(vd.videoWidth>0)+',h:'+(vd.videoHeight>0)+',dur:'+(vd.duration>0);});"
      "vd.addEventListener('error',function(){window.__vd='err:'+(vd.error?vd.error.code:'?');});"
      "vd.src='data:video/webm;base64,"
      R"B64(GkXfo59ChoEBQveBAULygQRC84EIQoKEd2VibUKHgQJChYECGFOAZwEAAAAAAAlFEU2bdLpNu4tTq4QVSalmU6yBoU27i1OrhBZUrmtTrIHYTbuMU6uEElTDZ1OsggEyTbuMU6uEHFO7a1Osggkv7AEAAAAAAABZAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAVSalmsirXsYMPQkBNgI1MYXZmNjAuMTYuMTAwV0GNTGF2ZjYwLjE2LjEwMESJiECfQAAAAAAAFlSua9WuAQAAAAAAAEzXgQFzxYia5fG0kqblX5yBACK1nIN1bmSIgQCGhVZfVlA4g4EBI+ODhAJiWgDgnbCCAUC6gfCagQJVsJBVuoEGVbGBBlW7gQZVuYEBElTDZ/xzc6BjwIBnyJpFo4dFTkNPREVSRIeNTGF2ZjYwLjE2LjEwMHNz1mPAi2PFiJrl8bSSpuVfZ8ihRaOHRU5DT0RFUkSHlExhdmM2MC4zMS4xMDIgbGlidnB4Z8ihRaOIRFVSQVRJT05Eh5MwMDowMDowMi4wMDAwMDAwMDAAH0O2dUd254EAo0FBgQAAgLAZAJ0BKkAB8AAARwiFhYiFhIgCAgJ11Qv6r+APKVZN5L+AP6V8YL2B/SDJ/7wH6A/wHEAf0B9/+7M/0BDOvyNdTe2kAOhssA11N7iM8FbiLywDXU3vpKf8sbEXlgGwkKEX/yxsReWC9ReWAa6m9xF8rFjYi8sA11N/st1N7aQpXKtwTRwOjvNUlj52/QP/R8q3BJM+NiLyjZjeEZpnl4llgGuqu3VupvcReWAcO+WAa6m9xF5ZwdDZYBrqb3EZ4K3EXlgGupvfSU/5Y2IvLANhIUIv/eAA/v8TQv6yqGPcwQRXjDp/8VrvQ2qcnhkpmvt2j5Y////6oC66ZjiEcEcC5FYSDFeSkq3RmH6Bc7f8AAAABEgEt/+vgEu5xzOlCBr1pYgIHuPMpafdVvCdAAAAAAAAAADuGs4AAAAAo56BACgA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEAUADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQB4ANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BAKAA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEAyADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQDwANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo62BARgAMQMABRAQFGAmPwKSjQ68AQ4ABc4TxgsFq1yTzAAAAAT4AMOAAAAAAACjnoEBQADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQFoANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BAZAA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEBuADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQHgANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BAggA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoECMADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQJYANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BAoAA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoECqADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQLQANECAAUQEBRgAGFgv9AAIgAQzX61yT5xzAAAo56BAvgA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEDIADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQNIANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BA3AA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEDmADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQPAANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BA+gA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEEEADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQQ4ANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BBGAA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEEiADRAgAFEBAUYABhYL/QACIAEM1+tck+ccwAAKOegQSwANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BBNgA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEFAADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQUoANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BBVAA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEFeADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQWgANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BBcgA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEF8ADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQYYANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BBkAA0QIABRAQFGAAYWC/0AAiABDNfrXJPnHMAACjnoEGaADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQaQANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BBrgA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEG4ADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQcIANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BBzAA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEHWADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQeAANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BB6gA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAAAcU7trkbuPs4EAt4r3gQHxggGz8IED)B64"
      "';vd.load();");
    std::string vd;
    for (int i = 0; i < 200; ++i) {
      mbWait(v, 25);
      vd = Eval(v, "window.__vd");
      if (!vd.empty())
        break;
    }
    Expect(vd == "w:true,h:true,dur:true",
           "a <video> element reports video metadata (dimensions + duration via FFmpeg)",
           "vd=[" + vd + "]");
  }

  // 41j. A <video> FRAME DECODES + paints (media step 3b, the visible payoff): the
  // player decodes the first video frame (libvpx via media::VideoThumbnailDecoder) and
  // exposes it through GetCurrentFrameThenUpdate, so drawImage(video) paints it onto a
  // 2D canvas -> readback shows an OPAQUE pixel (a real frame, not transparent).
  {
    mbLoadHTML(v, "<body>vf</body>", "about:blank");
    mbRunJS(v,
      "window.__vf='';var vd=document.createElement('video');"
      "vd.addEventListener('loadeddata',function(){try{"
      "var c=document.createElement('canvas');c.width=16;c.height=16;"
      "var x=c.getContext('2d');x.drawImage(vd,0,0,16,16);"
      "var d=x.getImageData(8,8,1,1).data;"
      "window.__vf='a:'+d[3]+',sum:'+(d[0]+d[1]+d[2]);}"
      "catch(e){window.__vf='drawerr:'+e;}});"
      "vd.addEventListener('error',function(){window.__vf='err:'+(vd.error?vd.error.code:'?');});"
      "vd.src='data:video/webm;base64,"
      R"B64(GkXfo59ChoEBQveBAULygQRC84EIQoKEd2VibUKHgQJChYECGFOAZwEAAAAAAAlFEU2bdLpNu4tTq4QVSalmU6yBoU27i1OrhBZUrmtTrIHYTbuMU6uEElTDZ1OsggEyTbuMU6uEHFO7a1Osggkv7AEAAAAAAABZAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAVSalmsirXsYMPQkBNgI1MYXZmNjAuMTYuMTAwV0GNTGF2ZjYwLjE2LjEwMESJiECfQAAAAAAAFlSua9WuAQAAAAAAAEzXgQFzxYia5fG0kqblX5yBACK1nIN1bmSIgQCGhVZfVlA4g4EBI+ODhAJiWgDgnbCCAUC6gfCagQJVsJBVuoEGVbGBBlW7gQZVuYEBElTDZ/xzc6BjwIBnyJpFo4dFTkNPREVSRIeNTGF2ZjYwLjE2LjEwMHNz1mPAi2PFiJrl8bSSpuVfZ8ihRaOHRU5DT0RFUkSHlExhdmM2MC4zMS4xMDIgbGlidnB4Z8ihRaOIRFVSQVRJT05Eh5MwMDowMDowMi4wMDAwMDAwMDAAH0O2dUd254EAo0FBgQAAgLAZAJ0BKkAB8AAARwiFhYiFhIgCAgJ11Qv6r+APKVZN5L+AP6V8YL2B/SDJ/7wH6A/wHEAf0B9/+7M/0BDOvyNdTe2kAOhssA11N7iM8FbiLywDXU3vpKf8sbEXlgGwkKEX/yxsReWC9ReWAa6m9xF8rFjYi8sA11N/st1N7aQpXKtwTRwOjvNUlj52/QP/R8q3BJM+NiLyjZjeEZpnl4llgGuqu3VupvcReWAcO+WAa6m9xF5ZwdDZYBrqb3EZ4K3EXlgGupvfSU/5Y2IvLANhIUIv/eAA/v8TQv6yqGPcwQRXjDp/8VrvQ2qcnhkpmvt2j5Y////6oC66ZjiEcEcC5FYSDFeSkq3RmH6Bc7f8AAAABEgEt/+vgEu5xzOlCBr1pYgIHuPMpafdVvCdAAAAAAAAAADuGs4AAAAAo56BACgA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEAUADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQB4ANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BAKAA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEAyADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQDwANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo62BARgAMQMABRAQFGAmPwKSjQ68AQ4ABc4TxgsFq1yTzAAAAAT4AMOAAAAAAACjnoEBQADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQFoANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BAZAA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEBuADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQHgANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BAggA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoECMADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQJYANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BAoAA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoECqADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQLQANECAAUQEBRgAGFgv9AAIgAQzX61yT5xzAAAo56BAvgA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEDIADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQNIANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BA3AA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEDmADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQPAANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BA+gA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEEEADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQQ4ANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BBGAA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEEiADRAgAFEBAUYABhYL/QACIAEM1+tck+ccwAAKOegQSwANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BBNgA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEFAADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQUoANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BBVAA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEFeADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQWgANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BBcgA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEF8ADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQYYANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BBkAA0QIABRAQFGAAYWC/0AAiABDNfrXJPnHMAACjnoEGaADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQaQANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BBrgA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEG4ADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQcIANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BBzAA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEHWADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQeAANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BB6gA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAAAcU7trkbuPs4EAt4r3gQHxggGz8IED)B64"
      "';vd.load();");
    std::string vf;
    for (int i = 0; i < 300; ++i) {
      mbWait(v, 25);
      vf = Eval(v, "window.__vf");
      if (!vf.empty())
        break;
    }
    // Opaque alpha (255) => a real decoded frame was painted (not a transparent miss).
    Expect(vf.rfind("a:255,", 0) == 0,
           "a <video> frame decodes (libvpx) + drawImage(video) paints it to a canvas",
           "vf=[" + vf + "]");
  }

  // 41k. A <video> element in the PAGE shows its frame in a screenshot (mbPaintToBitmap),
  // not just via drawImage onto a canvas. Like a WebGL canvas, a <video> is normally
  // composited (a cc_layer the absent compositor would draw); this checks whether the
  // decoded frame reaches the software page paint. (If it does not, the fix is the same
  // class as the WebGL screenshot-compositing one.)
  {
    mbLoadHTML(v,
      "<body style='margin:0;background:#fff'>"
      "<video id='pv' width='64' height='64'></video>"
      "<script>window.__pv='';var pv=document.getElementById('pv');"
      "pv.addEventListener('loadeddata',function(){window.__pv='ready';});"
      "pv.src='data:video/webm;base64,"
      R"B64(GkXfo59ChoEBQveBAULygQRC84EIQoKEd2VibUKHgQJChYECGFOAZwEAAAAAAAlFEU2bdLpNu4tTq4QVSalmU6yBoU27i1OrhBZUrmtTrIHYTbuMU6uEElTDZ1OsggEyTbuMU6uEHFO7a1Osggkv7AEAAAAAAABZAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAVSalmsirXsYMPQkBNgI1MYXZmNjAuMTYuMTAwV0GNTGF2ZjYwLjE2LjEwMESJiECfQAAAAAAAFlSua9WuAQAAAAAAAEzXgQFzxYia5fG0kqblX5yBACK1nIN1bmSIgQCGhVZfVlA4g4EBI+ODhAJiWgDgnbCCAUC6gfCagQJVsJBVuoEGVbGBBlW7gQZVuYEBElTDZ/xzc6BjwIBnyJpFo4dFTkNPREVSRIeNTGF2ZjYwLjE2LjEwMHNz1mPAi2PFiJrl8bSSpuVfZ8ihRaOHRU5DT0RFUkSHlExhdmM2MC4zMS4xMDIgbGlidnB4Z8ihRaOIRFVSQVRJT05Eh5MwMDowMDowMi4wMDAwMDAwMDAAH0O2dUd254EAo0FBgQAAgLAZAJ0BKkAB8AAARwiFhYiFhIgCAgJ11Qv6r+APKVZN5L+AP6V8YL2B/SDJ/7wH6A/wHEAf0B9/+7M/0BDOvyNdTe2kAOhssA11N7iM8FbiLywDXU3vpKf8sbEXlgGwkKEX/yxsReWC9ReWAa6m9xF8rFjYi8sA11N/st1N7aQpXKtwTRwOjvNUlj52/QP/R8q3BJM+NiLyjZjeEZpnl4llgGuqu3VupvcReWAcO+WAa6m9xF5ZwdDZYBrqb3EZ4K3EXlgGupvfSU/5Y2IvLANhIUIv/eAA/v8TQv6yqGPcwQRXjDp/8VrvQ2qcnhkpmvt2j5Y////6oC66ZjiEcEcC5FYSDFeSkq3RmH6Bc7f8AAAABEgEt/+vgEu5xzOlCBr1pYgIHuPMpafdVvCdAAAAAAAAAADuGs4AAAAAo56BACgA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEAUADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQB4ANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BAKAA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEAyADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQDwANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo62BARgAMQMABRAQFGAmPwKSjQ68AQ4ABc4TxgsFq1yTzAAAAAT4AMOAAAAAAACjnoEBQADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQFoANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BAZAA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEBuADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQHgANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BAggA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoECMADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQJYANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BAoAA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoECqADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQLQANECAAUQEBRgAGFgv9AAIgAQzX61yT5xzAAAo56BAvgA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEDIADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQNIANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BA3AA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEDmADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQPAANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BA+gA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEEEADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQQ4ANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BBGAA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEEiADRAgAFEBAUYABhYL/QACIAEM1+tck+ccwAAKOegQSwANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BBNgA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEFAADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQUoANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BBVAA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEFeADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQWgANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BBcgA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEF8ADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQYYANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BBkAA0QIABRAQFGAAYWC/0AAiABDNfrXJPnHMAACjnoEGaADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQaQANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BBrgA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEG4ADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQcIANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BBzAA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEHWADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQeAANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BB6gA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAAAcU7trkbuPs4EAt4r3gQHxggGz8IED)B64"
      "';pv.load();</script></body>",
      "about:blank");
    std::string pv;
    for (int i = 0; i < 300; ++i) {
      mbWait(v, 25);
      pv = Eval(v, "window.__pv");
      if (pv == "ready")
        break;
    }
    std::vector<uint8_t> cv(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, cv.data(), W, H, W * 4);
    size_t ci = (32u * W + 32u) * 4;  // inside the 64x64 video box
    // Opaque + not the white background => the video frame rasterized into the page.
    bool painted = cv[ci + 3] == 255 &&
                   !(cv[ci] == 255 && cv[ci + 1] == 255 && cv[ci + 2] == 255);
    Expect(painted,
           "a <video> element paints its frame into the page screenshot (mbPaintToBitmap)",
           "ready=[" + pv + "] B=" + std::to_string(cv[ci]) + " G=" +
               std::to_string(cv[ci + 1]) + " R=" + std::to_string(cv[ci + 2]) +
               " A=" + std::to_string(cv[ci + 3]));
  }

  // 41q. PER-currentTime FRAME STEPPING: the player decodes the WHOLE video stream
  // (libvpx, all packets) and indexes frames by presentation timestamp, so the picture
  // changes with currentTime. We draw the frame at t=0, seek near the end, draw again,
  // and assert the two frames differ (the asset has distinct frames over its 2s timeline).
  {
    mbLoadHTML(v, "<body>fs</body>", "about:blank");
    mbRunJS(v,
      "window.__fs='';var vd=document.createElement('video');"
      "function sig(){var c=document.createElement('canvas');c.width=320;c.height=240;"
      "var x=c.getContext('2d');x.drawImage(vd,0,0,320,240);"
      "var d=x.getImageData(0,110,320,12).data,h=0;"
      "for(var i=0;i<d.length;i++){h=((h*31+d[i])>>>0);}return h;}"
      "vd.addEventListener('loadeddata',function(){"
      "window.__s0=sig();vd.currentTime=1.8;});"
      "vd.addEventListener('seeked',function(){"
      "window.__s1=sig();"
      "window.__fs='s0='+window.__s0+',s1='+window.__s1+',diff='+(window.__s0!==window.__s1);});"
      "vd.addEventListener('error',function(){window.__fs='err:'+(vd.error?vd.error.code:'?');});"
      "vd.src='data:video/webm;base64,"
      R"B64(GkXfo59ChoEBQveBAULygQRC84EIQoKEd2VibUKHgQJChYECGFOAZwEAAAAAAAlFEU2bdLpNu4tTq4QVSalmU6yBoU27i1OrhBZUrmtTrIHYTbuMU6uEElTDZ1OsggEyTbuMU6uEHFO7a1Osggkv7AEAAAAAAABZAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAVSalmsirXsYMPQkBNgI1MYXZmNjAuMTYuMTAwV0GNTGF2ZjYwLjE2LjEwMESJiECfQAAAAAAAFlSua9WuAQAAAAAAAEzXgQFzxYia5fG0kqblX5yBACK1nIN1bmSIgQCGhVZfVlA4g4EBI+ODhAJiWgDgnbCCAUC6gfCagQJVsJBVuoEGVbGBBlW7gQZVuYEBElTDZ/xzc6BjwIBnyJpFo4dFTkNPREVSRIeNTGF2ZjYwLjE2LjEwMHNz1mPAi2PFiJrl8bSSpuVfZ8ihRaOHRU5DT0RFUkSHlExhdmM2MC4zMS4xMDIgbGlidnB4Z8ihRaOIRFVSQVRJT05Eh5MwMDowMDowMi4wMDAwMDAwMDAAH0O2dUd254EAo0FBgQAAgLAZAJ0BKkAB8AAARwiFhYiFhIgCAgJ11Qv6r+APKVZN5L+AP6V8YL2B/SDJ/7wH6A/wHEAf0B9/+7M/0BDOvyNdTe2kAOhssA11N7iM8FbiLywDXU3vpKf8sbEXlgGwkKEX/yxsReWC9ReWAa6m9xF8rFjYi8sA11N/st1N7aQpXKtwTRwOjvNUlj52/QP/R8q3BJM+NiLyjZjeEZpnl4llgGuqu3VupvcReWAcO+WAa6m9xF5ZwdDZYBrqb3EZ4K3EXlgGupvfSU/5Y2IvLANhIUIv/eAA/v8TQv6yqGPcwQRXjDp/8VrvQ2qcnhkpmvt2j5Y////6oC66ZjiEcEcC5FYSDFeSkq3RmH6Bc7f8AAAABEgEt/+vgEu5xzOlCBr1pYgIHuPMpafdVvCdAAAAAAAAAADuGs4AAAAAo56BACgA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEAUADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQB4ANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BAKAA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEAyADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQDwANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo62BARgAMQMABRAQFGAmPwKSjQ68AQ4ABc4TxgsFq1yTzAAAAAT4AMOAAAAAAACjnoEBQADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQFoANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BAZAA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEBuADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQHgANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BAggA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoECMADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQJYANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BAoAA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoECqADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQLQANECAAUQEBRgAGFgv9AAIgAQzX61yT5xzAAAo56BAvgA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEDIADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQNIANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BA3AA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEDmADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQPAANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BA+gA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEEEADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQQ4ANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BBGAA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEEiADRAgAFEBAUYABhYL/QACIAEM1+tck+ccwAAKOegQSwANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BBNgA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEFAADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQUoANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BBVAA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEFeADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQWgANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BBcgA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEF8ADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQYYANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BBkAA0QIABRAQFGAAYWC/0AAiABDNfrXJPnHMAACjnoEGaADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQaQANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BBrgA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEG4ADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQcIANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BBzAA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAACjnoEHWADRAgAFEKwAGAAYWC/0AAiABDNfrXJPnHMAAKOegQeAANECAAUQrAAYABhYL/QACIAEM1+tck+ccwAAo56BB6gA0QIABRCsABgAGFgv9AAIgAQzX61yT5xzAAAcU7trkbuPs4EAt4r3gQHxggGz8IED)B64"
      "';vd.load();");
    std::string fs;
    for (int i = 0; i < 400; ++i) {
      mbWait(v, 25);
      fs = Eval(v, "window.__fs");
      if (!fs.empty())
        break;
    }
    Expect(fs.find(",diff=true") != std::string::npos,
           "per-currentTime frame stepping: seeking to a later time shows a different frame",
           "fs=[" + fs + "]");
  }

  // 41l. Media loads from a file:// URL (real binary, not a data: URL): write a small
  // WAV to disk and load it into an <audio>; the player fetches it (MbFetchUrl file://)
  // and decodes it (FFmpeg) -> duration. The page is also at file:// so it is same-origin
  // with the media. Confirms media isn't data:-only (the common <audio src="...file">).
  {
    const char* wav_path = "/tmp/mb_media_test.wav";
    if (FILE* f = std::fopen(wav_path, "wb")) {
      const uint32_t sr = 8000, n = 800, datalen = n * 2;
      auto w32 = [&](uint32_t x) {
        uint8_t b[4] = {uint8_t(x), uint8_t(x >> 8), uint8_t(x >> 16),
                        uint8_t(x >> 24)};
        std::fwrite(b, 1, 4, f);
      };
      auto w16 = [&](uint16_t x) {
        uint8_t b[2] = {uint8_t(x), uint8_t(x >> 8)};
        std::fwrite(b, 1, 2, f);
      };
      std::fwrite("RIFF", 1, 4, f); w32(36 + datalen); std::fwrite("WAVE", 1, 4, f);
      std::fwrite("fmt ", 1, 4, f); w32(16); w16(1); w16(1);
      w32(sr); w32(sr * 2); w16(2); w16(16);
      std::fwrite("data", 1, 4, f); w32(datalen);
      for (uint32_t i = 0; i < n; ++i)
        w16(static_cast<uint16_t>(static_cast<int16_t>(std::sin(i / 10.0) * 10000)));
      std::fclose(f);
    }
    mbLoadHTML(v, "<body>fau</body>", "file:///tmp/mb_media_page.html");
    mbRunJS(v,
      "window.__fa='';var a=document.createElement('audio');"
      "a.addEventListener('loadedmetadata',function(){window.__fa='dur:'+a.duration.toFixed(2);});"
      "a.addEventListener('error',function(){window.__fa='err:'+(a.error?a.error.code:'?');});"
      "a.src='file:///tmp/mb_media_test.wav';a.load();");
    std::string fa;
    for (int i = 0; i < 200; ++i) {
      mbWait(v, 25);
      fa = Eval(v, "window.__fa");
      if (!fa.empty())
        break;
    }
    Expect(fa == "dur:0.10",
           "an <audio> element loads + decodes from a file:// URL (real binary, not data:)",
           "fa=[" + fa + "]");
  }

  // 41m. WebCodecs encode + decode round-trip: encode a canvas frame to VP8
  // (VideoEncoder, libvpx) then decode the chunk back (VideoDecoder) -> a VideoFrame of
  // the same size. WebCodecs is a major modern API (low-level codec access for JS); this
  // proves the platform-less in-process media encoders/decoders are reachable from JS.
  {
    mbLoadHTML(v, "<body>wc</body>", "https://wc.test/");
    mbRunJS(v,
      "window.__wc='';(async function(){try{"
      "var cn=document.createElement('canvas');cn.width=32;cn.height=32;"
      "var cx=cn.getContext('2d');cx.fillStyle='#00ff00';cx.fillRect(0,0,32,32);"
      "var fr=new VideoFrame(cn,{timestamp:0});"
      "var chunks=[];"
      "var enc=new VideoEncoder({output:function(c){chunks.push(c);},"
      "error:function(e){window.__wc='encerr:'+e.message;}});"
      "enc.configure({codec:'vp8',width:32,height:32});"
      "enc.encode(fr,{keyFrame:true});fr.close();await enc.flush();"
      "if(!chunks.length){window.__wc='noenc';return;}"
      "var dec=new VideoDecoder({output:function(f){"
      "window.__wc='decoded:'+f.codedWidth+'x'+f.codedHeight;f.close();},"
      "error:function(e){window.__wc='decerr:'+e.message;}});"
      "dec.configure({codec:'vp8'});dec.decode(chunks[0]);await dec.flush();"
      "}catch(e){window.__wc='throw:'+e;}})();");
    std::string wc;
    for (int i = 0; i < 300; ++i) {
      mbWait(v, 25);
      wc = Eval(v, "window.__wc");
      if (!wc.empty())
        break;
    }
    Expect(wc == "decoded:32x32",
           "WebCodecs VideoEncoder->VideoDecoder round-trips a frame (vp8, in-process)",
           "wc=[" + wc + "]");
  }

  // 41n. navigator.gpu WORKS end to end (WebGPU bring-up milestone C2): requestAdapter()
  // resolves to a REAL adapter and requestDevice() to a real device, backed by the
  // in-process Dawn-over-SwiftShader provider (MbPlatform::CreateWebGPUGraphicsContext3D-
  // ProviderAsync -> mb_webgpu.cc). Before C2 this settled to null (graceful degradation);
  // now a page's WebGPU path runs for real. The adapter info reports the SwiftShader CPU
  // device. (Both requestAdapter and requestDevice are awaited, so this also guards against
  // a regression to the old hang.)
  {
    mbLoadHTML(v, "<body>wg</body>", "https://wg.test/");
    mbRunJS(v,
      "window.__wg='pending';(async function(){try{"
      "var a=await navigator.gpu.requestAdapter();"
      "if(!a){window.__wg='null';return;}"
      "var d=await a.requestDevice();"
      "var info=a.info||{};"
      "window.__wgvendor=String(info.vendor||'');"
      "window.__wgarch=String(info.architecture||'');"
      "window.__wg=d?'device':'adapter-only';"
      "}catch(e){window.__wg='err:'+e.name;}})();");
    std::string wg;
    for (int i = 0; i < 240; ++i) {
      mbWait(v, 25);
      wg = Eval(v, "window.__wg");
      if (wg != "pending" && !wg.empty())
        break;
    }
    const std::string vendor = Eval(v, "window.__wgvendor");
    const std::string arch = Eval(v, "window.__wgarch");
    Expect(wg == "device",
           "navigator.gpu.requestAdapter()+requestDevice() return a real adapter+device",
           "wg=[" + wg + "] vendor=[" + vendor + "] arch=[" + arch + "]");
  }

  // 41n2. Compositor-ADJACENT CSS/animation features work (no crash/hang) without the
  // compositor: scroll-driven animations (ScrollTimeline — element.animate with a scroll
  // timeline runs), Houdini @property (a registered '<length>' custom property applies its
  // typed value), and the Web Animations registry (getAnimations). These are the same class
  // as View Transitions (which DID hang) — this guards that they keep degrading gracefully.
  {
    mbLoadHTML(v,
      "<body style='margin:0'>"
      "<style>@property --p{syntax:'<length>';inherits:false;initial-value:0px}"
      "#hp{--p:5px;width:var(--p)}</style>"
      "<div style='height:3000px'></div><div id='sda'>s</div><div id='hp'>h</div>"
      "<script>window.__r={};"
      "try{var a=document.getElementById('sda').animate({opacity:[1,.3]},"
      "{timeline:new ScrollTimeline({source:document.documentElement})});"
      "window.__r.st=a?a.playState:'noanim';}catch(e){window.__r.st='err:'+e.name;}"
      "try{window.__r.prop=getComputedStyle(document.getElementById('hp')).width;}catch(e){window.__r.prop='err:'+e.name;}"
      "try{document.getElementById('hp').animate([{opacity:1},{opacity:0}],1000);"
      "window.__r.ga=document.getAnimations().length;}catch(e){window.__r.ga='err:'+e.name;}"
      "</script></body>","https://anim.test/");
    mbWait(v, 200);
    const std::string st = Eval(v, "String(window.__r.st)");
    const std::string prop = Eval(v, "String(window.__r.prop)");
    const std::string ga = Eval(v, "String(window.__r.ga)");
    Expect((st == "running" || st == "paused" || st == "finished") && prop == "5px" &&
               std::atoi(ga.c_str()) >= 1,
           "scroll-driven animations + Houdini @property + getAnimations work (no compositor)",
           "st=[" + st + "] prop=[" + prop + "] ga=[" + ga + "]");
  }

  // 41z-zoom. PAGE ZOOM (mbSetZoomFactor) re-lays out the whole page bigger and the change
  // shows in a SCREENSHOT — the webview Ctrl+/Ctrl- feature. A 40x40 box at the top-left:
  // at 100% the pixel (60,60) is the white background (outside the box); at 200% the box is
  // 80x80 device px so (60,60) falls INSIDE it (box color). Verifies zoom affects rendering.
  {
    mbLoadHTML(v,
        "<body style='margin:0;background:#fff'>"
        "<div style='position:absolute;top:0;left:0;width:40px;height:40px;"
        "background:rgb(20,180,40)'></div></body>",
        "https://zoom.test/");
    mbWait(v, 60);
    auto box_at = [&](int x, int y) {
      std::vector<uint8_t> cv(static_cast<size_t>(W) * H * 4, 0);
      mbPaintToBitmap(v, cv.data(), W, H, W * 4);
      size_t i = (static_cast<size_t>(y) * W + x) * 4;
      // box is rgb(20,180,40) -> BGRA B=40,G=180,R=20 (green-ish), not white.
      return cv[i + 1] > 120 && cv[i] < 120 && cv[i + 2] < 120;
    };
    mbSetZoomFactor(v, 1.0f);
    mbWait(v, 40);
    const bool at100 = box_at(60, 60);   // outside the 40px box -> false
    mbSetZoomFactor(v, 2.0f);
    mbWait(v, 60);
    const bool at200 = box_at(60, 60);   // inside the 80px box -> true
    const float gz = mbGetZoomFactor(v);
    mbSetZoomFactor(v, 1.0f);            // reset
    Expect(!at100 && at200 && gz == 2.0f,
           "mbSetZoomFactor zooms the page (a box doubles in the screenshot at 200%)",
           "at100=" + std::string(at100 ? "1" : "0") + " at200=" +
               (at200 ? "1" : "0") + " gz=" + std::to_string(gz));
  }

  // 41o0. View Transitions DEGRADE GRACEFULLY (patch 0009): document.startViewTransition()
  // needs the compositor to capture/animate snapshots; with our non-compositing widget the
  // capture never completes, so ready/finished hung FOREVER (probed: neither settled in 5s).
  // The patch makes blink skip the transition when the frame can't composite (no
  // AnimationHost) — like its existing no-View skip — so the DOM-update callback STILL RUNS
  // and the promises settle promptly. A page using View Transitions no longer hangs.
  {
    mbLoadHTML(v,
      "<body><h1 id='t'>before</h1><script>"
      "window.__vt='';window.__upd='';"
      "if(document.startViewTransition){"
      "var tr=document.startViewTransition(function(){"
      "document.getElementById('t').textContent='after';window.__upd='ran';});"
      "tr.finished.then(function(){window.__vt='finished';},"
      "function(e){window.__vt='rej:'+(e&&e.name?e.name:e);});"
      "tr.updateCallbackDone.then(function(){window.__ucb='1';},function(){window.__ucb='rej';});"
      "}else{window.__vt='absent';}"
      "</script></body>","https://vt.test/");
    std::string vt;
    for (int i = 0; i < 120 && vt.empty(); ++i) { mbWait(v, 25); vt = Eval(v, "String(window.__vt)"); }
    const std::string upd = Eval(v, "String(window.__upd)");
    const std::string txt = Eval(v, "document.getElementById('t').textContent");
    // The promise settles (finished or a clean rejection — NOT a hang), the DOM update ran,
    // and the new content is live. (We accept either 'finished' or a 'rej:' skip outcome.)
    const bool settled = vt == "finished" || vt.rfind("rej:", 0) == 0;
    Expect(settled && upd == "ran" && txt == "after",
           "View Transitions degrade gracefully (no hang): callback runs, promise settles",
           "vt=[" + vt + "] upd=[" + upd + "] txt=[" + txt + "]");
  }

  // 41o1. MORE browser-backed promise APIs DEGRADE GRACEFULLY (no hang) — extends 41o's
  // "settle, don't hang" guarantee to the rest of the heavy hitters: requestFullscreen,
  // wakeLock.request, requestStorageAccess, getScreenDetails, setAppBadge, IdleDetector,
  // PaymentRequest.canMakePayment, credentials.get. None may sit forever pending (which would
  // freeze a page that awaits them). Each must reach a terminal state (resolved/rejected/
  // threw). (navigator.serviceWorker.ready is deliberately NOT here: with no SW backend it
  // stays pending — spec-compliant, same as a real browser with no registered worker.)
  {
    mbLoadHTML(v, "<body><div id='d'>x</div></body>", "https://hang2.test/");
    mbRunJS(v,
      "window.__h={};function S(k,p){window.__h[k]='pending';try{Promise.resolve(p).then("
      "function(){window.__h[k]='resolved';},function(e){window.__h[k]='rej:'+(e&&e.name||e);});}"
      "catch(e){window.__h[k]='threw';}}"
      "S('fullscreen',document.getElementById('d').requestFullscreen&&document.getElementById('d').requestFullscreen());"
      "S('wakelock',navigator.wakeLock&&navigator.wakeLock.request('screen'));"
      "S('storageaccess',document.requestStorageAccess&&document.requestStorageAccess());"
      "S('screendetails',window.getScreenDetails&&window.getScreenDetails());"
      "S('appbadge',navigator.setAppBadge&&navigator.setAppBadge(1));"
      "S('idle',window.IdleDetector&&IdleDetector.requestPermission&&IdleDetector.requestPermission());"
      "try{var pr=window.PaymentRequest&&new PaymentRequest([{supportedMethods:'basic-card'}],{total:{label:'t',amount:{currency:'USD',value:'1'}}});"
      "S('paycanmake',pr&&pr.canMakePayment());}catch(e){window.__h['paycanmake']='threw';}"
      "S('credentials',navigator.credentials&&navigator.credentials.get({}));");
    mbWait(v, 1500);
    const char* keys[] = {"fullscreen","wakelock","storageaccess","screendetails",
                          "appbadge","idle","paycanmake","credentials"};
    std::string report;
    bool any_pending = false;
    for (const char* k : keys) {
      std::string s = Eval(v, (std::string("String(window.__h['")+k+"']||'absent')").c_str());
      report += std::string(k) + "=" + s + " ";
      if (s == "pending" || s == "absent")
        any_pending = true;
    }
    Expect(!any_pending,
           "browser promise APIs all settle, none hang (fullscreen/wakelock/storage/screen/"
           "badge/idle/payment/credentials)",
           report);
  }

  // 41o. Browser-service-backed promise APIs DEGRADE GRACEFULLY — they SETTLE (resolve
  // or reject), never HANG. With no browser process these must not leave a page awaiting
  // forever (the worst failure mode — cf. the WebGPU requestAdapter hang fixed in 41n).
  // getUserMedia (no camera), showOpenFilePicker (no dialog), serviceWorker.register,
  // RTCPeerConnection.createOffer, WebTransport, and storage.estimate all settle.
  {
    mbLoadHTML(v, "<body>hp</body>", "https://hp.test/");
    mbRunJS(v,
      "window.__hp='';var names=['gum','ofp','sw','rtc','wt','est'];var r={};"
      "names.forEach(n=>r[n]='HANG');"
      "function s(n,v){if(r[n]==='HANG')r[n]=v;}"
      "function P(n,p){try{p.then(x=>s(n,'ok'),e=>s(n,'rej'));}catch(e){s(n,'throw');}}"
      "try{P('gum',navigator.mediaDevices.getUserMedia({video:true}));}catch(e){s('gum','throw');}"
      "try{P('ofp',window.showOpenFilePicker());}catch(e){s('ofp','throw');}"
      "try{P('sw',navigator.serviceWorker.register('/sw.js'));}catch(e){s('sw','throw');}"
      "try{P('rtc',new RTCPeerConnection().createOffer());}catch(e){s('rtc','throw');}"
      "try{P('wt',new WebTransport('https://hp.test/wt').ready);}catch(e){s('wt','throw');}"
      "try{P('est',navigator.storage.estimate());}catch(e){s('est','throw');}"
      "setTimeout(function(){window.__hp=names.map(n=>r[n]).join(',');},2000);");
    std::string hp;
    for (int i = 0; i < 200; ++i) {
      mbWait(v, 25);
      hp = Eval(v, "window.__hp");
      if (!hp.empty())
        break;
    }
    Expect(hp.find("HANG") == std::string::npos && !hp.empty(),
           "browser-backed promise APIs settle, never hang (graceful degradation)",
           "hp=[" + hp + "]");
  }

  // 41p. WebRTC SDP/negotiation layer works: an RTCPeerConnection with a data channel
  // produces a valid SDP offer (type 'offer', a media section for the data channel) and
  // accepts it as the local description. Full peer connectivity needs ICE/network, but
  // the SDP machinery — what apps build signaling on — runs in-process.
  {
    mbLoadHTML(v, "<body>rtc</body>", "https://rtc.test/");
    mbRunJS(v,
      "window.__rtc='';try{var pc=new RTCPeerConnection();pc.createDataChannel('chat');"
      "pc.createOffer().then(function(o){return pc.setLocalDescription(o).then(function(){"
      "var dc=(o.sdp.indexOf('webrtc-datachannel')>=0||o.sdp.indexOf('m=application')>=0);"
      "window.__rtc='type:'+o.type+',dc:'+dc+',local:'+(pc.localDescription?true:false);});},"
      "function(e){window.__rtc='rej:'+e.name;});}catch(e){window.__rtc='throw:'+e.name;}");
    std::string rtc;
    for (int i = 0; i < 200; ++i) {
      mbWait(v, 25);
      rtc = Eval(v, "window.__rtc");
      if (!rtc.empty())
        break;
    }
    Expect(rtc == "type:offer,dc:true,local:true",
           "WebRTC: RTCPeerConnection generates a data-channel SDP offer + sets it local",
           "rtc=[" + rtc + "]");
  }

  // 41q. WebAssembly works end to end (a major modern feature, previously untested): the
  // canonical `add(i32,i32)` module compiles + instantiates via BOTH WebAssembly.instantiate
  // (raw bytes, pure V8) AND instantiateStreaming (fetch of an application/wasm Response —
  // exercises the streaming-compile + MIME path), and the exported function computes.
  {
    mbLoadHTML(v, "<body>wasm</body>", "https://wasm.test/");
    mbRunJS(v,
      "window.__wasm='';(async function(){try{"
      // (module (func (export \"add\") (param i32 i32) (result i32) local.get 0 local.get 1 i32.add))
      "var bytes=new Uint8Array([0,97,115,109,1,0,0,0,1,7,1,96,2,127,127,1,127,3,2,1,0,"
      "7,7,1,3,97,100,100,0,0,10,9,1,7,0,32,0,32,1,106,11]);"
      "var m=await WebAssembly.instantiate(bytes);var r1=m.instance.exports.add(2,3);"
      "var b64=btoa(String.fromCharCode.apply(null,bytes));"
      "var m2=await WebAssembly.instantiateStreaming("
      "fetch('data:application/wasm;base64,'+b64));"
      "var r2=m2.instance.exports.add(20,22);"
      "window.__wasm='inst='+r1+' stream='+r2;"
      "}catch(e){window.__wasm='err:'+(e&&e.message?e.message:e);}})();");
    std::string w;
    for (int i = 0; i < 200 && w.empty(); ++i) { mbWait(v, 25); w = Eval(v, "window.__wasm"); }
    Expect(w == "inst=5 stream=42",
           "WebAssembly: instantiate + instantiateStreaming compile and run an exported func",
           "wasm=[" + w + "]");
  }

  // 42. Drawing to a canvas via mbEvalJS (not just mbRunJS) must also be
  // crash-safe. EvalToString/EvalIsolated used to run ExecuteScript
  // synchronously, so a draw inside an eval expression hit the same
  // CanvasPerformanceMonitor NOTREACHED as the old RunJS path. Both now run the
  // script inside a scheduler task. This eval draws green and reads the pixel
  // back in one expression: if the eval-draw path regressed it would SIGABRT
  // before returning; success returns the green channel (255).
  mbLoadHTML(v, "<body>eval-draw</body>", "about:blank");
  Expect(Eval(v,
              "(function(){var c=document.createElement('canvas');"
              "c.width=4;c.height=4;var x=c.getContext('2d');"
              "x.fillStyle='#00ff00';x.fillRect(0,0,4,4);"
              "return x.getImageData(1,1,1,1).data[1];})()") == "255" &&
             Eval(v, "document.body.textContent") == "eval-draw",
         "canvas draw via mbEvalJS is crash-safe (task-bracketed)");

  // 43. Same guard in the ISOLATED world (mbEvalJSIsolated shares the DOM but
  // has its own globals; it routes through the same task-bracketing fix).
  Expect(EvalIso(v,
                 "(function(){var c=document.createElement('canvas');"
                 "c.width=2;c.height=2;var x=c.getContext('2d');"
                 "x.fillStyle='#0000ff';x.fillRect(0,0,2,2);"
                 "return x.getImageData(0,0,1,1).data[2];})()") == "255",
         "canvas draw via mbEvalJSIsolated is crash-safe (task-bracketed)");

  // 44. JS dialogs must NOT hang the host. alert()/confirm()/prompt() are
  // [Sync] mojo calls to LocalFrameHost (RunModal*Dialog); with no browser
  // process to service them they deadlock the main thread forever — and pages
  // call them during load, so this is a severe common-case hazard. The
  // 0002-suppress-js-dialogs patch auto-dismisses them (headless semantics):
  // alert returns, confirm/prompt return their "Cancel" defaults (false/null).
  // This calls all three INLINE DURING LOAD (the realistic hang path); if the
  // suppression regressed, the whole smoke run would hang here (caught by the
  // bounded watchdog). Asserts completion + the documented default values.
  mbLoadHTML(v,
    "<body>dlg<script>"
    "window.__a=alert('hi');"
    "window.__c=confirm('ok?');"
    "window.__p=prompt('name?','def');"
    "window.__done=true;"
    "</script></body>", "about:blank");
  Expect(Eval(v, "String(window.__done)") == "true" &&
             Eval(v, "String(window.__a)") == "undefined" &&
             Eval(v, "String(window.__c)") == "false" &&
             Eval(v, "String(window.__p)") == "null",
         "JS dialogs auto-dismiss, no hang (alert/confirm/prompt)");

  // 45. Clipboard is crash/hang-safe. ClipboardHost has [Sync] read methods
  // (ReadText/IsFormatAvailable/...) — the same deadlock class as the JS
  // dialogs — but Blink gates them behind permission/gesture, so page JS never
  // reaches the sync call without a backend: execCommand('copy'/'paste') return
  // false, and navigator.clipboard read/write reject (NotAllowedError). Verify
  // none of it hangs and the host stays scriptable. (A regression that made a
  // clipboard op block would hang the suite, caught by the watchdog.)
  mbLoadHTML(v, "<body><textarea id='t'>x</textarea></body>", "about:blank");
  mbRunJS(v,
    "window.__done=false;var t=document.getElementById('t');t.select();"
    "try{window.__copy=document.execCommand('copy');}catch(e){window.__copy='threw';}"
    "try{window.__paste=document.execCommand('paste');}catch(e){window.__paste='threw';}"
    "window.__clip=(typeof navigator.clipboard);"
    "try{if(navigator.clipboard&&navigator.clipboard.writeText)"
    "navigator.clipboard.writeText('x').then(function(){},function(){});}catch(e){}"
    "window.__done=true;");
  mbWait(v, 60);
  // The invariant is hang-safety: __done==true proves the whole script — copy,
  // paste, and the clipboard-API call — ran to completion without blocking, and
  // the host still evaluates JS. (Specific return values and clipboard
  // availability vary by origin/secure-context, so we don't pin them; we only
  // require execCommand returned a real boolean rather than hanging/throwing.)
  Expect(Eval(v, "1+1") == "2" &&
             Eval(v, "String(window.__done)") == "true" &&
             (Eval(v, "String(window.__copy)") == "false" ||
              Eval(v, "String(window.__copy)") == "true"),
         "clipboard ops degrade gracefully, no hang (copy/paste/clipboard API)");

  // 46. URL.createObjectURL must not hang. It is the last [Sync]-mojo hazard:
  // PublicURLManager::RegisterURL makes the [Sync] BlobURLStore.Register call on
  // a store bound through the frame's navigation-associated channel, which no
  // browser process services — so any page calling createObjectURL() used to
  // deadlock the host forever (confirmed: mb_shot exit 137). Our in-process
  // MbBlobURLStore now services that [Sync] Register off-thread, so
  // createObjectURL returns a blob: URL without blocking AND the URL resolves to
  // data (see case 46b). Blob data ops (size/text/arrayBuffer/FileReader) were
  // always fine. This calls createObjectURL DURING LOAD (the realistic hang path)
  // and also revokes; a regression would hang the whole suite (watchdog catches it).
  mbLoadHTML(v,
    "<body>blob<script>"
    "var b=new Blob(['hello'],{type:'text/plain'});"
    "window.__sz=b.size;"
    "window.__u=URL.createObjectURL(b);"           // used to hang here
    "window.__isblob=(window.__u.indexOf('blob:')===0);"
    "try{URL.revokeObjectURL(window.__u);window.__rev=true;}catch(e){window.__rev=false;}"
    "window.__done=true;"
    "</script></body>", "about:blank");
  Expect(Eval(v, "String(window.__done)") == "true" &&
             Eval(v, "String(window.__sz)") == "5" &&
             Eval(v, "String(window.__isblob)") == "true" &&
             Eval(v, "String(window.__rev)") == "true",
         "URL.createObjectURL/revokeObjectURL no longer hang (blob: URL returned)");

  // 46b. Blob: URL resolution actually SERVES the bytes — not merely "returns a
  // blob: URL without hanging" (case 46). The in-process MbBlobURLStore
  // (ResolveAsURLLoaderFactory) resolves blob: URLs, so createObjectURL + fetch
  // round-trips the content for BOTH inline (<=256 KB) and BytesProvider (>256 KB)
  // blobs. (This supersedes the old 0003-skip-blob-url-register behavior, where
  // the URL did not resolve.) Async — the script signals window.__bd when done.
  // The blob: URL [Sync] Register lands on the service thread, so in the busy
  // long-lived suite process the register->fetch ordering can transiently race
  // (a fresh single-shot process never sees it; verified 13/13 via mb_shot). We
  // retry the fetch a few times so the test stays deterministic when the product
  // is correct, yet still fails (BAD:<lengths>) if blob: URLs truly don't resolve.
  {
    mbLoadHTML(v,
      "<body><div id='r'>p</div><script>(async function(){"
      "var ok=false,last='';"
      "for(var i=0;i<25&&!ok;i++){try{"
      "var s=await (await fetch(URL.createObjectURL(new Blob(['hi blob'])))).text();"
      "var big='z'.repeat(300*1024);"   // > 256 KB inline cap -> BytesProvider path
      "var t=await (await fetch(URL.createObjectURL(new Blob([big])))).text();"
      "ok=(s==='hi blob'&&t===big);last='s='+s.length+',t='+t.length;"
      "}catch(e){last='THREW:'+e.name;break;}"
      "if(!ok)await new Promise(function(r){setTimeout(r,40);});}"
      "document.getElementById('r').textContent=ok?'OK':('BAD:'+last);"
      "window.__bd=true;})();</script></body>", "about:blank");
    const int ready = mbWaitForFunction(v, "window.__bd===true", 8000);
    const std::string r = Eval(v, "document.getElementById('r').textContent");
    Expect(ready == 1 && r == "OK",
           "blob: URL fetch resolves bytes (inline + BytesProvider >256 KB)",
           std::string("ready=") + std::to_string(ready) + " r=" + r);
  }

  // 47. Web Crypto works. crypto.subtle.* used to SIGSEGV: SubtleCrypto derefs
  // Platform::Current()->Crypto() unconditionally, and base Platform returns
  // null — so any crypto.subtle call crashed the host. MbPlatform now returns a
  // real BoringSSL-backed webcrypto::WebCryptoImpl. Verify the async digest
  // actually computes (SHA-256("abc") has a known value) and getRandomValues
  // fills bytes. (Secure origin: Web Crypto requires a secure context.)
  mbLoadHTML(v, "<body>crypto</body>", "https://miniblink.test/");
  mbRunJS(v,
    "window.__d='pending';var r=new Uint8Array(16);crypto.getRandomValues(r);"
    "window.__rnd=r.some(function(x){return x!==0;});"
    "crypto.subtle.digest('SHA-256',new TextEncoder().encode('abc')).then(function(h){"
    "var b=new Uint8Array(h),s='';for(var i=0;i<4;i++)s+=('0'+b[i].toString(16)).slice(-2);"
    "window.__d=s+':'+b.length;},function(e){window.__d='REJ:'+e.name;});");
  mbWait(v, 250);
  Expect(Eval(v, "String(window.__rnd)") == "true" &&
             Eval(v, "window.__d") == "ba7816bf:32",  // SHA-256("abc") prefix + len
         "Web Crypto: getRandomValues + subtle.digest(SHA-256) compute");

  // 48. Web Audio must not crash. `new AudioContext()` used to SIGSEGV: base
  // Platform::CreateAudioDevice returns null and AudioDestination's ctor derefs
  // it unguarded. MbPlatform now returns a silent stub device, so a realtime
  // AudioContext constructs and a graph can be wired (no sound, but no crash).
  // Also exercise OfflineAudioContext (always worked — renders to a buffer).
  // A regression would crash the suite before the assert.
  mbLoadHTML(v, "<body>audio</body>", "about:blank");
  mbRunJS(v,
    "window.__ok=false;"
    "var ac=new AudioContext();var o=ac.createOscillator();var g=ac.createGain();"
    "o.connect(g);g.connect(ac.destination);o.start();"
    "window.__sr=ac.sampleRate;window.__st=ac.state;"
    "var oc=new OfflineAudioContext(1,128,44100);window.__osr=oc.sampleRate;"
    "window.__ok=true;");
  mbWait(v, 40);
  Expect(Eval(v, "String(window.__ok)") == "true" &&
             Eval(v, "String(window.__sr)") == "48000" &&
             Eval(v, "String(window.__osr)") == "44100" &&
             Eval(v, "1+1") == "2",
         "Web Audio: AudioContext + OfflineAudioContext construct, no crash");

  // 49. Streams API actually moves data (not just constructs). A ReadableStream
  // whose source enqueues a chunk must deliver it through a reader — exercises
  // the async stream plumbing end to end. (ReadableStream/Transform/Compression/
  // TextDecoder streams + MessageChannel all construct without crashing too.)
  mbLoadHTML(v, "<body>streams</body>", "about:blank");
  mbRunJS(v,
    "window.__s='pending';"
    "var rs=new ReadableStream({start:function(c){c.enqueue('hello');c.close();}});"
    "rs.getReader().read().then(function(r){window.__s=r.value+':'+r.done;});");
  mbWait(v, 60);
  Expect(Eval(v, "window.__s") == "hello:false",
         "Streams: ReadableStream delivers an enqueued chunk via a reader");

  // 50. Native form controls paint (the WebThemeEngine path). Painting a
  // checkbox/button/range/progress is a distinct subsystem from text/box paint;
  // if the theme engine were missing it would crash or paint nothing. Render on
  // white and assert some non-white pixels exist (a control drew).
  mbLoadHTML(v,
    "<body style='margin:0;background:#fff'>"
    "<input type='checkbox' checked><button>OK</button>"
    "<input type='range'><progress value='0.6'></progress></body>",
    "about:blank");
  std::vector<uint8_t> fpx(static_cast<size_t>(W) * H * 4, 255);  // white
  mbPaintToBitmap(v, fpx.data(), W, H, W * 4);
  bool drew = false;
  for (size_t i = 0; i + 2 < fpx.size(); i += 4) {
    if (fpx[i] < 240 || fpx[i + 1] < 240 || fpx[i + 2] < 240) { drew = true; break; }
  }
  Expect(drew, "form controls paint via WebThemeEngine (non-blank output)");

  // 51. Paint correctness: a renderer must produce the RIGHT pixels, not just
  // non-blank ones. Verify exact layout (getBoundingClientRect) AND exact paint:
  //  (a) flexbox space-between positions children at 0 and 300 in a 400px row;
  //  (b) a solid #00ff00 fill rasterizes to pure green;
  //  (c) rgba(0,0,255,0.5) over white composites to ~(128,128,255).
  // px is BGRA: [0]=B [1]=G [2]=R. Sample interior points to avoid AA edges.
  {
    Expect(Eval(v,
      "(function(){document.body.style.margin='0';"
      "document.body.innerHTML="
      "\"<div style='display:flex;justify-content:space-between;width:400px'>\"+"
      "\"<i id=a style=\\\"width:100px;height:20px;display:block\\\"></i>\"+"
      "\"<i id=b style=\\\"width:100px;height:20px;display:block\\\"></i></div>\";"
      "var a=document.getElementById('a').getBoundingClientRect(),"
      "b=document.getElementById('b').getBoundingClientRect();"
      "return a.x+','+b.x;})()") == "0,300",
      "layout: flexbox space-between (children at 0 and 300)");

    mbLoadHTML(v,
      "<body style='margin:0;background:#fff'>"
      "<div style='width:80px;height:80px;background:#00ff00'></div></body>",
      "about:blank");
    std::vector<uint8_t> g(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, g.data(), W, H, W * 4);
    size_t i = (40u * W + 40u) * 4;  // inside the green box
    Expect(g[i + 2] == 0 && g[i + 1] == 255 && g[i] == 0,
           "paint: solid #00ff00 rasterizes to pure green");

    mbLoadHTML(v,
      "<body style='margin:0;background:#fff'>"
      "<div style='width:80px;height:80px;background:rgba(0,0,255,0.5)'></div></body>",
      "about:blank");
    std::vector<uint8_t> a2(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, a2.data(), W, H, W * 4);
    size_t j = (40u * W + 40u) * 4;
    int R = a2[j + 2], G = a2[j + 1], B = a2[j];
    auto near = [](int x, int t) { return x >= t - 4 && x <= t + 4; };
    Expect(near(R, 128) && near(G, 128) && B == 255,
           "paint: rgba(0,0,255,.5) over white composites to ~(128,128,255)");
  }

  // 52. Stacking + gradient paint. z-index/DOM order: a blue box painted over a
  // red box wins at the overlap. And a horizontal red->blue linear-gradient is
  // red-ish at the left edge and blue-ish at the right.
  {
    mbLoadHTML(v,
      "<body style='margin:0'>"
      "<div style='position:absolute;left:0;top:0;width:60px;height:60px;background:#ff0000'></div>"
      "<div style='position:absolute;left:0;top:0;width:60px;height:60px;background:#0000ff'></div>"
      "</body>", "about:blank");
    std::vector<uint8_t> s(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, s.data(), W, H, W * 4);
    size_t k = (30u * W + 30u) * 4;
    Expect(s[k] == 255 && s[k + 2] == 0,  // B=255, R=0 -> blue on top
           "paint: later box stacks over earlier (blue over red)");

    mbLoadHTML(v,
      "<body style='margin:0'>"
      "<div style='width:200px;height:40px;"
      "background:linear-gradient(to right,#ff0000,#0000ff)'></div></body>",
      "about:blank");
    std::vector<uint8_t> gr(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, gr.data(), W, H, W * 4);
    size_t L = (20u * W + 6u) * 4, Rt = (20u * W + 193u) * 4;
    Expect(gr[L + 2] > 200 && gr[L] < 60 &&        // left: red-ish
               gr[Rt] > 200 && gr[Rt + 2] < 60,    // right: blue-ish
           "paint: horizontal linear-gradient is red->blue across width");
  }

  // 53. CSS filter: grayscale(1) on a red box must desaturate it to gray
  // (R==G==B), exercising the filter paint pipeline (SkImageFilter). A
  // non-modern/!filter engine would leave it red.
  {
    mbLoadHTML(v,
      "<body style='margin:0;background:#fff'>"
      "<div style='width:80px;height:80px;background:#ff0000;filter:grayscale(1)'></div>"
      "</body>", "about:blank");
    std::vector<uint8_t> f(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, f.data(), W, H, W * 4);
    size_t i = (40u * W + 40u) * 4;
    int R = f[i + 2], G = f[i + 1], B = f[i];
    Expect(std::abs(R - G) <= 4 && std::abs(G - B) <= 4 && R > 20 && R < 200,
           "paint: filter:grayscale(1) desaturates red to gray (R==G==B)");
  }

  // 54. border-radius clipping: a 50% radius makes a circle; the corner is
  // clipped to the page background while the center is the box color. Proves
  // rounded-corner clipping actually rasterizes (not just a layout attribute).
  {
    mbLoadHTML(v,
      "<body style='margin:0;background:#ffffff'>"
      "<div style='width:80px;height:80px;border-radius:50%;background:#0000ff'></div>"
      "</body>", "about:blank");
    std::vector<uint8_t> c(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, c.data(), W, H, W * 4);
    size_t ctr = (40u * W + 40u) * 4, corner = (3u * W + 3u) * 4;
    Expect(c[ctr] == 255 && c[ctr + 2] == 0 &&          // center: blue
               c[corner] == 255 && c[corner + 1] == 255 && c[corner + 2] == 255,  // corner: white
           "paint: border-radius:50% clips corners to background (circle)");
  }

  // 55. box-shadow paints outside the border box. A solid 10px spread (no blur,
  // no offset) draws a black ring around a white box; a pixel in the ring is
  // black, a pixel inside the box is white.
  {
    mbLoadHTML(v,
      "<body style='margin:0;background:#fff'>"
      "<div style='margin:40px;width:60px;height:60px;background:#fff;"
      "box-shadow:0 0 0 10px #000'></div></body>", "about:blank");
    std::vector<uint8_t> sh(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, sh.data(), W, H, W * 4);
    size_t ring = (70u * W + 35u) * 4;   // x=35 in [30,40): inside the 10px ring
    size_t box = (70u * W + 70u) * 4;    // inside the white box
    Expect(sh[ring] < 40 && sh[ring + 1] < 40 && sh[ring + 2] < 40 &&  // ring: black
               sh[box] == 255 && sh[box + 1] == 255 && sh[box + 2] == 255,  // box: white
           "paint: box-shadow spread draws a ring outside the border box");
  }

  // 56. Text actually RASTERIZES to glyphs (fonts were a documented gap). Render
  // black text on white and scan the text band: correct glyph rendering yields
  // both dark pixels (the strokes) AND white pixels (gaps between/within glyphs).
  // All-white => no glyphs (tofu/blank/missing fonts); all-dark => a solid block,
  // not text. Requires real font data + shaping + rasterization.
  {
    mbLoadHTML(v,
      "<body style='margin:0;background:#fff'>"
      "<div style='font:30px monospace;color:#000;line-height:40px'>WWWWWWWW</div>"
      "</body>", "about:blank");
    std::vector<uint8_t> t(static_cast<size_t>(W) * H * 4, 255);
    mbPaintToBitmap(v, t.data(), W, H, W * 4);
    int dark = 0, light = 0;
    for (int y = 0; y < 40; ++y)
      for (int x = 0; x < 220; ++x) {
        int r = t[(static_cast<size_t>(y) * W + x) * 4 + 2];
        if (r < 60) ++dark; else if (r > 200) ++light;
      }
    Expect(dark > 50 && light > 50,
           "text: glyphs rasterize (dark strokes + white gaps present)");
  }

  // 56b. Emoji rasterize IN COLOR: U+1F600 😀 paints with Apple Color Emoji
  // (the macOS system font — nothing bundled). Historically this rendered
  // monochrome: blink's mac fallback downgraded the substituted color face to
  // "Apple Symbols" even for emoji-presentation runs when the direct
  // Apple-Color-Emoji family lookup failed (patch 0028 guards that
  // downgrade). The assertions: a real glyph rasterizes (dark + light
  // pixels), and it carries vivid color (the yellow face).
  {
    mbLoadHTML(v,
      "<body style='margin:0;background:#fff'>"
      "<span style='font-size:72px;line-height:1'>\xf0\x9f\x98\x80</span>"
      "</body>", "about:blank");
    std::vector<uint8_t> ep(static_cast<size_t>(W) * H * 4, 255);
    mbPaintToBitmap(v, ep.data(), W, H, W * 4);
    int dark = 0, light = 0, colorful = 0;
    for (int y = 4; y < 84; ++y)
      for (int x = 2; x < 82; ++x) {
        const size_t o = (static_cast<size_t>(y) * W + x) * 4;
        int b = ep[o], g = ep[o + 1], r = ep[o + 2];
        int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
        int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
        if (mx - mn > 60 && mx > 80) ++colorful;  // a vivid, non-gray pixel
        if (r < 60) ++dark; else if (r > 200) ++light;
      }
    Expect(light > 20 && colorful > 50,
           "emoji rasterizes in COLOR (Apple Color Emoji via patch 0028)",
           std::string("dark=") + std::to_string(dark) + " light=" +
               std::to_string(light) + " colorful=" + std::to_string(colorful));
  }

  // 56c. Multiple distinct fonts are available — not a single fallback. The three
  // generic families must render the same text at DIFFERENT advance widths,
  // proving the build ships a real serif + sans-serif + monospace, so screenshots
  // of sites that specify font families keep the right look. A font-config
  // regression that collapsed everything to one fallback would make these equal.
  {
    mbLoadHTML(v, "<body>fonts</body>", "about:blank");
    const std::string r = Eval(v,
        "(function(){var c=document.createElement('canvas').getContext('2d');"
        "function w(f){c.font='40px '+f;"
        "return Math.round(c.measureText('Wikipedia mix& Quilt').width);}"
        "var s=w('serif'),n=w('sans-serif'),m=w('monospace');"
        "return (s>0&&n>0&&m>0&&s!==m&&n!==m&&s!==n)+':'+s+','+n+','+m;})()");
    Expect(r.substr(0, 5) == "true:",
           "fonts: serif/sans/monospace render at distinct widths (real font set)",
           r);
  }

  // 57. Font metrics scale: canvas measureText must report a real, font-size-
  // proportional advance width (text shaping with metrics, not a stub). 40px
  // text is ~2x the width of the same string at 20px.
  Expect(Eval(v,
      "(function(){var x=document.createElement('canvas').getContext('2d');"
      "x.font='40px monospace';var w40=x.measureText('MMMM').width;"
      "x.font='20px monospace';var w20=x.measureText('MMMM').width;"
      "return (w20>0 && w40>w20*1.5 && w40<w20*2.5)?'ok':(w40+'/'+w20);})()") == "ok",
      "text: canvas measureText advance scales with font size");

  // 58. SVG renders (shapes + fills). Inline SVG is ubiquitous (icons, charts,
  // logos) and is a distinct rendering path from CSS boxes. Draw a green rect
  // and a red circle on white and sample: inside the rect -> green, inside the
  // circle -> red, an empty corner -> white. Proves SVG geometry + fill paint.
  {
    mbLoadHTML(v,
      "<body style='margin:0;background:#fff'>"
      "<svg width='100' height='100' xmlns='http://www.w3.org/2000/svg'>"
      "<rect x='0' y='0' width='50' height='50' fill='#00ff00'/>"
      "<circle cx='75' cy='25' r='18' fill='#ff0000'/></svg></body>",
      "about:blank");
    std::vector<uint8_t> s(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, s.data(), W, H, W * 4);
    size_t rect = (25u * W + 25u) * 4;     // inside green rect
    size_t circ = (25u * W + 75u) * 4;     // inside red circle
    size_t gap = (90u * W + 90u) * 4;      // empty -> white
    Expect(s[rect + 1] == 255 && s[rect + 2] == 0 &&            // green
               s[circ + 2] == 255 && s[circ] == 0 &&           // red
               s[gap] == 255 && s[gap + 1] == 255 && s[gap + 2] == 255,  // white
           "SVG: rect + circle render with correct fills");
  }

  // 59. CSS transition animates over time: a property must interpolate when the
  // clock advances (not jump or stay). Start width 0, transition to 100px; after
  // driving the engine it should be partway/complete, i.e. > 0.
  {
    mbLoadHTML(v,
      "<body><div id='an' style='width:0px;height:10px;background:#000;"
      "transition:width 0.1s linear'></div></body>", "about:blank");
    mbRunJS(v, "var e=document.getElementById('an');"
               "getComputedStyle(e).width;"          // flush start value
               "e.style.width='100px';");            // trigger transition
    mbWait(v, 200);                                  // drive past the 100ms duration
    Expect(Eval(v, "parseInt(getComputedStyle(document.getElementById('an')).width)") == "100",
           "CSS transition: width animates to its target when the clock advances");
  }

  // 60. Holistic integration: a realistic composed page exercising several
  // subsystems together (flex header + CSS grid body + inline SVG + text), to
  // catch cross-subsystem bugs that isolated unit checks miss. Assert exact grid
  // layout (geometry) AND that both the SVG icon and the text actually painted
  // (pixels) in one render.
  {
    mbLoadHTML(v,
      "<body style='margin:0;background:#fff;font:16px monospace;color:#000'>"
      "<header style='display:flex;align-items:center;height:40px'>"
      "<svg width='30' height='30' xmlns='http://www.w3.org/2000/svg'>"
      "<rect width='30' height='30' fill='#008000'/></svg><b>Title</b></header>"
      "<main style='display:grid;grid-template-columns:repeat(2,150px)'>"
      "<section id='c0'>Left</section><section id='c1'>Right</section></main>"
      "</body>", "about:blank");
    // Geometry: grid columns at x=0 and x=150.
    bool grid_ok = Eval(v,
      "(function(){var a=document.getElementById('c0').getBoundingClientRect(),"
      "b=document.getElementById('c1').getBoundingClientRect();"
      "return a.x+','+b.x;})()") == "0,150";
    // Paint: SVG icon green at ~(15,15); some dark text pixels in the header band.
    std::vector<uint8_t> p(static_cast<size_t>(W) * H * 4, 255);
    mbPaintToBitmap(v, p.data(), W, H, W * 4);
    size_t icon = (15u * W + 15u) * 4;
    bool icon_ok = p[icon + 1] >= 100 && p[icon + 2] < 80 && p[icon] < 80;  // greenish
    int darktext = 0;
    for (int y = 0; y < 40; ++y)
      for (int x = 40; x < 260; ++x)
        if (p[(static_cast<size_t>(y) * W + x) * 4 + 2] < 80) ++darktext;
    Expect(grid_ok && icon_ok && darktext > 20,
           "integration: flex+grid layout + SVG icon + text compose in one page");
  }

  // 61. mbClickSelector clicks an element by CSS selector (resolves its box,
  // clicks the center) — the Puppeteer-style page.click primitive. Place a
  // button, click it by selector, and confirm its handler ran. Also confirm a
  // non-matching selector returns 0 (failure) without clicking.
  {
    mbLoadHTML(v,
      "<body style='margin:0'><button id='go' "
      "style='position:absolute;left:30px;top:30px;width:90px;height:30px' "
      "onclick='window.__c=(window.__c||0)+1'>Go</button></body>", "about:blank");
    std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // force layout for hit-testing
    int hit = mbClickSelector(v, "#go");
    int miss = mbClickSelector(v, "#nope");
    Expect(hit == 1 && miss == 0 && Eval(v, "String(window.__c)") == "1",
           "mbClickSelector: clicks element by selector; 0 when no match");
  }

  // 62. mbFillSelector fills a field by selector and fires events (Playwright
  // fill). Fill an input, then confirm its .value updated AND an 'input' event
  // was observed (frameworks rely on the event, not just the value). Also a
  // non-matching selector returns 0.
  {
    mbLoadHTML(v,
      "<body><input id='name' value=''>"
      "<script>window.__ev=0;document.getElementById('name')"
      ".addEventListener('input',function(){window.__ev++;});</script></body>",
      "about:blank");
    int ok = mbFillSelector(v, "#name", "Ada Lovelace");
    int miss = mbFillSelector(v, "#missing", "x");
    Expect(ok == 1 && miss == 0 &&
               Eval(v, "document.getElementById('name').value") == "Ada Lovelace" &&
               Eval(v, "String(window.__ev>0)") == "true",
           "mbFillSelector: sets value + fires input event; 0 when no match");
  }

  // 62-file. mbSetFileForSelector sets an <input type=file>'s files from a disk PATH —
  // the privileged op a page's own script is forbidden to do (file-upload automation).
  // The real proof is byte access: write a known file, set it, then read it back via
  // FileReader (the same byte path a form submit uses). A text input + a non-match
  // must be rejected (return 0).
  {
    const char* path = "/tmp/mb_upload.txt";
    const char* content = "UPLOAD-CONTENT-42";  // 17 bytes
    if (FILE* f = std::fopen(path, "wb")) {
      std::fwrite(content, 1, std::strlen(content), f);
      std::fclose(f);
    }
    mbLoadHTML(v,
      "<body><input type='file' id='up'><input type='text' id='txt'>"
      "<script>window.__chg=0;document.getElementById('up')"
      ".addEventListener('change',function(){window.__chg++;});</script></body>",
      "about:blank");
    const int set_ok = mbSetFileForSelector(v, "#up", path);
    const int wrong_type = mbSetFileForSelector(v, "#txt", path);  // not a file input
    const int no_match = mbSetFileForSelector(v, "#nope", path);   // no element
    const std::string n =
        Eval(v, "document.getElementById('up').files.length?"
                "document.getElementById('up').files[0].name:''");
    const std::string sz =
        Eval(v, "''+document.getElementById('up').files[0].size");
    const std::string chg = Eval(v, "''+window.__chg");
    Eval(v, "(function(){var fr=new FileReader();fr.onload=function(){window.__fc="
            "fr.result;};fr.onerror=function(){window.__fc='ERR';};"
            "fr.readAsText(document.getElementById('up').files[0]);})()");
    mbWaitForFunction(v, "window.__fc!==undefined", 2000);
    const std::string fc = Eval(v, "window.__fc===undefined?'TIMEOUT':window.__fc");
    Expect(set_ok == 1 && wrong_type == 0 && no_match == 0 && n == "mb_upload.txt" &&
               sz == "17" && chg == "1" && fc == "UPLOAD-CONTENT-42",
           "mbSetFileForSelector sets a file input (name+size+readable bytes) + change",
           "set=" + std::to_string(set_ok) + " wrong=" + std::to_string(wrong_type) +
               " n=[" + n + "] size=" + sz + " chg=" + chg + " bytes=[" + fc + "]");
  }

  // 62-sep. JsEscape robustness: a value containing the JS line terminators
  // U+2028 / U+2029 must round-trip intact through the eval-based fill. These are
  // legal in ES2019+ string literals (V8's JSON-superset) but would terminate a
  // pre-ES2019 literal and break the generated JS — JsEscape escapes \ " \n \r
  // but NOT these, so this case is the regression guard proving the embedding
  // stays correct if V8's parser or JsEscape ever changes (real text — some PDFs
  // / rich-text sources — does carry these separators).
  {
    mbLoadHTML(v, "<body><input id='sep' value=''></body>", "about:blank");
    const char* sep_val = "a\xe2\x80\xa8" "b\xe2\x80\xa9" "c";  // a U+2028 b U+2029 c
    int ok = mbFillSelector(v, "#sep", sep_val);
    const std::string probe = Eval(v,
        "var x=document.getElementById('sep').value;"
        "x.length + ',' + x.charCodeAt(1) + ',' + x.charCodeAt(3)");
    Expect(ok == 1 && probe == "5,8232,8233",  // 5 code points; 0x2028, 0x2029
           "JsEscape: U+2028/U+2029 in a filled value round-trip intact",
           std::string("probe=") + probe);
  }

  // 62-popup. Popup / new-window: with no host new-window handler registered, a
  // target=_blank link (or window.open to a real URL) opens in THIS view — the
  // single-window default — so the link still works instead of being a dead no-op
  // (e.g. baidu.com's target=_blank navbar links). window.open('about:blank') is still
  // denied (returns null): we can't host a JS-populated blank popup. Neither path may
  // crash the single-process host. (Uses a data: href so the in-place navigation is
  // instant + offline and doesn't bleed a real fetch into later tests.)
  {
    mbLoadHTML(v,
        "<body><a id='b' href='data:text/html,<title>popped</title>hi' "
        "target='_blank'>x</a>"
        "<script>window.__r=String(window.open('about:blank'));</script></body>",
        "about:blank");
    const std::string opened = Eval(v, "window.__r");  // "null" == blank popup denied
    mbClickSelector(v, "#b");  // _blank link -> opens its href in this view
    std::string title;
    for (int i = 0; i < 100; ++i) {
      mbWait(v, 25);
      title = Eval(v, "document.title");
      if (title == "popped")
        break;
    }
    Expect(opened == "null" && title == "popped",
           "popup: window.open(blank)->null; target=_blank link opens in the view",
           std::string("open=") + opened + " title=[" + title + "]");
  }

  // 62-csp. A strict Content-Security-Policy blocks the PAGE's own scripts but NOT
  // the host's extraction. Host eval (mbEvalJS / the selector readers) runs in a
  // privileged context like DevTools, so script-src 'none' can't stop it — which
  // is what lets us scrape the large fraction of the real web that ships strict
  // CSP. The page's inline script is blocked (proving CSP is active), yet
  // mbGetTextForSelector / mbCountSelector / mbEvalJS all still read the DOM.
  // Run on a dedicated view: a document's CSP from <meta> persists on the frame,
  // so reusing the shared `v` would carry script-src 'none' into later cases
  // (mb_shot is one-page-per-process, so that never bites the real tool).
  {
    mbView* cv = mbCreateView(W, H);
    mbLoadHTML(cv,
        "<meta http-equiv='Content-Security-Policy' "
        "content=\"script-src 'none'; default-src 'none'\">"
        "<body><h1 id='t'>protected</h1><div class='r'>a</div><div class='r'>b</div>"
        "<script>window.__pageran=true;</script></body>",  // CSP blocks this
        "https://csp.test/");
    const bool page_blocked =
        Eval(cv, "String(typeof window.__pageran)") == "undefined";
    char tb[64] = {0};
    const bool host_text = mbGetTextForSelector(cv, "#t", tb, sizeof(tb)) >= 0 &&
                           std::string(tb) == "protected";
    const bool host_count = mbCountSelector(cv, ".r") == 2;
    const bool host_eval =
        Eval(cv, "document.getElementById('t').textContent") == "protected";
    Expect(page_blocked && host_text && host_count && host_eval,
           "CSP script-src 'none' blocks page scripts, not host extraction",
           std::string("pageBlocked=") + (page_blocked ? "1" : "0") + " text=" +
               tb + " count=" + (host_count ? "2" : "?"));
    mbDestroyView(cv);
  }

  // 62b. mbDispatchEvent fires arbitrary DOM events that click/fill don't — a
  // mouseover handler and a custom-event handler both run; no-match -> 0.
  {
    mbLoadHTML(v,
      "<body><div id='d'>x</div><script>window.__o=0;window.__c=0;"
      "document.getElementById('d').addEventListener('mouseover',function(){window.__o++;});"
      "document.getElementById('d').addEventListener('ping',function(){window.__c++;});"
      "</script></body>", "about:blank");
    const bool over = mbDispatchEvent(v, "#d", "mouseover") == 1 &&
                      Eval(v, "String(window.__o)") == "1";
    const bool custom = mbDispatchEvent(v, "#d", "ping") == 1 &&
                        Eval(v, "String(window.__c)") == "1";
    const bool none_ok = mbDispatchEvent(v, "#none", "click") == 0;
    Expect(over && custom && none_ok,
           "mbDispatchEvent fires mouseover + custom events; 0 on no match",
           std::string("over=") + (over ? "1" : "0") + " custom=" +
               (custom ? "1" : "0") + " none=" + (none_ok ? "1" : "0"));
  }

  // 63. mbWaitForFunction polls a JS predicate until truthy (general wait). A
  // setTimeout sets a flag after 50ms; waitForFunction must return 1 once it
  // flips. A condition that never holds must time out -> 0 (and not hang past
  // the timeout). Generalizes waitForSelector.
  {
    mbLoadHTML(v, "<body>wf<script>setTimeout(function(){window.__ready=1;},50);"
                  "</script></body>", "about:blank");
    int got = mbWaitForFunction(v, "window.__ready===1", 2000);
    int timedout = mbWaitForFunction(v, "window.__never", 60);
    Expect(got == 1 && timedout == 0 &&
               Eval(v, "String(window.__ready)") == "1",
           "mbWaitForFunction: resolves when predicate turns truthy; times out otherwise");
  }

  // 64. Blob DATA now resolves (in-process BlobRegistry/Blob on the service
  // thread). Previously blob reads stayed pending forever; now blob.text(),
  // arrayBuffer() and FileReader deliver the bytes. This exercises the [Sync]
  // BlobRegistry.Register serviced off the main thread + Blob.ReadAll over a
  // data pipe — the payoff of the service-host work.
  {
    mbLoadHTML(v, "<body>blob-data</body>", "about:blank");
    mbRunJS(v,
      "window.__t='pending';window.__ab=-1;window.__fr='pending';"
      "var b=new Blob(['hello'],{type:'text/plain'});"
      "b.text().then(function(s){window.__t=s;});"
      "b.arrayBuffer().then(function(a){window.__ab=a.byteLength;});"
      "var fr=new FileReader();fr.onload=function(){window.__fr=fr.result;};"
      "fr.readAsText(new Blob(['world']));");
    mbWait(v, 300);  // drive the async reads to completion
    Expect(Eval(v, "window.__t") == "hello" &&
               Eval(v, "String(window.__ab)") == "5" &&
               Eval(v, "window.__fr") == "world",
           "Blob data resolves: text()/arrayBuffer()/FileReader deliver bytes");
  }

  // 65. Blob data works at realistic sizes, not just a few bytes. A 100 KB blob
  // (still inline embedded_data, <=256 KB) must round-trip fully through
  // ReadAll's data-pipe write. Guards that the shipped path handles real content
  // (canvas exports, fetched bodies), not only tiny strings. (>256 KB needs the
  // BytesProvider path — a documented follow-up.)
  {
    mbLoadHTML(v, "<body>blob100k</body>", "about:blank");
    mbRunJS(v,
      "window.__n=-1;"
      "var b=new Blob(['z'.repeat(100000)]);"
      "b.text().then(function(s){window.__n=s.length;});");
    mbWait(v, 300);
    Expect(Eval(v, "String(window.__n)") == "100000",
           "Blob data round-trips at 100 KB (realistic size, not just bytes)");
  }

  // 66. Large blobs (>256 KB) resolve too: no inline embedded_data, so the bytes
  // come via the BytesProvider (fetched after Register replies), and the read
  // streams over the pipe in chunks (SimpleWatcher). Verifies the full data path
  // for any size. 500 KB exceeds both the 256 KB inline cap and the pipe buffer.
  {
    mbLoadHTML(v, "<body>bigblob</body>", "about:blank");
    mbRunJS(v,
      "window.__big=-1;window.__bab=-1;"
      "var b=new Blob(['q'.repeat(500000)]);"
      "b.text().then(function(s){window.__big=s.length;});"
      "b.arrayBuffer().then(function(a){window.__bab=a.byteLength;});");
    mbWait(v, 500);  // BytesProvider fetch + chunked write
    Expect(Eval(v, "String(window.__big)") == "500000" &&
               Eval(v, "String(window.__bab)") == "500000",
           "Blob >256KB resolves via BytesProvider + chunked pipe write");
  }

  // 67. canvas.toBlob() works end to end (canvas -> Skia encode -> blob -> read)
  // — headless image export. toBlob encodes on an IDLE task; idle tasks now run
  // in WaitMs (StartIdlePeriodForTesting), so the callback fires promptly instead
  // of via the ~1s fallback. Read the blob back and check the PNG signature.
  {
    mbLoadHTML(v, "<body><canvas id='c' width='16' height='16'></canvas></body>",
               "about:blank");
    mbRunJS(v,
      "window.__png='pending';"
      "var cv=document.getElementById('c'),x=cv.getContext('2d');"
      "x.fillStyle='#ff8800';x.fillRect(0,0,16,16);"
      "cv.toBlob(function(b){if(!b){window.__png='null';return;}"
      "b.arrayBuffer().then(function(ab){var u=new Uint8Array(ab);"
      "window.__png=(u[0]===0x89&&u[1]===0x50&&u[2]===0x4E&&u[3]===0x47&&u.length>0)"
      "?'PNG':'bad';});}, 'image/png');");
    mbWait(v, 400);  // idle encode + blob read (prompt now that idle tasks run)
    Expect(Eval(v, "window.__png") == "PNG",
           "canvas.toBlob() -> readable PNG blob (idle-task encode runs)");
  }

  // 68. Blobs that REFERENCE other blobs resolve (is_blob DataElements). A
  // blob composed of another blob registers an is_blob element holding a Blob
  // remote + offset/length; reading it must read through to the referenced
  // blob. Covers Response.blob() (wraps the body blob) and Blob.slice() (an
  // offset/length view). Previously these read empty (is_blob was ignored).
  {
    mbLoadHTML(v, "<body>blobref</body>", "about:blank");
    mbRunJS(v,
      "window.__resp='pending';window.__slice='pending';"
      "new Response('hello-resp').blob().then(function(b){return b.text();})"
      ".then(function(t){window.__resp=t;});"
      "new Blob(['0123456789']).slice(2,5).text().then(function(t){window.__slice=t;});");
    mbWait(v, 300);
    Expect(Eval(v, "window.__resp") == "hello-resp" &&
               Eval(v, "window.__slice") == "234",
           "Blob-of-blob resolves: Response.blob() + Blob.slice() read through");
  }

  // 69. fetch('data:...') resolves (loader decodes data: via net::DataURL::Parse)
  // for .text(), .arrayBuffer() AND .blob(). A fetched response's .blob() streams
  // the body through BlobRegistry.RegisterFromStream (fetch_data_loader.cc), which
  // we now service by draining the body into a blob.
  {
    mbLoadHTML(v, "<body>fetchdata</body>", "about:blank");
    mbRunJS(v,
      "window.__ft='pending';window.__fa=-1;window.__fb='pending';"
      "fetch('data:text/plain,hello-data').then(function(r){return r.text();})"
      ".then(function(t){window.__ft=t;},function(e){window.__ft='rej';});"
      "fetch('data:application/octet-stream,abcd').then(function(r){return r.arrayBuffer();})"
      ".then(function(a){window.__fa=a.byteLength;},function(e){window.__fa='rej';});"
      "fetch('data:text/plain,blob-data').then(function(r){return r.blob();})"
      ".then(function(b){return b.text();}).then(function(t){window.__fb=t;},"
      "function(e){window.__fb='rej';});");
    mbWait(v, 400);
    Expect(Eval(v, "window.__ft") == "hello-data" &&
               Eval(v, "String(window.__fa)") == "4" &&
               Eval(v, "window.__fb") == "blob-data",
           "fetch('data:...') resolves: .text(), .arrayBuffer(), .blob()");
  }

  // 70. CSS generated content (::before) renders. Ubiquitous (icons, badges,
  // quotes, numbering). Assert both the computed-style content and that the
  // generated box actually paints (a green ::before block at top-left).
  {
    mbLoadHTML(v,
      "<style>#g::before{content:'';display:block;width:50px;height:50px;"
      "background:#00ff00}</style><body style='margin:0;background:#fff'>"
      "<div id='g'>x</div></body>", "about:blank");
    std::vector<uint8_t> bpx(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, bpx.data(), W, H, W * 4);
    size_t bi = (10u * W + 10u) * 4;  // inside the ::before box
    bool painted = bpx[bi + 1] == 255 && bpx[bi + 2] == 0 && bpx[bi] == 0;  // green
    bool computed =
        Eval(v, "getComputedStyle(document.getElementById('g'),'::before')"
                ".backgroundColor") == "rgb(0, 255, 0)";
    Expect(painted && computed,
           "CSS ::before generated content paints + computed style reads it");
  }

  // 71. clip-path polygon clips paint to an arbitrary shape (distinct from
  // border-radius). A top-left triangle: inside it paints, the clipped corner
  // shows the page background.
  {
    mbLoadHTML(v,
      "<body style='margin:0;background:#ffffff'>"
      "<div style='width:80px;height:80px;background:#0000ff;"
      "clip-path:polygon(0 0,100% 0,0 100%)'></div></body>", "about:blank");
    std::vector<uint8_t> clp(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, clp.data(), W, H, W * 4);
    size_t tin = (8u * W + 8u) * 4;        // inside the triangle -> blue
    size_t tout = (70u * W + 70u) * 4;     // clipped corner -> white
    Expect(clp[tin] == 255 && clp[tin + 2] == 0 &&                         // blue
               clp[tout] == 255 && clp[tout + 1] == 255 && clp[tout + 2] == 255,
           "clip-path: polygon() clips paint to the shape");
  }

  // 72. CSS custom properties (var()), calc(), and clamp()/min()/max() — the
  // building blocks of modern stylesheets and design systems. All resolve via
  // computed style. (Custom properties also cascade + inherit; checked via a
  // child reading a property set on :root.)
  mbLoadHTML(v,
    "<style>:root{--accent:rgb(10,20,30);--gap:8px}"
    "#a{color:var(--accent)} "
    "#b{width:calc(100px + 50px)} "
    "#c{width:clamp(10px,40px,100px)} "
    "#d{margin-left:calc(var(--gap) * 2)}</style>"
    "<body><i id='a'>x</i><i id='b' style='display:block'>x</i>"
    "<i id='c' style='display:block'>x</i><i id='d' style='display:block'>x</i></body>",
    "about:blank");
  Expect(Eval(v, "getComputedStyle(document.getElementById('a')).color") == "rgb(10, 20, 30)" &&
             Eval(v, "getComputedStyle(document.getElementById('b')).width") == "150px" &&
             Eval(v, "getComputedStyle(document.getElementById('c')).width") == "40px" &&
             Eval(v, "getComputedStyle(document.getElementById('d')).marginLeft") == "16px",
         "CSS var()/calc()/clamp() resolve (custom properties + math)");

  // 73. Multiple concurrent views are independent. A real embedder (tabs)
  // creates several mbViews over one shared runtime; verify a second view has
  // its own DOM/JS world and does not disturb the first. (If the host assumed a
  // single view, this would crash or cross-contaminate.)
  {
    mbView* v2 = mbCreateView(W, H);
    Expect(v2 != nullptr, "multi-view: second mbCreateView succeeds");
    if (v2) {
      mbLoadHTML(v2, "<body><b id='x'>view2</b></body>", "about:blank");
      mbLoadHTML(v, "<body><b id='x'>view1</b></body>", "about:blank");
      mbRunJS(v2, "window.__who='two';");
      mbRunJS(v, "window.__who='one';");
      Expect(Eval(v2, "document.getElementById('x').textContent") == "view2" &&
                 Eval(v, "document.getElementById('x').textContent") == "view1" &&
                 Eval(v2, "window.__who") == "two" &&
                 Eval(v, "window.__who") == "one",
             "multi-view: two views keep independent DOM + JS globals");
      mbDestroyView(v2);
      // First view still usable after the second is destroyed.
      Expect(Eval(v, "1+1") == "2",
             "multi-view: first view survives second view's destruction");
    }
  }

  // 73b. Cross-origin IndexedDB ISOLATION: two views at DIFFERENT origins that open
  // the SAME db name + key must NOT see each other's data (IDB is strictly per-origin).
  // Previously the backend Registry was keyed by db name only -> cross-origin read/write
  // of persistent data; now it's keyed by (origin, name). view A writes 'fromA', view B
  // writes 'fromB' to "shared"; A re-reading still sees 'fromA' (B's write was isolated).
  {
    mbView* a = mbCreateView(W, H);
    mbView* b = mbCreateView(W, H);
    if (a && b) {
      const std::string kPut =
          "function P(val,done){var q=indexedDB.open('shared',1);"
          "q.onupgradeneeded=function(e){e.target.result.createObjectStore('s',"
          "{keyPath:'id'});};q.onsuccess=function(e){var db=e.target.result;"
          "var t=db.transaction('s','readwrite');t.objectStore('s').put("
          "{id:1,val:val});t.oncomplete=function(){var g=db.transaction('s')."
          "objectStore('s').get(1);g.onsuccess=function(){window[done]="
          "g.result?g.result.val:'none';};};};}";
      mbLoadHTML(a, "<body>A</body>", "https://a-idb.test/");
      mbLoadHTML(b, "<body>B</body>", "https://b-idb.test/");
      mbRunJS(a, (kPut + "P('fromA','__ra');").c_str());
      for (int i = 0; i < 80 && Eval(a, "''+window.__ra") == "undefined"; ++i)
        mbWait(a, 25);
      mbRunJS(b, (kPut + "P('fromB','__rb');").c_str());
      for (int i = 0; i < 80 && Eval(b, "''+window.__rb") == "undefined"; ++i)
        mbWait(b, 25);
      // A re-reads AFTER B wrote 'fromB' to the same db name at a different origin.
      mbRunJS(a,
              "var q=indexedDB.open('shared',1);q.onsuccess=function(e){var g="
              "e.target.result.transaction('s').objectStore('s').get(1);"
              "g.onsuccess=function(){window.__ra2=g.result?g.result.val:'none';};};");
      for (int i = 0; i < 80 && Eval(a, "''+window.__ra2") == "undefined"; ++i)
        mbWait(a, 25);
      const std::string ra = Eval(a, "window.__ra"), rb = Eval(b, "window.__rb"),
                        ra2 = Eval(a, "window.__ra2");
      Expect(ra == "fromA" && rb == "fromB" && ra2 == "fromA",
             "cross-origin IndexedDB is isolated (same db name, different origins)",
             "ra=" + ra + " rb=" + rb + " ra2(isolated)=" + ra2);
    }
    if (a) mbDestroyView(a);
    if (b) mbDestroyView(b);
  }

  // 73c. Cross-origin OPFS ISOLATION: navigator.storage.getDirectory() is a per-
  // ORIGIN private file system (a process-wide root would let one origin read
  // another's private files). View A (origin X) writes f.txt; view B (origin Y)
  // must NOT find it (NotFoundError) — its OPFS root is a different origin's.
  {
    mbView* a = mbCreateView(W, H);
    mbView* b = mbCreateView(W, H);
    if (a && b) {
      mbLoadHTML(a, "<body>A</body>", "https://a-opfs.test/");
      mbLoadHTML(b, "<body>B</body>", "https://b-opfs.test/");
      mbRunJS(a,
        "window.__oa='';navigator.storage.getDirectory().then(function(r){"
        "return r.getFileHandle('f.txt',{create:true}).then(function(fh){"
        "return fh.createWritable().then(function(w){return w.write('fromA')."
        "then(function(){return w.close();});});});}).then(function(){"
        "window.__oa='wrote';}).catch(function(e){window.__oa='err:'+e.name;});");
      for (int i = 0; i < 120 && Eval(a, "window.__oa") == ""; ++i)
        mbWait(a, 25);
      mbRunJS(b,
        "window.__ob='';navigator.storage.getDirectory().then(function(r){"
        "return r.getFileHandle('f.txt').then(function(fh){return fh.getFile()."
        "then(function(f){return f.text();});});}).then(function(t){"
        "window.__ob='got:'+t;}).catch(function(e){window.__ob='err:'+e.name;});");
      for (int i = 0; i < 120 && Eval(b, "window.__ob") == ""; ++i)
        mbWait(b, 25);
      const std::string oa = Eval(a, "window.__oa"), ob = Eval(b, "window.__ob");
      Expect(oa == "wrote" && ob == "err:NotFoundError",
             "cross-origin OPFS is isolated (one origin can't see another's files)",
             "oa=" + oa + " ob(isolated)=" + ob);
    }
    if (a) mbDestroyView(a);
    if (b) mbDestroyView(b);
  }

  // 73c2. OPFS PERSISTS TO DISK: write a file, mbSaveOPFS, removeEntry it (gone from
  // memory), mbLoadOPFS, read it back — proving the bytes were restored from DISK, not the
  // in-memory tree. The peer of the IndexedDB disk persistence for the file-storage API.
  {
    const char* opfs_path = "/tmp/mb_opfs_persist.bin";
    mbLoadHTML(v, "<body>opfspersist</body>", "https://opfspersist.test/");
    mbRunJS(v,
      "window.__o='';navigator.storage.getDirectory().then(function(r){"
      "return r.getFileHandle('persist.txt',{create:true}).then(function(fh){"
      "return fh.createWritable().then(function(w){return w.write('OPFSDISK')."
      "then(function(){return w.close();});});});}).then(function(){"
      "window.__o='wrote';}).catch(function(e){window.__o='err:'+e.name;});");
    std::string o;
    for (int i = 0; i < 160 && o != "wrote"; ++i) { mbWait(v, 25); o = Eval(v, "window.__o"); }
    const bool wrote = (o == "wrote");
    mbSaveOPFS(opfs_path);
    mbRunJS(v,
      "window.__o='';navigator.storage.getDirectory().then(function(r){"
      "return r.removeEntry('persist.txt');}).then(function(){"
      "return navigator.storage.getDirectory().then(function(r){"
      "return r.getFileHandle('persist.txt').then(function(){return 'still';},"
      "function(e){return 'gone:'+e.name;});});}).then(function(s){"
      "window.__o=s;}).catch(function(e){window.__o='err:'+e.name;});");
    o.clear();
    for (int i = 0; i < 160 && o.empty(); ++i) { mbWait(v, 25); o = Eval(v, "window.__o"); }
    const bool removed = (o.rfind("gone:", 0) == 0);  // confirmed gone from memory
    mbLoadOPFS(opfs_path);  // restore from disk
    mbRunJS(v,
      "window.__o='';navigator.storage.getDirectory().then(function(r){"
      "return r.getFileHandle('persist.txt').then(function(fh){return fh.getFile()."
      "then(function(f){return f.text();});});}).then(function(t){"
      "window.__o='got:'+t;}).catch(function(e){window.__o='err:'+e.name;});");
    o.clear();
    for (int i = 0; i < 200 && o.rfind("got:", 0) != 0 && o.rfind("err", 0) != 0; ++i) {
      mbWait(v, 25);
      o = Eval(v, "window.__o");
    }
    Expect(wrote && removed && o == "got:OPFSDISK",
           "OPFS files persist to disk (write/save/remove/load round-trip)",
           "wrote=" + std::string(wrote ? "1" : "0") + " removed=" +
               (removed ? "1" : "0") + " final=[" + o + "]");
  }

  // 73c3. OPFS persistence handles the recursive serializer's untested paths: a NESTED
  // directory (sub/nested.bin) AND BINARY content (bytes incl. 0x00 and 0xFF, which a
  // text/length-naive format would corrupt). Build the tree, mbSaveOPFS, clear it, reload,
  // and verify both the nested path and the exact bytes survive.
  {
    const char* opfs_path = "/tmp/mb_opfs_nested.bin";
    mbLoadHTML(v, "<body>opfsnest</body>", "https://opfsnest.test/");
    // Write sub/nested.bin = [0,1,2,255,65,0,200] via a writable stream.
    mbRunJS(v,
      "window.__o='';navigator.storage.getDirectory().then(function(r){"
      "return r.getDirectoryHandle('sub',{create:true}).then(function(d){"
      "return d.getFileHandle('nested.bin',{create:true}).then(function(fh){"
      "return fh.createWritable().then(function(w){"
      "return w.write(new Uint8Array([0,1,2,255,65,0,200]))."
      "then(function(){return w.close();});});});});}).then(function(){"
      "window.__o='wrote';}).catch(function(e){window.__o='err:'+e.name;});");
    std::string o;
    for (int i = 0; i < 160 && o != "wrote"; ++i) { mbWait(v, 25); o = Eval(v, "window.__o"); }
    const bool wrote = (o == "wrote");
    mbSaveOPFS(opfs_path);
    // Remove the whole 'sub' dir (recursive) -> gone from memory.
    mbRunJS(v,
      "window.__o='';navigator.storage.getDirectory().then(function(r){"
      "return r.removeEntry('sub',{recursive:true});}).then(function(){"
      "return navigator.storage.getDirectory().then(function(r){"
      "return r.getDirectoryHandle('sub').then(function(){return 'still';},"
      "function(e){return 'gone:'+e.name;});});}).then(function(s){window.__o=s;})"
      ".catch(function(e){window.__o='err:'+e.name;});");
    o.clear();
    for (int i = 0; i < 160 && o.empty(); ++i) { mbWait(v, 25); o = Eval(v, "window.__o"); }
    const bool removed = (o.rfind("gone:", 0) == 0);
    mbLoadOPFS(opfs_path);
    // Read sub/nested.bin back as bytes and report them.
    mbRunJS(v,
      "window.__o='';navigator.storage.getDirectory().then(function(r){"
      "return r.getDirectoryHandle('sub').then(function(d){"
      "return d.getFileHandle('nested.bin').then(function(fh){return fh.getFile()."
      "then(function(f){return f.arrayBuffer();});});});}).then(function(ab){"
      "var u=new Uint8Array(ab);window.__o='bytes:'+u.join(',');})"
      ".catch(function(e){window.__o='err:'+e.name;});");
    o.clear();
    for (int i = 0; i < 200 && o.rfind("bytes:", 0) != 0 && o.rfind("err", 0) != 0; ++i) {
      mbWait(v, 25);
      o = Eval(v, "window.__o");
    }
    Expect(wrote && removed && o == "bytes:0,1,2,255,65,0,200",
           "OPFS persistence handles nested dirs + binary content (0x00/0xFF intact)",
           "wrote=" + std::string(wrote ? "1" : "0") + " removed=" +
               (removed ? "1" : "0") + " final=[" + o + "]");
  }

  // 73d. Cross-origin Cache Storage ISOLATION: caches.open(name) is per-ORIGIN (the
  // registry was keyed by bare cache name -> cross-origin cache sharing). View A
  // (origin X) puts an entry; view B (origin Y) opening the same-named cache must
  // NOT match it. Status-only (does match find it) — orthogonal to the known cached-
  // body-bytes bug.
  {
    mbView* a = mbCreateView(W, H);
    mbView* b = mbCreateView(W, H);
    if (a && b) {
      mbLoadHTML(a, "<body>A</body>", "https://a-cache.test/");
      mbLoadHTML(b, "<body>B</body>", "https://b-cache.test/");
      mbRunJS(a,
        "window.__ca='';caches.open('shared').then(function(c){"
        "return c.put('/x',new Response('hi')).then(function(){window.__ca='put';});"
        "}).catch(function(e){window.__ca='err:'+e.name;});");
      for (int i = 0; i < 120 && Eval(a, "window.__ca") == ""; ++i)
        mbWait(a, 25);
      mbRunJS(b,
        "window.__cb='';caches.open('shared').then(function(c){"
        "return c.match('/x').then(function(r){window.__cb=r?'found':'miss';});"
        "}).catch(function(e){window.__cb='err:'+e.name;});");
      for (int i = 0; i < 120 && Eval(b, "window.__cb") == ""; ++i)
        mbWait(b, 25);
      const std::string ca = Eval(a, "window.__ca"), cb = Eval(b, "window.__cb");
      Expect(ca == "put" && cb == "miss",
             "cross-origin Cache Storage is isolated (one origin can't match another's entry)",
             "ca=" + ca + " cb(isolated)=" + cb);
    }
    if (a) mbDestroyView(a);
    if (b) mbDestroyView(b);
  }

  // 73e. Cache-storage LARGE-body durability under rapid succession (BACKLOG
  // B1): a >256000-byte body registers via a BytesProvider (not embedded
  // bytes), and rapid put+match+read cycles historically came back empty
  // ~50% of the time (provider stall). 10 cycles, every read must return the
  // full 300*1024 bytes.
  {
    mbLoadHTML(v, "<body>b1</body>", "https://blobstall.test/");
    Eval(v,
      "window.__b1='';"
      "(async function(){try{"
      "  const out=[];"
      "  for(let i=0;i<10;i++){"
      "    const c=await caches.open('b1-'+i);"
      "    await c.put('https://blobstall.test/big',"
      "                new Response('x'.repeat(300*1024)));"
      "    const m=await c.match('https://blobstall.test/big');"
      "    const t=await m.text();"
      "    out.push(t.length);"
      "  }"
      "  window.__b1=out.join(',');"
      "}catch(e){window.__b1='err:'+e;}})();");
    for (int i = 0; i < 400 && Eval(v, "window.__b1") == ""; ++i)
      mbWait(v, 25);
    const std::string r = Eval(v, "window.__b1");
    std::string want;
    for (int i = 0; i < 10; ++i)
      want += (i ? "," : "") + std::to_string(300 * 1024);
    Expect(r == want,
           "cache-storage >256KB bodies survive 10 rapid put/match/read cycles (B1)",
           "lengths=[" + r + "]");
  }

  // 78b. Storage partitioning across views: two views are independent top-level browsing
  // contexts, so their sessionStorage is ISOLATED (each view mints a unique session-namespace
  // id), while localStorage is per-ORIGIN and therefore SHARED process-wide. Same origin in both.
  {
    mbView* v2 = mbCreateView(W, H);
    if (v2) {
      mbLoadHTML(v, "<body>x</body>", "https://ssview.test/");
      mbLoadHTML(v2, "<body>x</body>", "https://ssview.test/");
      mbRunJS(v, "sessionStorage.setItem('sk','viewA');"
                 "localStorage.setItem('lk','shared');");
      mbWait(v, 20);
      mbWait(v2, 20);
      const std::string sa = Eval(v, "sessionStorage.getItem('sk')");
      const std::string sb =
          Eval(v2, "sessionStorage.getItem('sk')===null?'null':sessionStorage.getItem('sk')");
      const std::string lb = Eval(v2, "localStorage.getItem('lk')");
      const std::string r = sa + "," + sb + "," + lb;
      Expect(r == "viewA,null,shared",
             "cross-view: sessionStorage isolated per view, localStorage shared per origin",
             "ssview=[" + r + "]");
      mbDestroyView(v2);
    }
  }

  // 78c. Multi-view history isolation: each view's page-driven history.back() routes to ITS
  // OWN frame. The LocalFrameHost traversal sink is keyed per-frame (not a single global slot
  // that a second view would clobber). v pushes /a then back()s to /start (popstate fires);
  // v2 — which also pushed /a — must be untouched (still /a, no popstate).
  {
    mbView* v2 = mbCreateView(W, H);
    if (v2) {
      mbLoadHTML(v, "<body>x</body>", "https://hv1.test/start");
      mbLoadHTML(v2, "<body>x</body>", "https://hv2.test/start");
      mbRunJS(v, "window.__p='';addEventListener('popstate',function(){window.__p='pop';});"
                 "history.pushState({},'','/a');");
      mbRunJS(v2, "window.__p='';addEventListener('popstate',function(){window.__p='pop';});"
                  "history.pushState({},'','/a');");
      mbWait(v, 20);
      mbWait(v2, 20);
      mbRunJS(v, "history.back();");
      mbWaitForFunction(v, "window.__p==='pop'", 3000);
      const std::string r1 = Eval(v, "location.pathname+','+window.__p");
      const std::string r2 = Eval(v2, "location.pathname+','+window.__p");
      Expect(r1 == "/start,pop" && r2 == "/a,",
             "multi-view: page-driven history.back() routes to its own view only",
             "mvh=[v1:" + r1 + "|v2:" + r2 + "]");
      mbDestroyView(v2);
    }
  }

  // 78c2. Page-driven session history is COMPLETE: on a FRESH view, history.length tracks
  // pushState entries (the archive flagged it stuck at 1), and history.back()/forward() traverse
  // them firing popstate with the right location. start -> /a -> /b: length 3, back to /a (pop),
  // back to /start (pop), forward to /a (pop). (A fresh view: the shared `v` accumulates ~50
  // navigations across the suite + caps history.length at 50, which is itself correct.)
  {
    mbView* hv = mbCreateView(W, H);
    mbLoadHTML(hv, "<body>h</body>", "https://histlen.test/start");
    mbRunJS(hv, "window.__log='';addEventListener('popstate',function(){"
                "window.__log+=location.pathname+';';});"
                "history.pushState({},'','/a');history.pushState({},'','/b');");
    mbWait(hv, 30);
    const std::string len = Eval(hv, "''+history.length");
    mbRunJS(hv, "history.back();");  // -> /a
    mbWaitForFunction(hv, "window.__log.indexOf('/a;')>=0", 2000);
    mbRunJS(hv, "history.back();");  // -> /start
    mbWaitForFunction(hv, "window.__log.indexOf('/start;')>=0", 2000);
    mbRunJS(hv, "history.forward();");  // -> /a
    mbWaitForFunction(hv, "window.__log.split('/a;').length>=3", 2000);
    const std::string log = Eval(hv, "window.__log");
    const std::string loc = Eval(hv, "location.pathname");
    mbDestroyView(hv);
    Expect(len == "3" && log == "/a;/start;/a;" && loc == "/a",
           "session history: history.length tracks pushState; back/forward traverse + popstate",
           "len=[" + len + "] log=[" + log + "] loc=[" + loc + "]");
  }

  // 78d. BroadcastChannel cross-origin isolation: two views of DIFFERENT origins with the SAME
  // channel name must NOT cross-talk (BroadcastChannel is same-origin per spec). The in-process
  // registry now scopes delivery by the frame's origin (frame_key->origin map).
  {
    mbView* v2 = mbCreateView(W, H);
    if (v2) {
      mbLoadHTML(v, "<body>x</body>", "https://bca.test/");   // origin A
      mbLoadHTML(v2, "<body>x</body>", "https://bcb.test/");  // origin B (cross-origin)
      mbRunJS(v, "window.__bc='none';var c=new BroadcastChannel('shared');"
                 "c.onmessage=function(e){window.__bc=e.data;};");
      mbRunJS(v2, "var c2=new BroadcastChannel('shared');");
      mbWait(v, 30);
      mbWait(v2, 30);
      mbRunJS(v2, "c2.postMessage('from-B');");
      mbWait(v, 60);
      mbWait(v2, 60);
      const std::string r = Eval(v, "window.__bc");
      Expect(r == "none",
             "BroadcastChannel: cross-origin views with same channel name don't cross-talk",
             "bc=[" + r + "]");
      mbDestroyView(v2);
    }
  }

  // 74. Stability across many sequential loads (a long-running scraper does
  // thousands). Load varied documents repeatedly on one view, evaluating and
  // painting each, and confirm every load renders correctly — no state leak or
  // accumulated breakage degrading later loads. A crash/leak would surface here
  // (and in the no-survivors check).
  {
    bool all_ok = true;
    std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
    for (int n = 0; n < 25 && all_ok; ++n) {
      char html[256];
      std::snprintf(html, sizeof(html),
                    "<body style='margin:0'><div id='n' "
                    "style='width:%dpx;height:10px;background:#%06x'>%d</div>"
                    "<script>window.__k=%d*2;</script></body>",
                    10 + n, (n * 9973) & 0xffffff, n, n);
      mbLoadHTML(v, html, "about:blank");
      if (Eval(v, "document.getElementById('n').textContent") != std::to_string(n) ||
          Eval(v, "String(window.__k)") != std::to_string(n * 2)) {
        all_ok = false;
      }
      if (n % 8 == 0)
        mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // exercise paint too
    }
    Expect(all_ok &&
               Eval(v, "document.getElementById('n').textContent") == "24",
           "stability: 25 sequential loads each render + script correctly");
  }

  // 75. file:// URLs with percent-encoded chars (spaces) decode correctly. The
  // loader used to pass url.path() (still %20-encoded) to the filesystem, so any
  // path with a space failed (common on macOS: "Application Support", fonts with
  // spaces — this broke @font-face). Now it goes through net::FileURLToFilePath.
  // Portable check (no system-font dependency): write a stylesheet whose name has
  // a space, link it via file:///...%20..., and confirm the style applies.
  {
    const char* css_path = "/tmp/mb url space.css";
    if (FILE* f = std::fopen(css_path, "wb")) {
      std::fputs("#z{color:rgb(7,8,9)}", f);
      std::fclose(f);
    }
    // file:// base so the file:// stylesheet is same-origin (isolates the decode
    // fix from opaque-origin subresource policy).
    mbLoadHTML(v,
      "<head><link rel='stylesheet' href='file:///tmp/mb%20url%20space.css'>"
      "</head><body><b id='z'>x</b></body>", "file:///tmp/mb_page.html");
    mbWaitForSelector(v, "#z", 1000);
    mbWait(v, 80);  // let the stylesheet fetch + apply
    Expect(Eval(v, "getComputedStyle(document.getElementById('z')).color") ==
               "rgb(7, 8, 9)",
           "file:// path with spaces decodes + loads (net::FileURLToFilePath)");
    std::remove(css_path);
  }

  // 75a2. mbScrollToBottom drives lazy loading: a scroll handler appends a tall
  // block on each scroll (up to 3), so the page only finishes growing if something
  // actually scrolls it. A small viewport (< one block) guarantees each scrollTo
  // moves. Auto-scrolling to the bottom must trigger all 3 (1 initial + 3 = 4).
  {
    mbResize(v, W, 300);  // viewport shorter than a 600px block -> scrolling moves
    mbLoadHTML(v,
        "<body style='margin:0'>"
        "<div class='blk' style='height:600px'>0</div>"
        "<script>window.__n=0;"
        "window.addEventListener('scroll',function(){"
        "if(window.__n<3){window.__n++;"
        "var d=document.createElement('div');d.className='blk';"
        "d.style.height='600px';d.textContent=window.__n;"
        "document.body.appendChild(d);}});</script></body>",
        "about:blank");
    const int grew = mbScrollToBottom(v, 10);
    const bool all_loaded = Eval(v, "String(document.querySelectorAll('.blk').length)")
                                == "4" &&
                            Eval(v, "String(window.__n)") == "3";
    Expect(grew > 0 && all_loaded,
           "mbScrollToBottom triggers lazy scroll-loaded content (4 blocks)",
           std::string("grew=") + std::to_string(grew) + " blocks=" +
               Eval(v, "String(document.querySelectorAll('.blk').length)"));
    mbResize(v, W, H);  // restore the shared viewport for later cases
  }

  // 75a3. mbScrollToBottom drives IntersectionObserver-based lazy loading — the
  // dominant modern pattern (loading="lazy" / IO libraries), distinct from 75a2's
  // scroll-event approach (it exercises ForceUpdateViewportIntersections between
  // scrolls). A below-fold element watched by an IO flips a flag only once it
  // scrolls into view, so the flag must be false before auto-scroll and true after.
  {
    mbResize(v, W, 300);  // #lz sits ~3000px down, well below this viewport
    mbLoadHTML(v,
        "<body style='margin:0'><div style='height:3000px'></div>"
        "<div id='lz' style='height:40px'></div>"
        "<script>window.__seen=false;"
        "new IntersectionObserver(function(es){es.forEach(function(e){"
        "if(e.isIntersecting)window.__seen=true;});})"
        ".observe(document.getElementById('lz'));</script></body>",
        "about:blank");
    const bool before = Eval(v, "String(window.__seen)") == "false";  // below fold
    mbScrollToBottom(v, 10);  // reveals #lz; this page doesn't grow (flag-only)
    const bool after = Eval(v, "String(window.__seen)") == "true";     // IO fired
    Expect(before && after,
           "mbScrollToBottom drives IntersectionObserver lazy-load (below-fold IO)",
           std::string("before_unseen=") + (before ? "1" : "0") + " after_seen=" +
               (after ? "1" : "0"));
    mbResize(v, W, H);  // restore the shared viewport for later cases
  }

  // 75b. Request log: the loader records every subresource it fetches. Clear it,
  // load a page that links a file:// stylesheet, and confirm the log captured the
  // stylesheet URL; then clear and confirm it empties. (Offline — file:// flows
  // through the same loader chokepoint as network subresources.)
  {
    const char* rl_css = "/tmp/mb_reqlog.css";
    if (FILE* f = std::fopen(rl_css, "wb")) {
      std::fputs("#q{color:rgb(3,2,1)}", f);
      std::fclose(f);
    }
    mbClearRequestLog();
    mbLoadHTML(v,
        "<head><link rel='stylesheet' href='file:///tmp/mb_reqlog.css'></head>"
        "<body><b id='q'>x</b></body>", "file:///tmp/mb_rl_page.html");
    // Wait until the stylesheet applies — proves its fetch reached the loader.
    mbWaitForFunction(
        v, "getComputedStyle(document.getElementById('q')).color==='rgb(3, 2, 1)'",
        2000);
    char rb[4096] = {0};
    int rlen = mbGetRequestLog(rb, sizeof(rb));
    const bool logged =
        rlen > 0 && std::string(rb).find("mb_reqlog.css") != std::string::npos;
    mbClearRequestLog();
    const bool cleared = mbGetRequestLog(nullptr, 0) == 0;
    Expect(logged && cleared,
           "mbGetRequestLog records subresource fetches; mbClearRequestLog empties it",
           std::string("logged=") + (logged ? "1" : "0") + " cleared=" +
               (cleared ? "1" : "0"));
    std::remove(rl_css);
  }

  // 75c. Request blocking: a blocked subresource never loads. With "mb_block.css"
  // blocked, the file:// stylesheet's request fails -> #q keeps the default color;
  // after mbClearUrlBlocks a reload loads it -> #q turns rgb(5,5,5).
  {
    const char* bl_css = "/tmp/mb_block.css";
    if (FILE* f = std::fopen(bl_css, "wb")) {
      std::fputs("#q{color:rgb(5,5,5)}", f);
      std::fclose(f);
    }
    const char* doc =
        "<head><link rel='stylesheet' href='file:///tmp/mb_block.css'></head>"
        "<body><b id='q'>x</b></body>";
    mbBlockUrl("mb_block.css");
    mbLoadHTML(v, doc, "file:///tmp/mb_blk_page.html");
    mbWaitForSelector(v, "#q", 1000);
    mbWait(v, 80);  // give the (blocked) request time to resolve
    const bool blocked =
        Eval(v, "getComputedStyle(document.getElementById('q')).color") !=
        "rgb(5, 5, 5)";
    mbClearUrlBlocks();
    mbLoadHTML(v, doc, "file:///tmp/mb_blk_page.html");  // reload, now unblocked
    const bool applies = mbWaitForFunction(
        v, "getComputedStyle(document.getElementById('q')).color==='rgb(5, 5, 5)'",
        2000) == 1;
    Expect(blocked && applies,
           "mbBlockUrl blocks a subresource; mbClearUrlBlocks restores it",
           std::string("blocked=") + (blocked ? "1" : "0") + " applies=" +
               (applies ? "1" : "0"));
    std::remove(bl_css);
  }

  // 75d. Response mocking (mbMockResponse) — the #1 interception feature. A
  // registered https URL serves a canned stylesheet body WITHOUT any real fetch
  // (so it works fully offline): a <link> to the never-served URL turns #q green.
  // Then mbClearMocks + a re-mock with red proves clearing works and a later mock
  // overrides — all offline, no network. (https subresources route through the
  // loader's Deliver chokepoint, same as the block/log cases.)
  {
    const char* doc =
        "<head><link rel='stylesheet' href='https://mock.test/s.css'></head>"
        "<body><b id='q'>x</b></body>";
    mbClearMocks();
    mbMockResponse("https://mock.test/s.css", "#q{color:rgb(0,170,0)}", "text/css",
                   200);
    mbLoadHTML(v, doc, "https://mock.test/p1.html");
    const bool green = mbWaitForFunction(
        v, "getComputedStyle(document.getElementById('q')).color==='rgb(0, 170, 0)'",
        2000) == 1;
    mbClearMocks();  // drop the green mock...
    mbMockResponse("https://mock.test/s.css", "#q{color:rgb(170,0,0)}", "text/css",
                   200);  // ...and replace with red
    mbLoadHTML(v, doc, "https://mock.test/p2.html");
    const bool red = mbWaitForFunction(
        v, "getComputedStyle(document.getElementById('q')).color==='rgb(170, 0, 0)'",
        2000) == 1;
    mbClearMocks();
    Expect(green && red,
           "mbMockResponse serves a canned body with no fetch; clear/re-mock works",
           std::string("green=") + (green ? "1" : "0") + " red=" +
               (red ? "1" : "0"));
  }

  // 75e. URL rewriting (mbRewriteUrl) — request redirected before fetch. The page
  // links orig.test/r.css; a rewrite sends the request to mock.test/r.css, which a
  // mock serves green. So #q turns green purely via rewrite+mock, no network. Clear
  // the rewrite and a fresh page linking orig.test no longer styles (no mock there).
  {
    mbClearMocks();
    mbClearUrlRewrites();
    mbMockResponse("https://mock.test/r.css", "#q{color:rgb(0,150,30)}", "text/css",
                   200);
    mbRewriteUrl("orig.test/r.css", "mock.test/r.css");  // redirect the request
    mbLoadHTML(v,
        "<head><link rel='stylesheet' href='https://orig.test/r.css'></head>"
        "<body><b id='q'>x</b></body>",
        "https://orig.test/rw1.html");
    const bool rewritten = mbWaitForFunction(
        v, "getComputedStyle(document.getElementById('q')).color==='rgb(0, 150, 30)'",
        2000) == 1;
    // Clear the rewrite; mock the ORIGINAL url red. The page now gets red (served
    // by orig.test's own mock), NOT the rewrite's green — proving clear worked,
    // and staying fully offline (no fetch of an unmocked URL).
    mbClearUrlRewrites();
    mbMockResponse("https://orig.test/r.css", "#q{color:rgb(150,0,0)}", "text/css",
                   200);
    mbLoadHTML(v,
        "<head><link rel='stylesheet' href='https://orig.test/r.css'></head>"
        "<body><b id='q'>x</b></body>",
        "https://orig.test/rw2.html");
    const bool not_rewritten = mbWaitForFunction(
        v, "getComputedStyle(document.getElementById('q')).color==='rgb(150, 0, 0)'",
        2000) == 1;
    mbClearMocks();
    Expect(rewritten && not_rewritten,
           "mbRewriteUrl redirects a request before fetch; clear restores",
           std::string("rewritten=") + (rewritten ? "1" : "0") + " cleared=" +
               (not_rewritten ? "1" : "0"));
  }

  // 76. CSS background-image renders (data: SVG). Distinct from <img>: exercises
  // the CSS background paint path + a data: URL image + SVG-as-image. A 30x30 div
  // with a green-SVG background should paint green at its center.
  {
    mbLoadHTML(v,
      "<body style='margin:0;background:#fff'>"
      "<div style='width:30px;height:30px;background-image:url("
      "\"data:image/svg+xml,<svg xmlns=%22http://www.w3.org/2000/svg%22 "
      "width=%2230%22 height=%2230%22><rect width=%2230%22 height=%2230%22 "
      "fill=%22%2300ff00%22/></svg>\")'></div></body>",
      "about:blank");
    mbWait(v, 80);  // background image decode + paint
    std::vector<uint8_t> bg(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, bg.data(), W, H, W * 4);
    size_t mid = (15u * W + 15u) * 4;  // center of the div
    Expect(bg[mid] == 0 && bg[mid + 1] == 255 && bg[mid + 2] == 0,  // green
           "CSS background-image (data: SVG) paints");
  }

  // 77. iframes work end to end: the child frame is created (CreateChildFrame)
  // AND its srcdoc content commits (BeginNavigation fills the body + policy
  // container). frames.length==1, and the child document holds the srcdoc DOM —
  // contentDocument.body.textContent is the iframe's content. Parent unaffected.
  {
    mbLoadHTML(v,
      "<body><b id='p'>parent</b>"
      "<iframe id='f' srcdoc='<b>child-body</b>' width='80' height='40'></iframe>"
      "</body>", "about:blank");
    mbWait(v, 100);  // let the child commit + parse
    Expect(Eval(v, "1+1") == "2" &&
               Eval(v, "document.getElementById('p').textContent") == "parent" &&
               Eval(v, "String(window.frames.length)") == "1" &&
               Eval(v, "document.getElementById('f').contentDocument.body.textContent")
                   == "child-body",
           "iframe loads: child frame created + srcdoc content commits");
  }

  // 77b. iframe content RENDERS into the parent's paint — not just the DOM (77).
  // The child frame must composite into the screenshot for captures of pages with
  // ads / embeds / maps / social widgets to be correct. Place a solid-green iframe
  // at the top-left and read a pixel inside its box.
  {
    mbResize(v, W, H);
    mbLoadHTML(v,
        "<body style='margin:0'>"
        "<iframe srcdoc=\"<body style='margin:0;background:rgb(0,128,0)'></body>\" "
        "width='150' height='80' style='border:0;display:block'></iframe></body>",
        "about:blank");
    mbWait(v, 150);  // let the child commit + paint
    std::vector<uint8_t> ifpx(static_cast<size_t>(W) * H * 4, 255);
    mbPaintToBitmap(v, ifpx.data(), W, H, W * 4);
    const size_t o = (static_cast<size_t>(30) * W + 40) * 4;  // inside the iframe box
    const int pb = ifpx[o], pg = ifpx[o + 1], pr = ifpx[o + 2];
    Expect(pr < 16 && pg > 110 && pg < 145 && pb < 16,
           "iframe content renders into the parent paint (green child paints)",
           std::string("rgb(") + std::to_string(pr) + "," + std::to_string(pg) +
               "," + std::to_string(pb) + ")");
  }

  // 78. iframe src= loads too (not just srcdoc): the child's navigation fetches
  // the src body via the loader (MbFetchUrl) and commits it. Uses a data: src
  // (portable; file/http go through the same path). The parent is loaded from a
  // file:// base so the child data: document inherits that origin (same-origin),
  // letting the parent read contentDocument; with an opaque (about:blank) parent
  // the child would get a fresh opaque origin and the read would be cross-origin
  // blocked — that's correct browser behavior, not a commit failure.
  {
    mbLoadHTML(v,
      "<body>p<iframe id='f' src='data:text/html,<b>src-child</b>' "
      "width='80' height='40'></iframe></body>", "file:///tmp/p.html");
    mbWait(v, 250);  // child navigation: fetch + commit + parse
    Expect(Eval(v,
        "document.getElementById('f').contentDocument.body.textContent") ==
            "src-child",
        "iframe src= loads: child fetches + commits its document");
  }

  // 78b. Sub-frame eval reads a CROSS-ORIGIN iframe (audit gap #2: per-frame
  // targeting). Under an https parent a data: iframe gets an opaque origin, so the
  // parent's iframe.contentDocument is same-origin-policy blocked — but
  // mbEvalJSInFrame runs host-privileged in the child's OWN world and reads it.
  // mbGetFrameCount reports the child. This is how iframe content (ads/embeds that
  // are cross-origin) becomes scrapable.
  {
    mbLoadHTML(v,
        "<body>parent<iframe src='data:text/html,"
        "<body>XFRAME-SECRET</body>' width='80' height='40'></iframe></body>",
        "https://parent.test/");
    mbWait(v, 250);  // child navigation + commit
    const int frames = mbGetFrameCount(v);
    // Parent CANNOT read the cross-origin child via contentDocument (SOP).
    const std::string parent_read = Eval(v,
        "(function(){try{var d=document.querySelector('iframe').contentDocument;"
        "return d?d.body.textContent:'NULLDOC';}catch(e){return 'THREW:'+e.name;}})()");
    // ...but the host can, via a privileged eval in the child frame's own context.
    char fb[128] = {0};
    mbEvalJSInFrame(v, 0, "document.body.textContent", fb, sizeof(fb));
    const std::string in_frame(fb);
    Expect(frames == 1 && parent_read != "XFRAME-SECRET" &&
               in_frame == "XFRAME-SECRET",
           "mbEvalJSInFrame reads a cross-origin iframe the parent can't",
           std::string("frames=") + std::to_string(frames) + " parent=[" +
               parent_read + "] inframe=[" + in_frame + "]");
  }

  // 78b2. Per-frame selector ops (fill + get-text) reach a CROSS-ORIGIN iframe:
  // the typed peers of mbFillSelector / mbGetTextForSelector scoped to a child
  // frame. mbFillSelectorInFrame fills a form field inside the (opaque-origin)
  // child with the same React-compatible value-set + input/change dispatch;
  // mbGetTextForSelectorInFrame reads an element's innerText there. So an embedded
  // cross-origin form/widget is fillable + scrapable like the main document — DOM
  // only, no synthetic gesture (no cross-frame coordinate mapping needed).
  {
    mbLoadHTML(v,
        "<body>parent<iframe src='data:text/html,"
        "<input id=f><div id=t>FRAME-TEXT</div>"
        "' width='120' height='60'></iframe></body>",
        "https://parent.test/");
    mbWait(v, 250);  // child navigation + commit
    const int filled = mbFillSelectorInFrame(v, 0, "#f", "typed-in-frame");
    char vb[128] = {0};  // read the filled value back from the child's own world
    mbEvalJSInFrame(v, 0, "document.querySelector('#f').value", vb, sizeof(vb));
    const std::string fval(vb);
    char tb[128] = {0};
    const int tlen = mbGetTextForSelectorInFrame(v, 0, "#t", tb, sizeof(tb));
    const std::string ftext(tb);
    // A non-matching selector in the same frame returns -1 (distinct from "").
    const int miss = mbGetTextForSelectorInFrame(v, 0, "#nope", tb, sizeof(tb));
    Expect(filled == 1 && fval == "typed-in-frame" && tlen == 10 &&
               ftext == "FRAME-TEXT" && miss == -1,
           "per-frame fill + get-text reach a cross-origin iframe's form/content",
           "filled=" + std::to_string(filled) + " val=[" + fval + "] text=[" +
               ftext + "] miss=" + std::to_string(miss));
  }

  // 78c. Non-UTF-8 iframe charset (audit #14): a child frame whose response declares a
  // non-UTF-8 charset must decode with THAT charset, not the old hardcoded UTF-8 (which
  // turned the bytes into mojibake). Mock an iframe serving the Shift-JIS bytes for
  // "日本" (0x93 0xFA 0x96 0x7B) with charset=shift_jis; the child must decode to the
  // correct code points (U+65E5 U+672C), read host-privileged via mbEvalJSInFrame.
  {
    mbClearMocks();
    mbMockResponse("sjis.test/p", "<body>\x93\xFA\x96\x7B</body>",
                   "text/html; charset=shift_jis", 200);
    mbLoadHTML(v,
        "<body>p<iframe src='https://sjis.test/p' width='60' height='30'></iframe>"
        "</body>", "https://parent.test/");
    mbWait(v, 300);
    char cc[64] = {0};
    mbEvalJSInFrame(v, 0,
        "''+document.body.textContent.charCodeAt(0)+','+"
        "document.body.textContent.charCodeAt(1)", cc, sizeof(cc));
    mbClearMocks();
    Expect(std::string(cc) == "26085,26412",  // U+65E5 (日), U+672C (本)
           "non-UTF-8 (Shift-JIS) iframe decodes by its declared charset, not mojibake",
           std::string("codes=[") + cc + "]");
  }

  // 79. <iframe sandbox> is enforced: the owner's FramePolicy sandbox flags
  // reach the committed child document (CreateChildFrame -> BeginNavigation
  // applies them). The cleanest origin-independent signal is script blocking:
  // a sandboxed child (no allow-scripts) must NOT run its inline script, while
  // a non-sandboxed sibling does. Each child's script tags its own <body> with
  // data-ran; we read it back from the parent. (We avoid asserting via a
  // cross-origin read because the file:// parent has universal access here, so
  // it can read even an opaque-origin child — origin enforcement still happens,
  // but isn't observable that way.)
  {
    mbLoadHTML(v,
      "<body>"
      "<iframe id='sb' sandbox srcdoc=\"<body><script>"
      "document.body.setAttribute('data-ran','yes')</scr" "ipt></body>\">"
      "</iframe>"
      "<iframe id='op' srcdoc=\"<body><script>"
      "document.body.setAttribute('data-ran','yes')</scr" "ipt></body>\">"
      "</iframe>"
      "</body>", "file:///tmp/p.html");
    mbWait(v, 200);
    // Sandboxed (no allow-scripts): inline script must not run.
    Expect(Eval(v,
        "''+document.getElementById('sb').contentDocument.body"
        ".getAttribute('data-ran')") == "null",
        "iframe sandbox enforced: sandboxed child's script is blocked");
    // Non-sandboxed sibling: its script runs.
    Expect(Eval(v,
        "''+document.getElementById('op').contentDocument.body"
        ".getAttribute('data-ran')") == "yes",
        "iframe sandbox scoped: non-sandboxed sibling's script runs");
  }

  // 78. element.scrollIntoView() works (a common automation primitive: scroll a
  // target into view before clicking/capturing). Our non-compositing widget
  // handles scroll specially, so verify programmatic scroll-into-view actually
  // moves the viewport and lands the element on-screen.
  {
    mbLoadHTML(v,
      "<body style='margin:0'><div style='height:1500px'></div>"
      "<b id='t'>T</b><div style='height:400px'></div></body>", "about:blank");
    std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // force layout for scroll
    Expect(Eval(v,
        "(function(){window.scrollTo(0,0);var t=document.getElementById('t');"
        "var b=window.scrollY;t.scrollIntoView();var a=window.scrollY;"
        "var top=t.getBoundingClientRect().top;"
        "return (a>b && top>=0 && top<window.innerHeight)?'ok'"
        ":(a+'/'+b+'/'+Math.round(top));})()") == "ok",
        "scrollIntoView scrolls the target into the viewport");
  }

  // 80. Page-initiated main-frame navigation: a link click or location=
  // assignment must actually navigate the top frame. Previously BeginNavigation
  // early-returned for the main frame, so a page that navigated itself did
  // nothing. Now the main frame posts the commit. Drive it via a JS location
  // assignment (the anchor default-action path is the same BeginNavigation
  // hook) and confirm the new document is live.
  {
    const char* a = "<body><div id=o>navA-here</div></body>";
    const char* b = "<body><div id=o>navB-here</div></body>";
    if (FILE* f = std::fopen("/tmp/mb_navA.html", "wb")) {
      std::fwrite(a, 1, std::strlen(a), f); std::fclose(f);
    }
    if (FILE* f = std::fopen("/tmp/mb_navB.html", "wb")) {
      std::fwrite(b, 1, std::strlen(b), f); std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_navA.html");
    const bool on_a =
        Eval(v, "document.getElementById('o').textContent") == "navA-here";
    mbRunJS(v, "location.href='file:///tmp/mb_navB.html';");
    mbWait(v, 300);  // posted commit + fetch + parse
    const std::string after =
        Eval(v, "document.getElementById('o').textContent");
    Expect(on_a && after == "navB-here",
           "page-initiated main-frame navigation (location=) commits", after);
  }

  // 81. Character encoding: a non-UTF-8 page is decoded via <meta charset>.
  // CommitHtml used to force UTF-8 (authoritative), turning latin-1/Shift_JIS
  // bytes into mojibake. The encoding is now tentative so the parser honors the
  // declared charset. (Eval returns the JS string as UTF-8, so 'é' compares as
  // the 2-byte 0xC3 0xA9 sequence.)
  {
    // ISO-8859-1 source bytes: 0xE9='é', 0xE8='è'.
    const char* doc =
        "<meta charset=\"ISO-8859-1\"><body><div id=o>caf\xe9 cr\xe8me</div></body>";
    if (FILE* f = std::fopen("/tmp/mb_latin1.html", "wb")) {
      std::fwrite(doc, 1, std::strlen(doc), f); std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_latin1.html");
    mbWait(v, 50);
    Expect(Eval(v, "document.getElementById('o').textContent") ==
               "caf\xc3\xa9 cr\xc3\xa8me",
           "non-UTF-8 page decodes via <meta charset> (ISO-8859-1)");
  }

  // 81b. Legacy CJK charsets decode too — not just latin-1. Confirms the bundled
  // ICU/codec data covers the common Asian-site encodings (a real international-
  // scraping need). Checked via code points (charCodeAt) to avoid UTF-8
  // byte-compare noise.
  {
    // Shift_JIS bytes 0x93FA 0x967B = 日(U+65E5=26085) 本(U+672C=26412).
    const char* sjis =
        "<meta charset=\"Shift_JIS\"><body><span id=o>\x93\xfa\x96\x7b</span></body>";
    if (FILE* f = std::fopen("/tmp/mb_sjis.html", "wb")) {
      std::fwrite(sjis, 1, std::strlen(sjis), f); std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_sjis.html");
    mbWait(v, 50);
    const std::string sj = Eval(v,
        "var t=document.getElementById('o').textContent;"
        "t.length+':'+t.charCodeAt(0)+','+t.charCodeAt(1)");
    // GBK bytes 0xD6D0 0xCEC4 = 中(U+4E2D=20013) 文(U+6587=25991).
    const char* gbk =
        "<meta charset=\"GBK\"><body><span id=o>\xd6\xd0\xce\xc4</span></body>";
    if (FILE* f = std::fopen("/tmp/mb_gbk.html", "wb")) {
      std::fwrite(gbk, 1, std::strlen(gbk), f); std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_gbk.html");
    mbWait(v, 50);
    const std::string gb = Eval(v,
        "var t=document.getElementById('o').textContent;"
        "t.length+':'+t.charCodeAt(0)+','+t.charCodeAt(1)");
    Expect(sj == "2:26085,26412" && gb == "2:20013,25991",
           "legacy CJK charsets decode via <meta charset> (Shift_JIS + GBK)",
           std::string("sjis=") + sj + " gbk=" + gb);
  }

  // 82. UTF-8 without a <meta charset> still decodes (auto-detection) — guards
  // that the tentative-encoding change didn't regress the common UTF-8 default.
  {
    const char* doc = "<body><div id=o>caf\xc3\xa9</div></body>";  // UTF-8 'é'
    if (FILE* f = std::fopen("/tmp/mb_utf8nm.html", "wb")) {
      std::fwrite(doc, 1, std::strlen(doc), f); std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_utf8nm.html");
    mbWait(v, 50);
    Expect(Eval(v, "document.getElementById('o').textContent") == "caf\xc3\xa9",
           "UTF-8 page without <meta charset> decodes (auto-detect)");
  }

  // 83. mbGetURL / mbGetTitle: read the committed document's URL + title through
  // the C API (no JS), straight from the frame.
  {
    mbLoadHTML(v, "<head><title>My Title</title></head><body>x</body>",
               "file:///tmp/mb_titletest.html");
    char ub[512] = {0}, tb[256] = {0};
    mbGetURL(v, ub, sizeof(ub));
    mbGetTitle(v, tb, sizeof(tb));
    Expect(std::string(ub) == "file:///tmp/mb_titletest.html",
           "mbGetURL returns the document URL", ub);
    Expect(std::string(tb) == "My Title",
           "mbGetTitle returns the document title", tb);
  }

  // 84. mbGetText / mbGetHTML scraping accessors.
  {
    mbLoadHTML(v, "<body><p>Alpha</p><p>Beta</p></body>", "about:blank");
    char xb[1024] = {0};
    mbGetText(v, xb, sizeof(xb));
    Expect(std::string(xb).find("Alpha") != std::string::npos &&
               std::string(xb).find("Beta") != std::string::npos,
           "mbGetText returns visible text", xb);
    char hb[2048] = {0};
    int hlen = mbGetHTML(v, hb, sizeof(hb));
    Expect(hlen > 0 && std::string(hb).find("<p>Alpha</p>") != std::string::npos,
           "mbGetHTML returns serialized DOM", hb);
  }

  // 85. mbReload re-fetches the document: mutate the DOM, reload, the mutation is
  // gone (the file is re-read and re-committed).
  {
    const char* doc = "<body><div id=o>ORIGINAL</div></body>";
    if (FILE* f = std::fopen("/tmp/mb_reload.html", "wb")) {
      std::fwrite(doc, 1, std::strlen(doc), f); std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_reload.html");
    mbRunJS(v, "document.getElementById('o').textContent='MUTATED';");
    const bool mutated =
        Eval(v, "document.getElementById('o').textContent") == "MUTATED";
    mbReload(v);
    mbWait(v, 50);
    Expect(mutated && Eval(v, "document.getElementById('o').textContent") ==
                          "ORIGINAL",
           "mbReload re-fetches + re-commits the document");
  }

  // 86. Host-driven history (mbGoBack/mbGoForward) over a host load + a
  // page-initiated navigation: A --(location.href)--> B, back to A, forward to B.
  {
    const char* a = "<body><div id=o>histA</div></body>";
    const char* b = "<body><div id=o>histB</div></body>";
    if (FILE* f = std::fopen("/tmp/mb_hA.html", "wb")) {
      std::fwrite(a, 1, std::strlen(a), f); std::fclose(f);
    }
    if (FILE* f = std::fopen("/tmp/mb_hB.html", "wb")) {
      std::fwrite(b, 1, std::strlen(b), f); std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_hA.html");          // history: [A]
    mbRunJS(v, "location.href='file:///tmp/mb_hB.html';");  // page nav -> [A,B]
    mbWait(v, 300);
    const bool on_b =
        Eval(v, "document.getElementById('o').textContent") == "histB";
    const bool cgb = mbCanGoBack(v) == 1 && mbCanGoForward(v) == 0;
    mbGoBack(v);
    mbWait(v, 100);
    const bool back_a =
        Eval(v, "document.getElementById('o').textContent") == "histA" &&
        mbCanGoForward(v) == 1;
    mbGoForward(v);
    mbWait(v, 100);
    const bool fwd_b =
        Eval(v, "document.getElementById('o').textContent") == "histB";
    Expect(on_b && cgb && back_a && fwd_b,
           "mbGoBack/mbGoForward navigate the history stack",
           std::string("on_b=") + (on_b ? "1" : "0") + " cgb=" +
               (cgb ? "1" : "0") + " back_a=" + (back_a ? "1" : "0") +
               " fwd_b=" + (fwd_b ? "1" : "0"));
  }

  // 86b. Page-driven history (history.back/forward/go from JS) is crash-safe. The host now
  // SERVICES page-initiated session-history nav (GoToEntryAtOffset -> traversal; verified in
  // mb_smoke 23at), so back()/go(-1) here may actually navigate the view away. The invariant
  // under test is the safety one: an untrusted page driving session history must never crash
  // the single-process host. We confirm by driving it, then loading a fresh page and checking
  // the host is still alive and scriptable.
  {
    mbLoadHTML(v,
        "<body><script>try{history.back();history.forward();history.go(-1);}"
        "catch(e){}</script></body>",
        "https://hist.test/");
    mbWait(v, 50);  // let any async traversal settle
    mbLoadHTML(v, "<body><p id='m'>alive</p></body>", "https://hist2.test/");
    Expect(Eval(v, "document.getElementById('m').textContent") == "alive" &&
               Eval(v, "1+1") == "2",
           "page-driven history.back/forward/go is crash-safe (host survives)",
           Eval(v, "1+1"));
  }

  // REVIEW-FIX #7. An aborted version-change (upgrade) transaction must roll back the
  // SCHEMA (version + object stores) — the per-txn data snapshot doesn't cover
  // metadata, so pre-fix an aborted upgrade left the DB at the new version with a
  // half-built store. Abort inside onupgradeneeded, then reopen: the schema must be
  // back at version 0 with no stores (a fresh upgrade fires). (Whether open()'s
  // onerror fires is reported but not gated: the abort signal goes through the
  // unchanged db_callbacks->Abort and is Blink-timing-dependent in this synchronous
  // in-process model; the reviewed+fixed defect here is the schema rollback.)
  {
    mbLoadHTML(v, "<body>idbabort</body>", "https://idbabort.test/");
    mbRunJS(v,
        "window.__s='';var q=indexedDB.open('abrt',1);"
        "q.onupgradeneeded=function(e){e.target.result.createObjectStore('s');"
        "  e.target.transaction.abort();};"
        "q.onerror=function(){window.__s='aborted';};"
        "q.onsuccess=function(){window.__s='opened';};");
    std::string s;
    for (int i = 0; i < 120 && s.empty(); ++i) { mbWait(v, 25); s = Eval(v, "window.__s"); }
    mbRunJS(v,
        "window.__r='';var q2=indexedDB.open('abrt',1);"
        "q2.onupgradeneeded=function(e){window.__r='upgrade:'+e.oldVersion+':'+"
        "  e.target.result.objectStoreNames.length;};"
        "q2.onsuccess=function(e){window.__r=window.__r||('noupg:'+"
        "  e.target.result.objectStoreNames.length);};"
        "q2.onerror=function(){window.__r='err';};");
    std::string r;
    for (int i = 0; i < 200 && r.empty(); ++i) { mbWait(v, 25); r = Eval(v, "window.__r"); }
    Expect(r.rfind("upgrade:0:", 0) == 0 && Eval(v, "1+1") == "2",
           "IndexedDB aborted upgrade rolls back schema; host survives (#7)",
           "open=[" + s + "] reopen=[" + r + "]");
  }

  // REVIEW-FIX #8. A 'prev' (descending) cursor's continue(key) must seek in the
  // cursor's direction. Pre-fix it compared ascending on the pre-reversed entry list
  // and mis-seeked: on keys 1..5, after the open delivers 5, continue(3) wrongly
  // landed on 4 instead of 3. (We isolate the keyed-seek behavior — the reviewed bug
  // — rather than full post-seek iteration, which folds in Blink's cursor prefetch.)
  {
    mbLoadHTML(v, "<body>idbcur</body>", "https://idbcur.test/");
    mbRunJS(v,
        "window.__s='';var q=indexedDB.open('cur',1);"
        "q.onupgradeneeded=function(e){var os=e.target.result.createObjectStore('s');"
        "  for(var i=1;i<=5;i++)os.put('v'+i,i);};"
        "q.onsuccess=function(e){var n=0;"
        "  var c=e.target.result.transaction('s').objectStore('s').openCursor(null,'prev');"
        "  c.onsuccess=function(ev){var cur=ev.target.result;n++;"
        "    if(n===1){if(!cur||cur.key!==5){window.__s='bad-open:'+(cur?cur.key:'null');return;}"
        "      cur.continue(3);}"  // descending: must land on the first key <= 3, i.e. 3
        "    else{window.__s='k='+(cur?cur.key:'null');}};};");
    std::string s;
    for (int i = 0; i < 200 && s.empty(); ++i) { mbWait(v, 25); s = Eval(v, "window.__s"); }
    Expect(s == "k=3",
           "IndexedDB prev-cursor continue(key) seeks descending, lands on 3 not 4 (#8)",
           "s=[" + s + "]");
  }

  // REVIEW-FIX #3. deleteDatabase with a STILL-OPEN handle must not free the backend
  // out from under it (use-after-free); the host must stay alive and a fresh reopen
  // must upgrade (version reset). (Release has no ASan, so this exercises the path +
  // asserts host survival rather than detecting the freed read directly.)
  {
    mbLoadHTML(v, "<body>idbuaf</body>", "https://idbuaf.test/");
    mbRunJS(v,
        "window.__s='';var q=indexedDB.open('uaf',1);"
        "q.onupgradeneeded=function(e){e.target.result.createObjectStore('s',{keyPath:'id'});};"
        "q.onsuccess=function(e){window.__db=e.target.result;"
        "  var t=window.__db.transaction('s','readwrite');t.objectStore('s').put({id:1});"
        "  t.oncomplete=function(){window.__s='put';};};");
    std::string s;
    for (int i = 0; i < 200 && s != "put"; ++i) { mbWait(v, 25); s = Eval(v, "window.__s"); }
    mbRunJS(v,
        "window.__r='';var d=indexedDB.deleteDatabase('uaf');"
        "d.onsuccess=function(){"
        "  try{window.__db.transaction('s').objectStore('s').get(1);}catch(e){}"  // touch stale handle
        "  var q2=indexedDB.open('uaf',2);"
        "  q2.onupgradeneeded=function(){window.__r='upgrade';};"  // fresh backend -> upgrade fires
        "  q2.onsuccess=function(){window.__r=window.__r||'noupg';};};");
    std::string r;
    for (int i = 0; i < 200 && r.empty(); ++i) { mbWait(v, 25); r = Eval(v, "window.__r"); }
    Expect(s == "put" && r == "upgrade" && Eval(v, "1+1") == "2",
           "IndexedDB deleteDatabase with a live handle is crash-safe + reopens fresh (#3)",
           "s=[" + s + "] r=[" + r + "]");
  }

  // REVIEW-FIX #6. A blob: URL fetched right after createObjectURL of a LARGE
  // (>256KB -> BytesProvider) blob must return the FULL bytes — pre-fix the clone
  // ran before materialization and served truncated/empty bytes.
  {
    mbLoadHTML(v, "<body>blobfetch</body>", "https://blobfetch.test/");
    mbRunJS(v,
        "window.__s='';var big='Z'.repeat(400000);"
        "var u=URL.createObjectURL(new Blob([big],{type:'text/plain'}));"
        "fetch(u).then(function(r){return r.text();}).then(function(t){"
        "  window.__s='len='+t.length+' ok='+(t.length===400000&&t[399999]==='Z');})"
        ".catch(function(){window.__s='err';});");
    std::string s;
    for (int i = 0; i < 240 && s.empty(); ++i) { mbWait(v, 25); s = Eval(v, "window.__s"); }
    Expect(s == "len=400000 ok=true",
           "fetch of a blob: URL for a >256KB blob returns full bytes (#6)",
           "s=[" + s + "]");
  }
}

MB_SMOKE_MAIN("mb_smoke_render")
