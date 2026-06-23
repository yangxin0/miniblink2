// mb_shot — headless renderer CLI. Renders an HTML file (or file:// URL) to a PNG using
// modern Blink via the miniblink_host C ABI.
//
//   mb_shot <input.html | file://URL> <out.png> [width height]
//
// This is the "product" the host enables: a standalone, single-process, modern-Blink
// screenshot tool — no browser process, no CEF.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "miniblink_host/capi/mb_capi.h"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: %s <input.html|file://URL> <out.png> [width height]\n",
                 argv[0]);
    return 2;
  }
  const std::string input = argv[1];
  const char* out = argv[2];
  const int w = argc > 3 ? std::atoi(argv[3]) : 1200;
  const int h = argc > 4 ? std::atoi(argv[4]) : 800;

  if (!mbInitialize()) {
    std::fprintf(stderr, "mb_shot: engine init failed\n");
    return 1;
  }
  mbView* view = mbCreateView(w, h);
  if (!view) {
    std::fprintf(stderr, "mb_shot: view creation failed\n");
    return 1;
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

  const int ok = mbSavePng(view, out, w, h);
  std::fprintf(stderr, "mb_shot: %s -> %s (%dx%d) %s\n", input.c_str(), out, w, h,
               (ok && load_ok) ? "OK" : "FAILED");

  mbDestroyView(view);
  mbShutdown();
  return (ok && load_ok) ? 0 : 1;
}
