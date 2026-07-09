// Sample 5 — File Loading (macOS + Windows).
//
// Two ways to serve app content without a web server:
//   1. Real files: file:// documents load their own file:// subresources
//     (CSS, images) directly — samples/assets/page.html here.
//   2. Virtual files: the interception layer serves bytes for ANY url —
//     mbMockResponse below answers a fetch() to https://virtual.example/
//     from memory — the interception layer doubles as a virtual filesystem,
//     and is the stronger primitive for content-serving hosts (it also covers
//     http URLs, per-view routing via mbOnRequestMock, and dynamic bodies).
//
// Run:  ./sample5_file_loading [assets-dir]
#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <direct.h>
#else
#include <limits.h>
#endif

#include "compat/mb_window.h"

// Absolute file:// URL for `path` (which may be relative).
static std::string FileUrl(const std::string& path) {
#ifdef _WIN32
  char abs[_MAX_PATH];
  if (!_fullpath(abs, path.c_str(), sizeof abs)) return std::string();
  std::string p(abs);
  for (char& c : p)
    if (c == '\\') c = '/';
  return "file:///" + p;              // file:///C:/...
#else
  char abs[PATH_MAX];
  if (!realpath(path.c_str(), abs)) return std::string();
  return std::string("file://") + abs;
#endif
}

static bool Exists(const std::string& path) {
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    std::fclose(f);
    return true;
  }
  return false;
}

int main(int argc, const char** argv) {
  // Locate the assets: an explicit argument, or the repo layout relative to
  // the usual run locations (dist/<mode>/ and the repo root).
  std::string dir = argc > 1 ? argv[1] : "";
  if (dir.empty()) {
    for (const char* cand : { "../../samples/sample5_file_loading/assets",
                              "samples/sample5_file_loading/assets",
                              "../samples/sample5_file_loading/assets" }) {
      if (Exists(std::string(cand) + "/page.html")) {
        dir = cand;
        break;
      }
    }
  }
  if (dir.empty() || !Exists(dir + "/page.html")) {
    std::fprintf(stderr, "assets not found — pass the dir: "
                         "./sample5_file_loading path/to/samples/assets\n");
    return 1;
  }

  if (!mbInitialize()) return 1;

  // The virtual file: served from memory for a URL that never touches the
  // network. The page fetch()es it and shows the payload.
  mbMockResponse("virtual.example/info.json",
                 "{\"served\":\"from memory\",\"by\":\"mbMockResponse\"}",
                 "application/json", 200);

  MbWindow* win = MbWindowCreate("Sample 5 — File Loading", 860, 560);
  if (!win) return 1;
  mbLoadURL(MbWindowView(win), FileUrl(dir + "/page.html").c_str());
  MbRunApp();
  return 0;
}
