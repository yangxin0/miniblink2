// mb_smoke_r6 — smoke coverage for IMPROVEMENT.md round 6:
//   47 pixel-format utilities        45 memory budget knobs
//   48 custom URL schemes            43 zero-copy image sources
//   46 structured console messages   42 per-frame load events + frame ids
//   44 TLS pinning (API/no-regression; a real pin needs a live TLS server)
//   41 in-engine DevTools endpoint (HTTP /json/list over a client socket)
// (40 NSEvent helpers are verified by the header's standalone ObjC compile.)
//
// This program has its own main() (not MB_SMOKE_MAIN) because two features are
// configured BEFORE mbInitialize: custom-scheme registration and the JS heap
// limit.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "miniblink_host/test/mb_smoke_harness.h"
#include "miniblink2/webview.h"

using mbsmoke::Eval;
using mbsmoke::Expect;

namespace {

// Pump the engine for `ms` while also giving background work (a socket client
// thread posting tasks to the main runner) a chance to be serviced.
void PumpFor(mbView* v, int ms) {
  for (int i = 0; i < ms; i += 5) {
    mbUpdate();
    mbWait(v, 5);
  }
}

// ---- 47. Pixel-format utilities ---------------------------------------------
void TestPixelUtils() {
  // One 50%-alpha pixel, premultiplied (channels already scaled by alpha=128).
  // BGRA: B=64 G=32 R=16 A=128 (each == straight*128/255 rounded).
  uint8_t px[4] = {32, 16, 8, 128};
  mbConvertToStraightAlpha(px, 1, 1, 0);
  // straight = premul*255/128: 32->64(ish), 16->32, 8->16 (rounding tolerant).
  bool straight_ok = px[0] >= 62 && px[0] <= 66 && px[1] >= 30 && px[1] <= 34 &&
                     px[2] >= 14 && px[2] <= 18 && px[3] == 128;
  Expect(straight_ok, "47 mbConvertToStraightAlpha unpremultiplies",
         std::to_string(px[0]) + "," + std::to_string(px[1]) + "," +
             std::to_string(px[2]));
  // Round-trip back to premultiplied lands near the original.
  mbConvertToPremultipliedAlpha(px, 1, 1, 0);
  bool premul_ok = px[0] >= 30 && px[0] <= 34 && px[1] >= 14 && px[1] <= 18 &&
                   px[2] >= 6 && px[2] <= 10 && px[3] == 128;
  Expect(premul_ok, "47 mbConvertToPremultipliedAlpha round-trips",
         std::to_string(px[0]) + "," + std::to_string(px[1]) + "," +
             std::to_string(px[2]));
  // Channel swap: BGRA {b,g,r,a} -> {r,g,b,a}.
  uint8_t sw[4] = {1, 2, 3, 4};
  mbSwapRedBlueChannels(sw, 1, 1, 0);
  Expect(sw[0] == 3 && sw[1] == 2 && sw[2] == 1 && sw[3] == 4,
         "47 mbSwapRedBlueChannels swaps B<->R");
}

// ---- 45. Memory budget knobs ------------------------------------------------
void TestMemoryKnobs(mbView* v) {
  // Callable without crashing; a page still loads afterward. (The cap's effect
  // isn't observable from the C API, so this is a no-regression check — the
  // pre-init JS heap limit was set in main().)
  mbSetImageCacheSize(8 * 1024 * 1024);
  mbSetFontCacheSize(1 * 1024 * 1024);
  mbSetImageCacheSize(0);  // restore default
  mbSetFontCacheSize(0);
  mbLoadHTML(v, "<body>mem-knobs</body>", "about:blank");
  mbWait(v, 200);
  Expect(Eval(v, "document.body.textContent") == "mem-knobs",
         "45 page loads after cache-size + heap-limit knobs applied");
}

// ---- 48. Custom URL schemes -------------------------------------------------
void TestCustomScheme(mbView* v) {
  // "app" was registered before mbInitialize (in main()). Serve app:// URLs
  // from the mock layer; navigate to one and verify it commits + a subresource
  // fetch()es.
  mbMockResponse("app://assets/page.html",
                 "<body>CUSTOM-SCHEME<script>"
                 "fetch('app://assets/data.json').then(r=>r.text())"
                 ".then(t=>{window.__d=t;});</script></body>",
                 "text/html", 200);
  mbMockResponse("app://assets/data.json", "{\"ok\":1}",
                 "application/json", 200);
  mbLoadURL(v, "app://assets/page.html");
  mbWait(v, 400);
  const std::string body = Eval(v, "document.body.textContent");
  Expect(body.find("CUSTOM-SCHEME") != std::string::npos,
         "48 app:// navigation commits from the mock layer", body);
  const std::string url = Eval(v, "location.href");
  Expect(url.rfind("app://assets/page.html", 0) == 0,
         "48 app:// is standard-parsed (location.href round-trips)", url);
  const std::string fetched = Eval(v, "window.__d||''");
  Expect(fetched.find("\"ok\":1") != std::string::npos,
         "48 fetch() over app:// reaches the mock", fetched);
}

// ---- 43. Zero-copy image sources --------------------------------------------
std::atomic<int> g_release_calls{0};
const void* g_released_ptr = nullptr;

void OnImageRelease(void* ud, const void* bgra) {
  g_release_calls.fetch_add(1);
  g_released_ptr = bgra;
}

void TestImageSourceBuffer(mbView* v) {
  // A 2x2 opaque BGRA buffer the engine BORROWS (no copy). It must stay alive
  // until release fires.
  static uint8_t pixels[2 * 2 * 4];
  for (int i = 0; i < 2 * 2; ++i) {
    pixels[i * 4 + 0] = 0;    // B
    pixels[i * 4 + 1] = 128;  // G
    pixels[i * 4 + 2] = 255;  // R
    pixels[i * 4 + 3] = 255;  // A
  }
  int ok = mbRegisterImageSourceBuffer("r6cam", pixels, 2, 2, 0, OnImageRelease,
                                       nullptr);
  Expect(ok == 1, "43 mbRegisterImageSourceBuffer accepts a borrowed buffer");

  mbLoadHTML(v,
             "<body><img id=im src='https://mb-image.internal/r6cam'"
             " onload='window.__w=this.naturalWidth'></body>",
             "about:blank");
  mbWait(v, 500);
  const std::string w = Eval(v, "window.__w||0");
  Expect(w == "2",
         "43 borrowed image lazily PNG-encodes + decodes (naturalWidth==2)", w);

  // Re-registering the same id releases the old borrowed buffer exactly once.
  static uint8_t pixels2[2 * 2 * 4] = {0};
  mbRegisterImageSourceBuffer("r6cam", pixels2, 2, 2, 0, OnImageRelease,
                              nullptr);
  Expect(g_release_calls.load() == 1 && g_released_ptr == pixels,
         "43 re-register releases the previous borrowed buffer once",
         std::to_string(g_release_calls.load()));

  // Unregister releases the second buffer.
  mbUnregisterImageSource("r6cam");
  Expect(g_release_calls.load() == 2 && g_released_ptr == pixels2,
         "43 unregister releases the current borrowed buffer",
         std::to_string(g_release_calls.load()));
}

// ---- 46. Structured console messages ----------------------------------------
struct ConsoleCapture {
  std::string level, message, source, category, stack;
  int line = -1, column = -1;
  int count = 0;
};

void TestStructuredConsole(mbView* v) {
  static ConsoleCapture* cap_ptr = new ConsoleCapture();  // leaked (no exit dtor)
  ConsoleCapture& cap = *cap_ptr;
  cap = ConsoleCapture{};
  mbOnConsoleMessage2(
      v,
      [](mbView*, void* ud, const mbConsoleMessageInfo* info) {
        auto* c = static_cast<ConsoleCapture*>(ud);
        // Capture the first console.error (skip anything else).
        if (c->count == 0 && std::string(info->level) == "error") {
          c->level = info->level;
          c->message = info->message;
          c->source = info->source ? info->source : "";
          c->category = info->category ? info->category : "";
          c->stack = info->stack ? info->stack : "";
          c->line = info->line;
          c->column = info->column;
        }
        c->count++;
      },
      &cap);
  mbLoadHTML(v, "<body><script>\nconsole.error('boom');\n</script></body>",
             "about:blank");
  mbWait(v, 300);
  Expect(cap.message == "boom" && cap.level == "error",
         "46 struct callback delivers level+message", cap.message);
  Expect(cap.line == 2, "46 line number is populated",
         std::to_string(cap.line));
  Expect(cap.column > 0, "46 column number is populated (patch 0043)",
         std::to_string(cap.column));
  Expect(cap.category == "console-api" || cap.category == "javascript",
         "46 source category is delivered", cap.category);
  mbOnConsoleMessage2(v, nullptr, nullptr);
}

// ---- 42. Per-frame load events + frame IDs ----------------------------------
struct FrameEvents {
  std::vector<uint64_t> begin_ids;
  std::vector<int> begin_is_main;
  int finish_count = 0;
};

void TestPerFrameEvents(mbView* v) {
  static FrameEvents* fe_ptr = new FrameEvents();  // leaked (no exit dtor)
  FrameEvents& fe = *fe_ptr;
  fe = FrameEvents{};
  mbMockResponse("frames.test/child", "<body>CHILD-FRAME</body>", "text/html",
                 200);
  mbMockResponse("frames.test/parent",
                 "<body>PARENT<iframe src='https://frames.test/child'>"
                 "</iframe></body>",
                 "text/html", 200);
  mbOnFrameLoadEvent(
      v,
      [](mbView*, void* ud, uint64_t frame_id, int is_main, int phase,
         const char*) {
        auto* f = static_cast<FrameEvents*>(ud);
        if (phase == MB_FRAME_LOAD_BEGIN) {
          f->begin_ids.push_back(frame_id);
          f->begin_is_main.push_back(is_main);
        } else if (phase == MB_FRAME_LOAD_FINISH) {
          f->finish_count++;
        }
      },
      &fe);
  mbLoadURL(v, "https://frames.test/parent");
  mbWait(v, 600);

  // Both the main frame and the child frame reported a BEGIN.
  bool saw_main = false, saw_child = false;
  for (size_t i = 0; i < fe.begin_is_main.size(); ++i) {
    if (fe.begin_is_main[i])
      saw_main = true;
    else
      saw_child = true;
  }
  Expect(saw_main && saw_child,
         "42 per-frame BEGIN fires for main AND child frame",
         std::to_string(fe.begin_ids.size()) + " begins");
  Expect(fe.finish_count >= 2, "42 per-frame FINISH fires for both frames",
         std::to_string(fe.finish_count));

  // mbGetFrameIds enumerates main + child; the count matches mbGetFrameCount+1.
  uint64_t ids[8] = {0};
  int n = mbGetFrameIds(v, ids, 8);
  Expect(n == 2, "42 mbGetFrameIds returns main + 1 child",
         std::to_string(n));

  // EvalInFrameById reaches the child's document (host-privileged, cross-origin
  // safe). The child frame id is the 2nd entry (main frame is first).
  if (n == 2) {
    char buf[128] = {0};
    mbEvalJSInFrameById(v, ids[1], "document.body.textContent", buf,
                        sizeof(buf));
    Expect(std::string(buf) == "CHILD-FRAME",
           "42 mbEvalJSInFrameById reads the child frame by stable id", buf);
  } else {
    Expect(false, "42 mbEvalJSInFrameById (skipped: wrong frame count)");
  }
  mbOnFrameLoadEvent(v, nullptr, nullptr);
}

// ---- 44. TLS pinning (API + no-regression) ----------------------------------
void TestTlsPinningApi(mbView* v) {
  // Set a pin from the request hook on every request. Mocked loads bypass curl,
  // so a mock still serves (the pin is a curl-layer option) — this verifies the
  // API is wired and doesn't break normal interception. A real mismatch test
  // needs a live TLS server (out of scope for an offline smoke).
  mbSetRequestHook(
      [](mbRequest* r, void*) {
        mbRequestPinPublicKey(r, "sha256//AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=");
      },
      nullptr);
  mbMockResponse("pin.test/x", "<body>PIN-OK</body>", "text/html", 200);
  mbLoadURL(v, "https://pin.test/x");
  mbWait(v, 300);
  Expect(Eval(v, "document.body.textContent") == "PIN-OK",
         "44 mbRequestPinPublicKey callable; mocked load unaffected");
  mbSetRequestHook(nullptr, nullptr);
  mbClearMocks();
}

// ---- 41. In-engine DevTools endpoint ----------------------------------------
// Minimal blocking HTTP GET over a loopback socket, run on a worker thread
// while the main thread pumps the engine (the server marshals /json/list to
// the main thread).
std::string HttpGet(int port, const std::string& path) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return {};
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return {};
  }
  std::string req = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
  ::send(fd, req.data(), req.size(), 0);
  std::string resp;
  char buf[2048];
  for (;;) {
    ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
    if (r <= 0)
      break;
    resp.append(buf, static_cast<size_t>(r));
  }
  ::close(fd);
  return resp;
}

void TestDevToolsServer(mbView* v) {
  mbLoadHTML(v, "<title>R6 Inspect Me</title><body>x</body>", "about:blank");
  mbWait(v, 150);
  const int port = 9333;
  int started = mbDevToolsStartServer(port);
  Expect(started == 1 && mbDevToolsServerPort() == port,
         "41 mbDevToolsStartServer binds the loopback port");
  if (!started)
    return;

  std::atomic<bool> done{false};
  std::string response;
  std::thread client([&] {
    response = HttpGet(port, "/json/list");
    done.store(true);
  });
  // Pump the engine so the server's main-thread marshaled task runs.
  for (int i = 0; i < 200 && !done.load(); ++i)
    PumpFor(v, 10);
  client.join();

  Expect(response.find("200 OK") != std::string::npos,
         "41 /json/list returns 200");
  Expect(response.find("webSocketDebuggerUrl") != std::string::npos &&
             response.find("R6 Inspect Me") != std::string::npos,
         "41 /json/list lists the live view as a CDP target",
         response.substr(0, 0));
  mbDevToolsStopServer();
  Expect(mbDevToolsServerPort() == 0,
         "41 mbDevToolsStopServer tears the endpoint down");
}

}  // namespace

int main() {
  // Pre-init configuration (item 48 + item 45): these MUST precede mbInitialize.
  mbRegisterCustomScheme("app");
  mbSetJsHeapLimit(256 * 1024 * 1024);

  if (!mbInitialize()) {
    std::fprintf(stderr, "init failed\n");
    return 1;
  }
  const int W = 400, H = 300;
  mbView* v = mbCreateView(W, H);
  if (!v)
    return 1;

  TestPixelUtils();
  TestMemoryKnobs(v);
  TestCustomScheme(v);
  TestImageSourceBuffer(v);
  TestStructuredConsole(v);
  TestPerFrameEvents(v);
  TestTlsPinningApi(v);
  TestDevToolsServer(v);

  mbDestroyView(v);
  mbShutdown();
  std::fprintf(stderr, "\nmb_smoke_r6: %d passed, %d failed\n", mbsmoke::g_pass,
               mbsmoke::g_fail);
  return mbsmoke::g_fail == 0 ? 0 : 1;
}
