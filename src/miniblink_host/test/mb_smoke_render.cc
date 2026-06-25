// mb_smoke_render — split from the mb_smoke monolith: modern/cutting-edge CSS,
// web components, platform-API crash-safety (workers/IDB/WS/dialogs/clipboard/
// crypto/audio/streams), blob, paint correctness, fonts/SVG/transitions,
// selector automation, navigation, charset, reload, host-driven history.
#include "miniblink_host/test/mb_smoke_harness.h"

using mbsmoke::Eval;
using mbsmoke::EvalIso;
using mbsmoke::Expect;

static void RunCases(mbView* v, int W, int H) {
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

  // 41. Canvas 2D full round-trip + WebGL graceful-null. Canvas is core for a
  // renderer and the backbone of chart/image libraries, so verify the complete
  // path works offline: get a 2D context, draw, read pixels back via
  // getImageData (exact color), and encode via toDataURL. Separately, WebGL has
  // no GPU backend here, so getContext('webgl') must return null (clean
  // feature-detection), not crash. Both are common; this locks in that 2D works
  // and WebGL degrades.
  mbLoadHTML(v, "<body><canvas id='c' width='20' height='20'></canvas></body>",
             "about:blank");
  mbRunJS(v,
    "var cv=document.getElementById('c'),x=cv.getContext('2d');"
    "x.fillStyle='#ff0000';x.fillRect(0,0,20,20);"
    "var d=x.getImageData(10,10,1,1).data;"
    "window.__rgba=d[0]+','+d[1]+','+d[2]+','+d[3];"
    "window.__png=cv.toDataURL().indexOf('data:image/png')===0;"
    "window.__gl=(document.createElement('canvas').getContext('webgl')===null);");
  Expect(Eval(v, "window.__rgba") == "255,0,0,255" &&
             Eval(v, "String(window.__png)") == "true" &&
             Eval(v, "String(window.__gl)") == "true",
         "Canvas 2D round-trip (draw/getImageData/toDataURL); WebGL null");

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

  // 56b. Emoji rasterize to glyph pixels — but MONOCHROME in this build. A color-
  // emoji font is not bundled, so U+1F600 😀 paints as a grayscale/black glyph
  // (saturated color pixels = 0), not Apple-Color-Emoji yellow. This is a known,
  // documented limitation (emoji in screenshots won't be colorful); the guard is
  // that it still rasterizes a real glyph (dark strokes + light gaps, like text)
  // and degrades gracefully rather than crashing or rendering tofu boxes. If a
  // color-emoji font is ever bundled, `colorful` jumps and this comment is stale.
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
    Expect(dark > 20 && light > 20 && colorful == 0,
           "emoji rasterizes (monochrome glyph; no color-emoji font bundled)",
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

  // 62-popup. Popup / new-window safety: a scraped or automated page that calls
  // window.open or activates a target=_blank link must never crash the
  // single-process host. Modern Blink (M150) has no WebViewClient::CreateView to
  // override — the factory methods migrated out of that interface — and the
  // default denies the popup, so window.open returns null and the _blank
  // activation is a safe no-op here. This locks that in: a crash would take down
  // any embedder that runs untrusted pages.
  {
    mbLoadHTML(v,
        "<body><a id='b' href='https://example.com/' target='_blank'>x</a>"
        "<script>window.__r=String(window.open('about:blank'));</script></body>",
        "about:blank");
    const std::string opened = Eval(v, "window.__r");  // "null" == popup denied
    const int clicked = mbClickSelector(v, "#b");       // must not crash the host
    const std::string alive = Eval(v, "'alive'");       // host still responsive
    Expect(opened == "null" && clicked == 1 && alive == "alive",
           "popup safety: window.open -> null; _blank click doesn't crash the host",
           std::string("open=") + opened + " click=" + std::to_string(clicked));
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

  // 86b. Page-driven history (history.back/forward/go from JS) is crash-safe. The
  // host routes page-initiated session-history nav through LocalFrameHost
  // (GoToEntryAtOffset); wiring that to actually navigate is an in-progress effort
  // (host-driven mbGoBack/mbGoForward already work, case 86). Until then it's a
  // graceful no-op — but an untrusted page calling history.back() must never crash
  // the single-process host. (pushState/replaceState DO work, case 39b.) This
  // safety invariant holds before AND after that wiring lands.
  {
    mbLoadHTML(v,
        "<body><p id='m'>alive</p><script>window.__hr='pre';"
        "try{history.back();history.forward();history.go(-1);window.__hr='ok';}"
        "catch(e){window.__hr='THREW:'+e.name;}</script></body>",
        "https://hist.test/");
    Expect(Eval(v, "window.__hr") == "ok" &&
               Eval(v, "document.getElementById('m').textContent") == "alive" &&
               Eval(v, "1+1") == "2",
           "page-driven history.back/forward/go is crash-safe",
           Eval(v, "window.__hr"));
  }


}

MB_SMOKE_MAIN("mb_smoke_render")
