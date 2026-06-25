// mb_smoke_harness — shared scaffolding for the split mb_smoke_* programs.
//
// The old monolithic mb_smoke.cc (180 cases in one main()) is being split into
// several small, themed smoke programs (engine / scrape / input / net / platform).
// Each is its own executable = one `mb_smoke_<theme>.cc` that:
//   #include "miniblink_host/test/mb_smoke_harness.h"
//   static void RunCases(mbView* v, int W, int H) { ... its cases ... }
//   MB_SMOKE_MAIN("mb_smoke_<theme>")
// All helpers are header-only `inline`, so each single-TU program gets its own
// pass/fail counters (correct — the programs run as separate processes).
#ifndef MINIBLINK_HOST_TEST_MB_SMOKE_HARNESS_H_
#define MINIBLINK_HOST_TEST_MB_SMOKE_HARNESS_H_

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "miniblink_host/capi/mb_capi.h"

namespace mbsmoke {

inline int g_pass = 0;
inline int g_fail = 0;

// Eval a JS expression and return its string result (the dominant assertion tool).
inline std::string Eval(mbView* v, const char* js) {
  char buf[512];
  mbEvalJS(v, js, buf, sizeof(buf));
  return std::string(buf);
}

// Eval in the isolated content-script world (separate globals, shared DOM).
inline std::string EvalIso(mbView* v, const char* js) {
  char buf[512];
  mbEvalJSIsolated(v, js, buf, sizeof(buf));
  return std::string(buf);
}

// Record a PASS/FAIL line; `got` is an optional diagnostic shown on the line.
inline void Expect(bool ok, const char* name, const std::string& got = "") {
  std::fprintf(stderr, "  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", name,
               got.empty() ? "" : " -> ", got.c_str());
  ok ? ++g_pass : ++g_fail;
}

}  // namespace mbsmoke

// Standard main() for a themed smoke program: init Blink, create a 400x300 view,
// run that program's RunCases(), tear down, print a `SUITE: N passed, M failed`
// summary, and exit non-zero iff any case failed. RunCases is provided by the .cc.
#define MB_SMOKE_MAIN(SUITE)                                                  \
  static void RunCases(mbView* v, int W, int H);                             \
  int main() {                                                               \
    if (!mbInitialize()) {                                                   \
      std::fprintf(stderr, "init failed\n");                                 \
      return 1;                                                              \
    }                                                                        \
    const int W = 400, H = 300;                                             \
    mbView* v = mbCreateView(W, H);                                          \
    if (!v)                                                                  \
      return 1;                                                              \
    RunCases(v, W, H);                                                       \
    mbDestroyView(v);                                                        \
    mbShutdown();                                                            \
    std::fprintf(stderr, "\n" SUITE ": %d passed, %d failed\n",             \
                 mbsmoke::g_pass, mbsmoke::g_fail);                          \
    return mbsmoke::g_fail == 0 ? 0 : 1;                                    \
  }

#endif  // MINIBLINK_HOST_TEST_MB_SMOKE_HARNESS_H_
