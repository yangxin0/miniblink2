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

  if (input.rfind("file://", 0) == 0 || input.rfind("http", 0) == 0) {
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

  const int ok = mbSavePng(view, out, w, h);
  std::fprintf(stderr, "mb_shot: %s -> %s (%dx%d) %s\n", input.c_str(), out, w, h,
               ok ? "OK" : "FAILED");

  mbDestroyView(view);
  mbShutdown();
  return ok ? 0 : 1;
}
