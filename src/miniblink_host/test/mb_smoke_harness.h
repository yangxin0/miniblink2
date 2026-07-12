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

#if !defined(_WIN32)
#include <signal.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

#include "miniblink2/automation.h"

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

#if !defined(_WIN32)
inline pid_t g_echo_server_pid = -1;
#endif

// Make the wire-level network cases self-contained: spawn the bundled
// echo_server.py on an OS-assigned loopback port and export MB_NET_TESTS /
// MB_NET_HOST for this process, so a plain `./mb_smoke` runs them too.
// Explicit environment still wins: MB_NET_TESTS=0 disables the cases outright,
// and a preset MB_NET_HOST is honored untouched (with the existing MB_NET_TESTS
// gate). When python3 or the script is unavailable (e.g. a CI image without
// python) this silently leaves the legacy opt-in gating in place — the suite
// must never fail for test-infrastructure reasons. The child is SIGTERMed at
// process exit. No-op on Windows (legacy gating applies).
inline void EnsureLocalEchoServer() {
#if !defined(_WIN32)
  const char* tests = std::getenv("MB_NET_TESTS");
  if (tests && std::strcmp(tests, "0") == 0) {
    // The pre-existing gates only test getenv() for non-null, so "0" would
    // OPEN them (against the httpbin.org default). Clear it so every
    // downstream gate reads the opt-out as off.
    unsetenv("MB_NET_TESTS");
    return;
  }
  if (std::getenv("MB_NET_HOST"))
    return;
  // Locate the script: env override, then staged-tree path relative to this
  // executable ($CHROMIUM/out/<profile>/mb_smoke*), then repo-relative cwd.
  std::string script;
  if (const char* override_path = std::getenv("MB_ECHO_SERVER")) {
    script = override_path;
  } else {
    std::string exe;
#if defined(__APPLE__)
    char exe_buf[4096];
    uint32_t exe_len = sizeof(exe_buf);
    if (_NSGetExecutablePath(exe_buf, &exe_len) == 0)
      exe = exe_buf;
#else
    char exe_buf[4096];
    const ssize_t n = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    if (n > 0) {
      exe_buf[n] = '\0';
      exe = exe_buf;
    }
#endif
    const std::string::size_type slash = exe.rfind('/');
    if (slash != std::string::npos) {
      script = exe.substr(0, slash) +
               "/../../third_party/blink/renderer/miniblink_host/test/"
               "echo_server.py";
      if (access(script.c_str(), R_OK) != 0)
        script.clear();
    }
    if (script.empty()) {
      script = "src/miniblink_host/test/echo_server.py";
      if (access(script.c_str(), R_OK) != 0)
        return;
    }
  }

  int fds[2];
  if (pipe(fds) != 0)
    return;
  const pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return;
  }
  if (pid == 0) {
    dup2(fds[1], 1);
    close(fds[0]);
    close(fds[1]);
    execlp("python3", "python3", script.c_str(), "0",
           static_cast<char*>(nullptr));
    _exit(127);
  }
  close(fds[1]);

  // Read the child's "listening on 127.0.0.1:<port>" line, bounded to ~5s so a
  // wedged interpreter cannot hang the suite. EOF means python is missing or
  // the script failed — fall back to legacy gating.
  std::string line;
  bool got_line = false;
  for (int i = 0; !got_line && i < 25; ++i) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fds[0], &rfds);
    struct timeval tv = {0, 200000};
    if (select(fds[0] + 1, &rfds, nullptr, nullptr, &tv) <= 0)
      continue;
    char buf[128];
    const ssize_t n = read(fds[0], buf, sizeof(buf));
    if (n <= 0)
      break;
    for (ssize_t j = 0; j < n && !got_line; ++j) {
      if (buf[j] == '\n')
        got_line = true;
      else
        line.push_back(buf[j]);
    }
  }
  close(fds[0]);
  const std::string::size_type colon = line.rfind(':');
  if (!got_line || colon == std::string::npos || colon + 1 >= line.size()) {
    kill(pid, SIGTERM);
    waitpid(pid, nullptr, 0);
    return;
  }
  const std::string host = "http://127.0.0.1:" + line.substr(colon + 1);
  setenv("MB_NET_HOST", host.c_str(), 1);
  setenv("MB_NET_TESTS", "1", 1);
  g_echo_server_pid = pid;
  std::atexit([] {
    if (g_echo_server_pid > 0) {
      kill(g_echo_server_pid, SIGTERM);
      waitpid(g_echo_server_pid, nullptr, 0);
    }
  });
#endif
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
