// Sample 8 — MiniBrowser (macOS + Windows).
//
// A tabbed browser where the CHROME (tab strip + toolbar + address bar) is
// itself a WEB PAGE rendered by a dedicated mbView and wired to native code
// with mbJsBindFunction — the engine dogfoods the browser's own UI. The
// window is the compat scaffold's browser shape: a chrome strip above a
// swappable page area (see compat/mb_window.h); this file is entirely
// OS-independent.
//
// Features:
//   - back/forward/refresh/stop, address bar with search fallback, per-tab
//     loading spinner, a real TAB STRIP.
//   - per-tab wiring: title/URL push, event-driven history state, engine
//     cursor + tooltip, console -> stdout, structured load failures
//     (mbOnFailLoadingEx) rendered as an error page.
//   - window.open / target=_blank -> an ADOPTED child view as a new tab
//     (mbOnCreateChildView: live window object, working opener/postMessage).
//   - downloads divert to ~/Downloads (mbOnDownload).
//   - DevTools: the 🔧 button (or F2 in the chrome) starts a loopback CDP
//     endpoint (cdp_bridge) — ordinary Chrome attaches as the frontend via
//     chrome://inspect, so no inspector UI ships in the binary.
//
// Run FROM the dist/out dir:  ./minibrowser [url]
//   MB_MINIBROWSER_DEVTOOLS=1  start the CDP endpoint immediately (scripts)
//   MB_SAMPLE_AUTOEXIT_MS=N    exit after N ms (smoke runs)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "miniblink2/automation.h"

#include "../compat/mb_window.h"
#include "assets.h"
#include "cdp_bridge.h"

static const int kChromeHeight = 78;   // tab strip (38) + toolbar (40), CSS px
static const int kDevToolsPort = 9222;

// ---- Tab model ------------------------------------------------------------------

struct Tab {
    int id = 0;
    mbView* view = nullptr;
    std::string title = "New Tab";
    std::string url;
    bool loading = false;
    int opener_id = 0;   // nonzero: adopted window.open child of that tab
};

static std::vector<Tab> g_tabs;
static int g_active_id = 0;
static int g_next_tab_id = 1;
static MbBrowserWindow* g_win = nullptr;
static bool g_tools_started = false;

static Tab* FindTab(int id) {
    for (Tab& t : g_tabs)
        if (t.id == id) return &t;
    return nullptr;
}
static Tab* ActiveTab() { return FindTab(g_active_id); }
static Tab* TabForView(mbView* v) {
    for (Tab& t : g_tabs)
        if (t.view == v) return &t;
    return nullptr;
}

// ---- Deferred work ----------------------------------------------------------------
// Engine callbacks (page events, chrome JS bindings) fire WITH the engine on
// the stack; anything that re-enters it — chrome-UI mbRunJS, loads, tab
// teardown — defers to the engine's next idle moment.

static void Defer(std::function<void()> f) {
    auto* p = new std::function<void()>(std::move(f));
    mbDefer([](void* ud) {
        auto* fn = static_cast<std::function<void()>*>(ud);
        (*fn)();
        delete fn;
    }, p);
}

static std::deque<std::string>& ChromeQueue() {
    static auto* q = new std::deque<std::string>();
    return *q;
}
static void DrainChromeJS(void*) {
    while (!ChromeQueue().empty()) {
        std::string js = std::move(ChromeQueue().front());
        ChromeQueue().pop_front();
        if (g_win) mbRunJS(MbBrowserWindowChrome(g_win), js.c_str());
    }
}
static void ChromeJS(std::string js) {
    ChromeQueue().push_back(std::move(js));
    mbDefer(DrainChromeJS, nullptr);
}

static std::string JsStr(const std::string& s) {   // JS string literal
    std::string out = "'";
    for (char c : s) {
        if (c == '\'' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n' || c == '\r') out += ' ';
        else out += c;
    }
    return out + "'";
}

// ---- Chrome-UI state pushes ---------------------------------------------------------

static void PushTabState(const Tab& t) {
    ChromeJS("updateTab(" + std::to_string(t.id) + ", " + JsStr(t.title) +
             ", " + (t.loading ? "true" : "false") + ")");
    if (t.id == g_active_id) {
        ChromeJS("updateURL(" + JsStr(t.url) + ")");
        ChromeJS(std::string("updateLoading(") +
                 (t.loading ? "true" : "false") + ")");
    }
}

static void PushNavState(const Tab& t) {
    if (t.id != g_active_id || !t.view) return;
    ChromeJS("updateNav(" +
             std::string(mbCanGoBack(t.view) ? "true" : "false") + ", " +
             (mbCanGoForward(t.view) ? "true" : "false") + ")");
}

// ---- Per-tab engine callbacks --------------------------------------------------------

static void OnTitleChanged(mbView* v, void*, const char* title) {
    if (Tab* t = TabForView(v)) {
        t->title = title ? title : "";
        PushTabState(*t);
    }
}

static void OnUrlChanged(mbView* v, void*, const char* url) {
    if (Tab* t = TabForView(v)) {
        t->url = url ? url : "";
        PushTabState(*t);
        PushNavState(*t);
        if (g_tools_started)
            MbBridgeRegister(v, t->title, t->url);   // refresh /json metadata
    }
}

static void OnBeginLoading(mbView* v, void*, const char*) {
    if (Tab* t = TabForView(v)) {
        t->loading = true;
        PushTabState(*t);
    }
}

static void OnLoadFinish(mbView* v, void*) {
    if (Tab* t = TabForView(v)) {
        t->loading = false;
        PushTabState(*t);
        PushNavState(*t);
    }
}

// Structured load failure -> an error page (REPLACES the entry, so Back still
// leaves the tab where it was).
static void OnFailLoading(mbView* v, void*, const char* url,
                          const char* domain, int code, const char* desc) {
    Tab* t = TabForView(v);
    if (!t) return;
    std::fprintf(stderr, "[minibrowser] load failed: %s (%s, %d) %s\n",
                 url ? url : "", domain ? domain : "", code, desc ? desc : "");
    const int tab_id = t->id;
    std::string u = url ? url : "", dm = domain ? domain : "",
                ds = desc ? desc : "";
    Defer([tab_id, u, dm, ds, code] {   // loading from a load callback re-enters
        Tab* tab = FindTab(tab_id);
        if (!tab || !tab->view) return;
        int need = std::snprintf(nullptr, 0, kErrorHTMLFmt, u.c_str(),
                                 ds.c_str(), dm.c_str(), code);
        std::string html((size_t)need + 1, '\0');
        std::snprintf(html.data(), html.size(), kErrorHTMLFmt, u.c_str(),
                      ds.c_str(), dm.c_str(), code);
        mbLoadHTMLEx(tab->view, html.c_str(), u.c_str(), /*add_to_history=*/0);
    });
}

static void OnHistoryChanged(mbView* v, void*, int can_back, int can_fwd) {
    if (Tab* t = TabForView(v); t && t->id == g_active_id)
        ChromeJS("updateNav(" + std::string(can_back ? "true" : "false") +
                 ", " + (can_fwd ? "true" : "false") + ")");
}

static void OnTooltipChanged(mbView* v, void*, const char* text) {
    if (Tab* t = TabForView(v); t && t->id == g_active_id && g_win)
        MbBrowserWindowSetTooltip(g_win, text);
}

static void OnConsole(mbView* v, void*, const char* level, const char* message) {
    Tab* t = TabForView(v);
    std::fprintf(stderr, "[tab %d console:%s] %s\n", t ? t->id : 0,
                 level ? level : "", message ? message : "");
}

// Downloads divert to ~/Downloads instead of committing garbage as a document.
static void OnDownload(mbView*, void*, const char* url, const char* mime,
                       const char* filename, const char* data, int len) {
    std::string name = (filename && *filename) ? filename : "download.bin";
    // basename only — never let a served filename traverse directories
    size_t cut = name.find_last_of("/\\");
    if (cut != std::string::npos) name = name.substr(cut + 1);
    if (name.empty()) name = "download.bin";
#ifdef _WIN32
    const char* home = getenv("USERPROFILE");
    const char sep = '\\';
#else
    const char* home = getenv("HOME");
    const char sep = '/';
#endif
    std::string dir = std::string(home ? home : ".") + sep + "Downloads";
    // De-dup "name (2).ext" style, like a real browser.
    std::string stem = name, ext;
    if (size_t dot = name.rfind('.'); dot != std::string::npos && dot > 0) {
        stem = name.substr(0, dot);
        ext = name.substr(dot);
    }
    std::string path = dir + sep + name;
    for (int i = 2;; ++i) {
        FILE* probe = std::fopen(path.c_str(), "rb");
        if (!probe) break;
        std::fclose(probe);
        path = dir + sep + stem + " (" + std::to_string(i) + ")" + ext;
    }
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(data, 1, (size_t)len, f);
        std::fclose(f);
        std::fprintf(stderr, "[minibrowser] download: %s (%s, %d bytes) -> %s\n",
                     url ? url : "", mime ? mime : "", len, path.c_str());
        ChromeJS("showToast(" + JsStr("Downloaded " + name) + ")");
    } else {
        ChromeJS("showToast(" + JsStr("Download failed: " + name) + ")");
    }
}

static void RegisterTabCallbacks(Tab& t);
static void SelectTab(int id);

// window.open()/target=_blank: ADOPT the engine-created child as a new tab —
// the popup gets a live window object and a working opener/postMessage link.
static int OnCreateChildView(mbView* parent, void*, mbView* child,
                             const char* url, const char*, int, int, int, int,
                             int) {
    Tab* opener = TabForView(parent);
    Tab t;
    t.id = g_next_tab_id++;
    t.view = child;
    t.url = url ? url : "";
    t.opener_id = opener ? opener->id : 0;
    g_tabs.push_back(t);
    RegisterTabCallbacks(g_tabs.back());
    ChromeJS("addTab(" + std::to_string(t.id) + ")");
    PushTabState(g_tabs.back());
    const int new_id = t.id;
    // We are INSIDE the page's window.open call: switching panes drives the
    // engine (resize/dirty), so it defers.
    Defer([new_id] { SelectTab(new_id); });
    return 1;   // adopted: this host now owns the child view
}

static void RegisterTabCallbacks(Tab& t) {
    mbOnTitleChanged(t.view, OnTitleChanged, nullptr);
    mbOnUrlChanged(t.view, OnUrlChanged, nullptr);
    mbOnBeginLoading(t.view, OnBeginLoading, nullptr);
    mbOnLoadFinish(t.view, OnLoadFinish, nullptr);
    mbOnFailLoadingEx(t.view, OnFailLoading, nullptr);
    mbOnHistoryChanged(t.view, OnHistoryChanged, nullptr);
    mbOnTooltipChanged(t.view, OnTooltipChanged, nullptr);
    mbOnConsoleMessage(t.view, OnConsole, nullptr);
    mbOnDownload(t.view, OnDownload, nullptr);
    mbOnCreateChildView(t.view, OnCreateChildView, nullptr);
    if (g_tools_started) MbBridgeRegister(t.view, t.title, t.url);
}

// ---- Tab operations (engine off the stack) --------------------------------------------

static void SelectTab(int id) {
    Tab* t = FindTab(id);
    if (!t) return;
    g_active_id = id;
    MbBrowserWindowSetPage(g_win, t->view);   // resizes + re-arms the blit
    ChromeJS("selectTab(" + std::to_string(id) + ")");
    PushTabState(*t);
    PushNavState(*t);
}

static Tab* NewTab(const std::string& url) {
    int w = 0, h = 0;
    MbBrowserWindowPageSize(g_win, &w, &h);
    Tab t;
    t.id = g_next_tab_id++;
    t.view = mbCreateView(w > 0 ? w : 800, h > 0 ? h : 600);
    if (!t.view) return nullptr;
    g_tabs.push_back(t);
    RegisterTabCallbacks(g_tabs.back());
    ChromeJS("addTab(" + std::to_string(t.id) + ")");
    SelectTab(t.id);
    if (url.empty())
        mbLoadHTML(g_tabs.back().view, kWelcomeHTML, "about:welcome");
    else
        mbLoadURL(g_tabs.back().view, url.c_str());
    return &g_tabs.back();
}

static void CloseTab(int id) {
    // Adopted children share the opener's agent cluster and must die FIRST
    // (webview.h lifetime rule) — close this tab's children before it.
restart:
    for (const Tab& t : g_tabs)
        if (t.opener_id == id) {
            CloseTab(t.id);
            goto restart;   // recursion mutated the vector; rescan
        }
    for (size_t i = 0; i < g_tabs.size(); ++i) {
        if (g_tabs[i].id != id) continue;
        if (g_tabs[i].view == nullptr) return;
        MbBridgeUnregister(g_tabs[i].view);
        if (g_active_id == id) MbBrowserWindowSetPage(g_win, nullptr);
        mbDestroyView(g_tabs[i].view);
        g_tabs.erase(g_tabs.begin() + (long)i);
        ChromeJS("closeTab(" + std::to_string(id) + ")");
        break;
    }
    if (g_tabs.empty()) {          // last tab closed: quit, like a browser
        std::exit(0);
    }
    if (g_active_id == id) SelectTab(g_tabs.back().id);
}

// ---- Chrome JS bindings --------------------------------------------------------------
// These fire from INSIDE the chrome page's JS; everything they do drives a
// page view, so everything defers.

static const char* BindBack(void*, int, const char**, const int*, int* type) {
    *type = 4;
    Defer([] {
        if (Tab* t = ActiveTab(); t && mbCanGoBack(t->view)) mbGoBack(t->view);
    });
    return "";
}
static const char* BindForward(void*, int, const char**, const int*, int* type) {
    *type = 4;
    Defer([] {
        if (Tab* t = ActiveTab(); t && mbCanGoForward(t->view))
            mbGoForward(t->view);
    });
    return "";
}
static const char* BindRefresh(void*, int, const char**, const int*, int* type) {
    *type = 4;
    Defer([] {
        if (Tab* t = ActiveTab()) mbReload(t->view);
    });
    return "";
}
static const char* BindStop(void*, int, const char**, const int*, int* type) {
    *type = 4;
    Defer([] {
        if (Tab* t = ActiveTab()) mbStopLoading(t->view);
    });
    return "";
}
static const char* BindChangeURL(void*, int argc, const char** argv,
                                 const int*, int* type) {
    *type = 4;
    if (argc > 0 && argv[0]) {
        std::string url = argv[0];
        Defer([url] {
            if (Tab* t = ActiveTab()) mbLoadURL(t->view, url.c_str());
        });
    }
    return "";
}
static const char* BindNewTab(void*, int, const char**, const int*, int* type) {
    *type = 4;
    Defer([] { NewTab(""); });
    return "";
}
static const char* BindSelectTab(void*, int argc, const char** argv,
                                 const int*, int* type) {
    *type = 4;
    if (argc > 0 && argv[0]) {
        int id = atoi(argv[0]);
        Defer([id] { SelectTab(id); });
    }
    return "";
}
static const char* BindCloseTab(void*, int argc, const char** argv,
                                const int*, int* type) {
    *type = 4;
    if (argc > 0 && argv[0]) {
        int id = atoi(argv[0]);
        Defer([id] { CloseTab(id); });
    }
    return "";
}
static const char* BindToggleTools(void*, int, const char**, const int*,
                                   int* type) {
    *type = 4;
    Defer([] {
        if (!MbBridgeStart(kDevToolsPort)) {
            ChromeJS("showToast('DevTools: port busy')");
            return;
        }
        g_tools_started = true;
        for (Tab& t : g_tabs) MbBridgeRegister(t.view, t.title, t.url);
        ChromeJS("showToast(" +
                 JsStr("DevTools ready — open chrome://inspect (port " +
                       std::to_string(kDevToolsPort) + ")") +
                 ")");
    });
    return "";
}

// ---- main ------------------------------------------------------------------------------

int main(int argc, const char** argv) {
    const char* url = (argc > 1) ? argv[1] : "";

    if (!mbInitialize()) {
        std::fprintf(stderr, "engine init failed\n");
        return 1;
    }
    mbSetMaxUpdateTime(0.008);   // bounded interactive slices

    g_win = MbBrowserWindowCreate("minibrowser (miniblink2)", 1100, 700,
                                  kChromeHeight);
    if (!g_win) return 1;

    mbView* chrome = MbBrowserWindowChrome(g_win);
    mbJsBindFunction(chrome, "OnBack", BindBack, nullptr);
    mbJsBindFunction(chrome, "OnForward", BindForward, nullptr);
    mbJsBindFunction(chrome, "OnRefresh", BindRefresh, nullptr);
    mbJsBindFunction(chrome, "OnStop", BindStop, nullptr);
    mbJsBindFunction(chrome, "OnRequestChangeURL", BindChangeURL, nullptr);
    mbJsBindFunction(chrome, "OnNewTab", BindNewTab, nullptr);
    mbJsBindFunction(chrome, "OnSelectTab", BindSelectTab, nullptr);
    mbJsBindFunction(chrome, "OnCloseTab", BindCloseTab, nullptr);
    mbJsBindFunction(chrome, "OnToggleTools", BindToggleTools, nullptr);
    mbLoadHTML(chrome, kChromeHTML, "about:chrome");

    NewTab(url);   // first tab: the argv URL, or the welcome page

    // Scripted verification: start the CDP endpoint without a click.
    if (const char* dt = getenv("MB_MINIBROWSER_DEVTOOLS")) {
        if (atoi(dt)) {
            int ignored = 0;
            BindToggleTools(nullptr, 0, nullptr, nullptr, &ignored);
        }
    }

    MbRunApp();
    return 0;
}
