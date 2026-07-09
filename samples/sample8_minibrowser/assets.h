// minibrowser_assets.h — the embedded UI pages of the minibrowser sample.
//
// Like Ultralight's MiniBrowser (whose chrome is assets/ui/ui.html rendered in
// an overlay view), the browser chrome here — tab bar, toolbar, address bar,
// loading spinner — is itself a WEB PAGE, rendered by a dedicated mbView and
// wired to native code with mbJsBindFunction. Embedding the HTML as strings
// (their embed_files.cmake, our raw literals) keeps the sample a single
// self-contained binary.
//
// Chrome page contract:
//   page -> native (bound fns): OnBack() OnForward() OnRefresh() OnStop()
//     OnRequestChangeURL(url) OnNewTab() OnCloseTab(id) OnSelectTab(id)
//     OnToggleTools()
//   native -> page (mbRunJS):  addTab(id) selectTab(id) closeTab(id)
//     updateTab(id, title, loading) updateURL(url) updateNav(back, fwd)
//     updateLoading(bool) showToast(text)
#ifndef MB_SAMPLES_MINIBROWSER_ASSETS_H_
#define MB_SAMPLES_MINIBROWSER_ASSETS_H_

// ---- The browser chrome (tab bar + toolbar), rendered by the chrome mbView.
static const char kChromeHTML[] = R"HTML(
<!doctype html>
<html><head><style>
  html, body { margin: 0; height: 100%; overflow: hidden; user-select: none;
               font: 13px system-ui, sans-serif; }
  body { display: flex; flex-direction: column; background: #dee1e6; }

  /* -- tab strip ----------------------------------------------------- */
  #tabs { display: flex; align-items: flex-end; height: 34px; padding: 4px 6px 0; }
  .tab { display: flex; align-items: center; gap: 6px; min-width: 0;
         width: 200px; height: 30px; padding: 0 8px 0 10px; margin-right: 1px;
         border-radius: 8px 8px 0 0; background: #cdd1d6; color: #333;
         cursor: default; }
  .tab.active { background: #f6f7f8; }
  .tab .title { flex: 1; overflow: hidden; text-overflow: ellipsis;
                white-space: nowrap; }
  .tab .close { width: 16px; height: 16px; line-height: 15px; text-align: center;
                border-radius: 8px; color: #666; flex: none; }
  .tab .close:hover { background: #b6babf; color: #222; }
  .tab .spin { width: 12px; height: 12px; flex: none; border-radius: 50%;
               border: 2px solid #9ab; border-top-color: #345;
               animation: spin .8s linear infinite; display: none; }
  .tab.loading .spin { display: inline-block; }
  @keyframes spin { to { transform: rotate(360deg); } }
  #newtab { width: 26px; height: 26px; margin: 0 0 2px 4px; border-radius: 13px;
            text-align: center; line-height: 24px; font-size: 17px; color: #444;
            flex: none; }
  #newtab:hover { background: #cdd1d6; }

  /* -- toolbar -------------------------------------------------------- */
  #bar { display: flex; align-items: center; gap: 4px; height: 40px;
         padding: 0 8px; background: #f6f7f8; }
  .btn { width: 30px; height: 30px; border-radius: 15px; display: flex;
         align-items: center; justify-content: center; color: #444; flex: none; }
  .btn:hover { background: #e2e5e9; }
  .btn.disabled { color: #bbb; pointer-events: none; }
  .btn svg { fill: currentColor; }
  #address { flex: 1; height: 28px; border: 0; border-radius: 14px;
             padding: 0 14px; background: #e8eaed; font: 13px system-ui;
             outline: none; }
  #address:focus { background: #fff; box-shadow: 0 0 0 2px #8ab4f8; }
  #stop { display: none; }
  body.loading #refresh { display: none; }
  body.loading #stop { display: flex; }

  /* -- toast (downloads / devtools hints) ----------------------------- */
  #toast { position: fixed; right: 10px; top: 44px; max-width: 70%;
           background: #333; color: #fff; padding: 8px 14px; border-radius: 8px;
           font-size: 12px; opacity: 0; transition: opacity .3s; z-index: 9;
           pointer-events: none; white-space: nowrap; overflow: hidden;
           text-overflow: ellipsis; }
  #toast.show { opacity: .92; }
</style></head>
<body>
  <div id="tabs">
    <div id="newtab" onclick="OnNewTab()">+</div>
  </div>
  <div id="bar">
    <div class="btn disabled" id="back" onclick="OnBack()">
      <svg width="18" height="18" viewBox="0 0 512 512"><path d="M427 277v-42h-260l119 -120l-30 -30l-171 171l171 171l30 -30l-119 -120h260z"/></svg>
    </div>
    <div class="btn disabled" id="forward" onclick="OnForward()">
      <svg width="18" height="18" viewBox="0 0 512 512"><path d="M256 427l171 -171l-171 -171l-30 30l119 120h-260v42h260l-119 120z"/></svg>
    </div>
    <div class="btn" id="refresh" onclick="OnRefresh()">
      <svg width="18" height="18" viewBox="0 0 512 512"><path transform="matrix(1 0 0 -1 0 512)" d="M377 377l50 50v-150h-150l69 69c-23 23 -55 38 -90 38c-71 0 -128 -57 -128 -128s57 -128 128 -128c56 0 104 35 121 85h44c-19 -74 -85 -128 -165 -128c-94 0 -170 77 -170 171s76 171 170 171c47 0 90 -19 121 -50z"/></svg>
    </div>
    <div class="btn" id="stop" onclick="OnStop()">
      <svg width="18" height="18" viewBox="0 0 512 512"><path d="M405 375l-119 -119l119 -119l-30 -30l-119 119l-119 -119l-30 30l119 119l-119 119l30 30l119 -119l119 119z"/></svg>
    </div>
    <input id="address" spellcheck="false" placeholder="Search or enter address">
    <div class="btn" id="tools" title="DevTools (F2)" onclick="OnToggleTools()">
      <svg width="16" height="16" viewBox="0 0 24 24"><path d="M22.7 19l-9.1-9.1c.9-2.3.4-5-1.5-6.9-2-2-5-2.4-7.4-1.3L9 6 6 9 1.6 4.7C.4 7.1.9 10.1 2.9 12.1c1.9 1.9 4.6 2.4 6.9 1.5l9.1 9.1c.4.4 1 .4 1.4 0l2.3-2.3c.5-.4.5-1.1.1-1.4z"/></svg>
    </div>
  </div>
  <div id="toast"></div>
  <script>
    // ---- native -> chrome ------------------------------------------------
    var tabs = {};          // id -> element
    function addTab(id) {
      var t = document.createElement('div');
      t.className = 'tab'; t.dataset.id = id;
      t.innerHTML = '<span class="spin"></span><span class="title">New Tab</span>' +
                    '<span class="close">×</span>';
      t.onmousedown = function(e) {
        if (e.target.classList.contains('close')) return;
        OnSelectTab(Number(id));
      };
      t.querySelector('.close').onclick = function(e) {
        e.stopPropagation(); OnCloseTab(Number(id));
      };
      document.getElementById('tabs').insertBefore(
          t, document.getElementById('newtab'));
      tabs[id] = t;
    }
    function selectTab(id) {
      for (var k in tabs) tabs[k].classList.toggle('active', k == id);
    }
    function closeTab(id) {
      if (tabs[id]) { tabs[id].remove(); delete tabs[id]; }
    }
    function updateTab(id, title, loading) {
      var t = tabs[id];
      if (!t) return;
      t.querySelector('.title').textContent = title || 'New Tab';
      t.classList.toggle('loading', !!loading);
    }
    function updateURL(url) {
      var a = document.getElementById('address');
      if (document.activeElement !== a) a.value = url;   // don't stomp edits
    }
    function updateNav(back, fwd) {
      document.getElementById('back').classList.toggle('disabled', !back);
      document.getElementById('forward').classList.toggle('disabled', !fwd);
    }
    function updateLoading(on) {
      document.body.classList.toggle('loading', !!on);
    }
    var toastTimer = 0;
    function showToast(text) {
      var el = document.getElementById('toast');
      el.textContent = text;
      el.classList.add('show');
      clearTimeout(toastTimer);
      toastTimer = setTimeout(function() { el.classList.remove('show'); }, 4000);
    }

    // ---- chrome -> native ------------------------------------------------
    // Address entry: URL-ish input navigates, anything else searches — the
    // same normalization Ultralight's ui.html does with anchorme.js, in a
    // dependency-free check.
    document.getElementById('address').addEventListener('keydown', function(e) {
      if (e.key !== 'Enter') return;
      var url = this.value.trim();
      if (!url) return;
      var lower = url.toLowerCase();
      if (lower.startsWith('http://') || lower.startsWith('https://') ||
          lower.startsWith('file://') || lower.startsWith('about:') ||
          lower.startsWith('data:')) {
        // already a URL
      } else if (url.indexOf(' ') < 0 && url.indexOf('.') >= 0) {
        url = 'https://' + url;                       // bare host/path
      } else {
        url = 'https://www.bing.com/search?q=' + encodeURIComponent(url);
      }
      this.blur();
      OnRequestChangeURL(url);
    });
    document.addEventListener('keydown', function(e) {
      if (e.key === 'F2') OnToggleTools();
    });
  </script>
</body></html>
)HTML";

// ---- The start page of a fresh tab.
static const char kWelcomeHTML[] = R"HTML(
<!doctype html>
<html><head><title>New Tab</title><style>
  html, body { height: 100%; margin: 0; }
  body { display: grid; place-items: center; background: #f6f7f8;
         font-family: system-ui, sans-serif; color: #333; }
  .box { text-align: center; max-width: 34em; }
  h1 { font-weight: 650; margin-bottom: .2em; }
  p  { color: #777; line-height: 1.5; }
  a  { color: #1a63c9; }
  .hint { margin-top: 2em; font-size: 12px; color: #999; }
</style></head>
<body>
  <div class="box">
    <h1>minibrowser</h1>
    <p>A browser built on the <b>miniblink2</b> engine — the chrome above is
       itself a web page talking to native code over the mb C API.</p>
    <p><a href="https://example.com">example.com</a> ·
       <a href="https://news.ycombinator.com">Hacker News</a> ·
       <a href="https://developer.mozilla.org" target="_blank">MDN (opens a new tab)</a></p>
    <div class="hint">F2 or 🔧 starts a DevTools endpoint — attach real Chrome
       via chrome://inspect. Downloads save to ~/Downloads.</div>
  </div>
</body></html>
)HTML";

// ---- Load-failure page (printf template: url, description, domain, code).
static const char kErrorHTMLFmt[] = R"HTML(
<!doctype html>
<html><head><title>Can't reach this page</title><style>
  html, body { height: 100%%; margin: 0; }
  body { display: grid; place-items: center; background: #fff;
         font-family: system-ui, sans-serif; color: #444; }
  .box { max-width: 36em; }
  h1 { font-size: 1.3em; }
  .url { color: #1a63c9; word-break: break-all; }
  .why { color: #888; font-size: 13px; margin-top: 1.5em;
         font-family: ui-monospace, Menlo, monospace; }
</style></head>
<body>
  <div class="box">
    <h1>This page can&rsquo;t be reached</h1>
    <p><span class="url">%s</span></p>
    <p>%s</p>
    <div class="why">error_domain=%s error_code=%d</div>
  </div>
</body></html>
)HTML";

#endif  // MB_SAMPLES_MINIBROWSER_ASSETS_H_
