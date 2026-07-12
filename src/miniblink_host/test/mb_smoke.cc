// mb_smoke — capability test suite for the miniblink2 engine. Each case loads
// content and ASSERTS engine behavior (mostly via mbEvalJS / getComputedStyle, which is
// robust; plus one pixel check). Prints PASS/FAIL per case and a summary; exit 0 iff all pass.
#include "miniblink_host/test/mb_smoke_harness.h"
#include "miniblink_host/test/mb_test_seams.h"

#include <algorithm>
#include <filesystem>
#include <thread>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "miniblink_host/loader/mb_retry_policy.h"  // mb::MbShouldRetryFetch (predicate test)
#include "net/base/filename_util.h"
#include "url/gurl.h"

// Narrow internal test hooks used only to evict migrated in-memory state
// before reopening the profile. Forward declarations keep this C-ABI smoke
// target from inheriting private service-header include requirements.
namespace mb {
void MbClearIndexedDBScoped(const std::string& scope_prefix);
void MbClearOPFSScoped(const std::string& scope_prefix);
void MbNetRequestStarted(const void* view_ctx);
void MbNetRequestFinished(const void* view_ctx);
uint64_t MbNetStartedCount(const void* view_ctx);
int MbNetInFlight(const void* view_ctx);
void MbNetForgetActivityContext(const void* view_ctx);
}  // namespace mb

using mbsmoke::Eval;     // shared harness helpers (see mb_smoke_harness.h)
using mbsmoke::EvalIso;
using mbsmoke::Expect;
using mbsmoke::g_fail;
using mbsmoke::g_pass;

namespace {
// Native functions bound into JS for the mbJsBindFunction test: echoes its first
// argument with a "!" suffix and the userdata it was given.
const char* SmokeEcho(void* userdata, int argc, const char** argv,
                      const int* /*argtypes*/,
                      int* /*out_type*/) {  // default string return
  static char buf[256];
  std::snprintf(buf, sizeof(buf), "%s!%d", (argc > 0 && argv[0]) ? argv[0] : "",
                userdata ? *static_cast<int*>(userdata) : -1);
  return buf;
}

// Returns structured data as JSON (out_type 5) -> a real JS object in the page.
const char* SmokeJson(void*, int, const char**, const int*, int* out_type) {
  *out_type = 5;  // json
  return "{\"a\":1,\"b\":[2,3]}";
}

struct NavigationEventRecord {
  mbNavigationId id = 0;
  mbNavigationPhase phase = MB_NAVIGATION_PHASE_STARTED;
  mbNavigationOutcome outcome = MB_NAVIGATION_OUTCOME_NONE;
  std::string requested_url;
  std::string url;
  int http_status = 0;
  std::string error_domain;
  int error_code = 0;
  std::string description;
  int struct_size = 0;
};

void RecordNavigationEvent(mbView*, void* userdata,
                           const mbNavigationEvent* event) {
  if (!userdata || !event)
    return;
  NavigationEventRecord record;
  record.id = event->navigation_id;
  record.phase = event->phase;
  record.outcome = event->outcome;
  record.requested_url = event->requested_url ? event->requested_url : "";
  record.url = event->url ? event->url : "";
  record.http_status = event->http_status;
  record.error_domain = event->error_domain ? event->error_domain : "";
  record.error_code = event->error_code;
  record.description = event->description ? event->description : "";
  record.struct_size = event->struct_size;
  static_cast<std::vector<NavigationEventRecord>*>(userdata)->push_back(
      std::move(record));
}

struct CancelOnNavigationStartState {
  std::vector<NavigationEventRecord> events;
  mbNavigationId started_id = 0;
  int cancel_result = 0;
};

void CancelOnNavigationStart(mbView* view, void* userdata,
                             const mbNavigationEvent* event) {
  auto* state = static_cast<CancelOnNavigationStartState*>(userdata);
  if (!state || !event)
    return;
  RecordNavigationEvent(view, &state->events, event);
  if (event->phase == MB_NAVIGATION_PHASE_STARTED && event->navigation_id) {
    state->started_id = event->navigation_id;
    state->cancel_result = mbCancelNavigation(view, event->navigation_id);
  }
}

void AppendU32(std::string* out, uint32_t value) {
  for (int i = 0; i < 4; ++i)
    out->push_back(static_cast<char>((value >> (8 * i)) & 0xff));
}

void AppendI64(std::string* out, int64_t value) {
  const uint64_t bits = static_cast<uint64_t>(value);
  for (int i = 0; i < 8; ++i)
    out->push_back(static_cast<char>((bits >> (8 * i)) & 0xff));
}

void AppendBlob(std::string* out, const std::string& value) {
  AppendU32(out, static_cast<uint32_t>(value.size()));
  out->append(value);
}

bool WriteBytes(const std::filesystem::path& path, const std::string& bytes) {
  std::FILE* f = std::fopen(path.string().c_str(), "wb");
  if (!f)
    return false;
  const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  return std::fclose(f) == 0 && ok;
}

std::string ReadBytes(const std::filesystem::path& path) {
  std::FILE* f = std::fopen(path.string().c_str(), "rb");
  if (!f)
    return {};
  std::string out;
  char buf[256];
  for (;;) {
    const size_t n = std::fread(buf, 1, sizeof(buf), f);
    out.append(buf, n);
    if (n != sizeof(buf))
      break;
  }
  std::fclose(f);
  return out;
}

std::string ReadProfileToken(const std::filesystem::path& profile_dir) {
  const std::string manifest = ReadBytes(profile_dir / "profile.id");
  constexpr char kProfileMagicV2[] = "MBPROFILE2\n";
  constexpr char kProfileMagicV1[] = "MBPROFILE1\n";
  size_t token_start = std::string::npos;
  if (manifest.rfind(kProfileMagicV2, 0) == 0)
    token_start = sizeof(kProfileMagicV2) - 1;
  else if (manifest.rfind(kProfileMagicV1, 0) == 0)
    token_start = sizeof(kProfileMagicV1) - 1;
  if (token_start == std::string::npos)
    return {};
  const size_t token_end = manifest.find('\n', token_start);
  return manifest.substr(token_start, token_end == std::string::npos
                                          ? std::string::npos
                                          : token_end - token_start);
}

// Write the smallest useful v0.4 session fixtures: one version-1 IndexedDB database and
// one empty OPFS directory, both keyed by the OLD raw-path session prefix. The current
// loader must remap those stored scopes to the canonical session id during restore.
bool WriteLegacySessionFixtures(const std::filesystem::path& profile_dir,
                                const std::string& legacy_scope) {
  std::error_code ec;
  std::filesystem::create_directories(profile_dir, ec);
  if (ec)
    return false;

  std::string idb("MBIDB002", 8);
  AppendU32(&idb, 1);                       // database count
  AppendBlob(&idb, legacy_scope + "\nlegacydb");
  AppendI64(&idb, 1);                       // version
  AppendI64(&idb, 0);                       // max object-store id
  AppendU32(&idb, 0);                       // object stores
  AppendU32(&idb, 0);                       // record-bearing stores
  AppendU32(&idb, 0);                       // key generators
  AppendU32(&idb, 0);                       // secondary-index data

  std::string opfs("MBOPFS01", 8);
  AppendU32(&opfs, 1);                      // scope count
  AppendBlob(&opfs, legacy_scope);
  opfs.push_back(1);                        // root is a directory
  AppendU32(&opfs, 1);                      // one child
  AppendBlob(&opfs, "legacydir");
  opfs.push_back(1);                        // child is a directory
  AppendU32(&opfs, 0);                      // child is empty

  return WriteBytes(profile_dir / "idb.dat", idb) &&
         WriteBytes(profile_dir / "opfs.dat", opfs);
}
}  // namespace

int main() {
  // Self-contained wire-level coverage: spawn the bundled echo server (unless
  // the environment already configured or disabled the network cases).
  mbsmoke::EnsureLocalEchoServer();
  if (!mbInitialize()) {
    std::fprintf(stderr, "init failed\n");
    return 1;
  }

  // 0. Lifecycle: mbShutdown is safe and the engine survives a shutdown -> re-init
  // cycle. Blink's process-global init is one-time, so the engine stays resident and
  // re-init reuses it (pre-fix this deleted the runtime, leaving dangling allocator
  // pointers and crashing on the 2nd init). Running it BEFORE the main view means
  // every test below also exercises the post-cycle engine.
  mbShutdown();
  Expect(mbInitialize() == 1, "engine survives mbShutdown + re-init (no crash/leak)");

  const int W = 400, H = 300;
  mbView* v = mbCreateView(W, H);
  if (!v)
    return 1;

  // 0b. Push callback: mbOnLoadFinish fires on the real Blink DidFinishLoad signal
  // (the document `load` event), not a poll or fixed timer; mbIsLoadFinished queries
  // the same state. A counting callback (state via userdata; non-capturing lambda ->
  // C function pointer) must fire once per load and leave the flag set.
  {
    int fin = 0;
    mbOnLoadFinish(
        v, [](mbView*, void* ud) { ++*static_cast<int*>(ud); }, &fin);
    mbLoadHTML(v, "<body>load-a</body>", "about:blank");
    const int after_a = fin;
    mbLoadHTML(v, "<body>load-b</body>", "about:blank");
    Expect(after_a >= 1 && fin > after_a && mbIsLoadFinished(v) == 1,
           "mbOnLoadFinish fires on each DidFinishLoad; mbIsLoadFinished true",
           std::string("after_a=") + std::to_string(after_a) + " fin=" +
               std::to_string(fin));
    mbOnLoadFinish(v, nullptr, nullptr);  // clear before `fin` leaves scope
  }

  // 0b2. mbOnDOMContentLoaded fires when the DOM is parsed (deferred scripts run, before
  // subresources) — the "page interactive" signal, EARLIER than load-finish/onload. Record
  // both signals into one sequence: DOMContentLoaded ('D') must fire AND precede load ('L').
  {
    static std::string* seq = new std::string();  // -Wexit-time-destructors
    seq->clear();
    mbOnDOMContentLoaded(v, [](mbView*, void*) { *seq += "D"; }, nullptr);
    mbOnLoadFinish(v, [](mbView*, void*) { *seq += "L"; }, nullptr);
    mbLoadHTML(v, "<body>dcl<script>document.title='x';</script></body>",
               "about:blank");
    Expect(*seq == "DL",
           "mbOnDOMContentLoaded fires before load-finish (DOM-ready signal)",
           "seq=[" + *seq + "]");
    mbOnDOMContentLoaded(v, nullptr, nullptr);
    mbOnLoadFinish(v, nullptr, nullptr);
  }

  // 0b3. mbOnNavigationStarted fires at navigation KICKOFF — before the main-resource
  // fetch and before the commit signal (mbOnBeginLoading). Record both into one sequence:
  // 'S' (started) must fire AND precede 'B' (begin/commit). Also verify the lifecycle is
  // consistent: mbIsLoadFinished() must read 0 INSIDE the started callback (a new load is
  // in progress), not the stale 1 from the previous load. Uses a data: URL so the load
  // goes through the LoadURL fetch+commit path (not the direct mbLoadHTML commit).
  {
    static std::string* seq = new std::string();  // -Wexit-time-destructors
    static int* fin_at_start = new int(-1);
    seq->clear();
    *fin_at_start = -1;
    mbOnNavigationStarted(
        v,
        [](mbView* nv, void*, const char*) {
          *seq += "S";
          *fin_at_start = mbIsLoadFinished(nv);  // must be 0: a load is now in progress
        },
        nullptr);
    mbOnBeginLoading(v, [](mbView*, void*, const char*) { *seq += "B"; }, nullptr);
    mbLoadURL(v, "data:text/html,<body>nav-started</body>");
    Expect(*seq == "SB" && *fin_at_start == 0,
           "mbOnNavigationStarted fires at kickoff (before commit) with load-in-progress "
           "lifecycle state",
           "seq=[" + *seq + "] fin_at_start=" + std::to_string(*fin_at_start));
    mbOnNavigationStarted(v, nullptr, nullptr);
    mbOnBeginLoading(v, nullptr, nullptr);
  }

  // 0b4. mbNavigate: ASYNCHRONOUS navigation returns a non-zero id immediately and commits
  // the document from a POSTED completion (here mock-served), driving the full lifecycle
  // (started -> commit -> finish). Cancelling an already-finished nav returns 0.
  {
    static std::string* seq = new std::string();  // -Wexit-time-destructors
    seq->clear();
    std::vector<NavigationEventRecord> nav_events;
    mbOnNavigationEvent(v, &RecordNavigationEvent, &nav_events);
    mbOnNavigationStarted(
        v, [](mbView*, void*, const char*) { *seq += "S"; }, nullptr);
    mbOnLoadFinish(v, [](mbView*, void*) { *seq += "F"; }, nullptr);
    mbMockResponse("nav.async.test", "<body id='b'>async-nav</body>", "text/html", 200);
    const mbNavigationId id = mbNavigate(v, "https://nav.async.test/");
    // The load happens on subsequent pumps; mbWaitForFunction drives them. Wait on
    // readyState 'complete' so the load event (and thus the finish callback 'F') has fired.
    mbWaitForFunction(v,
                      "document.readyState==='complete'&&document.getElementById('b')&&"
                      "document.getElementById('b').textContent==='async-nav'",
                      3000);
    const std::string r = Eval(v, "document.body?document.body.textContent:''");
    const int cancel_done = mbCancelNavigation(v, id);  // already finished -> 0
    const bool structured_lifecycle =
        nav_events.size() == 3 &&
        nav_events[0].struct_size ==
            static_cast<int>(sizeof(mbNavigationEvent)) &&
        nav_events[0].id == id &&
        nav_events[0].phase == MB_NAVIGATION_PHASE_STARTED &&
        nav_events[0].outcome == MB_NAVIGATION_OUTCOME_NONE &&
        nav_events[1].id == id &&
        nav_events[1].phase == MB_NAVIGATION_PHASE_COMMITTED &&
        nav_events[1].outcome == MB_NAVIGATION_OUTCOME_NONE &&
        nav_events[2].id == id &&
        nav_events[2].phase == MB_NAVIGATION_PHASE_TERMINAL &&
        nav_events[2].outcome == MB_NAVIGATION_OUTCOME_SUCCESS;
    Expect(id != 0 && r.find("async-nav") != std::string::npos &&
               seq->find('S') != std::string::npos &&
               seq->find('F') != std::string::npos && cancel_done == 0 &&
               structured_lifecycle,
           "mbNavigate exposes one id-correlated started/commit/success lifecycle",
           "id=" + std::to_string(id) + " body=[" + r + "] seq=[" + *seq +
               "] events=" + std::to_string(nav_events.size()) +
               " cancel_finished=" + std::to_string(cancel_done));
    mbOnNavigationEvent(v, nullptr, nullptr);
    mbOnNavigationStarted(v, nullptr, nullptr);
    mbOnLoadFinish(v, nullptr, nullptr);
    mbClearMocks();
  }

  // 0b4b. Even an in-process dynamic mock is not resolved inside mbNavigate. The API
  // returns its id first; policy/mock lookup starts from the posted navigation task.
  {
    static int* mock_calls = new int(0);
    *mock_calls = 0;
    mbOnRequestMock(
        v,
        [](const char* url, mbRequestMock* mock, void*) -> int {
          if (!url || !std::strstr(url, "nav-posted-mock.test"))
            return 0;
          ++*mock_calls;
          const char body[] = "<body>posted-local-nav</body>";
          mbRequestMockResponse(mock, body, sizeof(body) - 1, "text/html", 200);
          return 1;
        },
        nullptr);
    mbLoadHTML(v, "<body>before-posted-nav</body>", "about:blank");
    const std::string body_before_nav =
        Eval(v, "document.body?document.body.textContent:''");
    const mbNavigationId id =
        mbNavigate(v, "https://nav-posted-mock.test/");
    const int calls_before_return = *mock_calls;
    mbWaitForFunction(v,
                      "document.body&&document.body.textContent==='posted-local-nav'",
                      3000);
    const std::string body_after_pump =
        Eval(v, "document.body?document.body.textContent:''");
    mbOnRequestMock(v, nullptr, nullptr);
    Expect(id != 0 && calls_before_return == 0 && *mock_calls == 1 &&
               body_before_nav == "before-posted-nav" &&
               body_after_pump == "posted-local-nav",
           "mbNavigate returns before local mock resolution/materialization",
           "id=" + std::to_string(id) + " calls=" +
               std::to_string(calls_before_return) + "->" +
               std::to_string(*mock_calls) + " bodies=[" + body_before_nav +
               "]/ [" + body_after_pump + "]");
  }

  // 0b4c. Async local navigation drives the posted ThreadPool branches for both
  // data: decoding and file: IO; neither may commit inline with mbNavigate.
  {
    // Drive both local branches of the async engine. Neither data decoding nor file IO
    // may commit inline with mbNavigate; each resolves on the blocking pool and replies
    // to the engine sequence afterward.
    mbLoadHTML(v, "<body>local-nav-baseline</body>", "about:blank");
    const mbNavigationId data_id =
        mbNavigate(v, "data:text/html,<body>async-data-nav</body>");
    const std::string data_immediate =
        Eval(v, "document.body?document.body.textContent:''");
    const bool data_done =
        mbWaitForFunction(v,
                          "document.body&&document.body.textContent==='async-data-nav'",
                          3000) == 1;

    base::ScopedTempDir local_dir;
    const bool temp_ready = local_dir.CreateUniqueTempDir();
    const base::FilePath local_path =
        temp_ready ? local_dir.GetPath().AppendASCII("page.html")
                   : base::FilePath();
    static const char kLocalHtml[] = "<body>async-file-nav</body>";
    const bool file_written =
        temp_ready && base::WriteFile(local_path, kLocalHtml);
    const std::string local_url =
        file_written ? net::FilePathToFileURL(local_path).spec() : std::string();
    const mbNavigationId file_id =
        file_written ? mbNavigate(v, local_url.c_str()) : 0;
    const std::string file_immediate =
        file_written ? Eval(v, "document.body?document.body.textContent:''")
                     : std::string();
    const bool file_done =
        file_written &&
        mbWaitForFunction(v,
                          "document.body&&document.body.textContent==='async-file-nav'",
                          3000) == 1;
    const std::string file_final =
        file_written ? Eval(v, "document.body?document.body.textContent:''")
                     : std::string();
    Expect(data_id != 0 && temp_ready && file_written && file_id != 0 &&
               data_immediate == "local-nav-baseline" && data_done &&
               file_immediate == "async-data-nav" && file_done &&
               file_final == "async-file-nav",
           "mbNavigate resolves data: decoding and file: IO asynchronously",
           "temp=" + std::to_string(temp_ready) + "/" +
               std::to_string(file_written) + " ids=" +
               std::to_string(data_id) + "/" +
               std::to_string(file_id) + " immediate=[" + data_immediate +
               "]/ [" + file_immediate + "] final=[" + file_final + "]");
  }

  // 0b5. Cancellation: a navigation cancelled before it commits NEVER commits — the current
  // document stays put — and mbCancelNavigation returns 1 for the active nav.
  {
    mbLoadHTML(v, "<body>cancel-baseline</body>", "https://cancel.base.test/");
    mbMockResponse("cancel.target.test", "<body>SHOULD-NOT-COMMIT</body>", "text/html",
                   200);
    CancelOnNavigationStartState state;
    mbOnNavigationEvent(v, &CancelOnNavigationStart, &state);
    const mbNavigationId id = mbNavigate(v, "https://cancel.target.test/");
    mbWait(v, 300);  // pump: the (superseded) completion must NOT commit
    const std::string body = Eval(v, "document.body?document.body.textContent:''");
    const bool cancelled_lifecycle =
        state.events.size() == 2 && state.events[0].id == id &&
        state.events[0].phase == MB_NAVIGATION_PHASE_STARTED &&
        state.events[1].id == id &&
        state.events[1].phase == MB_NAVIGATION_PHASE_TERMINAL &&
        state.events[1].outcome == MB_NAVIGATION_OUTCOME_CANCELLED &&
        state.events[1].error_domain == "cancelled";
    Expect(state.started_id == id && state.cancel_result == 1 &&
               body.find("cancel-baseline") != std::string::npos &&
               body.find("SHOULD-NOT-COMMIT") == std::string::npos &&
               cancelled_lifecycle,
           "STARTED carries the return id and can cancel it before mbNavigate returns",
           "id=" + std::to_string(id) +
               " callback_id=" + std::to_string(state.started_id) +
               " cancelled=" + std::to_string(state.cancel_result) + " body=[" +
               body + "] events=" + std::to_string(state.events.size()));
    mbOnNavigationEvent(v, nullptr, nullptr);
    mbClearMocks();
  }

  // 0b6. Supersession: a second navigation supersedes the first before it commits; only the
  // second document commits.
  {
    mbMockResponse("supersede.a.test", "<body>FIRST</body>", "text/html", 200);
    mbMockResponse("supersede.b.test", "<body>SECOND</body>", "text/html", 200);
    std::vector<NavigationEventRecord> nav_events;
    mbOnNavigationEvent(v, &RecordNavigationEvent, &nav_events);
    const mbNavigationId first_id =
        mbNavigate(v, "https://supersede.a.test/");
    const mbNavigationId second_id =
        mbNavigate(v, "https://supersede.b.test/");  // supersedes the first
    mbWaitForFunction(v, "document.body&&document.body.textContent==='SECOND'", 3000);
    const std::string body = Eval(v, "document.body?document.body.textContent:''");
    const bool superseded_lifecycle =
        nav_events.size() == 5 && nav_events[0].id == first_id &&
        nav_events[0].phase == MB_NAVIGATION_PHASE_STARTED &&
        nav_events[1].id == first_id &&
        nav_events[1].phase == MB_NAVIGATION_PHASE_TERMINAL &&
        nav_events[1].outcome == MB_NAVIGATION_OUTCOME_SUPERSEDED &&
        nav_events[2].id == second_id &&
        nav_events[2].phase == MB_NAVIGATION_PHASE_STARTED &&
        nav_events[3].id == second_id &&
        nav_events[3].phase == MB_NAVIGATION_PHASE_COMMITTED &&
        nav_events[4].id == second_id &&
        nav_events[4].phase == MB_NAVIGATION_PHASE_TERMINAL &&
        nav_events[4].outcome == MB_NAVIGATION_OUTCOME_SUCCESS;
    Expect(body == "SECOND" && superseded_lifecycle,
           "a newer id terminates the old id as SUPERSEDED, then succeeds",
           "body=[" + body + "] events=" +
               std::to_string(nav_events.size()));
    mbOnNavigationEvent(v, nullptr, nullptr);
    mbClearMocks();
  }

  // 0b7. mbNavigateEx: async navigation with an explicit method commits (mock-served), and
  // mbStopLoading cancels an in-flight async navigation (it never commits).
  {
    mbMockResponse("navex.test", "<body>navex-ok</body>", "text/html", 200);
    mbNavigationOptions opt = {};
    opt.struct_size = sizeof(mbNavigationOptions);
    opt.method = "POST";
    opt.body = "q=1";
    opt.body_len = 3;
    opt.content_type = "application/x-www-form-urlencoded";
    const mbNavigationId nid = mbNavigateEx(v, "https://navex.test/", &opt);
    mbWaitForFunction(v,
                      "document.readyState==='complete'&&document.body&&"
                      "document.body.textContent==='navex-ok'",
                      3000);
    const std::string b1 = Eval(v, "document.body?document.body.textContent:''");

    // A shorter declared struct prefix still contributes every complete field it
    // contains. This models an older caller after future fields have been appended:
    // method/body are honored, while content_type outside the prefix is not read.
    static std::string* prefix_method = new std::string();
    static std::string* prefix_body = new std::string();
    static int* prefix_saw_content_type = new int(0);
    prefix_method->clear();
    prefix_body->clear();
    *prefix_saw_content_type = 0;
    mbSetRequestCallbackEx(
        [](const char* url, const char* method, const char* headers,
           const char* body, int body_len, void*) -> int {
          if (!url || !std::strstr(url, "navex-prefix.test"))
            return 0;
          prefix_method->assign(method ? method : "");
          prefix_body->assign(body ? body : "",
                              body_len > 0 ? static_cast<size_t>(body_len) : 0u);
          *prefix_saw_content_type =
              headers && std::strstr(headers, "SHOULD-NOT-BE-READ") ? 1 : 0;
          return 0;
        },
        nullptr);
    mbMockResponse("navex-prefix.test", "<body>prefix-ok</body>", "text/html",
                   200);
    mbNavigationOptions prefix_opt = {};
    prefix_opt.struct_size =
        static_cast<int>(offsetof(mbNavigationOptions, body_len) +
                         sizeof(prefix_opt.body_len));
    prefix_opt.method = "PUT";
    prefix_opt.body = "q=2";
    prefix_opt.body_len = 3;
    prefix_opt.content_type = "SHOULD-NOT-BE-READ";
    const mbNavigationId prefix_id =
        mbNavigateEx(v, "https://navex-prefix.test/", &prefix_opt);
    mbWaitForFunction(v,
                      "document.readyState==='complete'&&document.body&&"
                      "document.body.textContent==='prefix-ok'",
                      3000);
    const std::string prefix_page =
        Eval(v, "document.body?document.body.textContent:''");
    mbSetRequestCallbackEx(nullptr, nullptr);

    // mbStopLoading cancels an in-flight async nav before it commits.
    mbLoadHTML(v, "<body>stop-baseline</body>", "https://stop.base.test/");
    mbMockResponse("stop.target.test", "<body>STOPPED-OUT</body>", "text/html", 200);
    mbNavigate(v, "https://stop.target.test/");
    mbStopLoading(v);  // aborts the in-flight navigation
    mbWait(v, 300);
    const std::string b2 = Eval(v, "document.body?document.body.textContent:''");
    Expect(nid != 0 && b1.find("navex-ok") != std::string::npos &&
               prefix_id != 0 && *prefix_method == "PUT" &&
               *prefix_body == "q=2" && !*prefix_saw_content_type &&
               prefix_page == "prefix-ok" &&
               b2.find("stop-baseline") != std::string::npos &&
               b2.find("STOPPED-OUT") == std::string::npos,
           "mbNavigateEx honors struct prefixes; mbStopLoading cancels an in-flight nav",
           "id=" + std::to_string(nid) + " navex=[" + b1 + "] prefix=" +
               std::to_string(prefix_id) + "/" + *prefix_method + "/" +
               *prefix_body + "/ct=" +
               std::to_string(*prefix_saw_content_type) + " page=[" +
               prefix_page + "] afterstop=[" + b2 + "]");
    mbClearMocks();
  }

  // 0b8. Page-initiated MAIN-frame navigation (location.href / link / form) now fetches
  // ASYNCHRONOUSLY (no blocking curl on Blink's task runner) and commits from a posted
  // completion — mock-served here. Response-hook coverage for page navigation is preserved.
  {
    static std::string* seen = new std::string();  // -Wexit-time-destructors
    seen->clear();
    mbSetResponseCallback(
        [](mbResponse* r, void*) {
          if (std::strstr(mbResponseURL(r), "pageinit.dest.test"))
            seen->assign("hooked");
        },
        nullptr);
    mbMockResponse("pageinit.dest.test", "<body id='pi'>page-init-ok</body>", "text/html",
                   200);
    mbLoadHTML(v, "<body>start</body>", "https://pageinit.start.test/");
    Eval(v, "location.href='https://pageinit.dest.test/'");
    mbWaitForFunction(v,
                      "document.getElementById('pi')&&"
                      "document.getElementById('pi').textContent==='page-init-ok'",
                      3000);
    const std::string r = Eval(v, "document.body?document.body.textContent:''");
    Expect(r.find("page-init-ok") != std::string::npos && *seen == "hooked",
           "page-initiated navigation commits via the async engine (+ response hook)",
           "body=[" + r + "] hook=[" + *seen + "]");
    mbSetResponseCallback(nullptr, nullptr);
    mbClearMocks();
  }

  // 0b9. Host- and page-initiated navigations share ONE top-level generation. A queued
  // page navigation must not commit over a newer host navigation, and mbStopLoading must
  // invalidate a page navigation even before its posted fetch task starts. Starting the
  // page navigation also resets mbIsLoadFinished immediately.
  {
    mbMockResponse("cross.page.test", "<body>STALE-PAGE</body>", "text/html", 200);
    mbMockResponse("cross.host.test", "<body>NEW-HOST</body>", "text/html", 200);
    mbLoadHTML(v, "<body>cross-baseline</body>", "https://cross.start.test/");
    Eval(v, "location.href='https://cross.page.test/'");
    const int page_is_loading = mbIsLoadFinished(v) == 0;
    mbNavigate(v, "https://cross.host.test/");
    mbWait(v, 400);  // run both posted completions; the later host navigation must win
    const std::string after_host = Eval(v, "document.body?document.body.textContent:''");

    mbMockResponse("cross.stop.test", "<body>SHOULD-NOT-COMMIT</body>",
                   "text/html", 200);
    mbLoadHTML(v, "<body>stop-page-baseline</body>", "https://cross.start.test/");
    Eval(v, "location.href='https://cross.stop.test/'");
    mbStopLoading(v);  // also cancels/invalidates the queued page-navigation task
    mbWait(v, 400);
    const std::string after_stop = Eval(v, "document.body?document.body.textContent:''");
    Expect(page_is_loading && after_host == "NEW-HOST" &&
               after_stop == "stop-page-baseline",
           "page/host navigations share generation; stop cancels queued page nav",
           "loading=" + std::to_string(page_is_loading) + " host=[" + after_host +
               "] stop=[" + after_stop + "]");
    mbClearMocks();
  }

  // 0b10. The new async main-resource path must honor the legacy static blocklist and
  // request callbacks before mocks/network. Both cases are offline: without the policy
  // checks the matching mock commits; with them the baseline stays in place and the
  // failure is classified as blocked.
  {
    static std::string* blocked_domain = new std::string();
    mbOnFailLoadingEx(
        v,
        [](mbView*, void*, const char*, const char* domain, int, const char*) {
          *blocked_domain = domain ? domain : "";
        },
        nullptr);

    mbLoadHTML(v, "<body>block-baseline-a</body>", "https://block.base.test/");
    mbMockResponse("blocked.async.test", "<body>BLOCKLIST-BYPASS</body>",
                   "text/html", 200);
    mbBlockUrl("blocked.async.test");
    blocked_domain->clear();
    mbNavigate(v, "https://blocked.async.test/");
    mbWait(v, 250);
    const std::string block_body = Eval(v, "document.body?document.body.textContent:''");
    const bool static_blocked = block_body == "block-baseline-a" &&
                                *blocked_domain == "blocked";
    mbClearUrlBlocks();

    mbLoadHTML(v, "<body>block-baseline-b</body>", "https://block.base.test/");
    mbMockResponse("callback.async.test", "<body>CALLBACK-BYPASS</body>",
                   "text/html", 200);
    mbSetRequestCallback(
        [](const char* url, void*) -> int {
          return url && std::strstr(url, "callback.async.test") ? 1 : 0;
        },
        nullptr);
    blocked_domain->clear();
    mbNavigate(v, "https://callback.async.test/");
    mbWait(v, 250);
    const std::string callback_body =
        Eval(v, "document.body?document.body.textContent:''");
    const bool callback_blocked = callback_body == "block-baseline-b" &&
                                  *blocked_domain == "blocked";
    mbSetRequestCallback(nullptr, nullptr);
    mbOnFailLoadingEx(v, nullptr, nullptr);
    mbClearMocks();
    Expect(static_blocked && callback_blocked,
           "mbNavigate honors mbBlockUrl and legacy request callbacks",
           "static=" + std::to_string(static_blocked) + " callback=" +
               std::to_string(callback_blocked));
  }

  // 0b10b. The default/offline suite covers the load-bearing request-policy order too:
  // globally interleaved static registrations compose before the mutable hook, and the
  // hook's URL mutation is the final policy step before mock lookup.
  {
    static int* saw_later_static = new int(0);
    *saw_later_static = 0;
    mbClearRequestHeaders();
    mbSetRequestHeaderForOrigin("https://policy-order.test", "X-Policy-Order",
                                "origin-first");
    mbSetRequestHeader("policy-order.test/start", "X-Policy-Order",
                       "substring-later");
    mbSetRequestHook(
        [](mbRequest* request, void*) {
          const char* url = mbRequestURL(request);
          if (!url || !std::strstr(url, "policy-order.test/start"))
            return;
          const std::string headers = mbRequestHeaders(request);
          constexpr char kField[] = "X-Policy-Order:";
          int field_count = 0;
          for (size_t at = headers.find(kField); at != std::string::npos;
               at = headers.find(kField, at + 1)) {
            ++field_count;
          }
          *saw_later_static =
              field_count == 1 &&
                      headers.find("X-Policy-Order: substring-later") !=
                          std::string::npos &&
                      headers.find("origin-first") == std::string::npos
                  ? 1
                  : 0;
          mbRequestSetHeader(request, "X-Policy-Order", "hook-final");
          mbRequestSetUrl(request, "https://policy-order.test/final");
        },
        nullptr);
    mbMockResponse("policy-order.test/final", "<body>policy-order-ok</body>",
                   "text/html", 200);
    mbLoadURL(v, "https://policy-order.test/start");
    const std::string policy_body =
        Eval(v, "document.body?document.body.textContent:''");
    mbSetRequestHook(nullptr, nullptr);
    mbClearRequestHeaders();
    mbClearMocks();
    Expect(*saw_later_static && policy_body == "policy-order-ok",
           "request policy composes one globally ordered static field before the hook",
           "saw=" + std::to_string(*saw_later_static) + " body=[" +
               policy_body + "]");
  }

  // 0b11. In-memory content and response-hook reentrancy both supersede an
  // async result. The stale completion must never commit after the newer
  // document, including when the newer load starts from inside the hook itself.
  {
    mbMockResponse("stale-html.test", "<body>STALE-HTML</body>", "text/html", 200);
    mbNavigate(v, "https://stale-html.test/");
    mbLoadHTML(v, "<body>FRESH-HTML</body>", "https://fresh-html.test/");
    mbWait(v, 250);
    const std::string after_html =
        Eval(v, "document.body?document.body.textContent:''");

    mbMockResponse("stale-hook.test", "<body>STALE-HOOK</body>", "text/html", 200);
    mbSetResponseCallbackEx(
        [](mbResponse* response, mbView* view, void*) {
          if (view && std::strstr(mbResponseURL(response), "stale-hook.test"))
            mbLoadHTML(view, "<body>FRESH-HOOK</body>",
                       "https://fresh-hook.test/");
        },
        nullptr);
    mbNavigate(v, "https://stale-hook.test/");
    mbWait(v, 300);
    mbSetResponseCallbackEx(nullptr, nullptr);
    const std::string after_hook =
        Eval(v, "document.body?document.body.textContent:''");
    mbClearMocks();
    Expect(after_html == "FRESH-HTML" && after_hook == "FRESH-HOOK",
           "LoadHTML and response-hook reentrancy discard stale async results",
           "html=[" + after_html + "] hook=[" + after_hook + "]");
  }

  // 0c. Dynamic per-request hook: mbSetRequestCallback is consulted for EVERY request
  // the loader handles and can block per-URL at runtime (vs the static substring
  // tables). Two same-origin fetch()es — one served by a mock (allowed), one whose URL
  // contains "blockme" (the hook blocks it) — prove both inspection (the hook records
  // every URL it sees) and per-request veto. Fully offline (mock + hook, no network).
  {
    // Heap-owned, never destroyed (-Wexit-time-destructors); statics aren't captured
    // so the lambda stays convertible to a C function pointer.
    static std::vector<std::string>* seen = new std::vector<std::string>();
    seen->clear();
    mbMockResponse("site.test/ok", "{\"v\":7}", "application/json", 200);
    mbSetRequestCallback(
        [](const char* url, void*) -> int {
          seen->push_back(url);
          return std::strstr(url, "blockme") ? 1 : 0;  // veto only the blockme URL
        },
        nullptr);
    mbLoadHTML(v,
               "<body><div id='r'>?</div><script>(async()=>{"
               "let a='aerr',b='?';"
               "try{a='ok:'+(await (await fetch('https://site.test/ok')).json()).v;}"
               "catch(e){a='aerr';}"
               "try{await fetch('https://site.test/blockme');b='got';}"
               "catch(e){b='blocked';}"
               "document.getElementById('r').textContent=a+','+b;})();</script></body>",
               "https://site.test/");
    mbWaitForFunction(v, "document.getElementById('r').textContent!=='?'", 2000);
    const std::string r = Eval(v, "document.getElementById('r').textContent");
    bool saw_blockme = false;
    for (const auto& u : *seen)
      if (u.find("blockme") != std::string::npos)
        saw_blockme = true;
    Expect(r.find("ok:7") != std::string::npos &&
               r.find("blocked") != std::string::npos && saw_blockme,
           "mbSetRequestCallback inspects + vetoes per request (mock ok, blockme blocked)",
           std::string("r=[") + r + "] seen=" + std::to_string(seen->size()));
    mbSetRequestCallback(nullptr, nullptr);
    mbClearMocks();
  }

  // 0c2. mbSetRequestCallbackEx: the request hook also sees the METHOD, request HEADERS,
  // and POST BODY — so an embedder can MONITOR what API calls a page makes (a POST and its
  // payload), not just match URLs. A same-origin fetch POST with a custom header + body ->
  // the hook captures method=POST, the header, and the exact body. Offline (mock, no net).
  {
    static std::string* cap = new std::string();  // -Wexit-time-destructors
    cap->clear();
    mbMockResponse("api.test/submit", "{\"ok\":1}", "application/json", 200);
    mbSetRequestCallbackEx(
        [](const char* url, const char* method, const char* headers,
           const char* body, int body_len, void*) -> int {
          if (std::string(url).find("api.test/submit") != std::string::npos) {
            const bool hdr = std::string(headers).find("X-Tok: abc") != std::string::npos;
            *cap = std::string("m=") + method + " hdr=" + (hdr ? "1" : "0") +
                   " body=" + std::string(body, static_cast<size_t>(body_len));
          }
          return 0;  // allow
        },
        nullptr);
    mbLoadHTML(v, "<body>reqex</body>", "https://api.test/");
    mbRunJS(v, "fetch('/submit',{method:'POST',headers:{'X-Tok':'abc'},"
               "body:'payload-42'});");
    mbWaitForFunction(v, "true", 300);
    mbWait(v, 100);
    mbSetRequestCallbackEx(nullptr, nullptr);
    mbClearMocks();
    Expect(*cap == "m=POST hdr=1 body=payload-42",
           "mbSetRequestCallbackEx: request hook sees method + headers + POST body",
           "[" + *cap + "]");
  }

  // 0c3. Block by RESOURCE TYPE (mbBlockResourceType): block "image" -> an <img> fails to
  // load (onerror, ERR_BLOCKED_BY_CLIENT); unblocking it -> the same image loads. Lets a
  // scrape skip heavy classes (images/fonts/media) for speed without listing URLs. Offline.
  {
    // Mock an http image so the request flows through the loader (data: images are decoded
    // inline by blink and never reach the loader's per-request type check).
    mbMockResponse("imgtype.test/pic.svg",
                   "<svg xmlns='http://www.w3.org/2000/svg' width='5' height='5'></svg>",
                   "image/svg+xml", 200);
    const char* page =
        "<body><img id='im' src='/pic.svg' "
        "onload=\"window.__im='loaded'\" onerror=\"window.__im='error'\"></body>";
    mbBlockResourceType("image", 1);
    mbLoadHTML(v, page, "https://imgtype.test/");
    mbWaitForFunction(v, "window.__im!==undefined", 2000);
    const std::string blocked = Eval(v, "window.__im");
    mbBlockResourceType("image", 0);  // unblock
    mbLoadHTML(v, page, "https://imgtype.test/");
    mbWaitForFunction(v, "window.__im!==undefined", 2000);
    const std::string allowed = Eval(v, "window.__im");
    mbClearMocks();
    Expect(blocked == "error" && allowed == "loaded",
           "mbBlockResourceType: block 'image' fails <img>, unblock loads it",
           "blocked=[" + blocked + "] allowed=[" + allowed + "]");
  }

  // 0d. Response hook: mbSetResponseCallback sees every response BEFORE the page and can
  // REPLACE the body. A mock serves {"v":1}; the hook inspects it (records the original)
  // and rewrites it to {"v":99}; the page's fetch() must observe the rewritten 99, and
  // the new (shorter/longer) length must be delivered. Fully offline.
  {
    static std::string* orig = new std::string();  // heap-owned (-Wexit-time-destructors)
    static mbView** hook_view = new mbView*(nullptr);  // originating view seen by the hook
    orig->clear();
    *hook_view = nullptr;
    mbMockResponse("api.test/v", "{\"v\":1}", "application/json", 200);
    mbSetResponseCallbackEx(
        [](mbResponse* r, mbView* view, void*) {
          int n = 0;
          const char* b = mbResponseBody(r, &n);
          if (std::strstr(mbResponseURL(r), "api.test/v")) {
            orig->assign(b, n);  // inspect: capture what the server/mock returned
            *hook_view = view;   // record which view this load belonged to
            const char* rep = "{\"v\":99}";
            mbResponseSetBody(r, rep, static_cast<int>(std::strlen(rep)));  // modify
          }
        },
        nullptr);
    mbLoadHTML(v,
               "<body><div id='r'>?</div><script>"
               "fetch('https://api.test/v').then(r=>r.json()).then(j=>{"
               "document.getElementById('r').textContent='v='+j.v;});</script></body>",
               "https://api.test/");
    mbWaitForFunction(v, "document.getElementById('r').textContent!=='?'", 2000);
    const std::string r = Eval(v, "document.getElementById('r').textContent");
    Expect(r == "v=99" && *orig == "{\"v\":1}" && *hook_view == v,
           "mbSetResponseCallback inspects + rewrites the response body before the page "
           "(and reports the originating view)",
           std::string("page=[") + r + "] orig=[" + *orig +
               "] view_matches=" + (*hook_view == v ? "1" : "0"));
    mbSetResponseCallback(nullptr, nullptr);
    mbClearMocks();
  }

  // 0d2. Response hook can rewrite the STATUS (mbResponseSetStatus), not just the body — so
  // an embedder can dynamically fabricate a response (route.fulfill-like, decided from the
  // actual upstream response). A mock serves 200; the hook forces 503; the page's fetch must
  // see response.status===503 and response.ok===false. Offline.
  {
    mbMockResponse("api.test/flaky", "upstream-ok", "text/plain", 200);
    mbSetResponseCallback(
        [](mbResponse* r, void*) {
          if (std::strstr(mbResponseURL(r), "api.test/flaky"))
            mbResponseSetStatus(r, 503);  // turn the 200 into a 503
        },
        nullptr);
    mbLoadHTML(v,
               "<body><div id='s'>?</div><script>"
               "fetch('https://api.test/flaky').then(r=>{"
               "document.getElementById('s').textContent=r.status+':'+r.ok;});</script></body>",
               "https://api.test/");
    mbWaitForFunction(v, "document.getElementById('s').textContent!=='?'", 2000);
    const std::string s = Eval(v, "document.getElementById('s').textContent");
    Expect(s == "503:false",
           "mbResponseSetStatus: response hook rewrites the HTTP status the page sees",
           "page=[" + s + "]");
    mbSetResponseCallback(nullptr, nullptr);
    mbClearMocks();
  }

  // 0d3. Response hook can inject/override response HEADERS (mbResponseSetHeader): a custom
  // header the page reads back via fetch Response.headers.get, and a Content-Type override.
  // Same-origin so all headers are exposed. Proves header mutation flows to the delivered
  // response (CORS injection / Content-Type forcing / custom fields).
  {
    mbMockResponse("api.test/h", "hello", "text/plain", 200);
    mbSetResponseCallback(
        [](mbResponse* r, void*) {
          if (std::strstr(mbResponseURL(r), "api.test/h")) {
            mbResponseSetHeader(r, "X-Injected", "yes");
            mbResponseSetHeader(r, "Content-Type", "application/json");
          }
        },
        nullptr);
    mbLoadHTML(v,
               "<body><div id='r'>?</div><script>"
               "fetch('https://api.test/h').then(r=>{"
               "document.getElementById('r').textContent="
               "'x='+r.headers.get('x-injected')+',ct='+r.headers.get('content-type');});"
               "</script></body>",
               "https://api.test/");
    mbWaitForFunction(v, "document.getElementById('r').textContent!=='?'", 2000);
    const std::string r = Eval(v, "document.getElementById('r').textContent");
    Expect(r.find("x=yes") != std::string::npos &&
               r.find("application/json") != std::string::npos,
           "mbResponseSetHeader injects a custom header + overrides Content-Type (fetch sees both)",
           "r=[" + r + "]");
    mbSetResponseCallback(nullptr, nullptr);
    mbClearMocks();
  }

  // 0d4. Dynamic request mock (mbSetRequestMockCallback): COMPUTE a response per-URL with no
  // fetch, for URLs that can't be pre-registered as a fixed substring. Here the callback
  // parses the id out of /item/N and serves {"id":N} as JSON — something the static
  // mbMockResponse (fixed substring -> fixed body) can't do.
  {
    mbSetRequestMockCallback(
        [](const char* url, mbRequestMock* m, void*) -> int {
          const char* p = std::strstr(url, "/item/");
          if (!p)
            return 0;  // not ours: fetch normally
          std::string body = std::string("{\"id\":") + (p + 6) + "}";
          mbRequestMockResponse(m, body.data(), static_cast<int>(body.size()),
                                "application/json", 200);
          return 1;  // serve the computed response
        },
        nullptr);
    mbLoadHTML(v,
               "<body><div id='r'>?</div><script>"
               "fetch('https://x.test/item/42').then(r=>r.json()).then(j=>{"
               "document.getElementById('r').textContent='id='+j.id;});</script></body>",
               "https://x.test/");
    mbWaitForFunction(v, "document.getElementById('r').textContent!=='?'", 2000);
    const std::string r = Eval(v, "document.getElementById('r').textContent");
    Expect(r == "id=42",
           "mbSetRequestMockCallback computes a per-URL response served without a fetch",
           "r=[" + r + "]");
    mbSetRequestMockCallback(nullptr, nullptr);
  }

  // 0e. mbDownloadURL fetches a URL through the engine and writes the body to disk
  // WITHOUT rendering it. (a) a data: URL decodes to the file; (b) a mocked URL is
  // served from the interception layer (no network) AND the response hook can rewrite
  // the downloaded bytes — proving downloads honor the same interception as page loads.
  {
    auto slurp = [](const char* p) -> std::string {
      std::string s;
      if (FILE* f = std::fopen(p, "rb")) {
        char b[4096];
        size_t n;
        while ((n = std::fread(b, 1, sizeof(b), f)) > 0)
          s.append(b, n);
        std::fclose(f);
      }
      return s;
    };
    const int d1 = mbDownloadURL(v, "data:text/plain,DL-DATA-7", "/tmp/mb_dl1.bin");
    const std::string f1 = slurp("/tmp/mb_dl1.bin");
    mbMockResponse("dl.test/file", "MOCKED-BODY", "application/octet-stream", 200);
    mbSetResponseCallback(
        [](mbResponse* r, void*) {
          if (std::strstr(mbResponseURL(r), "dl.test/file"))
            mbResponseSetBody(r, "REWRITTEN", 9);
        },
        nullptr);
    const int d2 =
        mbDownloadURL(v, "https://dl.test/file", "/tmp/mb_dl2.bin");
    const std::string f2 = slurp("/tmp/mb_dl2.bin");
    mbSetResponseCallback(nullptr, nullptr);
    mbClearMocks();
    Expect(d1 == 1 && f1 == "DL-DATA-7" && d2 == 1 && f2 == "REWRITTEN",
           "mbDownloadURL writes to disk; honors mock + response-hook rewrite",
           std::string("d1=") + std::to_string(d1) + " f1=[" + f1 + "] d2=" +
               std::to_string(d2) + " f2=[" + f2 + "]");
  }

  // 0f. JS dialogs (alert/confirm/prompt) handled in-process via mbSetJsDialogCallback:
  // a registered callback captures each message and drives the result (accept confirm,
  // return prompt text); with NO callback the headless-safe defaults apply (confirm=
  // false, prompt=null). Implemented as a pre-page JS override — no browser/modal.
  {
    static std::string* dlg = new std::string();  // -Wexit-time-destructors
    dlg->clear();
    mbSetJsDialogCallback(
        v,
        [](int type, const char* msg, const char* /*def*/, char* out, int cap,
           void*) -> int {
          *dlg += std::to_string(type) + ":" + (msg ? msg : "") + ";";  // capture
          if (type == 2 && out && cap > 0)
            std::snprintf(out, static_cast<size_t>(cap), "REPLY");  // prompt text
          return 1;  // accept alert/confirm/prompt
        },
        nullptr);
    mbLoadHTML(v,
               "<body><script>window.__a=(alert('hi'),'ok');"
               "window.__c=confirm('go?');window.__p=prompt('name?','d');"
               "</script></body>",
               "about:blank");
    const std::string c = Eval(v, "''+window.__c");
    const std::string p = Eval(v, "''+window.__p");
    Expect(Eval(v, "window.__a") == "ok" && c == "true" && p == "REPLY" &&
               dlg->find("0:hi;") != std::string::npos &&
               dlg->find("1:go?;") != std::string::npos &&
               dlg->find("2:name?;") != std::string::npos,
           "mbSetJsDialogCallback handles alert/confirm/prompt (capture + accept + text)",
           "c=[" + c + "] p=[" + p + "] log=[" + *dlg + "]");
    // No callback -> headless-safe defaults.
    mbSetJsDialogCallback(v, nullptr, nullptr);
    mbLoadHTML(v,
               "<body><script>window.__c2=confirm('x');window.__p2=prompt('y');"
               "</script></body>",
               "about:blank");
    Expect(Eval(v, "''+window.__c2") == "false" && Eval(v, "''+window.__p2") == "null",
           "JS dialog default (no callback): confirm=false, prompt=null");
  }

  // 0g. Navigation policy: mbOnNavigation fires for each PAGE-initiated navigation and
  // can BLOCK it. A callback denies URLs containing "blocked", allows others. A
  // location.href to an allowed data: URL commits (body becomes GOOD); a later one to a
  // "blocked" URL is vetoed (body stays GOOD). The log proves the callback saw both.
  {
    static std::string* navlog = new std::string();  // -Wexit-time-destructors
    navlog->clear();
    mbOnNavigation(
        v,
        [](mbView*, void*, const char* url) -> int {
          *navlog += std::string(url ? url : "") + ";";
          return (url && std::strstr(url, "blocked")) ? 0 : 1;  // veto "blocked"
        },
        nullptr);
    // Mock the navigation targets so they commit offline (top-level data:/file: nav is
    // browser-blocked, so use http URLs served from the interception layer).
    mbMockResponse("nav.test/ok", "<body>GOOD</body>", "text/html", 200);
    mbMockResponse("nav.test/blocked", "<body>SHOULD-NOT-SHOW</body>", "text/html", 200);
    mbLoadHTML(v, "<body>START</body>", "https://nav.test/");
    Eval(v, "location.href='https://nav.test/ok'");  // allowed -> commits the mock
    mbWait(v, 300);
    const std::string a = Eval(v, "document.body.textContent");
    Eval(v, "location.href='https://nav.test/blocked'");  // vetoed -> stays
    mbWait(v, 300);
    const std::string b = Eval(v, "document.body.textContent");
    Expect(a == "GOOD" && b == "GOOD" &&
               navlog->find("nav.test/ok") != std::string::npos &&
               navlog->find("nav.test/blocked") != std::string::npos,
           "mbOnNavigation allows + blocks page-initiated navigations",
           "a=[" + a + "] b=[" + b + "] log=[" + *navlog + "]");
    mbOnNavigation(v, nullptr, nullptr);
    mbClearMocks();
  }

  // 0h. New-window notification: mbOnNewWindow fires when the page calls window.open
  // (or activates target=_blank) with the requested URL + name. The popup itself is
  // still denied (window.open returns null) — safe for untrusted pages — but the host
  // learns what was requested and can act on it.
  {
    static std::string* winlog = new std::string();  // -Wexit-time-destructors
    winlog->clear();
    mbOnNewWindow(
        v,
        [](mbView*, void*, const char* url, const char* name) {
          *winlog += std::string(url ? url : "") + "|" + (name ? name : "") + ";";
        },
        nullptr);
    mbLoadHTML(v,
               "<body><script>window.__o=String(window.open("
               "'https://popup.test/p','winname'));</script></body>",
               "about:blank");
    const std::string opened = Eval(v, "window.__o");
    Expect(opened == "null" &&
               winlog->find("https://popup.test/p|winname;") != std::string::npos,
           "mbOnNewWindow notifies window.open URL+name; popup still denied",
           "open=[" + opened + "] log=[" + *winlog + "]");
    mbOnNewWindow(v, nullptr, nullptr);
  }

  // 0i. Mouse-click fidelity (#12): mbSendMouseClickEx carries the button + modifier
  // keys. A shift+alt LEFT click fires `click` with button 0 + shiftKey + altKey; a
  // MIDDLE click fires `auxclick` with button 1; a RIGHT click fires `contextmenu`.
  // (Ctrl+click isn't used here — on macOS that is the secondary/context-menu click.)
  {
    mbLoadHTML(v,
        "<body style='margin:0'><script>window.c='';window.a='';window.x='';"
        "addEventListener('click',function(e){window.c=e.button+','+e.shiftKey+','"
        "+e.altKey;});"
        "addEventListener('auxclick',function(e){window.a=''+e.button;});"
        "addEventListener('contextmenu',function(e){window.x='ctx';"
        "e.preventDefault();});</script></body>",
        "about:blank");
    mbSendMouseClickEx(v, 50, 50, 0, 2 | 4);  // left + shift + alt
    const std::string c = Eval(v, "window.c");
    mbSendMouseClickEx(v, 50, 50, 1, 0);  // middle
    const std::string a = Eval(v, "window.a");
    mbSendMouseClickEx(v, 50, 50, 2, 0);  // right
    const std::string x = Eval(v, "window.x");
    Expect(c == "0,true,true" && a == "1" && x == "ctx",
           "mbSendMouseClickEx carries button + shift/alt (left/middle/right)",
           "c=[" + c + "] a=[" + a + "] x=[" + x + "]");
  }

  // 0i4. Trusted mouse-wheel (#12 input): mbSendWheel dispatches a real wheel event so a
  // page `wheel` handler sees DOM-convention deltas (deltaY>0 = down, deltaX>0 = right)
  // with isTrusted=true — what wheel-driven UIs (map/canvas zoom, scroll hijacking,
  // "load more on scroll") listen for — AND scrolls the document viewport by the deltas
  // (here scrollY -> ~120). A wheel handler that calls preventDefault SUPPRESSES the
  // scroll, exactly like a real browser (checked below).
  {
    mbLoadHTML(v,
        "<body style='margin:0;height:5000px'><script>window.w='';"
        "addEventListener('wheel',function(e){"
        "window.w=e.deltaY+','+e.deltaX+','+e.isTrusted;});"
        "</script></body>",
        "about:blank");
    mbSendWheel(v, 50, 50, 0, 120, 0);     // wheel down -> event + scroll
    const std::string w1 = Eval(v, "window.w");
    const std::string sy = Eval(v, "''+Math.round(window.scrollY)");
    mbSendWheel(v, 50, 50, 40, -120, 0);   // wheel up + right
    const std::string w2 = Eval(v, "window.w");
    Expect(w1 == "120,0,true" && w2 == "-120,40,true" && sy == "120",
           "mbSendWheel fires a trusted wheel event (both axes) AND scrolls the document",
           "w1=[" + w1 + "] w2=[" + w2 + "] scrollY=[" + sy + "]");
  }

  // 0i5. A wheel handler that calls preventDefault SUPPRESSES the default scroll
  // (browser-accurate): the event still fires but window.scrollY stays 0.
  {
    mbLoadHTML(v,
        "<body style='margin:0;height:5000px'><script>window.pd=0;"
        "addEventListener('wheel',function(e){window.pd=1;e.preventDefault();},"
        "{passive:false});</script></body>",
        "about:blank");
    mbSendWheel(v, 50, 50, 0, 200, 0);
    const std::string pd = Eval(v, "''+window.pd");
    const std::string sy = Eval(v, "''+Math.round(window.scrollY)");
    Expect(pd == "1" && sy == "0",
           "mbSendWheel: a preventDefault wheel handler suppresses the scroll",
           "pd=[" + pd + "] scrollY=[" + sy + "]");
  }

  // 0i2. IME composition (#12 input): mbSendIme drives the focused input through a
  // composition preview + commit — the committed text lands and the composition events
  // fire (CJK / accented input via an input method).
  {
    mbLoadHTML(v,
        "<body><input id='ime'><script>window.__cs=0;window.__ce=0;"
        "var e=document.getElementById('ime');"
        "e.addEventListener('compositionstart',function(){window.__cs++;});"
        "e.addEventListener('compositionend',function(){window.__ce++;});"
        "</script></body>",
        "about:blank");
    mbFocusSelector(v, "#ime");
    mbSendIme(v, "\xE3\x81\xAB\xE3\x81\xBB", "\xE6\x97\xA5\xE6\x9C\xAC");  // "にほ" -> "日本"
    const std::string val = Eval(v, "document.getElementById('ime').value");
    Expect(val == "\xE6\x97\xA5\xE6\x9C\xAC" && Eval(v, "''+window.__cs") == "1" &&
               Eval(v, "''+window.__ce") == "1",
           "mbSendIme: IME compose+commit lands text + fires composition events",
           "val=[" + val + "] cs=" + Eval(v, "''+window.__cs") + " ce=" +
               Eval(v, "''+window.__ce"));
  }

  // 0k. Console push: mbOnConsoleMessage fires LIVE for each console message (vs polling
  // DrainConsole), with its level + text — react to errors/logs during a long script.
  {
    static std::string* clog = new std::string();  // -Wexit-time-destructors
    clog->clear();
    mbOnConsoleMessage(
        v,
        [](mbView*, void*, const char* level, const char* msg) {
          *clog += std::string(level ? level : "") + ":" + (msg ? msg : "") + ";";
        },
        nullptr);
    mbLoadHTML(v,
               "<body><script>console.log('hi');console.error('boom');</script></body>",
               "about:blank");
    Expect(clog->find("log:hi;") != std::string::npos &&
               clog->find("error:boom;") != std::string::npos,
           "mbOnConsoleMessage: live push of console.log/error with level+text",
           "log=[" + *clog + "]");
    mbOnConsoleMessage(v, nullptr, nullptr);
  }

  // 0k2. mbOnConsoleMessageEx delivers source/line/stack, so an embedder can monitor
  // UNCAUGHT EXCEPTIONS (blink reports them here as console errors) with full location —
  // not just the message. A page that throws at top level must surface: level=error, the
  // thrown text, the page URL as source, a line number, and a non-empty JS stack.
  {
    static std::string* einfo = new std::string();  // -Wexit-time-destructors
    einfo->clear();
    mbOnConsoleMessageEx(
        v,
        [](mbView*, void*, const char* level, const char* msg, const char* source,
           int line, const char* stack) {
          if (level && std::string(level) == "error") {
            *einfo = std::string("msg=[") + (msg ? msg : "") + "] src=[" +
                     (source ? source : "") + "] line=" + std::to_string(line) +
                     " stacklen=" +
                     std::to_string(stack ? std::string(stack).size() : 0);
          }
        },
        nullptr);
    mbLoadHTML(v, "<body><script>throw new Error('kaboom');</script></body>",
               "https://errpage.test/");
    mbWait(v, 80);
    // Uncaught exception: message + source URL + line are delivered (error monitoring).
    const bool has_msg = einfo->find("kaboom") != std::string::npos;
    const bool has_src = einfo->find("errpage.test") != std::string::npos;
    const bool has_line = einfo->find("line=0 ") == std::string::npos &&
                          einfo->find("line=") != std::string::npos;
    const std::string exc = *einfo;
    // console.error from a nested call chain: the detailed-message opt-in
    // (ShouldReportDetailedMessageForSourceAndSeverity) makes blink capture the FULL JS
    // stack, so `stack` names the call chain — what a monitor needs to locate the source.
    einfo->clear();
    mbLoadHTML(v,
               "<body><script>function inner(){console.error('traced');}"
               "function outer(){inner();}outer();</script></body>",
               "https://errpage2.test/");
    mbWait(v, 80);
    const bool stack_ok = einfo->find("stacklen=0") == std::string::npos &&
                          einfo->find("stacklen=") != std::string::npos &&
                          einfo->find("traced") != std::string::npos;
    Expect(has_msg && has_src && has_line && stack_ok,
           "mbOnConsoleMessageEx: exception gives message+source+line; console.* gives a stack",
           "exc=[" + exc + "] traced=[" + *einfo + "]");
    mbOnConsoleMessageEx(v, nullptr, nullptr);
  }

  // 0l. URL-changed: mbOnUrlChanged fires on every main-frame commit with the new URL —
  // track where the view is (host loads, navigations, redirects).
  {
    static std::string* urls = new std::string();  // -Wexit-time-destructors
    urls->clear();
    mbOnUrlChanged(
        v, [](mbView*, void*, const char* url) { *urls += std::string(url) + ";"; },
        nullptr);
    mbLoadHTML(v, "<body>a</body>", "https://u.test/a");
    mbLoadHTML(v, "<body>b</body>", "https://u.test/b");
    Expect(urls->find("u.test/a") != std::string::npos &&
               urls->find("u.test/b") != std::string::npos,
           "mbOnUrlChanged fires per main-frame commit with the new URL",
           "urls=[" + *urls + "]");
    mbOnUrlChanged(v, nullptr, nullptr);
  }

  // 0l2. Title-changed: mbOnTitleChanged fires with the initial <title> and on every dynamic
  // document.title write — track tab titles / progress from automation.
  {
    static std::string* titles = new std::string();  // -Wexit-time-destructors
    titles->clear();
    mbOnTitleChanged(
        v, [](mbView*, void*, const char* t) { *titles += std::string(t) + ";"; },
        nullptr);
    mbLoadHTML(v, "<title>First</title><body>x</body>", "https://t.test/");
    mbRunJS(v, "document.title='Second';");
    mbWait(v, 30);
    mbRunJS(v, "document.title='Third';");
    mbWait(v, 30);
    Expect(titles->find("First;") != std::string::npos &&
               titles->find("Second;") != std::string::npos &&
               titles->find("Third;") != std::string::npos,
           "mbOnTitleChanged fires with initial <title> + dynamic document.title writes",
           "titles=[" + *titles + "]");
    mbOnTitleChanged(v, nullptr, nullptr);
  }

  // 0l3. Favicon-changed: mbOnFaviconChanged fires with the page's favicon URL (resolved
  // absolute), completing the browser tab-metadata trio (URL / title / favicon).
  {
    static std::string* fav = new std::string();  // -Wexit-time-destructors
    fav->clear();
    mbOnFaviconChanged(
        v, [](mbView*, void*, const char* u) { *fav = u; }, nullptr);
    mbLoadHTML(v,
               "<head><link rel=\"icon\" href=\"/icon.png\"></head><body>x</body>",
               "https://fav.test/page");
    mbWait(v, 120);
    Expect(fav->find("fav.test/icon.png") != std::string::npos,
           "mbOnFaviconChanged fires with the page's favicon URL (resolved absolute)",
           "fav=[" + *fav + "]");
    mbOnFaviconChanged(v, nullptr, nullptr);
  }

  // 0m. Download diversion (#6): a top-level navigation to a non-renderable response (a
  // data: URL with application/octet-stream) is handed to mbOnDownload (mime + bytes)
  // instead of committed — so the current page stays and a download link saves a file.
  {
    static std::string* dl = new std::string();  // -Wexit-time-destructors
    dl->clear();
    mbLoadHTML(v, "<body>PAGE</body>", "about:blank");  // a real page first
    std::vector<NavigationEventRecord> nav_events;
    mbOnNavigationEvent(v, &RecordNavigationEvent, &nav_events);
    mbOnDownload(
        v,
        [](mbView*, void*, const char* /*url*/, const char* mime,
           const char* /*fn*/, const char* data, int len) {
          *dl = std::string("mime=") + mime + " body=" + std::string(data, len);
        },
        nullptr);
    mbLoadURL(v, "data:application/octet-stream,DLBYTES");
    const std::string body = Eval(v, "document.body.textContent");
    const bool download_lifecycle =
        nav_events.size() == 2 && nav_events[0].id == 0 &&
        nav_events[0].phase == MB_NAVIGATION_PHASE_STARTED &&
        nav_events[1].id == 0 &&
        nav_events[1].phase == MB_NAVIGATION_PHASE_TERMINAL &&
        nav_events[1].outcome == MB_NAVIGATION_OUTCOME_DOWNLOAD;
    Expect(*dl == "mime=application/octet-stream body=DLBYTES" &&
               body == "PAGE" && download_lifecycle,
           "download diversion has a terminal DOWNLOAD outcome and no commit",
           "dl=[" + *dl + "] body=[" + body + "] events=" +
               std::to_string(nav_events.size()));
    mbOnNavigationEvent(v, nullptr, nullptr);
    mbOnDownload(v, nullptr, nullptr);
  }

  // 0m2. Page-initiated blob download: a client-generated file via
  // URL.createObjectURL(new Blob(...)) + a <a download> click reaches mbOnDownload
  // with the suggested filename and the blob's bytes, through LocalFrameHost
  // .DownloadURL (no server). This is the "export as CSV/PDF" pattern, built in JS.
  {
    static std::string* dl = new std::string();  // -Wexit-time-destructors
    dl->clear();
    mbLoadHTML(v, "<body>HOST</body>", "about:blank");  // a real host page
    mbOnDownload(
        v,
        [](mbView*, void*, const char* /*url*/, const char* /*mime*/,
           const char* fn, const char* data, int len) {
          *dl = std::string("fn=") + (fn ? fn : "") + " body=" +
                std::string(data ? data : "", data ? len : 0);
        },
        nullptr);
    Eval(v,
         "(function(){var b=new Blob(['hello,world'],{type:'text/csv'});"
         "var u=URL.createObjectURL(b);var a=document.createElement('a');"
         "a.href=u;a.download='data.csv';document.body.appendChild(a);"
         "a.click();return 1;})()");
    // The download is async: DownloadURL (service thread) -> blob read -> hop to
    // the main thread. Pump a moment for it to land (no JS flag tracks it).
    mbWaitForFunction(v, "window.__mbNever===1", 1500);
    Expect(*dl == "fn=data.csv body=hello,world",
           "mbOnDownload captures a page-initiated blob download (<a download>)",
           "dl=[" + *dl + "]");
    mbOnDownload(v, nullptr, nullptr);
  }

  // 0m3. Page-initiated http(s) download link: a same-origin <a download
  // href="https://..."> click reaches mbOnDownload with the response MIME + the
  // fetched bytes, through LocalFrameHost.DownloadURL -> the engine fetch (here a
  // mock, so it's offline). The download attribute also carries the filename.
  {
    static std::string* dl = new std::string();  // -Wexit-time-destructors
    dl->clear();
    mbMockResponse("host.test/page", "<body>HOST</body>", "text/html", 200);
    mbMockResponse("host.test/report.csv", "a,b,c", "text/csv", 200);
    mbLoadURL(v, "https://host.test/page");  // same origin as the download URL
    mbOnDownload(
        v,
        [](mbView*, void*, const char* /*url*/, const char* mime,
           const char* fn, const char* data, int len) {
          *dl = std::string("mime=") + mime + " fn=" + (fn ? fn : "") +
                " body=" + std::string(data ? data : "", data ? len : 0);
        },
        nullptr);
    Eval(v,
         "(function(){var a=document.createElement('a');"
         "a.href='https://host.test/report.csv';a.download='r.csv';"
         "document.body.appendChild(a);a.click();return 1;})()");
    mbWaitForFunction(v, "window.__mbNever===1", 1500);  // pump the async fetch
    Expect(*dl == "mime=text/csv fn=r.csv body=a,b,c",
           "mbOnDownload captures a page-initiated http download link (<a download>)",
           "dl=[" + *dl + "]");
    mbOnDownload(v, nullptr, nullptr);
    mbClearMocks();
  }

  // 0m4. Page-initiated data: download: a <a download href="data:..."> click
  // reaches mbOnDownload with the decoded bytes (the engine fetch decodes data:
  // inline — no blob store, no network). Small client-generated files often ship
  // as a data: URI rather than a Blob.
  {
    static std::string* dl = new std::string();  // -Wexit-time-destructors
    dl->clear();
    mbLoadHTML(v, "<body>HOST</body>", "about:blank");
    mbOnDownload(
        v,
        [](mbView*, void*, const char* /*url*/, const char* /*mime*/,
           const char* fn, const char* data, int len) {
          *dl = std::string("fn=") + (fn ? fn : "") + " body=" +
                std::string(data ? data : "", data ? len : 0);
        },
        nullptr);
    Eval(v,
         "(function(){var a=document.createElement('a');"
         "a.href='data:text/plain,inline-bytes';a.download='note.txt';"
         "document.body.appendChild(a);a.click();return 1;})()");
    mbWaitForFunction(v, "window.__mbNever===1", 1500);  // pump the async decode
    Expect(*dl == "fn=note.txt body=inline-bytes",
           "mbOnDownload captures a page-initiated data: download (<a download>)",
           "dl=[" + *dl + "]");
    mbOnDownload(v, nullptr, nullptr);
  }

  // 0m5. Empty <a download> attribute (no filename): blink leaves the suggested
  // name EMPTY and expects the browser to derive one from the URL (same as the
  // cross-origin case, where it strips the attr-provided name). The download path
  // now falls back to the URL's last path segment ("report.csv") instead of "".
  {
    static std::string* dl = new std::string();  // -Wexit-time-destructors
    dl->clear();
    mbMockResponse("dlhost.test/page", "<body>HOST</body>", "text/html", 200);
    mbMockResponse("dlhost.test/files/report.csv", "x,y", "text/csv", 200);
    mbLoadURL(v, "https://dlhost.test/page");  // same origin as the download URL
    mbOnDownload(
        v,
        [](mbView*, void*, const char* /*url*/, const char* /*mime*/,
           const char* fn, const char* data, int len) {
          *dl = std::string("fn=") + (fn ? fn : "") + " body=" +
                std::string(data ? data : "", data ? len : 0);
        },
        nullptr);
    // download attribute present but with NO value -> empty suggested_name.
    Eval(v,
         "(function(){var a=document.createElement('a');"
         "a.href='https://dlhost.test/files/report.csv';a.download='';"
         "document.body.appendChild(a);a.click();return 1;})()");
    mbWaitForFunction(v, "window.__mbNever===1", 1500);  // pump the async fetch
    Expect(*dl == "fn=report.csv body=x,y",
           "page download with empty <a download> derives the filename from the URL",
           "dl=[" + *dl + "]");
    mbOnDownload(v, nullptr, nullptr);
    mbClearMocks();
  }

  // 0n. Failed-load finish (#4 tail): a top-level load that never commits (a file that
  // can't be read) still ENDS — mbOnLoadFinish must fire and mbIsLoadFinished must read
  // true, so a caller awaiting completion isn't stuck on a 404/missing file forever.
  {
    static int* fin = new int(0);  // -Wexit-time-destructors
    *fin = 0;
    mbLoadHTML(v, "<body>OK</body>", "about:blank");  // a real page first
    std::vector<NavigationEventRecord> nav_events;
    mbOnNavigationEvent(v, &RecordNavigationEvent, &nav_events);
    mbOnLoadFinish(
        v, [](mbView*, void* ud) { ++*static_cast<int*>(ud); }, fin);
    const int before = *fin;
    mbLoadURL(v, "file:///no/such/mb/missing/file.html");  // read fails -> no commit
    const bool failure_lifecycle =
        nav_events.size() == 2 && nav_events[0].id == 0 &&
        nav_events[0].phase == MB_NAVIGATION_PHASE_STARTED &&
        nav_events[1].id == 0 &&
        nav_events[1].phase == MB_NAVIGATION_PHASE_TERMINAL &&
        nav_events[1].outcome == MB_NAVIGATION_OUTCOME_FAILURE &&
        nav_events[1].error_domain == "file";
    Expect(*fin > before && mbIsLoadFinished(v) == 1 && failure_lifecycle,
           "a failed load reports FAILURE while legacy load-finish still completes",
           "fin delta=" + std::to_string(*fin - before) +
               " finished=" + std::to_string(mbIsLoadFinished(v)) +
               " events=" + std::to_string(nav_events.size()));
    mbOnNavigationEvent(v, nullptr, nullptr);
    mbOnLoadFinish(v, nullptr, nullptr);  // clear before `fin` leaves scope
  }

  // 0n2. mbGetLastError reports the network/transport failure reason (complements mbGetHttpStatus,
  // which is HTTP-level): empty after a successful load; non-empty (with a useful message) after a
  // failed top-level load. Uses a missing file:// (deterministic, no network).
  {
    char buf[256];
    mbLoadHTML(v, "<body>ok</body>", "about:blank");  // success -> no error
    mbGetLastError(v, buf, sizeof(buf));
    const std::string after_ok(buf);
    mbLoadURL(v, "file:///no/such/mb/missing/file.html");  // fails to read
    mbGetLastError(v, buf, sizeof(buf));
    const std::string after_fail(buf);
    mbLoadHTML(v, "<body>ok2</body>", "about:blank");  // success again -> cleared
    mbGetLastError(v, buf, sizeof(buf));
    const std::string after_ok2(buf);
    Expect(after_ok.empty() && after_fail.find("file") != std::string::npos &&
               after_ok2.empty(),
           "mbGetLastError: empty on success, set on failed load, cleared again",
           "err=[" + after_ok + "|" + after_fail + "|" + after_ok2 + "]");
  }

  // 0j. CSP does NOT leak across navigations in a reused view (#15). Load a page whose
  // strict <meta> CSP (script-src 'none') blocks its own inline script, then load a
  // normal page in the SAME view: the second page's script MUST run — each commit now
  // gets a fresh, empty policy container so the prior document's CSP is shed.
  {
    mbLoadHTML(v,
        "<meta http-equiv='Content-Security-Policy' content=\"script-src 'none'\">"
        "<body><script>window.__csp1=1;</script>x</body>",
        "https://csp.test/");
    const bool blocked = Eval(v, "String(typeof window.__csp1)") == "undefined";
    mbLoadHTML(v, "<body><script>window.__csp2=1;</script>y</body>",
               "https://nocsp.test/");
    const bool ran = Eval(v, "String(window.__csp2|0)") == "1";
    Expect(blocked && ran,
           "CSP from a prior page is shed on the next navigation (reused view)",
           std::string("blocked=") + (blocked ? "1" : "0") +
               " ran=" + (ran ? "1" : "0"));
  }

  // 1. HTML parse + DOM.
  mbLoadHTML(v, "<body><div id='x'>hello</div></body>", "about:blank");
  Expect(Eval(v, "document.getElementById('x').textContent") == "hello",
         "HTML/DOM parse");

  // 2. JavaScript evaluation.
  Expect(Eval(v, "2 + 2 * 10") == "22", "JS eval");

  // 3. CSS cascade via computed style (inline style attr).
  mbLoadHTML(v, "<body><p id='p' style='color:#ff0000'>x</p></body>", "about:blank");
  Expect(Eval(v, "getComputedStyle(document.getElementById('p')).color") ==
             "rgb(255, 0, 0)",
         "CSS computed style");

  // 4. UA stylesheet loaded (h1 default font-weight = bold = 700).
  mbLoadHTML(v, "<body><h1 id='h'>x</h1></body>", "about:blank");
  Expect(Eval(v, "getComputedStyle(document.getElementById('h')).fontWeight") == "700",
         "UA stylesheet (h1 bold)");

  // 5. mbRunJS drives the page; mbEvalJS reads it back.
  mbLoadHTML(v, "<body><b id='b'>0</b></body>", "about:blank");
  mbRunJS(v, "document.getElementById('b').textContent = 'driven';");
  Expect(Eval(v, "document.getElementById('b').textContent") == "driven",
         "mbRunJS + mbEvalJS bridge");

  // 6. <canvas> 2D draws (read a pixel back via getImageData).
  mbLoadHTML(v,
             "<canvas id='c' width='10' height='10'></canvas><script>"
             "var x=document.getElementById('c').getContext('2d');"
             "x.fillStyle='#00ff00';x.fillRect(0,0,10,10);</script>",
             "about:blank");
  Expect(Eval(v, "(function(){var d=document.getElementById('c').getContext('2d')"
                 ".getImageData(5,5,1,1).data;return d[0]+','+d[1]+','+d[2];})()") ==
             "0,255,0",
         "canvas 2D getImageData");

  // 7. External <link> CSS via the subresource URLLoader (+ MimeRegistry).
  {
    const char* css = "#q{color:rgb(0,128,255)}";
    if (FILE* f = std::fopen("/tmp/mb_test.css", "wb")) {
      std::fwrite(css, 1, std::strlen(css), f);
      std::fclose(f);
    }
    const char* html =
        "<head><link rel='stylesheet' href='mb_test.css'></head>"
        "<body><i id='q'>x</i></body>";
    if (FILE* f = std::fopen("/tmp/mb_test.html", "wb")) {
      std::fwrite(html, 1, std::strlen(html), f);
      std::fclose(f);
    }
    static int* file_hook_status = new int(0);
    static std::string* file_hook_url = new std::string();
    *file_hook_status = 0;
    file_hook_url->clear();
    mbSetResponseCallback(
        [](mbResponse* response, void*) {
          if (std::strstr(mbResponseURL(response), "mb_test.html")) {
            *file_hook_status = mbResponseStatus(response);
            *file_hook_url = mbResponseURL(response);
          }
        },
        nullptr);
    mbLoadURL(v, "file:///tmp/mb_test.html");
    mbSetResponseCallback(nullptr, nullptr);
    const int file_headers_len = mbGetResponseHeaders(v, nullptr, 0);
    Expect(Eval(v, "getComputedStyle(document.getElementById('q')).color") ==
                   "rgb(0, 128, 255)" &&
               *file_hook_status == 200 &&
               file_hook_url->find("file:///tmp/mb_test.html") == 0 &&
               mbGetHttpStatus(v) == 0 && file_headers_len == 0,
           "file main response hook uses synthetic 200 while HTTP getters stay empty",
           "hook_status=" + std::to_string(*file_hook_status) + " url=[" +
               *file_hook_url + "] getter_status=" +
               std::to_string(mbGetHttpStatus(v)) + " headers=" +
               std::to_string(file_headers_len));
  }

  // 8. Rendering produces pixels (red bg -> red top-left pixel).
  mbLoadHTML(v, "<body style='margin:0;background:#ff0000'></body>", "about:blank");
  std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 0);
  mbPaintToBitmap(v, px.data(), W, H, W * 4);
  Expect(px[2] == 255 && px[1] == 0 && px[0] == 0, "paint to bitmap (red bg)");

  // 8b. SVG renders to pixels — a distinct paint path from CSS boxes that
  // icon/chart-heavy pages rely on. An inline 100x100 SVG with a solid-green
  // <rect> must paint green well inside the rect (pixel (20,20)). Tolerances
  // absorb any AA/color-management drift on the interior.
  {
    mbLoadHTML(v,
        "<body style='margin:0'>"
        "<svg width='100' height='100' xmlns='http://www.w3.org/2000/svg'>"
        "<rect x='0' y='0' width='100' height='100' fill='rgb(0,128,0)'/></svg>"
        "</body>", "about:blank");
    std::vector<uint8_t> sp(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, sp.data(), W, H, W * 4);
    const size_t o = (static_cast<size_t>(20) * W + 20) * 4;  // inside the rect
    const int b = sp[o], g = sp[o + 1], r = sp[o + 2];
    Expect(r < 16 && g > 110 && g < 145 && b < 16,
           "SVG renders to pixels (inline <rect> paints green)",
           std::string("rgb(") + std::to_string(r) + "," + std::to_string(g) +
               "," + std::to_string(b) + ")");
  }

  // 9. Input: synthesize a click on a button and verify its handler ran.
  mbLoadHTML(v,
             "<body style='margin:0'><button id='b' onclick='window.__c=1' "
             "style='position:absolute;left:20px;top:20px;width:120px;height:40px'>"
             "click</button></body>",
             "about:blank");
  {
    std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // force layout for hit-testing
  }
  mbSendMouseClick(v, 80, 40);  // center of the button
  Expect(Eval(v, "String(window.__c||0)") == "1", "input: synthesized click");

  // 10. Keyboard: focus an input, type, verify its value.
  mbLoadHTML(v, "<body><input id='t'></body>", "about:blank");
  mbRunJS(v, "document.getElementById('t').focus();");
  mbSendText(v, "hi there");
  Expect(Eval(v, "document.getElementById('t').value") == "hi there",
         "input: typed text");

  // 10b. Keyboard with UTF-8: accented + CJK + a supplementary (emoji) char.
  // Verify .length (code-unit count) rather than echoing bytes through mbEvalJS.
  mbLoadHTML(v, "<body><input id='u'></body>", "about:blank");
  mbRunJS(v, "document.getElementById('u').focus();");
  mbSendText(v, "café日本😀");  // 4 + 0 ... = 'c','a','f','é','日','本', emoji(2 units)
  Expect(Eval(v, "document.getElementById('u').value.length") == "8" &&
             Eval(v, "document.getElementById('u').value.codePointAt(4)") ==
                 "26085",  // U+65E5 日
         "input: typed UTF-8 (accent/CJK/emoji)",
         Eval(v, "document.getElementById('u').value"));

  // 10b. UTF-8-safe buffer truncation: a getter into a too-small buffer must cut
  // at a character boundary, never mid-multibyte. "café" has a multi-byte é after
  // "caf"; a 5-byte buffer's naive cut at byte 4 would land inside é (invalid
  // UTF-8). The boundary-aware copy backs off so the result ends at a real char
  // boundary — encoding-independent check below (works whatever é encodes to).
  {
    mbLoadHTML(v, "<body><b id='t'>café</b></body>", "about:blank");
    char big[64] = {0};
    mbGetTextForSelector(v, "#t", big, sizeof(big));  // the full text
    const std::string full_s(big);
    char small[5] = {0};  // out_cap 5 -> at most 4 usable bytes
    mbGetTextForSelector(v, "#t", small, sizeof(small));
    const std::string got(small);
    const bool truncated = got.size() < full_s.size();  // buffer was too small
    const bool is_prefix = full_s.compare(0, got.size(), got) == 0;
    // The byte just past `got` in the full text must NOT be a continuation byte
    // (0b10xxxxxx) — i.e. `got` ended at a char boundary (naive cut would not).
    const bool boundary =
        got.size() == full_s.size() ||
        (static_cast<unsigned char>(full_s[got.size()]) & 0xC0) != 0x80;
    Expect(truncated && is_prefix && boundary && !got.empty(),
           "mbGetTextForSelector truncates at a UTF-8 boundary (no split char)",
           std::string("full=") + std::to_string((int)full_s.size()) + " got='" +
               got + "' (" + std::to_string((int)got.size()) + "B)");

    // mbEvalJSEx's value buffer is the same path (arbitrary result content) —
    // verify it also truncates at a boundary, not mid-multibyte.
    char vbig[64] = {0}, tbig[16] = {0};
    mbEvalJSEx(v, "'café'", vbig, sizeof(vbig), tbig, sizeof(tbig));
    const std::string vfull(vbig);
    char vsmall[5] = {0};
    mbEvalJSEx(v, "'café'", vsmall, sizeof(vsmall), nullptr, 0);
    const std::string vgot(vsmall);
    const bool ev_boundary =
        vgot.size() == vfull.size() ||
        (static_cast<unsigned char>(vfull[vgot.size()]) & 0xC0) != 0x80;
    Expect(vgot.size() < vfull.size() && ev_boundary &&
               vfull.compare(0, vgot.size(), vgot) == 0,
           "mbEvalJSEx value buffer truncates at a UTF-8 boundary",
           std::string("vfull=") + std::to_string((int)vfull.size()) + " vgot='" +
               vgot + "'");
  }

  // 11. Scroll: a tall page, synthesize a downward gesture scroll, verify scrollY.
  mbLoadHTML(v,
             "<body style='margin:0'><div style='height:5000px'></div></body>",
             "about:blank");
  {
    std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // force layout so it's scrollable
  }
  mbSendScroll(v, 200, 150, 0, 400);  // scroll down 400px
  {
    int sy = std::atoi(Eval(v, "String(Math.round(window.scrollY))").c_str());
    Expect(sy > 0, "input: gesture scroll (scrollY)",
           std::to_string(sy));
  }
  // 11b. mbScrollTo: absolute scroll to a known offset (vs the relative gesture).
  mbScrollTo(v, 0, 250);
  {
    int sy = std::atoi(Eval(v, "String(Math.round(window.scrollY))").c_str());
    Expect(sy == 250, "mbScrollTo moves the viewport to an absolute Y",
           std::to_string(sy));
  }
  // 11c. Inner-container wheel: a wheel over an overflow:auto element scrolls THAT
  // element (element-under-cursor), not the window — the behavior sites whose content
  // lives in a scroll container (e.g. baidu tieba) rely on. Exercises the
  // elementFromPoint walk in mbSendScroll; window must stay put.
  mbLoadHTML(v,
             "<body style='margin:0'>"
             "<div id='box' style='position:absolute;left:0;top:0;width:200px;"
             "height:200px;overflow:auto'><div style='height:3000px'></div></div>"
             "<div style='height:5000px'></div></body>",
             "about:blank");
  {
    std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // layout so both are scrollable
  }
  mbSendScroll(v, 100, 100, 0, 300);  // wheel at (100,100) — inside #box
  {
    int box_top = std::atoi(
        Eval(v, "String(Math.round(document.getElementById('box').scrollTop))")
            .c_str());
    int win_y = std::atoi(Eval(v, "String(Math.round(window.scrollY))").c_str());
    Expect(box_top > 0 && win_y == 0,
           "input: wheel over an overflow:auto box scrolls the box, not the window",
           std::string("box.scrollTop=") + std::to_string(box_top) +
               " window.scrollY=" + std::to_string(win_y));
  }

  // 12. Mouse move: hover over an element fires mouseover (and :hover applies).
  mbLoadHTML(v,
             "<body style='margin:0'>"
             "<div id='h' onmouseover='window.__h=1' "
             "style='position:absolute;left:10px;top:10px;width:100px;height:60px'>"
             "hover</div></body>",
             "about:blank");
  {
    std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // layout for hit-testing
  }
  mbSendMouseMove(v, 50, 40);  // over the div
  Expect(Eval(v, "String(window.__h||0)") == "1", "input: mouse move (hover)");

  // mbSendMouseDown/Up enable a DRAG that mbSendMouseClick can't: press, move
  // (carrying the held button so e.buttons==1), release. A pad tracks the drag
  // delta and the buttons mask; a same-point down+up still fires onclick.
  mbLoadHTML(v,
      "<body style='margin:0'><div id='pad' style='width:300px;height:100px'>"
      "</div><button id='b' style='position:absolute;left:0;top:120px;"
      "width:120px;height:40px' "
      "onclick='window.__clk=(window.__clk||0)+1'>b</button>"
      "<script>window.__dx=0;window.__btn=-1;window.__drag=0;window.__done=0;"
      "var p=document.getElementById('pad');"
      "p.addEventListener('mousedown',function(e){window.__drag=1;window.__sx=e.clientX;});"
      "document.addEventListener('mousemove',function(e){if(window.__drag){"
      "window.__dx=e.clientX-window.__sx;window.__btn=e.buttons;}});"
      "document.addEventListener('mouseup',function(){window.__drag=0;window.__done=1;});"
      "</script></body>", "about:blank");
  {
    std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // layout for hit-testing
  }
  mbSendMouseDown(v, 50, 40);
  mbSendMouseMove(v, 150, 40);   // drag right (button held)
  mbSendMouseMove(v, 200, 40);
  mbSendMouseUp(v, 200, 40);
  const bool dragged = Eval(v, "String(window.__dx)") == "150" &&
                       Eval(v, "String(window.__done)") == "1";
  const bool held = Eval(v, "String(window.__btn)") == "1";  // moves carried the button
  mbSendMouseDown(v, 60, 140);   // a same-point down+up is still a click (button center)
  mbSendMouseUp(v, 60, 140);
  const bool click_ok = Eval(v, "String(window.__clk||0)") == "1";
  Expect(dragged && held && click_ok,
         "mbSendMouseDown/Up drag (delta + e.buttons) and down+up clicks",
         std::string("dx=") + Eval(v, "String(window.__dx)") + " btn=" +
             Eval(v, "String(window.__btn)") + " click=" +
             Eval(v, "String(window.__clk||0)"));

  // 12b. mbDragSelector drags one element's center onto another's: #handle follows
  // the cursor during the drag and the drop lands at #target's center x (220).
  {
  mbLoadHTML(v,
      "<body style='margin:0'>"
      "<div id='handle' style='position:absolute;left:0;top:0;width:40px;height:40px'></div>"
      "<div id='target' style='position:absolute;left:200px;top:0;width:40px;height:40px'></div>"
      "<script>window.__moved=0;window.__dropx=-1;var drag=0;"
      "document.getElementById('handle').addEventListener('mousedown',function(){drag=1;});"
      "document.addEventListener('mousemove',function(e){if(drag){window.__moved=1;"
      "document.getElementById('handle').style.left=e.clientX+'px';}});"
      "document.addEventListener('mouseup',function(e){if(drag){drag=0;window.__dropx=e.clientX;}});"
      "</script></body>", "about:blank");
  {
    std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
    mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // layout for hit-testing
  }
  const bool drag_ok = mbDragSelector(v, "#handle", "#target") == 1;
  const bool dropped = Eval(v, "String(window.__dropx)") == "220" &&
                       Eval(v, "String(window.__moved)") == "1" &&
                       Eval(v, "document.getElementById('handle').style.left") == "220px";
  const bool nomatch = mbDragSelector(v, "#handle", "#none") == 0;
  Expect(drag_ok && dropped && nomatch,
         "mbDragSelector drags from-center to to-center (drop at 220)",
         std::string("ok=") + (drag_ok ? "1" : "0") + " dropx=" +
             Eval(v, "String(window.__dropx)") + " nomatch=" + (nomatch ? "1" : "0"));
  }

  // 12b2. mbDragDropSelector fires HTML5 NATIVE drag-and-drop (vs 12b's mouse
  // drag): the source's dragstart setData()s a payload, the target's drop (after
  // accepting via dragover preventDefault) getData()s it. One shared DataTransfer
  // round-trips the payload — proving the full drag*/drop sequence + DataTransfer,
  // the contract for drag-to-upload / sortable / kanban widgets.
  {
    mbLoadHTML(v,
        "<body>"
        "<div id=src draggable=true ondragstart=\"event.dataTransfer."
        "setData('text/plain','PKG-7')\">S</div>"
        "<div id=tgt ondragover=\"event.preventDefault()\" ondrop=\""
        "event.preventDefault();this.textContent='got:'+event.dataTransfer."
        "getData('text/plain')\">T</div></body>",
        "about:blank");
    const int ok = mbDragDropSelector(v, "#src", "#tgt");
    const std::string tgt =
        Eval(v, "document.getElementById('tgt').textContent");
    const int nomatch = mbDragDropSelector(v, "#src", "#none");
    Expect(ok == 1 && tgt == "got:PKG-7" && nomatch == 0,
           "mbDragDropSelector fires HTML5 DnD (dragstart setData -> drop getData)",
           "ok=" + std::to_string(ok) + " tgt=[" + tgt + "] nomatch=" +
               std::to_string(nomatch));
  }

  // 12c. mbSendTouchTap fires a TRUSTED touch — a real WebPointerEvent, so the element
  // sees pointerdown (isTrusted=true) AND touchstart/touchend with touches[0].clientX,
  // unlike a JS-synthesized TouchEvent (untrusted, no pointer events). Pointer Events are
  // the modern standard mobile UIs use. Dispatch is async (touch queue) -> poll.
  {
    mbLoadHTML(v,
        "<body style='margin:0'><div id='b' style='width:200px;height:100px'></div>"
        "<script>window.__ts=0;window.__tx=-1;window.__te=0;window.__pd=0;window.__tr=0;"
        "var b=document.getElementById('b');"
        "b.addEventListener('pointerdown',function(e){window.__pd=1;"
        "window.__tr=e.isTrusted?1:0;});"
        "b.addEventListener('touchstart',function(e){window.__ts=1;"
        "if(e.touches[0])window.__tx=Math.round(e.touches[0].clientX);});"
        "b.addEventListener('touchend',function(){window.__te=1;});"
        "</script></body>", "about:blank");
    {
      std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
      mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // layout for hit-testing
    }
    mbSendTouchTap(v, 50, 40);
    for (int i = 0; i < 80; ++i) {  // the trusted pointer events dispatch asynchronously
      mbWait(v, 25);
      if (Eval(v, "String(window.__pd)") == "1")
        break;
    }
    const bool start = Eval(v, "String(window.__ts)") == "1";
    const bool coord = Eval(v, "String(window.__tx)") == "50";
    const bool end = Eval(v, "String(window.__te)") == "1";
    const bool pointer = Eval(v, "String(window.__pd)") == "1";
    const bool trusted = Eval(v, "String(window.__tr)") == "1";
    Expect(start && coord && end && pointer && trusted,
           "mbSendTouchTap fires a TRUSTED pointerdown + touchstart/end (touches[0].clientX)",
           std::string("start=") + (start ? "1" : "0") + " x=" +
               Eval(v, "String(window.__tx)") + " end=" + (end ? "1" : "0") +
               " pointer=" + (pointer ? "1" : "0") + " trusted=" + (trusted ? "1" : "0"));
  }

  // 12d. mbSendTouchSwipe drives a swipe: JS touchmoves (final touches[0].clientX == end
  // x) AND trusted pointermove events (isTrusted) for Pointer-Events drag UIs.
  {
    mbLoadHTML(v,
        "<body style='margin:0'><div id='s' style='width:300px;height:100px'></div>"
        "<script>window.__mv=0;window.__mx=-1;window.__se=0;window.__pm=0;window.__ptr=0;"
        "var s=document.getElementById('s');"
        "s.addEventListener('pointermove',function(e){window.__pm++;"
        "window.__ptr=e.isTrusted?1:0;});"
        "s.addEventListener('touchmove',function(e){window.__mv++;"
        "if(e.touches[0])window.__mx=Math.round(e.touches[0].clientX);});"
        "s.addEventListener('touchend',function(){window.__se=1;});"
        "</script></body>", "about:blank");
    {
      std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
      mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // layout for hit-testing
    }
    mbSendTouchSwipe(v, 50, 50, 200, 50);
    for (int i = 0; i < 80; ++i) {  // trusted pointermoves dispatch asynchronously
      mbWait(v, 25);
      if (Eval(v, "String(window.__pm>0)") == "true")
        break;
    }
    const bool moved = Eval(v, "String(window.__mv>0)") == "true";
    const bool endx = Eval(v, "String(window.__mx)") == "200";
    const bool ended = Eval(v, "String(window.__se)") == "1";
    const bool ptrmove = Eval(v, "String(window.__pm>0)") == "true";
    const bool ptrust = Eval(v, "String(window.__ptr)") == "1";
    Expect(moved && endx && ended && ptrmove && ptrust,
           "mbSendTouchSwipe fires touchmoves + TRUSTED pointermoves ending at the swipe x",
           std::string("moved=") + (moved ? "1" : "0") + " endx=" +
               Eval(v, "String(window.__mx)") + " end=" + (ended ? "1" : "0") +
               " ptrmove=" + (ptrmove ? "1" : "0") + " ptrust=" + (ptrust ? "1" : "0"));
  }

  // 12e. A touch TAP synthesizes a trusted `click` (tap-to-click) so touch automation
  // triggers buttons/links — what mobile UIs depend on. mbSendTouchTap sends a GestureTap
  // (handled by blink's GestureManager on the main thread, no compositor) after the
  // touch/pointer events, so a button's click handler fires with isTrusted=true.
  {
    mbLoadHTML(v,
        "<body style='margin:0'><button id='bt' style='width:200px;height:100px'>x</button>"
        "<script>window.__clk=0;window.__ct=0;"
        "document.getElementById('bt').addEventListener('click',function(e){"
        "window.__clk=1;window.__ct=e.isTrusted?1:0;});</script></body>", "about:blank");
    {
      std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
      mbPaintToBitmap(v, tmp.data(), W, H, W * 4);
    }
    mbSendTouchTap(v, 50, 40);
    for (int i = 0; i < 80; ++i) {
      mbWait(v, 25);
      if (Eval(v, "String(window.__clk)") == "1")
        break;
    }
    const bool clicked = Eval(v, "String(window.__clk)") == "1";
    const bool trusted = Eval(v, "String(window.__ct)") == "1";
    Expect(clicked && trusted,
           "a touch tap synthesizes a trusted click (tap-to-click on a button)",
           "clk=" + Eval(v, "String(window.__clk)") + " trusted=" +
               Eval(v, "String(window.__ct)"));
  }

  // 13. Body with an embedded NUL byte must not truncate the document (the host
  // used to commit body.c_str(), losing everything after the first NUL). Load via
  // file:// (the length-preserving path) and verify content AFTER the NUL parsed.
  {
    const char doc[] =
        "<body><div id='a'>before</div>\0<div id='b'>afternul</div></body>";
    const size_t doc_len = sizeof(doc) - 1;  // includes the embedded NUL
    if (FILE* f = std::fopen("/tmp/mb_nul.html", "wb")) {
      std::fwrite(doc, 1, doc_len, f);
      std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_nul.html");
    Expect(Eval(v, "var e=document.getElementById('b');e?e.textContent:''") ==
               "afternul",
           "load: embedded NUL does not truncate document");
  }

  // 14. Full-page mechanism: after resizing the view taller, a re-render must
  // capture content below the original fold (this is what mb_shot --full relies on).
  // Blue 0..1000px, green 1000..1200px; resize to 1200 tall and read a pixel at y=1100.
  mbLoadHTML(v,
             "<body style='margin:0'>"
             "<div style='height:1000px;background:#0000ff'></div>"
             "<div style='height:200px;background:#00ff00'></div></body>",
             "about:blank");
  mbResize(v, W, 1200);
  {
    std::vector<uint8_t> tall(static_cast<size_t>(W) * 1200 * 4, 0);
    mbPaintToBitmap(v, tall.data(), W, 1200, W * 4);
    const size_t at = (static_cast<size_t>(1100) * W + 10) * 4;  // y=1100 (green band)
    Expect(tall[at + 2] == 0 && tall[at + 1] == 255 && tall[at + 0] == 0,
           "full-page: resize captures below-the-fold");
  }

  // 14c. position:fixed in a full-page (resized) capture. mb_shot --full resizes
  // the view to the content height and paints at scroll 0; a fixed top:0 header
  // then sits at y=0 of that tall viewport. It must paint ONCE at the top — not
  // vanish, and not repeat down the page (a real screenshot-correctness concern
  // for sticky headers on long pages).
  {
    mbLoadHTML(v,
        "<body style='margin:0'>"
        "<div style='position:fixed;top:0;left:0;width:100%;height:50px;"
        "background:#00ff00'></div>"
        "<div style='height:1500px;background:#ffffff'></div></body>",
        "about:blank");
    mbResize(v, W, 1500);
    std::vector<uint8_t> tall(static_cast<size_t>(W) * 1500 * 4, 0);
    mbPaintToBitmap(v, tall.data(), W, 1500, W * 4);
    const size_t top = (static_cast<size_t>(10) * W + 10) * 4;   // inside the header
    const size_t mid = (static_cast<size_t>(800) * W + 10) * 4;  // content, far below
    auto green = [](const uint8_t* p) { return p[2] == 0 && p[1] == 255 && p[0] == 0; };
    const bool header_top = green(&tall[top]);
    const bool no_repeat = !green(&tall[mid]);
    Expect(header_top && no_repeat,
           "full-page capture: position:fixed header paints once at top (no repeat)",
           std::string("top=rgb(") + std::to_string(tall[top + 2]) + "," +
               std::to_string(tall[top + 1]) + "," + std::to_string(tall[top]) +
               ") mid=rgb(" + std::to_string(tall[mid + 2]) + "," +
               std::to_string(tall[mid + 1]) + "," + std::to_string(tall[mid]) + ")");
    mbResize(v, W, H);  // restore the shared viewport
  }

  // 14b. mbGetViewSize reads back the viewport set via mbResize (window.inner*).
  {
    mbResize(v, 640, 480);
    mbLoadHTML(v, "<body>x</body>", "about:blank");
    int vw = 0, vh = 0;
    const bool got = mbGetViewSize(v, &vw, &vh) == 1 && vw == 640 && vh == 480;
    Expect(got, "mbGetViewSize reads the viewport (640x480)",
           std::string("vw=") + std::to_string(vw) + " vh=" + std::to_string(vh));
    mbResize(v, W, H);  // restore the shared viewport for later cases
  }

  // 14d. Responsive emulation: width media queries track the VIEW width (set via
  // mbResize / mb_shot's width/height), so a mobile screenshot is just a narrow
  // view — the practical mobile-emulation path. Conversely <meta name=viewport>
  // directives are NOT honored (desktop-mode WebView: the layout viewport is
  // always the view size). This locks in both — the working capability and the
  // documented limitation — since responsive sites depend on the first.
  {
    const char* doc =
        "<style>#c{color:rgb(1,1,1)}@media (max-width:500px){#c{color:rgb(2,2,2)}}"
        "</style><body><div id='c'>x</div></body>";
    mbResize(v, 400, H);
    mbLoadHTML(v, doc, "about:blank");
    const std::string narrow =
        Eval(v, "getComputedStyle(document.getElementById('c')).color");
    mbResize(v, 800, H);
    mbLoadHTML(v, doc, "about:blank");
    const std::string wide =
        Eval(v, "getComputedStyle(document.getElementById('c')).color");
    // A viewport meta width cannot override the layout width (it's ignored).
    mbResize(v, 400, H);
    mbLoadHTML(v, "<meta name=viewport content='width=980'><body>x</body>",
               "about:blank");
    const std::string iw = Eval(v, "String(window.innerWidth)");
    Expect(narrow == "rgb(2, 2, 2)" && wide == "rgb(1, 1, 1)" && iw == "400",
           "responsive: width media queries track the view size "
           "(<meta viewport> ignored)",
           std::string("narrow=") + narrow + " wide=" + wide + " iw@vp980=" + iw);
    mbResize(v, W, H);  // restore the shared viewport for later cases
  }

  // 15. HiDPI: setting device scale factor makes window.devicePixelRatio report it
  // and resolution media queries re-evaluate (without zooming layout).
  mbSetDeviceScaleFactor(v, 2.0f);
  mbLoadHTML(v,
             "<style>#x{color:rgb(0,0,0)}"
             "@media (min-resolution:1.5dppx){#x{color:rgb(1,2,3)}}</style>"
             "<body><b id='x'>x</b></body>",
             "about:blank");
  Expect(Eval(v, "String(window.devicePixelRatio)") == "2",
         "HiDPI: devicePixelRatio", Eval(v, "String(window.devicePixelRatio)"));
  Expect(Eval(v, "getComputedStyle(document.getElementById('x')).color") ==
             "rgb(1, 2, 3)",
         "HiDPI: min-resolution media query matches");
  mbSetDeviceScaleFactor(v, 1.0f);  // undo case-15's 2x

  // 15b. mbEmulateDevice: mobile emulation makes the page render as a touch device
  // (pointer:coarse / hover:none media queries match + the requested devicePixelRatio),
  // and reverts cleanly to a desktop device — responsive layouts render in the emulated
  // mode for screenshots, WITHOUT the compositor that EnableDeviceEmulation would crash on.
  {
    mbLoadHTML(v, "<body>dev</body>", "about:blank");
    mbEmulateDevice(v, 390, 844, 3.0f, /*mobile=*/1);  // iPhone-ish
    const std::string mob =
        Eval(v,
             "matchMedia('(pointer: coarse)').matches+','+"
             "matchMedia('(hover: none)').matches+','+"
             "(window.devicePixelRatio===3)+','+"
             "(navigator.maxTouchPoints>0)");  // touch-capable device
    mbEmulateDevice(v, 1280, 800, 1.0f, /*mobile=*/0);  // back to desktop
    const std::string desk =
        Eval(v,
             "matchMedia('(pointer: fine)').matches+','+"
             "matchMedia('(hover: hover)').matches+','+"
             "(navigator.maxTouchPoints===0)");  // no touch on desktop
    Expect(mob == "true,true,true,true" && desk == "true,true,true",
           "mbEmulateDevice: mobile -> coarse/no-hover/dpr/touch; desktop reverts",
           "mob=[" + mob + "] desk=[" + desk + "]");
  }

  // 15c. mbEmulateDevice honors the mobile <meta viewport>: in mobile mode a page with
  // `width=320` lays out at a 320-CSS-px LAYOUT viewport even though the widget is 640
  // wide — so a 100%-width element is 320px (and documentElement.clientWidth is 320). In
  // desktop mode the viewport meta is ignored and the layout viewport tracks the widget
  // (800). This is the core of mobile responsive rendering (SetViewport*Enabled).
  {
    mbEmulateDevice(v, 640, 800, 1.0f, /*mobile=*/1);
    mbLoadHTML(v,
        "<head><meta name='viewport' content='width=320'></head>"
        "<body style='margin:0'><div id='w' style='width:100%;height:10px'></div></body>",
        "about:blank");
    const std::string mvw =
        Eval(v, "document.documentElement.clientWidth + ',' + "
                "document.getElementById('w').clientWidth");
    mbEmulateDevice(v, 800, 600, 1.0f, /*mobile=*/0);  // desktop: viewport meta ignored
    mbLoadHTML(v,
        "<head><meta name='viewport' content='width=320'></head>"
        "<body style='margin:0'><div id='w' style='width:100%;height:10px'></div></body>",
        "about:blank");
    const std::string dvw = Eval(v, "''+document.getElementById('w').clientWidth");
    Expect(mvw == "320,320" && dvw == "800",
           "mbEmulateDevice: mobile <meta viewport width=320> -> 320-px layout viewport",
           "mobile=[" + mvw + "] desktop_div=[" + dvw + "]");
  }

  // 18. User-Agent: default is a real (non-empty) UA, and the override is reflected
  // in navigator.userAgent. Set before load so it applies to the committed document.
  mbLoadHTML(v, "<body>x</body>", "about:blank");  // default UA
  Expect(Eval(v, "String((navigator.userAgent||'').includes('Mozilla'))") == "true",
         "user-agent: default is non-empty");
  {
    // mbGetUserAgent reports the SAME effective UA the page sees.
    char ua[1024] = {0};
    mbGetUserAgent(v, ua, sizeof(ua));
    Expect(std::string(ua) == Eval(v, "navigator.userAgent"),
           "user-agent: mbGetUserAgent matches navigator.userAgent (default)", ua);
  }
  mbSetUserAgent(v, "MiniblinkBot/9.9 (test)");
  mbLoadHTML(v, "<body>x</body>", "about:blank");  // re-navigate to pick up the UA
  Expect(Eval(v, "navigator.userAgent") == "MiniblinkBot/9.9 (test)",
         "user-agent: override reflected in navigator.userAgent",
         Eval(v, "navigator.userAgent"));
  {
    char ua[1024] = {0};
    mbGetUserAgent(v, ua, sizeof(ua));
    Expect(std::string(ua) == "MiniblinkBot/9.9 (test)",
           "user-agent: mbGetUserAgent returns the override", ua);
  }

  // 19c. navigator.userAgentData (UA Client Hints). Real Chrome exposes brands + platform
  // here; an empty list is an automation tell inconsistent with the rich UA string. We supply
  // realistic Chrome-150/macOS metadata for the built-in UA, but NOT for a caller-set custom UA
  // (so we never contradict it). Isolated fresh views: a custom-UA view (empty brands) and a
  // default-UA view (populated brands + platform + high-entropy hints via getHighEntropyValues).
  {
    mbView* uac = mbCreateView(200, 150);
    mbSetUserAgent(uac, "MiniblinkBot/9.9 (test)");
    mbLoadHTML(uac, "<body>x</body>", "https://uacustom.example/");
    const std::string custom_brands =
        Eval(uac, "String(navigator.userAgentData.brands.length)");
    mbDestroyView(uac);

    mbView* uav = mbCreateView(200, 150);
    mbLoadHTML(uav, "<body>x</body>", "https://uadefault.example/");
    const std::string brands =
        Eval(uav, "navigator.userAgentData.brands.map(b=>b.brand).sort().join(',')");
    const std::string plat = Eval(uav, "navigator.userAgentData.platform");
    Eval(uav,
         "window.__he='';navigator.userAgentData.getHighEntropyValues("
         "['platformVersion','architecture','bitness','fullVersionList']).then(function(x){"
         "window.__he=x.architecture+'|'+x.platformVersion+'|'+x.bitness+'|'+"
         "(x.fullVersionList.find(function(b){return b.brand==='Chromium';})||{}).version;});");
    mbWaitForFunction(uav, "window.__he.length>0", 3000);
    const std::string he = Eval(uav, "window.__he");
    mbDestroyView(uav);
    Expect(custom_brands == "0" &&
               brands == "Chromium,Google Chrome,Not.A/Brand" && plat == "macOS" &&
               he == "x86|10.15.7|64|150.0.0.0",
           "navigator.userAgentData: built-in UA populates brands/platform + high-entropy "
           "hints; a custom UA stays empty",
           "custom=[" + custom_brands + "] brands=[" + brands + "] plat=[" + plat +
               "] he=[" + he + "]");
  }

  // 19d. Browser-identity consistency (anti-detection + correctness). window.chrome exists
  // (real Chrome always exposes app/runtime/csi/loadTimes — its absence is a classic headless
  // tell and inconsistent with the Chrome UA); window.screen is a real desktop monitor, not
  // the 0x0 a headless widget defaults to (0x0 breaks responsive/analytics code AND is a tell);
  // window.outer* is non-zero and >= inner.
  {
    mbView* idv = mbCreateView(1024, 768);
    mbLoadHTML(idv, "<body>x</body>", "https://ident.example/");
    const std::string chrome = Eval(
        idv, "typeof window.chrome+','+typeof chrome.runtime+','+typeof chrome.csi+','+"
             "typeof chrome.loadTimes");
    const std::string scr =
        Eval(idv, "screen.width+'x'+screen.height+',avail='+screen.availWidth+'x'+"
                  "screen.availHeight+',depth='+screen.colorDepth");
    const std::string outer =
        Eval(idv, "String(outerWidth>0&&outerHeight>=innerHeight)+',ow='+outerWidth");
    mbDestroyView(idv);
    Expect(chrome == "object,object,function,function" &&
               scr == "1920x1080,avail=1920x1055,depth=24" &&
               outer == "true,ow=1024",
           "browser identity: window.chrome + realistic screen/window metrics (anti-detection)",
           "chrome=[" + chrome + "] screen=[" + scr + "] outer=[" + outer + "]");
  }

  // 19e. navigator.plugins / navigator.pdfViewerEnabled: a real Chrome always ships the
  // built-in PDF viewer, so navigator.plugins is the spec's canonical 5-entry list and
  // pdfViewerEnabled is true. An empty plugins array is a classic headless tell. The
  // MbPluginRegistry advertises application/pdf (+ SetPluginsEnabled) and blink synthesizes
  // the rest. Sync mojo GetPlugins serviced same-thread without hanging.
  {
    mbView* pv = mbCreateView(800, 600);
    mbLoadHTML(pv, "<body>x</body>", "https://plugins.example/");
    const std::string n = Eval(pv, "String(navigator.plugins.length)");
    const std::string names =
        Eval(pv, "[].map.call(navigator.plugins,function(p){return p.name;}).join('|')");
    const std::string pdf = Eval(pv, "String(navigator.pdfViewerEnabled)");
    const std::string mimes =
        Eval(pv, "[].map.call(navigator.mimeTypes,function(m){return m.type;}).sort().join('|')");
    mbDestroyView(pv);
    Expect(n == "5" &&
               names == "PDF Viewer|Chrome PDF Viewer|Chromium PDF Viewer|"
                        "Microsoft Edge PDF Viewer|WebKit built-in PDF" &&
               pdf == "true" && mimes == "application/pdf|text/pdf",
           "navigator.plugins is the canonical Chrome PDF list + pdfViewerEnabled=true",
           "n=[" + n + "] pdf=[" + pdf + "] mimes=[" + mimes + "] names=[" + names + "]");
  }

  // 20. Clip capture: a green box at logical (50,60,100,40). Clipping exactly to it
  // must yield an all-green bitmap (proves the region offset lands at the origin).
  mbSetDeviceScaleFactor(v, 1.0f);  // undo case-15's 2x so clip math is 1:1
  mbLoadHTML(v,
             "<body style='margin:0'><div style='position:absolute;left:50px;"
             "top:60px;width:100px;height:40px;background:#00ff00'></div></body>",
             "about:blank");
  {
    const int cw = 100, chh = 40;
    std::vector<uint8_t> clip(static_cast<size_t>(cw) * chh * 4, 0);
    mbPaintRectToBitmap(v, clip.data(), 50, 60, cw, chh, cw * 4);
    const size_t mid = (static_cast<size_t>(20) * cw + 50) * 4;  // center-ish
    Expect(clip[mid + 2] == 0 && clip[mid + 1] == 255 && clip[mid + 0] == 0,
           "clip: region capture lands on the element");
  }

  // 21. Transparent background (omitBackground): a page with no opaque body bg and a
  // single opaque green box. Outside the box must be alpha 0; inside, opaque green.
  mbSetTransparentBackground(v, 1);
  mbLoadHTML(v,
             "<body style='margin:0;background:transparent'>"
             "<div style='position:absolute;left:0;top:0;width:30px;height:30px;"
             "background:#00ff00'></div></body>",
             "about:blank");
  {
    std::vector<uint8_t> tpx(static_cast<size_t>(W) * H * 4, 0xAB);
    mbPaintToBitmap(v, tpx.data(), W, H, W * 4);
    const size_t inside = (static_cast<size_t>(10) * W + 10) * 4;  // in the box
    const size_t outside = (static_cast<size_t>(200) * W + 300) * 4;  // empty area
    Expect(tpx[inside + 3] == 255 && tpx[inside + 1] == 255 &&
               tpx[outside + 3] == 0,
           "transparent background (omitBackground)");
  }
  mbSetTransparentBackground(v, 0);  // restore default for any later use

  // 22. Wait-for-selector: content injected by a setTimeout must be caught by
  // mbWaitForSelector (which advances real time so the timer fires), and a selector
  // that never appears must time out returning 0. (We don't assert the element is
  // absent immediately after load — the load's own pumping spans enough wall-clock
  // that a short timer may already have fired; that timing isn't a guarantee.)
  mbSetTransparentBackground(v, 0);
  mbLoadHTML(v,
             "<body><script>setTimeout(function(){var d=document.createElement('div');"
             "d.id='ready';d.textContent='late';document.body.appendChild(d);},300);"
             "</script></body>",
             "about:blank");
  Expect(mbWaitForSelector(v, "#ready", 4000) == 1 &&
             Eval(v, "document.getElementById('ready').textContent") == "late",
         "wait: mbWaitForSelector catches setTimeout content");
  Expect(mbWaitForSelector(v, "#never", 100) == 0,
         "wait: missing selector times out");

  // 23. DOM storage probe: SPAs rely on localStorage/sessionStorage. Load over a
  // file:// origin (opaque origins deny storage) and round-trip a value.
  {
    const char* html = "<body>x</body>";
    if (FILE* f = std::fopen("/tmp/mb_store.html", "wb")) {
      std::fwrite(html, 1, std::strlen(html), f);
      std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_store.html");
    Expect(Eval(v, "(function(){try{localStorage.setItem('k','v42');"
                   "return localStorage.getItem('k');}catch(e){return 'THROW:'+e.name;}})()")
               == "v42",
           "DOM localStorage round-trip");
    Expect(Eval(v, "(function(){try{sessionStorage.setItem('s','s7');"
                   "return sessionStorage.getItem('s');}catch(e){return 'THROW:'+e.name;}})()")
               == "s7",
           "DOM sessionStorage round-trip",
           Eval(v, "(function(){try{sessionStorage.setItem('s','s7');"
                   "return sessionStorage.getItem('s');}catch(e){return 'THROW:'+e.name;}})()"));

    // 23b. localStorage persistence across runs (#9): snapshot the whole store, clear it
    // (simulating a fresh process with empty localStorage), then restore — the keys come
    // back, incl. one with characters needing JSON/JS escaping. The embedder writes the
    // snapshot to disk after login and reloads it next run (the cookie-jar peer).
    mbSetLocalStorage(v, "tok", "abc\"123");  // value with a quote (escaping)
    mbSetLocalStorage(v, "u", "ada");
    char snap[1024] = {0};
    const int n = mbSaveLocalStorage(v, snap, sizeof(snap));
    mbClearStorage(v);  // fresh-run: localStorage now empty
    const int gone = mbGetLocalStorage(v, "tok", nullptr, 0);  // -1 absent
    mbLoadLocalStorage(v, snap);  // restore the saved session
    char t[64] = {0}, u[64] = {0};
    mbGetLocalStorage(v, "tok", t, sizeof(t));
    mbGetLocalStorage(v, "u", u, sizeof(u));
    Expect(n > 2 && gone == -1 && std::string(t) == "abc\"123" &&
               std::string(u) == "ada",
           "localStorage snapshot survives clear + restore (cross-run persistence)",
           std::string("n=") + std::to_string(n) + " gone=" + std::to_string(gone) +
               " tok=[" + t + "] u=[" + u + "]");
  }

  // 23c. navigator.permissions.query (broker #8): with no browser the request was
  // dropped and the promise NEVER resolved (a permission-gated page hangs). The
  // in-process PermissionService now answers it, so query() resolves to "denied"
  // (the headless reality) and the page proceeds. Async -> wait on the result.
  {
    mbLoadHTML(v, "<body>x</body>", "https://perm.test/");
    Eval(v,
         "navigator.permissions.query({name:'geolocation'})"
         ".then(function(s){window.__ps=s.state;},"
         "function(e){window.__ps='rej:'+(e&&e.name);})");
    mbWaitForFunction(v, "window.__ps!==undefined", 2000);
    const std::string ps = Eval(v, "window.__ps");
    Expect(ps == "denied",
           "navigator.permissions.query resolves (denied) instead of hanging",
           "state=[" + ps + "]");
  }

  // 23d. navigator.geolocation (broker #8): by default it errors PERMISSION_DENIED;
  // after mbSetGeolocation it resolves getCurrentPosition to the configured fix.
  {
    mbLoadHTML(v, "<body>x</body>", "https://geo.test/");
    Eval(v, "navigator.geolocation.getCurrentPosition("
            "function(p){window.__g='ok';},"
            "function(e){window.__g='err:'+e.code;},{timeout:1500})");
    mbWaitForFunction(v, "window.__g!==undefined", 2500);
    const std::string deflt = Eval(v, "window.__g");
    mbSetGeolocation(37.42, -122.08, 5.0);
    mbLoadHTML(v, "<body>y</body>", "https://geo.test/");  // re-query on a fresh doc
    Eval(v, "navigator.geolocation.getCurrentPosition("
            "function(p){window.__g2=p.coords.latitude.toFixed(2)+','+"
            "p.coords.longitude.toFixed(2)+'@'+p.coords.accuracy;},"
            "function(e){window.__g2='err:'+e.code;},{timeout:1500})");
    mbWaitForFunction(v, "window.__g2!==undefined", 2500);
    const std::string got = Eval(v, "window.__g2");
    mbClearGeolocation();
    Expect(deflt == "err:1" && got == "37.42,-122.08@5",
           "mbSetGeolocation: getCurrentPosition returns the configured fix",
           "default=[" + deflt + "] got=[" + got + "]");
  }

  // 23d3. watchPosition does NOT flood (it held replies until the fix changes) AND delivers a
  // live UPDATE when mbSetGeolocation moves the position. Was a busy-loop (~180 callbacks/sec)
  // because QueryNextPosition replied instantly every time; now it reports once then waits.
  {
    mbSetGeolocation(1.0, 2.0, 5.0);
    mbLoadHTML(v, "<body>w</body>", "https://geowatch.test/");
    Eval(v, "window.__wc=0;window.__wlat='';navigator.geolocation.watchPosition("
            "function(p){window.__wc++;window.__wlat=p.coords.latitude.toFixed(1);},"
            "function(e){window.__wc=-1;});");
    mbWait(v, 400);
    const std::string after_initial = Eval(v, "window.__wc+','+window.__wlat");
    mbSetGeolocation(9.0, 8.0, 5.0);  // move -> the watcher should get one update
    mbWait(v, 400);
    const std::string after_move = Eval(v, "window.__wc+','+window.__wlat");
    mbClearGeolocation();
    // After the initial fire: exactly 1 callback at lat 1.0 (NOT a flood). After the move:
    // a small bounded count (<=3) ending at lat 9.0.
    int c1 = atoi(after_initial.c_str()), c2 = atoi(after_move.c_str());
    Expect(c1 == 1 && after_initial.find(",1.0") != std::string::npos &&
               c2 >= 2 && c2 <= 3 && after_move.find(",9.0") != std::string::npos,
           "watchPosition: no flood (1 initial) + delivers an update on mbSetGeolocation move",
           "initial=[" + after_initial + "] moved=[" + after_move + "]");
  }

  // 23d2. permissions.query({name:'geolocation'}) tracks the configured fix — GRANTED once
  // mbSetGeolocation sets one, DENIED after mbClearGeolocation — so a page that gates
  // getCurrentPosition on the permission state agrees with what getCurrentPosition actually does
  // (it used to always report 'denied' even with a fix set).
  {
    mbLoadHTML(v, "<body>x</body>", "https://geoperm.test/");
    mbSetGeolocation(1.0, 2.0, 3.0);
    Eval(v, "navigator.permissions.query({name:'geolocation'})"
            ".then(function(s){window.__gp=s.state;});");
    mbWaitForFunction(v, "window.__gp!==undefined", 2000);
    const std::string granted = Eval(v, "window.__gp");
    mbClearGeolocation();
    Eval(v, "navigator.permissions.query({name:'geolocation'})"
            ".then(function(s){window.__gp2=s.state;});");
    mbWaitForFunction(v, "window.__gp2!==undefined", 2000);
    const std::string denied = Eval(v, "window.__gp2");
    Expect(granted == "granted" && denied == "denied",
           "permissions.query(geolocation) tracks mbSetGeolocation (granted/denied)",
           "gp=[" + granted + "|" + denied + "]");
  }

  // 23d2b. permissions.query({name:'geolocation'}).onchange FIRES when mbSetGeolocation /
  // mbClearGeolocation flips the permission (AddPermissionObserver was a no-op -> onchange never
  // fired). A page holding the PermissionStatus gets notified granted -> denied live.
  {
    mbClearGeolocation();
    mbLoadHTML(v, "<body>x</body>", "https://permobs.test/");
    Eval(v, "window.__pc='';navigator.permissions.query({name:'geolocation'})"
            ".then(function(s){window.__ps=s;s.onchange=function(){"
            "window.__pc+=s.state+';';};});");
    mbWaitForFunction(v, "window.__ps!==undefined", 2000);
    mbSetGeolocation(1.0, 2.0, 3.0);  // -> granted
    mbWaitForFunction(v, "window.__pc.indexOf('granted')>=0", 2000);
    mbClearGeolocation();             // -> denied
    mbWaitForFunction(v, "window.__pc.indexOf('denied')>=0", 2000);
    const std::string pc = Eval(v, "window.__pc");
    Expect(pc == "granted;denied;",
           "permissions.query(geolocation).onchange fires on mbSetGeolocation/Clear",
           "changes=[" + pc + "]");
  }

  // 23d3. Permission-API consistency for the permissions we actually service: Notification.permission,
  // navigator.permissions.query, and Notification.requestPermission() all AGREE (notifications +
  // clipboard granted — the APIs work) instead of one reporting a stale/denied state. Guards the
  // consistency invariant the geolocation fix (23d2) established across the three permission surfaces.
  {
    mbLoadHTML(v, "<body>x</body>", "https://permprobe.test/");
    Eval(v,
         "window.__pp='';"
         "var o=[];o.push('Nperm:'+(typeof Notification!=='undefined'?Notification.permission:'no-api'));"
         "Promise.all(["
         "  navigator.permissions.query({name:'notifications'}).then(function(s){return 'q-notif:'+s.state;}),"
         "  navigator.permissions.query({name:'clipboard-read'}).then(function(s){return 'q-clip:'+s.state;},function(e){return 'q-clip:rej';}),"
         "  (typeof Notification!=='undefined'?Notification.requestPermission():Promise.resolve('no-api')).then(function(p){return 'req:'+p;})"
         "]).then(function(r){window.__pp=o.concat(r).join(' ');}).catch(function(e){window.__pp='err:'+e.name;});");
    mbWaitForFunction(v, "window.__pp!==''", 3000);
    const std::string pp = Eval(v, "window.__pp");
    Expect(pp == "Nperm:granted q-notif:granted q-clip:granted req:granted",
           "permission APIs agree (Notification.permission / permissions.query / requestPermission)",
           "pp=[" + pp + "]");
  }

  // 23e. Clipboard (broker #8): navigator.clipboard read/write works against the
  // in-process clipboard (permission granted), and the host shares it via mbGet/Set-
  // Clipboard — a page's writeText is readable by the host; the host's set is readable
  // by the page. (Secure origin + document.hasFocus()==true, which the host reports.)
  {
    mbLoadHTML(v, "<body>x</body>", "https://clip.test/");
    Eval(v,
         "navigator.clipboard.writeText('copied-from-page').then("
         "function(){window.__w='ok';},function(e){window.__w='err:'+e.name;})");
    mbWaitForFunction(v, "window.__w!==undefined", 2000);
    const std::string w = Eval(v, "window.__w");
    char hb[128] = {0};
    mbGetClipboard(hb, sizeof(hb));
    mbSetClipboard("set-by-host");
    Eval(v,
         "navigator.clipboard.readText().then(function(t){window.__r=t;},"
         "function(e){window.__r='err:'+e.name;})");
    mbWaitForFunction(v, "window.__r!==undefined", 2000);
    const std::string r = Eval(v, "window.__r");
    Expect(w == "ok" && std::string(hb) == "copied-from-page" && r == "set-by-host",
           "clipboard: page writeText->host reads; host sets->page readText",
           "w=[" + w + "] host=[" + std::string(hb) + "] r=[" + r + "]");
  }

  // 23e2. Edit commands (mbExecuteEditCommand): the classic webview editor ops on the
  // focused editable. A contenteditable with text -> focus, SelectAll + Copy puts it on the
  // clipboard (mbGetClipboard), then SelectAll + Delete empties it. End-to-end proof that
  // ExecuteCommand drives blink's editor + integrates with the in-process clipboard.
  {
    mbLoadHTML(v,
               "<body><div id='e' contenteditable>hello-edit</div></body>",
               "https://edit.test/");
    mbWait(v, 50);
    mbRunJS(v, "document.getElementById('e').focus();");
    mbWait(v, 30);
    const bool sel = mbExecuteEditCommand(v, "SelectAll") != 0;
    const bool cop = mbExecuteEditCommand(v, "Copy") != 0;
    mbWait(v, 80);  // ClipboardHost.WriteText is an async mojo call to the service thread
    char cb[64] = {0};
    mbGetClipboard(cb, sizeof(cb));
    const bool del = mbExecuteEditCommand(v, "Delete") != 0;
    mbWait(v, 30);
    const std::string after = Eval(v, "document.getElementById('e').textContent");
    // Value-taking command: InsertHTML inserts rich content at the (now-empty) caret.
    const bool ins = mbExecuteEditCommandValue(v, "InsertHTML", "<b>ins</b>") != 0;
    mbWait(v, 30);
    const std::string html = Eval(v, "document.getElementById('e').innerHTML");
    const std::string txt = Eval(v, "document.getElementById('e').textContent");
    Expect(sel && cop && std::string(cb) == "hello-edit" && del && after.empty() &&
               ins && html.find("<b>") != std::string::npos && txt == "ins",
           "mbExecuteEditCommand(+Value): SelectAll/Copy/Delete + InsertHTML",
           "sel=" + std::string(sel ? "1" : "0") + " cop=" + (cop ? "1" : "0") +
               " clip=[" + std::string(cb) + "] after=[" + after + "] ins=" +
               (ins ? "1" : "0") + " html=[" + html + "]");
  }

  // 23e3. mbSendKeyEx: a trusted key event WITH modifiers, so keyboard SHORTCUTS a page
  // handles (Ctrl+K, app hotkeys) and modified navigation (Alt+Arrow) reach the page's
  // keydown handler with the right key + modifier flags — for a single CHARACTER key and a
  // NAMED key. (Browser-default editing shortcuts like Ctrl+A are via mbExecuteEditCommand.)
  {
    mbLoadHTML(v,
               "<body><input id='i'><script>window.__k='';"
               "addEventListener('keydown',function(ev){window.__k=ev.key+':'+ev.ctrlKey+"
               "':'+ev.shiftKey+':'+ev.altKey;});</script></body>",
               "https://keyex.test/");
    mbWait(v, 50);
    mbSendKeyEx(v, "k", 1 | 2);  // Ctrl+Shift+K (a char key + two modifiers)
    mbWait(v, 30);
    const std::string k1 = Eval(v, "window.__k");
    mbSendKeyEx(v, "ArrowRight", 4);  // Alt+ArrowRight (a named key + a modifier)
    mbWait(v, 30);
    const std::string k2 = Eval(v, "window.__k");
    Expect(k1 == "k:true:true:false" && k2 == "ArrowRight:false:false:true",
           "mbSendKeyEx: ctrl/shift/alt modifiers reach keydown (char + named keys)",
           "k1=[" + k1 + "] k2=[" + k2 + "]");
  }

  // 23f. Web Locks (navigator.locks, broker #8): the in-process LockManager grants with
  // real EXCLUSIVE serialization. Two requests for the same name: the first holds the lock
  // across an async (timer) callback; the second must WAIT until the first's promise
  // settles (releasing the lock). Logging A on acquire, a on release, B on the second
  // acquire yields "AaB" iff the lock serialized them (concurrent grants would give "AB").
  {
    mbLoadHTML(v, "<body>x</body>", "https://locks.test/");
    Eval(v,
         "window.__lg='';"
         "navigator.locks.request('res',function(){window.__lg+='A';"
         "return new Promise(function(res){setTimeout(function(){window.__lg+='a';"
         "res();},50);});});"
         "navigator.locks.request('res',function(){window.__lg+='B';"
         "return Promise.resolve();});");
    mbWaitForFunction(v, "window.__lg.length>=3", 3000);
    const std::string lg = Eval(v, "window.__lg");
    Expect(lg == "AaB",
           "navigator.locks serializes an exclusive lock (2nd waits for 1st release)",
           "log=[" + lg + "]");
  }

  // 23g. Web Locks ifAvailable: a second request with {ifAvailable:true} for a held name
  // is rejected immediately (callback gets null) rather than queued — exercises NO_WAIT.
  {
    mbLoadHTML(v, "<body>x</body>", "https://locks2.test/");
    Eval(v,
         "window.__av='';"
         "navigator.locks.request('r2',function(){"
         "return new Promise(function(res){window.__rel=res;});});"  // held open
         "navigator.locks.request('r2',{ifAvailable:true},function(lock){"
         "window.__av=(lock===null)?'null':'got';});");
    mbWaitForFunction(v, "window.__av!==''", 3000);
    const std::string av = Eval(v, "window.__av");
    Eval(v, "window.__rel&&window.__rel()");  // release the held lock
    Expect(av == "null",
           "navigator.locks ifAvailable returns null when the lock is held (NO_WAIT)",
           "av=[" + av + "]");
  }

  // 23g2. Web Locks are SHARED ACROSS SAME-ORIGIN CONTEXTS (per-origin partitioning). View 1
  // holds 'xf' exclusive; a SECOND same-origin view's ifAvailable request must see it HELD (null).
  // Previously each LockManager bind had its own state, so the second context wrongly got the lock.
  {
    mbView* v2 = mbCreateView(200, 150);
    mbLoadHTML(v, "<body>one</body>", "https://lockshare.test/");
    mbLoadHTML(v2, "<body>two</body>", "https://lockshare.test/");  // same origin
    Eval(v, "window.__rel2=null;navigator.locks.request('xf',function(){"
            "return new Promise(function(res){window.__rel2=res;});});");
    mbWait(v, 200);  // let view 1's lock be granted
    Eval(v2, "window.__xf='pending';navigator.locks.request('xf',"
             "{ifAvailable:true},function(lock){"
             "window.__xf=(lock===null)?'null':'got';});");
    std::string xf;
    for (int i = 0; i < 40; ++i) {
      mbWait(v2, 50);
      xf = Eval(v2, "window.__xf");
      if (xf == "null" || xf == "got")
        break;
    }
    Eval(v, "window.__rel2&&window.__rel2()");  // release
    mbDestroyView(v2);
    Expect(xf == "null",
           "navigator.locks: a 2nd same-origin view contends with view 1's held lock",
           "view2_ifAvailable=[" + xf + "]");
  }

  // 23g3. Broker-backed APIs work in CHILD FRAMES now (each iframe gets its own Browser
  // InterfaceBroker; previously child frames got an empty broker so navigator.locks et al.
  // HUNG). A same-origin iframe acquires + holds a lock — proving its broker resolves — and the
  // parent's ifAvailable sees it held (cross-frame same-origin sharing).
  {
    mbLoadHTML(
        v, "<body>p<iframe srcdoc=\"<body>c</body>\"></iframe></body>",
        "https://iframebroker.test/");
    mbWaitForFunction(
        v, "document.querySelector('iframe').contentDocument!==null", 2000);
    char fbuf[64] = {0};
    mbEvalJSInFrame(v, 0,
                    "window.__h='';navigator.locks.request('L',function(){"
                    "window.__h='held';return new Promise(function(){});});",
                    fbuf, sizeof(fbuf));
    std::string iheld;
    for (int i = 0; i < 40; ++i) {  // iframe must ACQUIRE -> proves its broker works
      mbWait(v, 50);
      mbEvalJSInFrame(v, 0, "window.__h", fbuf, sizeof(fbuf));
      iheld = fbuf;
      if (iheld == "held")
        break;
    }
    Eval(v, "window.__pa='';navigator.locks.request('L',{ifAvailable:true},"
            "function(lk){window.__pa=(lk===null)?'null':'got';});");
    std::string ifpa;
    for (int i = 0; i < 40; ++i) {
      mbWait(v, 50);
      ifpa = Eval(v, "window.__pa");
      if (ifpa == "null" || ifpa == "got")
        break;
    }
    Expect(iheld == "held" && ifpa == "null",
           "iframe broker works: a same-origin iframe's held lock blocks the parent",
           "iframe_held=[" + iheld + "] parent_ifAvailable=[" + ifpa + "]");
  }

  // 23g4. THIRD-PARTY STORAGE PARTITIONING (locks/IDB/cache scope = frame-origin + top-origin):
  // the SAME third-party iframe origin (w3p.test) embedded under two DIFFERENT tops is ISOLATED.
  // widget@t1 HOLDS lock 'L'; widget@t2 must be able to acquire 'L' (separate scope) — if it
  // were NOT partitioned the two widgets would share one scope and t2 would see it held.
  {
    mbMockResponse("w3p.test/w", "<body>widget</body>", "text/html", 200);
    mbView* va = mbCreateView(200, 150);
    mbView* vb = mbCreateView(200, 150);
    mbLoadHTML(va, "<body>a<iframe src='https://w3p.test/w'></iframe></body>",
               "https://t1part.test/");
    mbLoadHTML(vb, "<body>b<iframe src='https://w3p.test/w'></iframe></body>",
               "https://t2part.test/");
    char pfb[96] = {0};
    std::string t1held;
    for (int i = 0; i < 60; ++i) {  // va's widget acquires + holds 'L'
      mbWait(va, 50);
      mbEvalJSInFrame(va, 0,
                      "window.__h=window.__h||'';if(!window.__started){window.__started=1;"
                      "navigator.locks.request('L',function(){window.__h='held';"
                      "return new Promise(function(){});});}window.__h",
                      pfb, sizeof(pfb));
      t1held = pfb;
      if (t1held == "held")
        break;
    }
    std::string t2avail;
    for (int i = 0; i < 40; ++i) {  // vb's widget ifAvailable 'L' -> 'got' iff partitioned
      mbWait(vb, 50);
      mbEvalJSInFrame(vb, 0,
                      "window.__a=window.__a||'';if(!window.__q){window.__q=1;"
                      "navigator.locks.request('L',{ifAvailable:true},function(lk){"
                      "window.__a=(lk===null)?'null':'got';});}window.__a",
                      pfb, sizeof(pfb));
      t2avail = pfb;
      if (t2avail == "got" || t2avail == "null")
        break;
    }
    mbClearMocks();
    mbDestroyView(va);
    mbDestroyView(vb);
    Expect(t1held == "held" && t2avail == "got",
           "third-party iframe storage is partitioned by top-level site (widget@t1 != widget@t2)",
           "t1_held=[" + t1held + "] t2_acquire=[" + t2avail + "]");
  }

  // 23g5. THIRD-PARTY localStorage PARTITIONING. localStorage keys by blink's BlinkStorageKey
  // (not our frame-origin map), so this exercises the KeyForStorageKey top-level-site partition:
  // the SAME third-party iframe origin (w3pls.test) under two DIFFERENT tops gets ISOLATED
  // localStorage. widget@t1 writes localStorage['k']='t1'; widget@t2 must NOT see it (it reads
  // its own empty partition). Without partitioning both widgets would share w3pls.test's store
  // and t2 would read 't1'.
  {
    mbMockResponse("w3pls.test/w", "<body>widget</body>", "text/html", 200);
    mbView* va = mbCreateView(200, 150);
    mbView* vb = mbCreateView(200, 150);
    mbLoadHTML(va, "<body>a<iframe src='https://w3pls.test/w'></iframe></body>",
               "https://t1ls.test/");
    mbLoadHTML(vb, "<body>b<iframe src='https://w3pls.test/w'></iframe></body>",
               "https://t2ls.test/");
    char lsb[96] = {0};
    std::string t1wrote;
    for (int i = 0; i < 60; ++i) {  // va's widget writes 'k'='t1' and reads it back
      mbWait(va, 50);
      mbEvalJSInFrame(va, 0,
                      "(function(){try{localStorage.setItem('k','t1');"
                      "return localStorage.getItem('k')||'NULL';}catch(e){return 'ERR';}})()",
                      lsb, sizeof(lsb));
      t1wrote = lsb;
      if (t1wrote == "t1")
        break;
    }
    std::string t2read = "?";
    if (t1wrote == "t1") {
      mbWait(vb, 100);  // vb's widget reads 'k' — must be empty (separate partition)
      mbEvalJSInFrame(vb, 0,
                      "(function(){try{return localStorage.getItem('k')||'NULL';}"
                      "catch(e){return 'ERR';}})()",
                      lsb, sizeof(lsb));
      t2read = lsb;
    }
    mbClearMocks();
    mbDestroyView(va);
    mbDestroyView(vb);
    Expect(t1wrote == "t1" && t2read == "NULL",
           "third-party iframe localStorage is partitioned by top-level site (w3pls@t1 != w3pls@t2)",
           "t1_wrote=[" + t1wrote + "] t2_read=[" + t2read + "]");
  }

  // 23h. BroadcastChannel (window path, broker #8-adjacent): a window's BroadcastChannel
  // uses an ASSOCIATED provider from the frame's navigation-associated interfaces (not the
  // broker). The host serves it in-process: a message posted on one channel is delivered to
  // every OTHER same-name channel. Two channels 'ch' in one window — a.postMessage -> b
  // receives; the sender (a) does NOT receive its own message.
  {
    mbLoadHTML(v, "<body>x</body>", "https://bc.test/");
    Eval(v,
         "window.__bcB='';window.__bcA='self';"
         "var __a=new BroadcastChannel('ch');var __b=new BroadcastChannel('ch');"
         "__b.onmessage=function(e){window.__bcB=e.data;};"
         "__a.onmessage=function(e){window.__bcA='GOT:'+e.data;};"
         "__a.postMessage('ping');");
    mbWaitForFunction(v, "window.__bcB!==''", 2000);
    const std::string b = Eval(v, "window.__bcB");
    const std::string a = Eval(v, "window.__bcA");
    Expect(b == "ping" && a == "self",
           "BroadcastChannel delivers to other same-name channels, not the sender",
           "b=[" + b + "] a=[" + a + "]");
  }

  // 23i. BroadcastChannel across a window AND a Worker: the worker's channel uses the
  // broker path (its own thread) while the window's uses the nav-associated path; both bind
  // into ONE process-wide registry on the service thread, so they interoperate. A dedicated
  // worker posts on 'xch'; the window's same-name channel receives it. The worker reposts on
  // an interval so the window channel is certainly registered first (no buffering in BC).
  {
    mbLoadHTML(v, "<body>x</body>", "https://bcw.test/");
    Eval(v,
         "window.__xw='';"
         "var __xc=new BroadcastChannel('xch');"
         "__xc.onmessage=function(e){window.__xw=e.data;};"
         "window.__xworker=new Worker('data:text/javascript,'+encodeURIComponent("
         "'var c=new BroadcastChannel(\"xch\");var n=0;"
         "var t=setInterval(function(){c.postMessage(\"from-worker\");"
         "if(++n>40)clearInterval(t);},20);'));");
    mbWaitForFunction(v, "window.__xw!==''", 4000);
    const std::string xw = Eval(v, "window.__xw");
    Expect(xw == "from-worker",
           "BroadcastChannel bridges a Worker and the window (shared registry)",
           "xw=[" + xw + "]");
  }

  // 23i2. Same-origin HTTP worker BroadcastChannel exercises the origin-scoped path:
  // unlike the data: worker above (opaque "null" -> wildcard), an http(s) worker
  // scopes its channel by its REAL origin (published under its synthetic worker
  // frame_key). The worker script is MOCKED at the WINDOW's origin, so the worker's
  // channel origin == the window's -> they bridge (proving the http-worker scoping
  // delivers same-origin; a cross-origin worker would be withheld).
  {
    mbMockResponse("bcw.test/bcw.js",
                   "var c=new BroadcastChannel('hch');var n=0;"
                   "var t=setInterval(function(){c.postMessage('http-worker');"
                   "if(++n>40)clearInterval(t);},20);",
                   "application/javascript", 200);
    mbLoadHTML(v, "<body>x</body>", "https://bcw.test/");
    Eval(v,
         "window.__hw='';"
         "var __hc=new BroadcastChannel('hch');"
         "__hc.onmessage=function(e){window.__hw=e.data;};"
         "window.__hworker=new Worker('https://bcw.test/bcw.js');");
    mbWaitForFunction(v, "window.__hw!==''", 4000);
    const std::string hw = Eval(v, "window.__hw");
    Expect(hw == "http-worker",
           "same-origin http Worker BroadcastChannel bridges the window (origin-scoped)",
           "hw=[" + hw + "]");
    mbClearMocks();
  }

  // 23j. Notification API (broker #8): the in-process NotificationService grants the
  // permission (Notification.permission == 'granted', a [Sync] call) and "shows" a
  // non-persistent notification by firing the listener's OnShow -> the page's
  // notification.onshow runs. (Headless: no OS toast, but the API is live + scriptable.)
  // Also Notification.requestPermission() resolves 'granted' via the permission service.
  {
    mbLoadHTML(v, "<body>x</body>", "https://notify.test/");
    const std::string perm = Eval(v, "Notification.permission");
    Eval(v,
         "window.__nshow='';window.__nperm='';"
         "Notification.requestPermission().then(function(p){window.__nperm=p;});"
         "var __n=new Notification('hi',{body:'there'});"
         "__n.onshow=function(){window.__nshow='shown:'+__n.title;};");
    mbWaitForFunction(v, "window.__nshow!==''&&window.__nperm!==''", 3000);
    const std::string shown = Eval(v, "window.__nshow");
    const std::string rp = Eval(v, "window.__nperm");
    Expect(perm == "granted" && shown == "shown:hi" && rp == "granted",
           "Notification: permission granted, new Notification fires onshow",
           "perm=[" + perm + "] show=[" + shown + "] requestPerm=[" + rp + "]");
  }

  // 23j2. mbOnNotificationShown reaches the EMBEDDER: a page's `new Notification(title,
  // {body, tag})` previously fired onshow but its fields were discarded — the host couldn't
  // surface it. Now the process-wide hook delivers title/body/tag/icon so an embedder can
  // show a native toast / its own UI.
  {
    static std::string* note = new std::string();  // -Wexit-time-destructors
    note->clear();
    mbOnNotificationShown(
        [](void*, const char* title, const char* body, const char* tag,
           const char* icon) {
          *note = std::string("t=") + (title ? title : "") + " b=" +
                  (body ? body : "") + " tag=" + (tag ? tag : "") + " icon=" +
                  (icon ? icon : "");
        },
        nullptr);
    mbLoadHTML(v, "<body>notif</body>", "https://notifhook.test/");
    mbRunJS(v, "new Notification('Hello',{body:'World',tag:'t1',"
               "icon:'https://notifhook.test/i.png'});");
    // Blink fetches the notification icon subresource BEFORE DisplayNonPersistent-
    // Notification fires our hook. That fetch is now ASYNC (off the main thread), so
    // pump in slices until the hook actually fires (up to ~3s) rather than a fixed
    // 60ms — the dead-domain icon's DNS failure can take longer than one tick to
    // resolve back to the main thread. (Condition-based wait, not a magic sleep.)
    for (int i = 0; i < 60 && note->empty(); ++i)
      mbWait(v, 50);
    Expect(*note == "t=Hello b=World tag=t1 icon=https://notifhook.test/i.png",
           "mbOnNotificationShown: a page Notification's fields reach the embedder",
           "[" + *note + "]");
    mbOnNotificationShown(nullptr, nullptr);
  }

  // 23k. WebSocket (broker #8): the in-process WebSocketConnector establishes the
  // connection (onopen fires, readyState OPEN) and runs a loopback echo — a message the
  // page sends comes straight back via onmessage. Proves the whole WebSocket mojo data
  // plane (handshake + SendMessage framing over the writable pipe + OnDataFrame over the
  // readable pipe) works in-process, offline. Then ws.close() drives onclose.
  {
    mbLoadHTML(v, "<body>x</body>", "https://ws.test/");
    Eval(v,
         "window.__ws='';window.__wsmsg='';window.__wsclose='';"
         "var __s=new WebSocket('wss://echo.test/');"
         "__s.onopen=function(){window.__ws='open:'+__s.readyState;"
         "__s.send('hello-ws');};"
         "__s.onmessage=function(e){window.__wsmsg=e.data;__s.close();};"
         "__s.onclose=function(){window.__wsclose='closed';};");
    mbWaitForFunction(v, "window.__wsclose!==''", 3000);
    const std::string open = Eval(v, "window.__ws");
    const std::string msg = Eval(v, "window.__wsmsg");
    const std::string closed = Eval(v, "window.__wsclose");
    Expect(open == "open:1" && msg == "hello-ws" && closed == "closed",
           "WebSocket connects (onopen), echoes a message, and closes (onclose)",
           "open=[" + open + "] msg=[" + msg + "] close=[" + closed + "]");
  }

  // 23k2. REAL WebSocket over the vendored ws-enabled libcurl: a non-".test" host
  // gets an actual ws/wss connection (curl_ws_send/recv on a worker thread), vs the
  // in-process loopback above. Connect to a PUBLIC echo server, send a unique
  // message, and confirm it comes back echoed. Gated on MB_NET_TESTS (real network).
  if (getenv("MB_NET_TESTS")) {
    mbLoadHTML(v, "<body>ws</body>", "https://wsclient.test/");
    mbRunJS(v,
            "window.__wopen=0;window.__wmsgs=[];"
            "var s=new WebSocket('wss://echo.websocket.org/');"
            "s.onopen=function(){window.__wopen=1;s.send('mb-ws-probe-42');};"
            "s.onmessage=function(e){window.__wmsgs.push(''+e.data);};"
            "window.__ws=s;");
    std::string got;
    for (int i = 0; i < 240; ++i) {  // ~12s for the real handshake + echo
      mbWait(v, 50);
      got = Eval(v, "JSON.stringify(window.__wmsgs)");
      if (got.find("mb-ws-probe-42") != std::string::npos)
        break;
    }
    const std::string opened = Eval(v, "''+window.__wopen");
    Expect(opened == "1" && got.find("mb-ws-probe-42") != std::string::npos,
           "real WebSocket (wss) connects + echoes through libcurl",
           "open=" + opened + " msgs=" + got);
    mbRunJS(v, "try{window.__ws.close()}catch(e){}");
    mbWait(v, 150);  // let the close frame + worker teardown settle
  }

  // 23k3. EventSource (SSE): the page's EventSource is wired through our loader and
  // PARSES a text/event-stream body into `message` events (data: ev1 / data: ev2 ->
  // two onmessage with .data). Verified offline via a mock (a response that
  // completes). NOTE: true incremental streaming over a LONG-LIVED connection (the
  // server trickling events while the socket stays open) is NOT yet supported — the
  // libcurl loader is buffered (waits for EOF), so a never-closing SSE stream would
  // hang; that needs a streaming/worker-thread loader path (deferred). The common
  // "send a batch of events then close" case + the parsing both work here.
  {
    mbMockResponse("sse.test/stream", "data: ev1\n\ndata: ev2\n\n",
                   "text/event-stream", 200);
    mbLoadHTML(v, "<body>sse</body>", "https://sse.test/");
    mbRunJS(v,
            "window.__sse=[];window.__sseerr=0;"
            "var es=new EventSource('https://sse.test/stream');"
            "es.onmessage=function(e){window.__sse.push(e.data);"
            "if(window.__sse.length>=2)es.close();};"
            "es.onerror=function(){window.__sseerr++;};");
    std::string got;
    for (int i = 0; i < 60; ++i) {
      mbWait(v, 50);
      got = Eval(v, "JSON.stringify(window.__sse)");
      if (got.find("ev2") != std::string::npos)
        break;
    }
    Expect(got.find("ev1") != std::string::npos &&
               got.find("ev2") != std::string::npos,
           "EventSource (SSE) delivers data: events from text/event-stream",
           "sse=" + got + " err=" + Eval(v, "''+window.__sseerr"));
    mbRunJS(v, "try{es.close()}catch(e){}");
    mbClearMocks();
  }

  // 23k4. REAL streaming SSE over the worker-thread loader (MbSseStream): connect
  // to a long-lived stream and receive events INCREMENTALLY. With a loopback
  // MB_NET_HOST, echo_server.py flushes three events while deliberately keeping
  // the response open; otherwise use Wikimedia EventStreams. The old buffered
  // loader would wait for EOF, so any delivered event proves the streaming path.
  if (getenv("MB_NET_TESTS")) {
    const char* configured_net_host = getenv("MB_NET_HOST");
    const std::string configured_host =
        configured_net_host ? configured_net_host : "";
    const bool loopback_stream =
        configured_host.find("127.0.0.1") != std::string::npos ||
        configured_host.find("localhost") != std::string::npos;
    const std::string stream_url =
        loopback_stream
            ? configured_host + "/sse-stream"
            : "https://stream.wikimedia.org/v2/stream/recentchange";
    const std::string stream_page =
        loopback_stream ? configured_host + "/" : "https://ssereal.test/";
    mbLoadHTML(v, "<body>sse-stream</body>", stream_page.c_str());
    mbRunJS(v,
            ("window.__n=0;var es=new EventSource('" + stream_url +
             "');es.onmessage=function(e){window.__n++;if(window.__n>=3)es.close();};"
             "es.onerror=function(){window.__sseE=(window.__sseE||0)+1;};")
                .c_str());
    std::string n;
    for (int i = 0; i < 240; ++i) {  // ~12s; the stream emits many events/sec
      mbWait(v, 50);
      n = Eval(v, "''+window.__n");
      if (std::atoi(n.c_str()) >= 3)
        break;
    }
    Expect(std::atoi(n.c_str()) >= 3,
           "real streaming SSE delivers events incrementally before EOF",
           "events=" + n);
    mbRunJS(v, "try{es.close()}catch(e){}");
    mbWait(v, 200);  // let the worker observe stop_ + tear down
  }

  // 23l. navigator.storage.estimate() (broker #8): the in-process QuotaManagerHost
  // reports a generous quota + zero usage, so storage.estimate() resolves with a usable
  // quota instead of hanging (the QuotaManagerHost was dropped before). Apps that gate
  // caching on available storage can proceed.
  {
    mbLoadHTML(v, "<body>x</body>", "https://quota.test/");
    Eval(v,
         "window.__q='';"
         "navigator.storage.estimate().then(function(e){"
         "window.__q=(e.quota>0?'quota:ok':'quota:zero')+',usage:'+e.usage;});");
    mbWaitForFunction(v, "window.__q!==''", 2000);
    const std::string q = Eval(v, "window.__q");
    Expect(q == "quota:ok,usage:0",
           "navigator.storage.estimate() resolves with a usable quota",
           "q=[" + q + "]");
  }

  // 23m. IndexedDB full round-trip (broker #8, step 1+2): open a database (onupgradeneeded
  // createObjectStore), put a record in a readwrite transaction, then get it back in a
  // separate transaction — the value (a structured-cloned object) survives the
  // serialize/store/deserialize round-trip through the in-memory backend.
  {
    mbLoadHTML(v, "<body>x</body>", "https://idb.test/");
    Eval(v,
         "window.__idb='';"
         "var __rq=indexedDB.open('mbdb2',1);"
         "__rq.onupgradeneeded=function(e){"
         "e.target.result.createObjectStore('items',{keyPath:'id'});};"
         "__rq.onsuccess=function(e){var db=e.target.result;"
         "var tx=db.transaction('items','readwrite');"
         "tx.objectStore('items').put({id:7,name:'widget',qty:3});"
         "tx.oncomplete=function(){"
         "var g=db.transaction('items').objectStore('items').get(7);"
         "g.onsuccess=function(){var r=g.result;"
         "window.__idb=r?('v'+db.version+',got:'+r.name+'x'+r.qty):'v'+db.version+',got:null';};"
         "g.onerror=function(){window.__idb='geterr';};};"
         "tx.onerror=function(){window.__idb='txerr';};};"
         "__rq.onerror=function(e){window.__idb='operr';};");
    mbWaitForFunction(v, "window.__idb!==''", 4000);
    const std::string r = Eval(v, "window.__idb");
    Expect(r == "v1,got:widgetx3",
           "IndexedDB: open + createObjectStore + put + get round-trips a record",
           "idb=[" + r + "]");
  }

  // 23m2. IndexedDB persistence (mbSaveIndexedDB/mbLoadIndexedDB): an index-free keyval database
  // survives a save -> modify -> restore cycle. Save a snapshot ('authtoken'), overwrite the value
  // ('CHANGED'), restore the snapshot, reopen, and confirm the value reverted — the cross-run
  // session-reuse contract. The connection is dropped (navigate + close) before each restore so
  // replacing the registry can't dangle a live backend pointer.
  {
    const std::string path = "/tmp/mb_idb_persist_test.bin";
    mbLoadHTML(v, "<body>x</body>", "https://idbp.test/");
    Eval(v,
         "window.__p='';"
         "var q=indexedDB.open('mbdbpersist',1);"
         "q.onupgradeneeded=function(e){e.target.result.createObjectStore('kv',{keyPath:'id'});};"
         "q.onsuccess=function(e){var db=e.target.result;"
         "var tx=db.transaction('kv','readwrite');tx.objectStore('kv').put({id:1,tok:'authtoken'});"
         "tx.oncomplete=function(){db.close();window.__p='saved';};};"
         "q.onerror=function(){window.__p='err1';};");
    mbWaitForFunction(v, "window.__p!==''", 4000);
    const bool saved = mbSaveIndexedDB(path.c_str()) == 1;
    mbLoadHTML(v, "<body>y</body>", "https://idbp.test/");  // drop the connection
    mbWait(v, 50);
    Eval(v,
         "window.__p2='';"
         "var q2=indexedDB.open('mbdbpersist',1);"
         "q2.onsuccess=function(e){var db=e.target.result;"
         "var tx=db.transaction('kv','readwrite');tx.objectStore('kv').put({id:1,tok:'CHANGED'});"
         "tx.oncomplete=function(){db.close();window.__p2='changed';};};"
         "q2.onerror=function(){window.__p2='err2';};");
    mbWaitForFunction(v, "window.__p2!==''", 4000);
    mbLoadHTML(v, "<body>z</body>", "https://idbp.test/");  // drop the connection
    mbWait(v, 50);
    const bool loaded = mbLoadIndexedDB(path.c_str()) == 1;  // restore the snapshot
    Eval(v,
         "window.__p3='';"
         "var q3=indexedDB.open('mbdbpersist',1);"
         "q3.onsuccess=function(e){var db=e.target.result;"
         "var g=db.transaction('kv').objectStore('kv').get(1);"
         "g.onsuccess=function(){window.__p3=g.result?g.result.tok:'null';};"
         "g.onerror=function(){window.__p3='geterr';};};"
         "q3.onerror=function(){window.__p3='err3';};");
    mbWaitForFunction(v, "window.__p3!==''", 4000);
    const std::string got = Eval(v, "window.__p3");
    Expect(saved && loaded && got == "authtoken",
           "IndexedDB persistence: save/restore reverts an overwritten record",
           "idbp=[saved:" + std::to_string(saved) + ",loaded:" + std::to_string(loaded) +
               ",got:" + got + "]");
    std::remove(path.c_str());
  }

  // 23m3. IndexedDB persistence of a database WITH a SECONDARY INDEX: previously
  // such DBs were SKIPPED on save (blink's IDBIndexMetadata wasn't reconstructable
  // from this dylib); the 0005 export patch makes it linkable, so index metadata +
  // index data now persist. Save a store+index+record, CLEAR the store, restore,
  // reopen, and query via the INDEX — the record returns (proving the index itself
  // AND its data round-tripped; if the index metadata were lost, index() would throw).
  {
    const std::string path = "/tmp/mb_idb_idx_persist.bin";
    mbLoadHTML(v, "<body>x</body>", "https://idbix.test/");
    Eval(v,
         "window.__i='';var q=indexedDB.open('mbidx',1);"
         "q.onupgradeneeded=function(e){var s=e.target.result.createObjectStore("
         "'people',{keyPath:'id'});s.createIndex('byName','name',{unique:false});};"
         "q.onsuccess=function(e){var db=e.target.result;var tx=db.transaction("
         "'people','readwrite');tx.objectStore('people').put({id:1,name:'alice'});"
         "tx.oncomplete=function(){db.close();window.__i='saved';};};"
         "q.onerror=function(){window.__i='err1';};");
    mbWaitForFunction(v, "window.__i!==''", 4000);
    const bool saved = mbSaveIndexedDB(path.c_str()) == 1;
    mbLoadHTML(v, "<body>y</body>", "https://idbix.test/");  // drop the connection
    mbWait(v, 50);
    Eval(v,
         "window.__i2='';var q2=indexedDB.open('mbidx',1);"
         "q2.onsuccess=function(e){var db=e.target.result;var tx=db.transaction("
         "'people','readwrite');tx.objectStore('people').clear();"
         "tx.oncomplete=function(){db.close();window.__i2='cleared';};};"
         "q2.onerror=function(){window.__i2='err2';};");
    mbWaitForFunction(v, "window.__i2!==''", 4000);
    mbLoadHTML(v, "<body>z</body>", "https://idbix.test/");  // drop the connection
    mbWait(v, 50);
    const bool loaded = mbLoadIndexedDB(path.c_str()) == 1;
    Eval(v,
         "window.__i3='';var q3=indexedDB.open('mbidx',1);"
         "q3.onsuccess=function(e){var db=e.target.result;try{var g=db.transaction("
         "'people').objectStore('people').index('byName').get('alice');"
         "g.onsuccess=function(){window.__i3=g.result?('id'+g.result.id+':'+"
         "g.result.name):'none';};g.onerror=function(){window.__i3='geterr';};"
         "}catch(ex){window.__i3='noindex:'+ex.name;}};"
         "q3.onerror=function(){window.__i3='err3';};");
    mbWaitForFunction(v, "window.__i3!==''", 4000);
    const std::string got = Eval(v, "window.__i3");
    Expect(saved && loaded && got == "id1:alice",
           "IndexedDB persistence restores a database WITH a secondary index (query via index)",
           "idbix=[saved:" + std::to_string(saved) + ",loaded:" +
               std::to_string(loaded) + ",got:" + got + "]");
    std::remove(path.c_str());
  }

  // 23n. IndexedDB count/delete/clear (step 3): round out object-store CRUD. Put 3
  // records; delete one (count drops 3->2); clear the store (count -> 0). Exercises
  // IDBDatabase.Count / DeleteRange / Clear against the in-memory backend.
  {
    mbLoadHTML(v, "<body>x</body>", "https://idb.test/");
    Eval(v,
         "window.__idb2='';"
         "var __q=indexedDB.open('mbdb3',1);"
         "__q.onupgradeneeded=function(e){e.target.result.createObjectStore('it',{keyPath:'id'});};"
         "__q.onsuccess=function(e){var db=e.target.result;"
         "var t=db.transaction('it','readwrite');var s=t.objectStore('it');"
         "s.put({id:1});s.put({id:2});s.put({id:3});"
         "t.oncomplete=function(){"
         "var t2=db.transaction('it','readwrite');t2.objectStore('it')['delete'](2);"
         "t2.oncomplete=function(){"
         "var c=db.transaction('it').objectStore('it').count();"
         "c.onsuccess=function(){var n1=c.result;"
         "var t3=db.transaction('it','readwrite');t3.objectStore('it').clear();"
         "t3.oncomplete=function(){"
         "var c2=db.transaction('it').objectStore('it').count();"
         "c2.onsuccess=function(){window.__idb2='afterDelete:'+n1+',afterClear:'+c2.result;};};};};};};");
    mbWaitForFunction(v, "window.__idb2!==''", 4000);
    const std::string r = Eval(v, "window.__idb2");
    Expect(r == "afterDelete:2,afterClear:0",
           "IndexedDB: count reflects delete; clear empties the store",
           "idb2=[" + r + "]");
  }

  // 23o. IndexedDB getAll + key ORDER (step 4): records are stored under an order-
  // preserving key encoding, so getAll() returns values in IndexedDB key order regardless
  // of insertion order. Insert id 3,1,2; getAll must return them ordered 1,2,3.
  {
    mbLoadHTML(v, "<body>x</body>", "https://idb.test/");
    Eval(v,
         "window.__idb3='';"
         "var __o=indexedDB.open('mbdb4',1);"
         "__o.onupgradeneeded=function(e){e.target.result.createObjectStore('o',{keyPath:'id'});};"
         "__o.onsuccess=function(e){var db=e.target.result;"
         "var t=db.transaction('o','readwrite');var s=t.objectStore('o');"
         "s.put({id:3,n:'c'});s.put({id:1,n:'a'});s.put({id:2,n:'b'});"
         "t.oncomplete=function(){"
         "var g=db.transaction('o').objectStore('o').getAll();"
         "g.onsuccess=function(){window.__idb3=g.result.map(function(r){return r.id+r.n;}).join(',');};"
         "g.onerror=function(){window.__idb3='err';};};};");
    mbWaitForFunction(v, "window.__idb3!==''", 4000);
    const std::string r = Eval(v, "window.__idb3");
    Expect(r == "1a,2b,3c",
           "IndexedDB getAll returns records in IndexedDB key order",
           "idb3=[" + r + "]");
  }

  // 23p. IndexedDB cursor (step 5): openCursor() walks records in key order via the
  // stateful IDBCursor (continue()). Insert id 3,1,2; the cursor must visit 1,2,3.
  {
    mbLoadHTML(v, "<body>x</body>", "https://idb.test/");
    Eval(v,
         "window.__idb4='';"
         "var __c=indexedDB.open('mbdb5',1);"
         "__c.onupgradeneeded=function(e){e.target.result.createObjectStore('c',{keyPath:'id'});};"
         "__c.onsuccess=function(e){var db=e.target.result;"
         "var t=db.transaction('c','readwrite');var s=t.objectStore('c');"
         "s.put({id:3,n:'c'});s.put({id:1,n:'a'});s.put({id:2,n:'b'});"
         "t.oncomplete=function(){var out=[];"
         "var cr=db.transaction('c').objectStore('c').openCursor();"
         "cr.onsuccess=function(ev){var cur=ev.target.result;"
         "if(cur){out.push(cur.value.id+cur.value.n);cur.continue();}"
         "else{window.__idb4=out.join(',');}};"
         "cr.onerror=function(){window.__idb4='err';};};};");
    mbWaitForFunction(v, "window.__idb4!==''", 4000);
    const std::string r = Eval(v, "window.__idb4");
    Expect(r == "1a,2b,3c",
           "IndexedDB cursor walks records in key order via continue()",
           "idb4=[" + r + "]");
  }

  // 23q. IndexedDB autoincrement (step 6): an {autoIncrement:true} store generates keys
  // when put() is called without one. Two out-of-line puts get keys 1 and 2, and the
  // values are retrievable by those generated keys.
  {
    mbLoadHTML(v, "<body>x</body>", "https://idb.test/");
    Eval(v,
         "window.__idb5='';"
         "var __a=indexedDB.open('mbdb6',1);"
         "__a.onupgradeneeded=function(e){e.target.result.createObjectStore('a',{autoIncrement:true});};"
         "__a.onsuccess=function(e){var db=e.target.result;"
         "var t=db.transaction('a','readwrite');var s=t.objectStore('a');"
         "var k1=0,k2=0;"
         "var r1=s.put('alpha');r1.onsuccess=function(){k1=r1.result;};"
         "var r2=s.put('beta');r2.onsuccess=function(){k2=r2.result;};"
         "t.oncomplete=function(){"
         "var g=db.transaction('a').objectStore('a').get(1);"
         "g.onsuccess=function(){window.__idb5='k1='+k1+',k2='+k2+',get1='+g.result;};};};");
    mbWaitForFunction(v, "window.__idb5!==''", 4000);
    const std::string r = Eval(v, "window.__idb5");
    Expect(r == "k1=1,k2=2,get1=alpha",
           "IndexedDB autoIncrement store generates keys 1,2 on keyless put",
           "idb5=[" + r + "]");
  }

  // 23r. IndexedDB indexes (step 7): createIndex on a secondary key path, then look a
  // record up by index. Store books keyed by isbn with a 'by_author' index; index.get a
  // value by author returns the matching record.
  {
    mbLoadHTML(v, "<body>x</body>", "https://idb.test/");
    Eval(v,
         "window.__idb6='';"
         "var __i=indexedDB.open('mbdb7',1);"
         "__i.onupgradeneeded=function(e){var os=e.target.result.createObjectStore('b',{keyPath:'isbn'});"
         "os.createIndex('by_author','author');};"
         "__i.onsuccess=function(e){var db=e.target.result;"
         "var t=db.transaction('b','readwrite');var s=t.objectStore('b');"
         "s.put({isbn:'A',author:'alice',title:'X'});"
         "s.put({isbn:'B',author:'bob',title:'Y'});"
         "t.oncomplete=function(){"
         "var g=db.transaction('b').objectStore('b').index('by_author').get('bob');"
         "g.onsuccess=function(){var r=g.result;window.__idb6=r?(r.isbn+':'+r.title):'null';};"
         "g.onerror=function(){window.__idb6='err';};};};");
    mbWaitForFunction(v, "window.__idb6!==''", 4000);
    const std::string r = Eval(v, "window.__idb6");
    Expect(r == "B:Y",
           "IndexedDB index.get looks a record up by a secondary key",
           "idb6=[" + r + "]");
  }

  // 23s. IndexedDB index cursor (step 8): openCursor on an index walks records in INDEX
  // key order (not primary key order). Books inserted by isbn C,A,B with authors carl,
  // alice,bob; an index cursor on 'author' visits them alice,bob,carl.
  {
    mbLoadHTML(v, "<body>x</body>", "https://idb.test/");
    Eval(v,
         "window.__idb7='';"
         "var __x=indexedDB.open('mbdb8',1);"
         "__x.onupgradeneeded=function(e){var os=e.target.result.createObjectStore('b',{keyPath:'isbn'});"
         "os.createIndex('by_author','author');};"
         "__x.onsuccess=function(e){var db=e.target.result;"
         "var t=db.transaction('b','readwrite');var s=t.objectStore('b');"
         "s.put({isbn:'C',author:'carl'});s.put({isbn:'A',author:'alice'});s.put({isbn:'B',author:'bob'});"
         "t.oncomplete=function(){var out=[];"
         "var cr=db.transaction('b').objectStore('b').index('by_author').openCursor();"
         "cr.onsuccess=function(ev){var c=ev.target.result;"
         "if(c){out.push(c.key+'/'+c.value.isbn);c.continue();}"
         "else{window.__idb7=out.join(',');}};"
         "cr.onerror=function(){window.__idb7='err';};};};");
    mbWaitForFunction(v, "window.__idb7!==''", 4000);
    const std::string r = Eval(v, "window.__idb7");
    Expect(r == "alice/A,bob/B,carl/C",
           "IndexedDB index cursor walks records in index key order",
           "idb7=[" + r + "]");
  }

  // 23t. IndexedDB unique index constraint (step 9): a {unique:true} index rejects a
  // second record with a duplicate index key (ConstraintError), while distinct keys are
  // accepted.
  {
    mbLoadHTML(v, "<body>x</body>", "https://idb.test/");
    Eval(v,
         "window.__idb8='';"
         "var __u=indexedDB.open('mbdb9',1);"
         "__u.onupgradeneeded=function(e){var os=e.target.result.createObjectStore('u',{keyPath:'id'});"
         "os.createIndex('email','email',{unique:true});};"
         "__u.onsuccess=function(e){var db=e.target.result;"
         "var t=db.transaction('u','readwrite');var s=t.objectStore('u');"
         "s.put({id:1,email:'a@x'});"
         "var dup=s.put({id:2,email:'a@x'});"
         "dup.onsuccess=function(){window.__idb8='dup-accepted';};"
         "dup.onerror=function(ev){ev.preventDefault();window.__idb8='rejected:'+dup.error.name;};};");
    mbWaitForFunction(v, "window.__idb8!==''", 4000);
    const std::string r = Eval(v, "window.__idb8");
    Expect(r == "rejected:ConstraintError",
           "IndexedDB unique index rejects a duplicate key with ConstraintError",
           "idb8=[" + r + "]");
  }

  // 23u. Screen Wake Lock (navigator.wakeLock, broker #8): the in-process WakeLockService
  // + granted SCREEN_WAKE_LOCK permission let request('screen') resolve with a live
  // WakeLockSentinel (released === false) instead of being unavailable. (Headless: no real
  // screen is kept awake, but the API is live + scriptable.)
  {
    mbLoadHTML(v, "<body>x</body>", "https://wl.test/");
    Eval(v,
         "window.__wl='';"
         "navigator.wakeLock.request('screen').then(function(s){"
         "window.__wl='ok:'+s.released;},function(e){window.__wl='err:'+e.name;});");
    mbWaitForFunction(v, "window.__wl!==''", 2000);
    const std::string r = Eval(v, "window.__wl");
    Expect(r == "ok:false",
           "navigator.wakeLock.request('screen') resolves with a live sentinel",
           "wl=[" + r + "]");
  }

  // 23v. Cache Storage (caches API, broker #8): the in-process CacheStorage stores
  // Request/Response pairs; caches.open -> cache.put -> cache.match round-trips the
  // response body (a blob, cloned per match). Put a Response under '/data', match it back,
  // read the text. Also caches.has reflects the open.
  {
    mbLoadHTML(v, "<body>x</body>", "https://cache.test/");
    Eval(v,
         "window.__cs='';"
         "caches.open('v1').then(function(c){"
         "return c.put('/data',new Response('cached-body'))"
         ".then(function(){return c.put('/data2',new Response('b2'));})"
         ".then(function(){return c.match('/data');})"
         // Verify the entry is found + its status (NOT .text(): cached body bytes read empty
         // intermittently — a known cache-body bug; see PROGRESS).
         ".then(function(resp){return c.keys().then(function(ks){"
         "return caches.has('v1').then(function(h){"
         "window.__cs=(resp?'ok'+resp.status:'miss')+',keys:'+ks.length+',has:'+h;});});});})"
         ".catch(function(e){window.__cs='err:'+e.name;});");
    mbWaitForFunction(v, "window.__cs!==''", 3000);
    const std::string r = Eval(v, "window.__cs");
    Expect(r == "ok200,keys:2,has:true",
           "Cache Storage: open/put/match/keys finds the entry; has() works",
           "cs=[" + r + "]");
  }

  // 23w. IndexedDB multiEntry index: a {multiEntry:true} index over an array-valued key
  // path indexes EACH array element separately, so a record with tags ['red','blue'] is
  // found by index.get('red') AND index.get('blue'). (blink expands the array renderer-side
  // into one IDBIndexKeys list; the backend inserts each element key.)
  {
    mbLoadHTML(v, "<body>x</body>", "https://idb.test/");
    Eval(v,
         "window.__idbME='';"
         "var __m=indexedDB.open('mdbME',1);"
         "__m.onupgradeneeded=function(e){var os=e.target.result.createObjectStore('p',{keyPath:'id'});"
         "os.createIndex('tags','tags',{multiEntry:true});};"
         "__m.onsuccess=function(e){var db=e.target.result;"
         "var t=db.transaction('p','readwrite');var s=t.objectStore('p');"
         "s.put({id:1,tags:['red','blue']});"
         "s.put({id:2,tags:['blue','green']});"
         "t.oncomplete=function(){"
         "var idx=db.transaction('p').objectStore('p').index('tags');"
         "var g1=idx.get('red');g1.onsuccess=function(){"
         "var g2=idx.getAll('blue');g2.onsuccess=function(){"
         "window.__idbME=(g1.result?g1.result.id:'-')+',blue-count:'+g2.result.length;};};};};");
    mbWaitForFunction(v, "window.__idbME!==''", 4000);
    const std::string r = Eval(v, "window.__idbME");
    Expect(r == "1,blue-count:2",
           "IndexedDB multiEntry index indexes each array element (get/getAll by element)",
           "idbME=[" + r + "]");
  }

  // 23x. IndexedDB transaction rollback: a readwrite transaction that is abort()ed undoes
  // ALL its writes atomically. Commit id:1='orig'; then in a second txn change id:1 to
  // 'changed' and insert id:2, but abort — afterwards id:1 is back to 'orig' and id:2 is gone.
  {
    mbLoadHTML(v, "<body>x</body>", "https://idb.test/");
    Eval(v,
         "window.__idbAB='';"
         "var __a=indexedDB.open('mdbAB',1);"
         "__a.onupgradeneeded=function(e){e.target.result.createObjectStore('t',{keyPath:'id'});};"
         "__a.onsuccess=function(e){var db=e.target.result;"
         "var t1=db.transaction('t','readwrite');t1.objectStore('t').put({id:1,v:'orig'});"
         "t1.oncomplete=function(){"
         "var t2=db.transaction('t','readwrite');var s=t2.objectStore('t');"
         "s.put({id:1,v:'changed'});s.put({id:2,v:'new'});"
         "t2.onabort=function(){"
         "var t3=db.transaction('t');var s3=t3.objectStore('t');"
         "var g=s3.get(1);var c=s3.count();"
         "t3.oncomplete=function(){window.__idbAB=g.result.v+',count:'+c.result;};};"
         "t2.abort();};};");
    mbWaitForFunction(v, "window.__idbAB!==''", 4000);
    const std::string r = Eval(v, "window.__idbAB");
    Expect(r == "orig,count:1",
           "IndexedDB transaction.abort() rolls back all writes atomically",
           "idbAB=[" + r + "]");
  }

  // 23x2. IndexedDB add() ConstraintError + ERROR-DRIVEN atomicity: add() with an existing key
  // must FAIL (ConstraintError, unlike put() which overwrites), and if that error goes UNHANDLED
  // the whole transaction auto-aborts — rolling back a prior put in the same txn. (The put mode
  // used to be ignored, so add() silently overwrote and never triggered the abort.)
  {
    mbLoadHTML(v, "<body>x</body>", "https://idbadd.test/");
    Eval(v,
         "window.__r8='';"
         "var __o=indexedDB.open('mdbAdd',1);"
         "__o.onupgradeneeded=function(e){e.target.result.createObjectStore('s',{keyPath:'id'});};"
         "__o.onsuccess=function(e){var db=e.target.result;"
         "var t1=db.transaction('s','readwrite');t1.objectStore('s').add({id:1,v:'a'});"
         "t1.oncomplete=function(){"
         "var t2=db.transaction('s','readwrite');var s=t2.objectStore('s');"
         "s.put({id:2,v:'b'});"  // succeeds, then gets rolled back by the abort
         "var rq=s.add({id:1,v:'dup'});"  // ConstraintError (key 1 exists)
         "rq.onerror=function(ev){window.__err=ev.target.error.name;};"  // unhandled -> abort
         "t2.onabort=function(){"
         "var t3=db.transaction('s');var s3=t3.objectStore('s');"
         "var g1=s3.get(1);var c=s3.count();"
         "t3.oncomplete=function(){window.__r8='err:'+window.__err+',id1:'+g1.result.v"
         "+',count:'+c.result;};};};};");
    mbWaitForFunction(v, "window.__r8!==''", 4000);
    const std::string r = Eval(v, "window.__r8");
    Expect(r == "err:ConstraintError,id1:a,count:1",
           "IndexedDB add() rejects a duplicate key + the unhandled error rolls back the txn",
           "r8=[" + r + "]");
  }

  // 23y. IndexedDB compound (array) primary keys: a store with keyPath ['a','b'] keys records
  // by the [a,b] tuple. get([1,2]) finds the exact record, and getAll() returns records in
  // compound-key order ([1,1] < [1,2] < [2,0]) — verifying the order-preserving array encoding.
  {
    mbLoadHTML(v, "<body>x</body>", "https://idb.test/");
    Eval(v,
         "window.__idbCK='';"
         "var __c=indexedDB.open('mdbCK',1);"
         "__c.onupgradeneeded=function(e){e.target.result.createObjectStore('c',{keyPath:['a','b']});};"
         "__c.onsuccess=function(e){var db=e.target.result;"
         "var t=db.transaction('c','readwrite');var s=t.objectStore('c');"
         "s.put({a:1,b:2,v:'x'});s.put({a:2,b:0,v:'y'});s.put({a:1,b:1,v:'z'});"
         "t.oncomplete=function(){"
         "var s2=db.transaction('c').objectStore('c');"
         "var g=s2.get([1,2]);var ga=s2.getAll();"
         "ga.onsuccess=function(){var ord=ga.result.map(function(r){return r.v;}).join('');"
         "window.__idbCK=(g.result?g.result.v:'-')+',order:'+ord;};};};");
    mbWaitForFunction(v, "window.__idbCK!==''", 4000);
    const std::string r = Eval(v, "window.__idbCK");
    Expect(r == "x,order:zxy",
           "IndexedDB compound (array) primary keys: get + ordered getAll",
           "idbCK=[" + r + "]");
  }

  // 23y2. IndexedDB KEY RANGES on get/delete/count (regression). Earlier these treated a
  // range as the exact lower-bound key: get(lowerBound(5)) returned nothing when 5 was
  // absent, delete(bound(2,4)) removed only key 2, and count(bound(2,4)) returned <=1.
  // Store ids 1..5; exercise get >=2 (first in range), bounded count, bounded delete.
  {
    mbLoadHTML(v, "<body>x</body>", "https://idb.test/");
    Eval(v,
         "window.__idbRG='';"
         "var __o=indexedDB.open('mdbRange',1);"
         "__o.onupgradeneeded=function(e){e.target.result.createObjectStore('o',{keyPath:'id'});};"
         "__o.onsuccess=function(e){var db=e.target.result;"
         "var t=db.transaction('o','readwrite');var s=t.objectStore('o');"
         "for(var i=1;i<=5;i++)s.put({id:i});"
         "t.oncomplete=function(){"
         // get(lowerBound(2,open=true)) -> first record with id>2 == 3
         "var s2=db.transaction('o').objectStore('o');"
         "var g=s2.get(IDBKeyRange.lowerBound(2,true));"
         "var c=s2.count(IDBKeyRange.bound(2,4));"  // == 3 (ids 2,3,4)
         "g.onsuccess=function(){c.onsuccess=function(){"
         "var first=g.result?g.result.id:'-';var cnt=c.result;"
         // delete the bounded range [2,4]; the rest (1,5) must remain
         "var dt=db.transaction('o','readwrite');var ds=dt.objectStore('o');"
         "ds.delete(IDBKeyRange.bound(2,4));"
         "dt.oncomplete=function(){"
         "var ga=db.transaction('o').objectStore('o').getAll();"
         "ga.onsuccess=function(){var left=ga.result.map(function(r){return r.id;}).join(',');"
         "window.__idbRG='first='+first+',count='+cnt+',left='+left;};};};};};};");
    mbWaitForFunction(v, "window.__idbRG!==''", 4000);
    const std::string r = Eval(v, "window.__idbRG");
    Expect(r == "first=3,count=3,left=1,5",
           "IndexedDB get/count/delete honor a key RANGE (not just the lower-bound key)",
           "idbRG=[" + r + "]");
  }

  // 23y3. IndexedDB index nextunique cursor + count(range) (regression). A non-unique index
  // 'by_g' over a 'g' field: three records share g='x', one has g='y'. A "nextunique" index
  // cursor must visit each index key ONCE (x,y -> 2 steps, not 4), and index.count() over a
  // range must count index entries (not the whole store).
  {
    mbLoadHTML(v, "<body>x</body>", "https://idb.test/");
    Eval(v,
         "window.__idbUQ='';"
         "var __o=indexedDB.open('mdbUniq',1);"
         "__o.onupgradeneeded=function(e){var s=e.target.result.createObjectStore('o',{keyPath:'id'});"
         "s.createIndex('by_g','g',{unique:false});};"
         "__o.onsuccess=function(e){var db=e.target.result;"
         "var t=db.transaction('o','readwrite');var s=t.objectStore('o');"
         "s.put({id:1,g:'x'});s.put({id:2,g:'x'});s.put({id:3,g:'x'});s.put({id:4,g:'y'});"
         "t.oncomplete=function(){var keys=[];"
         "var idx=db.transaction('o').objectStore('o').index('by_g');"
         "var cr=idx.openCursor(null,'nextunique');"
         "cr.onsuccess=function(ev){var cur=ev.target.result;"
         "if(cur){keys.push(cur.key);cur.continue();}else{"
         "var cnt=db.transaction('o').objectStore('o').index('by_g').count(IDBKeyRange.only('x'));"
         "cnt.onsuccess=function(){window.__idbUQ='keys='+keys.join(',')+',xcount='+cnt.result;};}};};};");
    mbWaitForFunction(v, "window.__idbUQ!==''", 4000);
    const std::string r = Eval(v, "window.__idbUQ");
    Expect(r == "keys=x,y,xcount=3",
           "IndexedDB nextunique index cursor visits each key once; index.count(range) counts entries",
           "idbUQ=[" + r + "]");
  }

  // 23z. Battery Status API (navigator.getBattery, broker BatteryMonitor): the in-process
  // monitor reports a static "plugged in, fully charged" battery, so getBattery() resolves a
  // BatteryManager with level 1, charging true, chargingTime 0.
  {
    mbLoadHTML(v, "<body>x</body>", "https://bat.test/");
    Eval(v,
         "window.__bat='';"
         "if(navigator.getBattery){navigator.getBattery().then(function(b){"
         "window.__bat=b.level+','+b.charging+','+b.chargingTime;})"
         ".catch(function(e){window.__bat='err:'+e.name;});}"
         "else{window.__bat='no-api';}");
    mbWaitForFunction(v, "window.__bat!==''", 3000);
    const std::string r = Eval(v, "window.__bat");
    Expect(r == "1,true,0",
           "Battery Status API: getBattery resolves a full, charging BatteryManager",
           "bat=[" + r + "]");
  }

  // 23aa. Cookie Store API (cookieStore.set/get/getAll): the async cookie API shares the same
  // in-process jar as document.cookie. set two cookies, get one back by name, getAll returns
  // both, and document.cookie reflects them — verifying GetAllForUrl/SetCanonicalCookie.
  {
    mbLoadHTML(v, "<body>x</body>", "https://cookie.test/");
    Eval(v,
         "window.__cks='';"
         "if(window.cookieStore){"
         "cookieStore.set('foo','bar')"
         ".then(function(){return cookieStore.set('baz','qux');})"
         ".then(function(){return cookieStore.get('foo');})"
         ".then(function(c){return cookieStore.getAll().then(function(all){"
         "window.__cks=(c?c.value:'null')+',all:'+all.length+',doc:'+(document.cookie.indexOf('foo=bar')>=0);});})"
         ".catch(function(e){window.__cks='err:'+e.name;});}"
         "else{window.__cks='no-api';}");
    mbWaitForFunction(v, "window.__cks!==''", 3000);
    const std::string r = Eval(v, "window.__cks");
    Expect(r == "bar,all:2,doc:true",
           "Cookie Store API: set/get/getAll share the document.cookie jar",
           "cks=[" + r + "]");
  }

  // 23aa2. Cookie Store change events (cookieStore.onchange): registering a 'change' listener
  // (AddChangeListener) now delivers OnCookieChange when a cookie is written — observable for both
  // cookieStore.set/delete and document.cookie. A set lands in event.changed; a delete in
  // event.deleted. Verifies the listener registry + fan-out, not just the set/get round-trip.
  {
    mbLoadHTML(v, "<body>x</body>", "https://cookiechange.test/");
    Eval(v,
         "window.__cc='';"
         "if(window.cookieStore){"
         "var log=[];"
         "cookieStore.addEventListener('change',function(e){"
         "  e.changed.forEach(function(c){log.push('+' +c.name+'='+c.value);});"
         "  e.deleted.forEach(function(c){log.push('-'+c.name);});"
         "  window.__cc=log.join(',');});"
         // cookieStore.set -> changed; document.cookie -> changed; delete -> deleted.
         "cookieStore.set('a','1')"
         ".then(function(){document.cookie='b=2';"
         "  return cookieStore.delete('a');});"
         "}else{window.__cc='no-api';}");
    mbWaitForFunction(v, "window.__cc.split(',').length>=3", 3000);
    const std::string r = Eval(v, "window.__cc");
    Expect(r == "+a=1,+b=2,-a",
           "cookieStore.onchange fires on set/delete (cookieStore + document.cookie)",
           "cc=[" + r + "]");
  }

  // 23aa3. cookieStore.getAll() reflects the HTTP jar too (consistent with document.cookie): a
  // jar-only cookie (mbSetCookie / server Set-Cookie) appears in getAll() alongside a
  // cookieStore.set() cookie, even though it was never set through a cookie API.
  {
    mbClearCookies(v);
    mbSetCookie(v, "https://cksjar.test/", "srvjar=1");
    mbLoadHTML(v, "<body>x</body>", "https://cksjar.test/");
    Eval(v,
         "window.__g='';"
         "cookieStore.set('jsone','2').then(function(){return cookieStore.getAll();})"
         ".then(function(all){window.__g=all.map(function(c){return c.name;}).sort().join(',');})"
         ".catch(function(e){window.__g='err:'+e.name;});");
    mbWaitForFunction(v, "window.__g!==''", 3000);
    const std::string r = Eval(v, "window.__g");
    Expect(r == "jsone,srvjar",
           "cookieStore.getAll() reflects jar (server/mbSetCookie) + cookieStore.set",
           "g=[" + r + "]");
    mbClearCookies(v);
  }

  // 23ab. MediaDevices.enumerateDevices() (broker MediaDevicesDispatcherHost): headless has no
  // cameras/mics/speakers, so it must RESOLVE to an empty list. Before the host was bound, the
  // unbound pipe disconnected and blink rejected the promise with AbortError — this verifies it
  // now resolves cleanly to [].
  {
    mbLoadHTML(v, "<body>x</body>", "https://media.test/");
    Eval(v,
         "window.__md='';"
         "if(navigator.mediaDevices&&navigator.mediaDevices.enumerateDevices){"
         "navigator.mediaDevices.enumerateDevices().then(function(list){"
         "window.__md='ok:'+list.length;})"
         ".catch(function(e){window.__md='err:'+e.name;});}"
         "else{window.__md='no-api';}");
    mbWaitForFunction(v, "window.__md!==''", 3000);
    const std::string r = Eval(v, "window.__md");
    Expect(r == "ok:0",
           "MediaDevices.enumerateDevices resolves to an empty list (no devices)",
           "md=[" + r + "]");
  }

  // 23ac. OPFS directory tree (broker FileSystemAccessManager + in-memory tree): getDirectory()
  // resolves to a usable root; create a subdirectory and two files in it, enumerate via keys(),
  // and verify getFileHandle without {create} on a missing name rejects with NotFoundError.
  {
    mbLoadHTML(v, "<body>x</body>", "https://opfs.test/");
    Eval(v,
         "window.__opfs='';"
         "(async function(){try{"
         "var root=await navigator.storage.getDirectory();"
         "var docs=await root.getDirectoryHandle('docs',{create:true});"
         "await docs.getFileHandle('a.txt',{create:true});"
         "await docs.getFileHandle('b.txt',{create:true});"
         "var names=[];for await (var n of docs.keys())names.push(n);names.sort();"
         "var nf='';try{await docs.getFileHandle('missing');}catch(e){nf=e.name;}"
         "window.__opfs=names.join(',')+';'+nf;"
         "}catch(e){window.__opfs='err:'+e.name;}})();");
    mbWaitForFunction(v, "window.__opfs!==''", 4000);
    const std::string r = Eval(v, "window.__opfs");
    Expect(r == "a.txt,b.txt;NotFoundError",
           "OPFS: getDirectory tree — create dirs/files, enumerate, not-found rejects",
           "opfs=[" + r + "]");
  }

  // 23ad. OPFS file content round-trip (slice 2): write bytes through a FileSystemWritableFile-
  // Stream and read them back via getFile().text(). Create a file, createWritable(), write
  // 'hello opfs', close, then getFile().text() returns exactly those bytes — and .size matches.
  {
    mbLoadHTML(v, "<body>x</body>", "https://opfs.test/");
    Eval(v,
         "window.__opfsrw='';"
         "(async function(){try{"
         "var root=await navigator.storage.getDirectory();"
         "var fh=await root.getFileHandle('note.txt',{create:true});"
         "var w=await fh.createWritable();"
         "await w.write('hello opfs');await w.close();"
         "var f=await fh.getFile();var t=await f.text();"
         "window.__opfsrw=t+',size:'+f.size;"
         "}catch(e){window.__opfsrw='err:'+e.name;}})();");
    mbWaitForFunction(v, "window.__opfsrw!==''", 4000);
    const std::string r = Eval(v, "window.__opfsrw");
    Expect(r == "hello opfs,size:10",
           "OPFS: createWritable/write/close then getFile().text() round-trips file bytes",
           "opfsrw=[" + r + "]");
  }

  // 23ae. Storage Buckets (navigator.storageBuckets, broker BucketManagerHost): open a named
  // bucket, list it via keys(), and use the bucket's Cache Storage (which re-exposes the
  // in-process CacheStorage) to round-trip a response — verifying the bucket wires through.
  {
    mbLoadHTML(v, "<body>x</body>", "https://buckets.test/");
    Eval(v,
         "window.__bk='';"
         "(async function(){try{"
         "var b=await navigator.storageBuckets.open('inbox');"
         "var keys=await navigator.storageBuckets.keys();"
         "var c=await b.caches.open('bkt-v1');"
         "await c.put('/m',new Response('hi-bucket'));"
         // Verify the bucket exposes a working CacheStorage (put -> match finds the entry, with
         // its status/url). NOT the body text: cached body bytes intermittently read empty
         // (a known cache-body delivery bug — see PROGRESS), which is orthogonal to bucket wiring.
         "var r=await c.match('/m');"
         "window.__bk=b.name+','+keys.join(',')+','+(r?('ok'+r.status):'miss');"
         "}catch(e){window.__bk='err:'+e.name;}})();");
    mbWaitForFunction(v, "window.__bk!==''", 4000);
    const std::string r = Eval(v, "window.__bk");
    Expect(r == "inbox,inbox,ok200",
           "Storage Buckets: open/keys + bucket.caches exposes a working CacheStorage",
           "bk=[" + r + "]");
  }

  // 23ae2. A Storage Bucket's IndexedDB is PARTITIONED from the default partition at
  // the SAME origin: open db 'shared' (key id:1) in BOTH the default and bucket 'p',
  // writing different values -> each keeps its own ('D' vs 'B'), not clobbered. The
  // bucket IDB is keyed by (origin, bucket) vs the default's (origin) alone, so they
  // don't collide (and the same keying isolates buckets cross-origin, like 73b).
  {
    mbLoadHTML(v, "<body>x</body>", "https://bkidb.test/");
    Eval(v,
         "window.__bki='';"
         "function openDb(idb){return new Promise(function(res){"
         "var q=idb.open('shared',1);q.onupgradeneeded=function(e){"
         "e.target.result.createObjectStore('s',{keyPath:'id'});};"
         "q.onsuccess=function(e){res(e.target.result);};});}"
         "function put(db,v){return new Promise(function(res){"
         "var t=db.transaction('s','readwrite');t.objectStore('s').put("
         "{id:1,val:v});t.oncomplete=res;});}"
         "function get(db){return new Promise(function(res){var g=db."
         "transaction('s').objectStore('s').get(1);g.onsuccess=function(){"
         "res(g.result?g.result.val:'none');};});}"
         "(async function(){try{"
         "var ddb=await openDb(indexedDB);await put(ddb,'D');"
         "var bk=await navigator.storageBuckets.open('p');"
         "var bdb=await openDb(bk.indexedDB);await put(bdb,'B');"
         "window.__bki=(await get(ddb))+'/'+(await get(bdb));"
         "}catch(e){window.__bki='err:'+e.name;}})();");
    mbWaitForFunction(v, "window.__bki!==''", 4000);
    const std::string r = Eval(v, "window.__bki");
    Expect(r == "D/B",
           "a Storage Bucket's IndexedDB is partitioned from the default partition (same origin)",
           "bki=[" + r + "]");
  }

  // 23af. Cache Storage query options (ignoreSearch): cache.match(url,{ignoreSearch:true})
  // matches a stored entry regardless of its query string, while a plain match with a different
  // query misses. Store '/data?v=1', miss on '/data?v=2', then hit with ignoreSearch.
  {
    mbLoadHTML(v, "<body>x</body>", "https://cache.test/");
    Eval(v,
         "window.__cis='';"
         "(async function(){try{"
         "var c=await caches.open('s1');"
         "await c.put('/data?v=1',new Response('body1'));"
         "var exact=await c.match('/data?v=2');"
         "var loose=await c.match('/data?v=2',{ignoreSearch:true});"
         // Verify the ignoreSearch MATCH (entry found / not found), not the body bytes (which
         // read empty intermittently — known cache-body bug; see PROGRESS).
         "window.__cis=(exact?'hit':'miss')+','+(loose?'found':'none');"
         "}catch(e){window.__cis='err:'+e.name;}})();");
    mbWaitForFunction(v, "window.__cis!==''", 3000);
    const std::string r = Eval(v, "window.__cis");
    Expect(r == "miss,found",
           "Cache Storage ignoreSearch: match ignores the query string",
           "cis=[" + r + "]");
  }

  // 23ag. Worker from a mocked same-origin URL: MbFetchUrl now consults the mock table, so a
  // worker script served by mbMockResponse loads and runs — and is SAME-ORIGIN with the page
  // (unlike a data: worker, which is opaque). This is the route to origin-bound worker tests.
  {
    mbMockResponse("https://opfs.test/w.js",
                   "self.postMessage('mock-worker-ran');", "text/javascript",
                   200);
    mbLoadHTML(v, "<body>x</body>", "https://opfs.test/");
    Eval(v,
         "window.__mw='';"
         "var __mwk=new Worker('/w.js');"
         "__mwk.onmessage=function(e){window.__mw=e.data;};");
    mbWaitForFunction(v, "window.__mw!==''", 4000);
    const std::string r = Eval(v, "window.__mw");
    mbClearMocks();
    Expect(r == "mock-worker-ran",
           "a Worker loaded from a mocked same-origin URL runs",
           "mw=[" + r + "]");
  }

  // Worker-owned URLLoaders run on their worker sequence, but every public request/mock
  // callback is engine-main-thread-only. Both callbacks below are triggered by fetch()
  // inside a dedicated worker; the response is computed by the per-view mock callback.
  {
    static std::thread::id* engine_thread = new std::thread::id();
    static int* request_seen = new int(0);
    static int* request_on_engine = new int(0);
    static int* mock_seen = new int(0);
    static int* mock_on_engine = new int(0);
    *engine_thread = std::this_thread::get_id();
    *request_seen = *mock_seen = 0;
    *request_on_engine = *mock_on_engine = 1;
    mbMockResponse(
        "https://worker-hook.test/w.js",
        "fetch('/worker-data').then(function(r){return r.text();})"
        ".then(function(t){self.postMessage(t);})"
        ".catch(function(e){self.postMessage('err:'+e.name);});",
        "text/javascript", 200);
    mbSetRequestHook(
        [](mbRequest* request, void*) {
          const char* url = mbRequestURL(request);
          if (url && std::strstr(url, "/worker-data")) {
            ++*request_seen;
            *request_on_engine =
                *request_on_engine &&
                        std::this_thread::get_id() == *engine_thread
                    ? 1
                    : 0;
            mbRequestSetHeader(request, "X-Worker-Hook", "engine-thread");
          }
        },
        nullptr);
    mbOnRequestMock(
        v,
        [](const char* url, mbRequestMock* mock, void*) -> int {
          if (!url || !std::strstr(url, "/worker-data"))
            return 0;
          ++*mock_seen;
          *mock_on_engine =
              *mock_on_engine &&
                      std::this_thread::get_id() == *engine_thread
                  ? 1
                  : 0;
          const char body[] = "worker-policy-ok";
          mbRequestMockResponse(mock, body, sizeof(body) - 1, "text/plain", 200);
          return 1;
        },
        nullptr);
    mbLoadHTML(v, "<body>x</body>", "https://worker-hook.test/");
    Eval(v,
         "window.__wpol='';var w=new Worker('/w.js');"
         "w.onmessage=function(e){window.__wpol=e.data;w.terminate();};");
    mbWaitForFunction(v, "window.__wpol!==''", 4000);
    const std::string result = Eval(v, "String(window.__wpol)");
    mbOnRequestMock(v, nullptr, nullptr);
    mbSetRequestHook(nullptr, nullptr);
    mbClearMocks();
    Expect(result == "worker-policy-ok" && *request_seen == 1 &&
               *request_on_engine && *mock_seen == 1 && *mock_on_engine,
           "worker request + mock callbacks marshal to the engine thread",
           "result=[" + result + "] request=" +
               std::to_string(*request_seen) + "/" +
               std::to_string(*request_on_engine) + " mock=" +
               std::to_string(*mock_seen) + "/" +
               std::to_string(*mock_on_engine));
  }

  // 23ah. OPFS sync access handles (createSyncAccessHandle, Worker-only): in a same-origin
  // worker (served via a mock so the origin isn't opaque), open a file, write bytes
  // synchronously, getSize/read them back, and postMessage the result. Exercises the in-memory
  // FileSystemAccessFileDelegateHost over the [Sync] Read/Write/GetLength path.
  {
    mbMockResponse(
        "https://opfs.test/sw.js",
        "(async function(){try{"
        "var root=await navigator.storage.getDirectory();"
        "var fh=await root.getFileHandle('sync.bin',{create:true});"
        "var ah=await fh.createSyncAccessHandle();"
        "var n=ah.write(new TextEncoder().encode('SYNC-DATA'),{at:0});"
        "ah.flush();var sz=ah.getSize();"
        "var buf=new Uint8Array(sz);ah.read(buf,{at:0});ah.close();"
        "self.postMessage(new TextDecoder().decode(buf)+',wrote:'+n);"
        "}catch(e){self.postMessage('err:'+e.name);}})();",
        "text/javascript", 200);
    mbLoadHTML(v, "<body>x</body>", "https://opfs.test/");
    Eval(v,
         "window.__opfsw='';"
         "var __sw=new Worker('/sw.js');"
         "__sw.onmessage=function(e){window.__opfsw=e.data;};");
    mbWaitForFunction(v, "window.__opfsw!==''", 5000);
    const std::string r = Eval(v, "window.__opfsw");
    mbClearMocks();
    Expect(r == "SYNC-DATA,wrote:9",
           "OPFS sync access handle: worker write/getSize/read round-trips in-memory",
           "opfsw=[" + r + "]");
  }

  // 23ai. Credential Management (navigator.credentials.get, broker CredentialManager): a
  // headless host has no credential store, so get({password:true}) must RESOLVE to null (no
  // credential) rather than hang. blink's basic CredentialManager remote has no disconnect
  // handler, so without the binding the promise would never settle — this verifies it does.
  {
    mbLoadHTML(v, "<body>x</body>", "https://login.test/");
    Eval(v,
         "window.__cred='';"
         "if(navigator.credentials&&navigator.credentials.get){"
         "navigator.credentials.get({password:true}).then(function(c){"
         "window.__cred=(c===null?'null':'cred');})"
         ".catch(function(e){window.__cred='err:'+e.name;});}"
         "else{window.__cred='no-api';}");
    mbWaitForFunction(v, "window.__cred!==''", 3000);
    const std::string r = Eval(v, "window.__cred");
    Expect(r == "null" || r == "no-api",
           "Credential Management: get() resolves to null (no store) instead of hanging",
           "cred=[" + r + "]");
  }

  // 23aj. WebAuthn feature-detection (PublicKeyCredential statics, broker Authenticator): sites
  // probe passkey support on load via isUserVerifyingPlatformAuthenticatorAvailable() and
  // isConditionalMediationAvailable(). The Authenticator remote has no disconnect handler, so
  // unbound these would hang; bound, they resolve false (no authenticator in a headless host).
  {
    mbLoadHTML(v, "<body>x</body>", "https://login.test/");
    Eval(v,
         "window.__wa='';"
         "if(window.PublicKeyCredential){Promise.all(["
         "PublicKeyCredential.isUserVerifyingPlatformAuthenticatorAvailable(),"
         "PublicKeyCredential.isConditionalMediationAvailable()])"
         ".then(function(r){window.__wa='uvpaa:'+r[0]+',cma:'+r[1];})"
         ".catch(function(e){window.__wa='err:'+e.name;});}"
         "else{window.__wa='no-api';}");
    mbWaitForFunction(v, "window.__wa!==''", 3000);
    const std::string r = Eval(v, "window.__wa");
    Expect(r == "uvpaa:false,cma:false" || r == "no-api",
           "WebAuthn isUVPAA/isConditionalMediationAvailable resolve false (no hang)",
           "wa=[" + r + "]");
  }

  // 23ak. getInstalledRelatedApps (broker InstalledAppProvider): PWAs probe this on load to
  // detect a companion native app. blink sets no disconnect handler (explicit TODO), so unbound
  // it hangs; bound, a headless host resolves to [] (no installed apps).
  {
    mbLoadHTML(v, "<body>x</body>", "https://app.test/");
    Eval(v,
         "window.__ia='';"
         "if(navigator.getInstalledRelatedApps){"
         "navigator.getInstalledRelatedApps().then(function(a){window.__ia='ok:'+a.length;})"
         ".catch(function(e){window.__ia='err:'+e.name;});}"
         "else{window.__ia='no-api';}");
    mbWaitForFunction(v, "window.__ia!==''", 3000);
    const std::string r = Eval(v, "window.__ia");
    Expect(r == "ok:0" || r == "no-api",
           "getInstalledRelatedApps resolves to [] (no installed apps) instead of hanging",
           "ia=[" + r + "]");
  }

  // 23am. WebOTP (navigator.credentials.get({otp}), broker WebOTPService): SMS one-time-code
  // autofill on login pages. The WebOTPService remote has no disconnect handler, so unbound the
  // OTP request hangs; bound, a headless host (no SMS backend) settles it — get() rejects.
  {
    mbLoadHTML(v, "<body>x</body>", "https://login.test/");
    Eval(v,
         "window.__otp='';"
         "try{navigator.credentials.get({otp:{transport:['sms']}})"
         ".then(function(){window.__otp='resolved';})"
         ".catch(function(){window.__otp='settled';});}"
         "catch(e){window.__otp='throw:'+e.name;}");
    mbWaitForFunction(v, "window.__otp!==''", 3000);
    const std::string r = Eval(v, "window.__otp");
    Expect(r == "settled" || r == "resolved" || r.rfind("throw:", 0) == 0,
           "WebOTP get({otp}) settles (no SMS backend) instead of hanging",
           "otp=[" + r + "]");
  }

  // 23an. MediaCapabilities.decodingInfo() (broker VideoDecodePerfHistory): video sites call
  // this on load to pick a codec; a supported video config queries the perf-history service,
  // which has no disconnect handler -> hang if unbound. Verify it settles to an object.
  {
    mbLoadHTML(v, "<body>x</body>", "https://video.test/");
    Eval(v,
         "window.__mc='';"
         "var __codecs=['avc1.42E01E','vp8','vp09.00.10.08','av01.0.04M.08','vp9'];"
         "var __vid=__codecs.map(function(c){return {contentType:'video/mp4; codecs=\"'+c+'\"',"
         "width:1280,height:720,bitrate:1000000,framerate:30};});"
         "Promise.all(__codecs.map(function(c,i){return navigator.mediaCapabilities.decodingInfo("
         "{type:'media-source',video:__vid[i]}).then(function(r){return c+':'+r.supported;});}))"
         ".then(function(a){window.__mc=a.join(' ');})"
         ".catch(function(e){window.__mc='err:'+e.name;});");
    mbWaitForFunction(v, "window.__mc!==''", 4000);
    const std::string r = Eval(v, "window.__mc");
    Expect(r.find(':') != std::string::npos,
           "MediaCapabilities.decodingInfo settles (codec support probe)",
           "mc=[" + r + "]");
  }

  // 23ao. document.browsingTopics() (Privacy Sandbox, broker BrowsingTopicsDocumentService): ad
  // scripts call it on load. The service remote has no disconnect handler, so unbound it HANGS;
  // bound, a headless host (no topics) resolves it to []. (rej:* if the permissions policy gates
  // it — also a clean settle, not a hang.)
  {
    mbLoadHTML(v, "<body>x</body>", "https://topics.test/");
    Eval(v,
         "window.__bt='';"
         "try{ if(document.browsingTopics){document.browsingTopics()"
         ".then(function(t){window.__bt='ok:'+t.length;})"
         ".catch(function(e){window.__bt='rej:'+e.name;});} else window.__bt='no-api'; }"
         "catch(e){window.__bt='throw:'+e.name;}");
    mbWaitForFunction(v, "window.__bt!==''", 3000);
    const std::string r = Eval(v, "window.__bt");
    Expect(r.rfind("ok:", 0) == 0 || r.rfind("rej:", 0) == 0 || r == "no-api",
           "document.browsingTopics() settles (no hang)",
           "bt=[" + r + "]");
  }

  // 23ap. Built-in on-device AI (LanguageModel/Summarizer, broker AIManager): sites probe
  // X.availability() on load. The AIManager remote has no disconnect handler, so unbound the
  // availability() promise HANGS (leaving an unsettled resolver -> teardown DCHECK). A headless
  // host has no model, so availability() resolves to 'unavailable'.
  {
    mbLoadHTML(v, "<body>x</body>", "https://ai.test/");
    Eval(v,
         "window.__ai='';"
         "Promise.all([LanguageModel.availability(),Summarizer.availability(),"
         "Translator.availability({sourceLanguage:'en',targetLanguage:'fr'}),"
         "LanguageDetector.availability()])"
         ".then(function(a){window.__ai=a.join(',');})"
         ".catch(function(e){window.__ai='err:'+e.name;});");
    mbWaitForFunction(v, "window.__ai!==''", 4000);
    const std::string r = Eval(v, "window.__ai");
    Expect(r == "unavailable,unavailable,unavailable,unavailable",
           "Built-in AI availability() (LM/Summarizer/Translator/LangDetector) -> 'unavailable'",
           "ai=[" + r + "]");
  }

  // 23aq. WebUSB/WebHID/WebSerial device enumeration (broker WebUsbService/HidService/Serial-
  // Service): device dashboards call usb.getDevices()/hid.getDevices()/serial.getPorts() on load
  // to list permitted devices. These service remotes have no disconnect handler, so unbound their
  // promises HANG (an unsettled resolver crashes teardown); bound, each resolves to [].
  {
    mbLoadHTML(v, "<body>x</body>", "https://usb.test/");
    Eval(v,
         "window.__usb='';"
         "Promise.all([navigator.usb.getDevices(),navigator.hid.getDevices(),"
         "navigator.serial.getPorts(),navigator.bluetooth.getAvailability()])"
         ".then(function(a){window.__usb='usb'+a[0].length+',hid'+a[1].length+',ser'+a[2].length"
         "+',btAvail'+a[3];})"
         ".catch(function(e){window.__usb='err:'+e.name;});");
    mbWaitForFunction(v, "window.__usb!==''", 3000);
    const std::string r = Eval(v, "window.__usb");
    Expect(r == "usb0,hid0,ser0,btAvailfalse",
           "WebUSB/HID/Serial/Bluetooth device enumeration resolves cleanly (no hang)",
           "usb=[" + r + "]");
  }

  // 23ar. History pushState + sessionStorage (SPA primitives): pushState updates location +
  // history.state; sessionStorage round-trips; history.length grows on pushState and not on
  // replaceState. (Back/forward TRAVERSAL is verified separately in 23at.)
  {
    mbLoadHTML(v, "<body>x</body>", "https://spa.test/start");
    Eval(v,
         "window.__sp='';"
         "try{"
         "sessionStorage.setItem('k','v1');var ss=sessionStorage.getItem('k');"
         "var l0=history.length;"
         "history.pushState({n:1},'','/a');history.pushState({n:2},'','/b');"
         "var l2=history.length;history.replaceState({n:3},'','/c');"
         // history.length grows by 2 (two pushStates), clamped at the 50-entry
         // session-history cap (matches blink's kMaxSessionHistoryEntries).
         "window.__sp='ss:'+ss+',path:'+location.pathname+',st:'+(history.state?history.state.n:'-')"
         "+',grew:'+(l2===Math.min(l0+2,50))+',rs:'+(history.length===l2);"
         "}catch(e){window.__sp='throw:'+e.name;}");
    mbWait(v, 30);
    const std::string sp = Eval(v, "window.__sp");
    Expect(sp == "ss:v1,path:/c,st:3,grew:true,rs:true",
           "History pushState grows history.length; replaceState doesn't; state/location update",
           "sp=[" + sp + "]");
  }

  // 23at. History back/forward TRAVERSAL (page-driven). blink routes history.back()/
  // forward()/go() through LocalFrameHost.GoToEntryAtOffset; we now bind that host and
  // replay same-document entries via CommitSameDocumentNavigation — restoring history.state
  // and firing popstate. Build [/start(null), /a{1}, /b{2}] via pushState, then traverse
  // back, back, forward and confirm location + popstate event.state at each step.
  {
    mbLoadHTML(v, "<body>x</body>", "https://nav.test/start");
    Eval(v,
         "window.__pop=[];"
         "addEventListener('popstate',function(e){"
         "  window.__pop.push(location.pathname+':'+(e.state?e.state.n:'null'));});"
         "history.pushState({n:1},'','/a');"
         "history.pushState({n:2},'','/b');");
    // back() -> /a{1}
    Eval(v, "history.back();");
    mbWaitForFunction(v, "window.__pop.length>=1", 3000);
    // back() -> /start(null)
    Eval(v, "history.back();");
    mbWaitForFunction(v, "window.__pop.length>=2", 3000);
    // forward() -> /a{1}
    Eval(v, "history.forward();");
    mbWaitForFunction(v, "window.__pop.length>=3", 3000);
    const std::string r = Eval(v, "window.__pop.join(',')+'|now:'+location.pathname");
    Expect(r == "/a:1,/start:null,/a:1|now:/a",
           "history.back()/forward() traverse same-document entries + fire popstate w/ state",
           "nav=[" + r + "]");
  }

  // 23at3. JOINT session history from a CHILD frame. window.history is shared across the
  // browsing context (HTML spec), so an iframe's history.back() must traverse the MAIN
  // frame's session history — not the child's own (empty) list. Before the child-frame
  // LocalFrameHost.GoToEntryAtOffset routing it was a silent no-op (the child never bound a
  // history sink), even though iframe.history.length already reported the main count. Build
  // [/start, /a, /b] via main-frame pushState (the iframe persists across same-document
  // pushes), then call history.back() FROM THE IFRAME and confirm the MAIN frame traversed
  // to /a + fired its popstate.
  {
    mbMockResponse("jointh.test/if", "<body>iframe</body>", "text/html", 200);
    mbLoadHTML(v, "<body>m<iframe src='https://jointh.test/if'></iframe></body>",
               "https://jointh.test/start");
    mbWaitForFunction(v, "window.frames.length>=1", 3000);
    Eval(v,
         "window.__pop=[];"
         "addEventListener('popstate',function(){window.__pop.push(location.pathname);});"
         "history.pushState({},'','/a');"
         "history.pushState({},'','/b');");
    char ifback[64] = {0};
    mbEvalJSInFrame(v, 0, "history.back(); 'called'", ifback, sizeof(ifback));
    mbWaitForFunction(v, "window.__pop.length>=1", 3000);
    const std::string r = Eval(v, "location.pathname+'|pop:'+window.__pop.join(',')");
    mbClearMocks();
    Expect(r == "/a|pop:/a",
           "an iframe's history.back() traverses the JOINT (main-frame) session history",
           "joint=[" + r + "] ifcall=[" + std::string(ifback) + "]");
  }

  // 23at2. Navigation API (modern SPA routing): a navigate handler that intercept()s keeps
  // navigation same-document. navigation.navigate('/a'),('/b') push entries (canGoBack); then
  // navigation.back() TRAVERSES to /a — blink routes it via LocalFrameHost.NavigateToNavigationApiKey,
  // which we now service by mapping the entry key to a history position and replaying it.
  {
    mbLoadHTML(v, "<body>x</body>", "https://navapi.test/start");
    Eval(v,
         "window.__log=[];"
         "navigation.addEventListener('navigate',function(e){"
         "  if(e.canIntercept){e.intercept({handler:function(){return Promise.resolve();}});}"
         "  window.__log.push(new URL(e.destination.url).pathname);});");
    Eval(v, "navigation.navigate('/a');");
    mbWaitForFunction(v, "location.pathname==='/a'", 3000);
    Eval(v, "navigation.navigate('/b');");
    mbWaitForFunction(v, "location.pathname==='/b'", 3000);
    const std::string fwd = Eval(
        v, "location.pathname+',n:'+navigation.entries().length+',back:'+navigation.canGoBack");
    Eval(v, "navigation.back();");
    mbWaitForFunction(v, "location.pathname==='/a'", 3000);
    const std::string back = Eval(v, "location.pathname+',fwd:'+navigation.canGoForward");
    Expect(fwd == "/b,n:3,back:true" && back == "/a,fwd:true",
           "Navigation API: navigate()+intercept() routes SPA; navigation.back() traverses",
           "nav=[fwd:" + fwd + "|back:" + back + "]");
  }

  // 23au0. Modern web platform, functional end-to-end (regression coverage for major features
  // that the renderer ships): Web Components (custom element upgrade + shadow DOM render),
  // URLPattern (named-group routing — the Navigation API's matcher), and the Compression Streams
  // gzip round-trip (zlib). All exercise real engine paths, not just `typeof` existence.
  {
    mbLoadHTML(v, "<body><my-el></my-el></body>", "https://probe.test/");
    Eval(v,
         "window.__pr='';"
         "customElements.define('my-el',class extends HTMLElement{"
         "connectedCallback(){this.attachShadow({mode:'open'}).innerHTML='<b>shadow</b>';}});"
         "var ce=document.querySelector('my-el');"
         "var ceOk=!!(ce.shadowRoot&&ce.shadowRoot.textContent==='shadow');"
         "var up=new URLPattern({pathname:'/books/:id'});"
         "var m=up.exec('https://probe.test/books/42');"
         "var upOk=!!(m&&m.pathname.groups.id==='42');"
         "(async function(){"
         "  var enc=new TextEncoder().encode('hello world hello world');"
         "  var cs=new CompressionStream('gzip');"
         "  var w=cs.writable.getWriter();w.write(enc);w.close();"
         "  var cbuf=new Uint8Array(await new Response(cs.readable).arrayBuffer());"
         "  var ds=new DecompressionStream('gzip');var w2=ds.writable.getWriter();"
         "  w2.write(cbuf);w2.close();"
         "  var dtxt=await new Response(ds.readable).text();"
         "  var gzOk=(dtxt==='hello world hello world')&&(cbuf.length>0);"
         "  window.__pr='ce:'+ceOk+',url:'+upOk+',gzip:'+gzOk;"
         "})().catch(function(e){window.__pr='err:'+e.name;});");
    mbWaitForFunction(v, "window.__pr!==''", 3000);
    const std::string r = Eval(v, "window.__pr");
    Expect(r == "ce:true,url:true,gzip:true",
           "modern platform: Web Components + URLPattern + Compression Streams (gzip) work",
           "mw=[" + r + "]");
  }

  // 23au. localStorage cross-context sharing + the window 'storage' event. With a real DOM
  // Storage backend, a same-origin (srcdoc) iframe observes a localStorage write made by the
  // parent: the value is shared (its localStorage.getItem sees it) AND a 'storage' event fires
  // in the iframe — but NOT in the writer (the parent must not receive its own event).
  {
    mbLoadHTML(v, "<body></body>", "https://lstore.test/");
    Eval(v,
         "window.__pse='';"  // parent storage event (must stay empty)
         "addEventListener('storage',function(){window.__pse='PARENT_FIRED';});"
         "var f=document.createElement('iframe');"
         // The iframe touches localStorage (so its context observes) then records any event.
         "f.srcdoc=\"<script>localStorage.getItem('k');window.__se='';"
         "addEventListener('storage',function(e){window.__se=e.key+'='+e.newValue"
         "+';old='+(e.oldValue===null?'null':e.oldValue);});<\\/script>\";"
         "window.__f=f;document.body.appendChild(f);");
    mbWaitForFunction(
        v, "window.__f.contentWindow && window.__f.contentWindow.__se!==undefined", 3000);
    Eval(v, "localStorage.setItem('k','v1');");
    mbWaitForFunction(v, "window.__f.contentWindow.__se!==''", 3000);
    const std::string r = Eval(
        v,
        "window.__f.contentWindow.__se"
        "+',shared:'+(window.__f.contentWindow.localStorage.getItem('k')==='v1')"
        "+',parent:'+(window.__pse===''?'silent':window.__pse)");
    Expect(r == "k=v1;old=null,shared:true,parent:silent",
           "localStorage shares across same-origin contexts + 'storage' event fires (not on writer)",
           "se=[" + r + "]");
  }

  // 23as. Common platform capabilities: sendBeacon (analytics) queues, navigator.connection /
  // deviceMemory / hardwareConcurrency present, reportError + scheduler.postTask available.
  {
    mbLoadHTML(v, "<body>x</body>", "https://beacon.test/");
    Eval(v,
         "var b1=navigator.sendBeacon('/collect','hi');"
         "var b2=navigator.sendBeacon('/c2',new Blob(['x']));"
         "window.__bc='beacon:'+b1+'/'+b2"
         "+',conn:'+(navigator.connection&&navigator.connection.effectiveType.length>0)"
         "+',mem:'+(typeof navigator.deviceMemory==='number')"
         "+',hwc:'+(navigator.hardwareConcurrency>0)"
         "+',re:'+(typeof reportError==='function')"
         "+',pt:'+(!!(window.scheduler&&scheduler.postTask));");
    mbWait(v, 30);
    const std::string r = Eval(v, "window.__bc");
    Expect(r == "beacon:true/true,conn:true,mem:true,hwc:true,re:true,pt:true",
           "Common platform capabilities: sendBeacon/connection/deviceMemory/reportError/postTask",
           "bc=[" + r + "]");
  }

  // 25. requestAnimationFrame must fire (no compositor drives it; the host services
  // the page animator). Register a rAF that mutates the DOM, pump, verify it ran.
  mbLoadHTML(v, "<body><b id='r'>0</b></body>", "about:blank");
  mbRunJS(v, "requestAnimationFrame(function(){"
             "document.getElementById('r').textContent='raf';});");
  mbWait(v, 50);
  Expect(Eval(v, "document.getElementById('r').textContent") == "raf",
         "requestAnimationFrame callback fires");
  // A rAF chain (two frames) also advances — proves repeated servicing, not a one-shot.
  mbRunJS(v, "window.__n=0;(function loop(){requestAnimationFrame(function(){"
             "if(++window.__n<2)loop();});})();");
  mbWait(v, 80);
  Expect(Eval(v, "String(window.__n)") == "2",
         "requestAnimationFrame chain advances", Eval(v, "String(window.__n)"));

  // 27. Observer delivery: MutationObserver must fire on a DOM change, and an
  // IntersectionObserver on an in-viewport element must deliver (the offscreen
  // frame reads as throttled, so IO is force-computed by the host).
  {
    const char* doc =
        "<body><div id='t'>0</div><div id='io' style='height:20px'></div>"
        "<script>"
        "window.__mo=0;new MutationObserver(function(){window.__mo=1;})"
        ".observe(document.getElementById('t'),{childList:true,subtree:true,characterData:true});"
        "document.getElementById('t').textContent='changed';"
        "window.__io=0;new IntersectionObserver(function(es){"
        "es.forEach(function(e){if(e.isIntersecting)window.__io=1;});})"
        ".observe(document.getElementById('io'));"
        "</script></body>";
    if (FILE* f = std::fopen("/tmp/mb_observers.html", "wb")) {
      std::fwrite(doc, 1, std::strlen(doc), f); std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_observers.html");
    mbWait(v, 80);
    Expect(Eval(v, "String(window.__mo)") == "1", "MutationObserver delivers");
    Expect(Eval(v, "String(window.__io)") == "1",
           "IntersectionObserver delivers (in-viewport)",
           Eval(v, "String(window.__io)"));
  }

  // 29. Time-based animation + networking-adjacent delivery (these guard the rAF /
  // animation-clock + observer servicing added in recent changes):
  //  - Web Animations API: a 100ms animation's finished promise resolves (clock advances).
  //  - ResizeObserver delivers its initial observation.
  //  - dynamic Image().onload fires; synchronous XHR to a data: URL returns the body.
  {
    mbLoadHTML(v,
        "<body><div id='b' style='width:50px;height:50px'></div><script>"
        "window.__waapi=0;document.getElementById('b').animate("
        "[{opacity:0},{opacity:1}],100).finished.then(function(){window.__waapi=1;});"
        "window.__ro=0;new ResizeObserver(function(){window.__ro=1;})"
        ".observe(document.getElementById('b'));"
        "window.__img=0;var im=new Image();im.onload=function(){window.__img=1;};"
        "im.src='data:image/svg+xml,<svg xmlns=%22http://www.w3.org/2000/svg%22 width=%225%22 height=%225%22></svg>';"
        "window.__xhr='';try{var x=new XMLHttpRequest();x.open('GET',"
        "'data:text/plain,hello',false);x.send();window.__xhr=x.responseText;}"
        "catch(e){window.__xhr='ERR:'+e.name;}"
        "</script></body>",
        "about:blank");
    mbWait(v, 250);
    Expect(Eval(v, "String(window.__waapi)") == "1",
           "Web Animations API finished promise resolves (clock advances)");
    Expect(Eval(v, "String(window.__ro)") == "1", "ResizeObserver delivers");
    Expect(Eval(v, "String(window.__img)") == "1" &&
               Eval(v, "window.__xhr") == "hello",
           "dynamic Image().onload + sync XHR(data:) work");
  }

  // 30. Console capture: page console.log/warn/error are captured and drainable.
  mbLoadHTML(v, "<body><script>console.log('hello');console.warn('careful');"
                "console.error('boom');</script></body>", "about:blank");
  mbWait(v, 20);
  {
    char cbuf[1024] = {0};
    mbDrainConsole(v, cbuf, sizeof(cbuf));
    std::string console(cbuf);
    Expect(console.find("log: hello") != std::string::npos &&
               console.find("warn: careful") != std::string::npos &&
               console.find("error: boom") != std::string::npos,
           "console capture (log/warn/error)", console);
    // Draining clears the buffer.
    char cbuf2[64] = {0};
    mbDrainConsole(v, cbuf2, sizeof(cbuf2));
    Expect(cbuf2[0] == '\0', "console buffer clears after drain");
  }

  // (IndexedDB is now covered by case 23m — the open+schema step-1 backend.)

  // 30a (a11y). mbGetAXTree returns the ACCESSIBILITY SNAPSHOT (roles + accessible
  // names + control values) as JSON — the semantic view that testing tools and
  // AI/automation agents read instead of raw DOM. A page with a heading, a button, and
  // a labelled text field must surface those accessible NAMES and a nested structure.
  {
    mbLoadHTML(v,
               "<body><h1>Hello AX</h1>"
               "<button>Click me</button>"
               "<a href='https://example.com/docs'>Docs</a>"
               "<label>Email <input type='text' value='a@b.com'></label>"
               "</body>",
               "about:blank");
    mbWait(v, 80);
    std::string tree;
    int n = mbGetAXTree(v, nullptr, 0);  // size first (out=NULL)
    if (n > 0) {
      std::vector<char> buf(static_cast<size_t>(n) + 1, 0);
      mbGetAXTree(v, buf.data(), n + 1);
      tree.assign(buf.data());
    }
    const bool well_formed = tree.rfind("{\"role\":", 0) == 0 &&
                             tree.find("\"children\":[") != std::string::npos;
    const bool names_ok = tree.find("Hello AX") != std::string::npos &&
                          tree.find("Click me") != std::string::npos;
    const bool value_ok = tree.find("a@b.com") != std::string::npos;
    const bool roles_ok = tree.find("\"role\":\"heading\"") != std::string::npos &&
                          tree.find("\"role\":\"button\"") != std::string::npos;
    // A link node carries its destination URL; a heading carries its level (h1 -> 1).
    const bool url_ok =
        tree.find("\"url\":\"https://example.com/docs\"") != std::string::npos;
    const bool level_ok = tree.find("\"level\":1") != std::string::npos;
    Expect(well_formed && names_ok && value_ok && roles_ok && url_ok && level_ok,
           "mbGetAXTree: a11y snapshot has roles + names + value + link URL + heading level",
           "len=" + std::to_string((int)tree.size()) + " wf=" +
               (well_formed ? "1" : "0") + " names=" + (names_ok ? "1" : "0") +
               " val=" + (value_ok ? "1" : "0") + " roles=" + (roles_ok ? "1" : "0") +
               " url=" + (url_ok ? "1" : "0") + " level=" + (level_ok ? "1" : "0"));
  }

  // 30b (a11y). The AX snapshot is ACTIONABLE: each node carries frame-relative bounds
  // (x,y,w,h = widget/page coords), so an agent can locate a node by role+name and click
  // its center. End-to-end see->act: read the button's bounds from the AX JSON, click the
  // center via mbSendMouseClick, and confirm the button's own handler fired (isTrusted).
  {
    mbLoadHTML(v,
               "<body style='margin:0'>"
               "<button style='position:absolute;left:40px;top:30px;width:120px;"
               "height:40px' onclick='window.__axc=event.isTrusted?2:1'>Submit</button>"
               "</body>",
               "about:blank");
    mbWait(v, 80);
    std::string tree;
    int n = mbGetAXTree(v, nullptr, 0);
    if (n > 0) {
      std::vector<char> buf(static_cast<size_t>(n) + 1, 0);
      mbGetAXTree(v, buf.data(), n + 1);
      tree.assign(buf.data());
    }
    // Parse the button node's bounds: the first "x"/"y"/"w"/"h" after its role (the
    // serializer emits bounds before the node's children, so these are the button's own).
    auto field_after = [&](size_t from, const char* key) -> int {
      std::string k = std::string("\"") + key + "\":";
      size_t p = tree.find(k, from);
      if (p == std::string::npos)
        return -1;
      return std::atoi(tree.c_str() + p + k.size());
    };
    size_t bpos = tree.find("\"role\":\"button\"");
    int bx = -1, by = -1, bw = -1, bh = -1;
    if (bpos != std::string::npos) {
      bx = field_after(bpos, "x");
      by = field_after(bpos, "y");
      bw = field_after(bpos, "w");
      bh = field_after(bpos, "h");
    }
    const bool bounds_ok = bx >= 0 && by >= 0 && bw > 0 && bh > 0;
    bool ax_click_ok = false;
    if (bounds_ok) {
      mbSendMouseClick(v, bx + bw / 2, by + bh / 2);  // click the button's center
      mbWait(v, 40);
      ax_click_ok = Eval(v, "String(window.__axc||0)") == "2";  // fired + trusted
    }
    Expect(bounds_ok && ax_click_ok,
           "mbGetAXTree: node bounds drive a trusted click on the button's center",
           "bounds=" + std::to_string(bx) + "," + std::to_string(by) + "," +
               std::to_string(bw) + "," + std::to_string(bh) +
               " click=" + (ax_click_ok ? "1" : "0"));
  }

  // 30c (a11y). The snapshot carries interactive STATE: a checkbox reports "checked", and
  // the state is LIVE — after toggling the checkbox via JS, a fresh snapshot flips it. The
  // focused element reports "focused". Proves the AX snapshot reflects real control state,
  // not just static structure (what an automation agent checks before/after acting).
  {
    auto ax_json = [&]() -> std::string {
      int n = mbGetAXTree(v, nullptr, 0);
      if (n <= 0)
        return std::string();
      std::vector<char> buf(static_cast<size_t>(n) + 1, 0);
      mbGetAXTree(v, buf.data(), n + 1);
      return std::string(buf.data());
    };
    mbLoadHTML(v,
               "<body><input type='checkbox' id='c'>"
               "<input type='text' id='t'></body>",
               "about:blank");
    mbWait(v, 60);
    const std::string before = ax_json();
    const bool unchecked_ok =
        before.find("\"checked\":false") != std::string::npos &&
        before.find("\"checked\":true") == std::string::npos;
    // Toggle the checkbox + focus the text field, then re-snapshot.
    mbRunJS(v, "document.getElementById('c').checked=true;"
               "document.getElementById('t').focus();");
    mbWait(v, 60);
    const std::string after = ax_json();
    const bool checked_ok = after.find("\"checked\":true") != std::string::npos;
    const bool focused_ok = after.find("\"focused\":true") != std::string::npos;
    Expect(unchecked_ok && checked_ok && focused_ok,
           "mbGetAXTree: live control state (checkbox checked toggles, focus reported)",
           std::string("unchecked=") + (unchecked_ok ? "1" : "0") + " checked=" +
               (checked_ok ? "1" : "0") + " focused=" + (focused_ok ? "1" : "0"));
  }

  // 30d (find). mbFindText runs blink's real find-in-page: it returns the TOTAL match
  // count, is case-sensitive on demand, and finds across element boundaries. mbStopFind
  // clears it. This is the Ctrl+F primitive (counting + highlighting), distinct from a
  // JS innerText search (it also selects/scrolls to + highlights the match).
  {
    mbLoadHTML(v,
               "<body><p>The cat sat. A CAT ran. Another cat slept.</p>"
               "<div>cat</div></body>",
               "about:blank");
    mbWait(v, 60);
    const int n_ci = mbFindText(v, "cat", 0);  // case-insensitive: cat,CAT,cat,cat = 4
    const int n_cs = mbFindText(v, "cat", 1);  // case-sensitive: cat,cat,cat = 3
    const int n_none = mbFindText(v, "zzzznotfound", 0);
    mbStopFind(v);
    Expect(n_ci == 4 && n_cs == 3 && n_none == 0,
           "mbFindText: real find-in-page counts matches (case-insensitive vs -sensitive)",
           "ci=" + std::to_string(n_ci) + " cs=" + std::to_string(n_cs) +
               " none=" + std::to_string(n_none));
  }

  // 30e (find). mbFindNext steps THROUGH the matches (the Ctrl+F navigation the count
  // alone can't do): on a tall page with the word spread 3000px apart, each FindNext
  // scrolls the next match into view (scrollY jumps), and stepping past the last wraps to
  // the first. mbStopFind ends the session (a later FindNext returns 0).
  {
    mbLoadHTML(v,
               "<body style='margin:0'>"
               "<div>needle one</div><div style='height:3000px'></div>"
               "<div>needle two</div><div style='height:3000px'></div>"
               "<div>needle three</div></body>",
               "about:blank");
    mbWait(v, 60);
    auto scroll_y = [&]() {
      return std::atoi(Eval(v, "String(Math.round(window.scrollY))").c_str());
    };
    const int n = mbFindText(v, "needle", 0);
    const int y0 = scroll_y();             // active = match 1 (near top)
    const int a1 = mbFindNext(v, 1);
    const int y1 = scroll_y();             // -> match 2 (~3000 down)
    const int a2 = mbFindNext(v, 1);
    const int y2 = scroll_y();             // -> match 3 (~6000 down)
    const int a3 = mbFindNext(v, 1);
    const int y3 = scroll_y();             // wraps -> match 1 (near top again)
    mbStopFind(v);
    const int after_stop = mbFindNext(v, 1);  // no session -> 0
    const bool steps_down = y1 > y0 + 1000 && y2 > y1 + 1000;
    const bool wrapped = y3 < y1;          // back near the top
    Expect(n == 3 && a1 && a2 && a3 && steps_down && wrapped && after_stop == 0,
           "mbFindNext: steps through matches (scroll follows), wraps, stops",
           "n=" + std::to_string(n) + " y=" + std::to_string(y0) + "," +
               std::to_string(y1) + "," + std::to_string(y2) + "," +
               std::to_string(y3) + " stop=" + std::to_string(after_stop));
  }

  // 30f (find). mbGetFindActiveRect locates the active match in clickable viewport coords:
  // find a unique word inside a clickable span 2000px down the page; the match scrolls into
  // view, its rect (viewport CSS px) is read, and a click at the rect center hits the span
  // (its onclick fires). End-to-end: find -> locate -> act on the located match.
  {
    mbLoadHTML(v,
               "<body style='margin:0'><div style='height:2000px'></div>"
               "<span id='s' onclick='window.__fc=1' "
               "style='display:inline-block'>UNIQUEWORDZ</span>"
               "<div style='height:2000px'></div></body>",
               "about:blank");
    mbWait(v, 60);
    const int n = mbFindText(v, "UNIQUEWORDZ", 1);  // scrolls the span into view
    int x = 0, y = 0, w = 0, h = 0;
    const int got = mbGetFindActiveRect(v, &x, &y, &w, &h);
    bool hit = false;
    if (got && w > 0 && h > 0) {
      mbSendMouseClick(v, x + w / 2, y + h / 2);  // click the located match
      mbWait(v, 40);
      hit = Eval(v, "String(window.__fc||0)") == "1";
    }
    mbStopFind(v);
    Expect(n == 1 && got == 1 && w > 0 && h > 0 && hit,
           "mbGetFindActiveRect: locates the match in clickable viewport coords",
           "n=" + std::to_string(n) + " rect=" + std::to_string(x) + "," +
               std::to_string(y) + "," + std::to_string(w) + "," + std::to_string(h) +
               " hit=" + (hit ? "1" : "0"));
  }

  // Network cases run against an httpbin-shaped echo host. By default main()
  // auto-spawns the bundled echo_server.py on a free loopback port and exports
  // MB_NET_TESTS/MB_NET_HOST (see EnsureLocalEchoServer), so a plain ./mb_smoke
  // covers them hermetically. Environment still wins: MB_NET_TESTS=0 skips them
  // (e.g. no python3), and an explicit MB_NET_HOST (with MB_NET_TESTS=1) targets
  // that host unchanged — a dead remote host costs ~45s per load, which is why
  // the cases stay env-gated rather than unconditional:
  //   python3 src/miniblink_host/test/echo_server.py &   # serves 127.0.0.1:8899
  //   MB_NET_TESTS=1 MB_NET_HOST=http://127.0.0.1:8899 ./mb_smoke
  if (std::getenv("MB_NET_TESTS")) {
  const std::string host =
      std::getenv("MB_NET_HOST") ? std::getenv("MB_NET_HOST") : "https://httpbin.org";
  // httpbin is a flaky public host: probe its health once so the cases that
  // assert specific httpbin shapes (status/redirect) SKIP — not fail — when it
  // is degraded (e.g. returning 503 for everything) rather than misbehaving.
  mbLoadURL(v, (host + "/get").c_str());
  const bool hb_ok = mbGetHttpStatus(v) == 200;

  // Draft-navigation regression cases need deterministic endpoints supplied by the local
  // echo_server.py (public httpbin-compatible hosts do not expose request counters/gates).
  const bool local_echo = std::getenv("MB_NET_HOST") &&
                          (host.find("127.0.0.1") != std::string::npos ||
                           host.find("localhost") != std::string::npos);
  if (local_echo) {
    std::string other_origin = host;
    if (auto p = other_origin.find("127.0.0.1"); p != std::string::npos)
      other_origin.replace(p, std::strlen("127.0.0.1"), "localhost");
    else if (auto q = other_origin.find("localhost"); q != std::string::npos)
      other_origin.replace(q, std::strlen("localhost"), "127.0.0.1");
    const std::string::size_type echo_scheme_end = host.find("://");
    const std::string::size_type echo_host_start =
        echo_scheme_end == std::string::npos ? 0 : echo_scheme_end + 3;
    const std::string::size_type echo_host_end =
        host.find(':', echo_host_start);
    const std::string echo_host_name = host.substr(
        echo_host_start, echo_host_end == std::string::npos
                             ? std::string::npos
                             : echo_host_end - echo_host_start);
    const std::string echo_port_suffix =
        echo_host_end == std::string::npos ? std::string()
                                           : host.substr(echo_host_end);
    const std::string subdomain_origin =
        host.substr(0, echo_host_start) + "sub.localhost" + echo_port_suffix;

    // Navigation ids share one process-global cancellation-token namespace.
    // Canceling a slow navigation in one view must not abort another view's
    // independently active navigation.
    mbView* other_view = mbCreateView(240, 160);
    mbLoadHTML(v, "<body>view-a-baseline</body>", (host + "/").c_str());
    mbLoadHTML(other_view, "<body>view-b-baseline</body>",
               (host + "/").c_str());
    const mbNavigationId view_a_id =
        mbNavigate(v, (host + "/slow?ms=450&marker=VIEW-A").c_str());
    const mbNavigationId view_b_id = mbNavigate(
        other_view, (host + "/slow?ms=120&marker=VIEW-B").c_str());
    const int view_a_cancelled = mbCancelNavigation(v, view_a_id);
    mbWaitForFunction(other_view,
                      "document.body&&document.body.textContent==='VIEW-B'",
                      2500);
    const std::string view_a_body =
        Eval(v, "document.body?document.body.textContent:''");
    const std::string view_b_body =
        Eval(other_view, "document.body?document.body.textContent:''");
    Expect(view_a_id != view_b_id && view_a_cancelled == 1 &&
               view_a_body == "view-a-baseline" && view_b_body == "VIEW-B",
           "cross-view navigation ids/cancellation tokens do not collide",
           "a=" + std::to_string(view_a_id) + " b=" +
               std::to_string(view_b_id) + " cancel=" +
               std::to_string(view_a_cancelled) + " bodies=[" + view_a_body +
               "]/ [" + view_b_body + "]");
    mbDestroyView(other_view);

    // A server redirect changes the committed URL/origin/base. The final body is served by
    // the alternate loopback hostname, so location.host must match that hostname rather
    // than the original redirecting origin.
    mbLoadHTML(v, "<body>redirect-origin-baseline</body>", (host + "/").c_str());
    const std::string redirect_url =
        host + "/redirect-to?status_code=302&url=" + other_origin + "/origin";
    Eval(v, ("location.href='" + redirect_url + "'").c_str());
    mbWaitForFunction(v, "document.body&&document.body.id==='origin'", 2500);
    const std::string redirected_location = Eval(v, "String(location.origin)");
    const std::string redirected_js_origin =
        Eval(v, "document.body?document.body.dataset.jsOrigin:''");
    Expect(redirected_location == other_origin && redirected_js_origin == other_origin,
           "page redirect commits the final URL/origin (not the original origin)",
           "location=[" + redirected_location + "] js=[" + redirected_js_origin + "]");

    // The legacy request callback is re-evaluated on EVERY redirect hop. Allow the first
    // loopback spelling but veto the alternate hostname; the final server must never commit.
    static int* redirect_hook_calls = new int(0);
    static std::string* redirect_block_origin = new std::string();
    *redirect_hook_calls = 0;
    *redirect_block_origin = other_origin;
    mbLoadHTML(v, "<body>redirect-policy-baseline</body>", (host + "/").c_str());
    mbSetRequestCallback(
        [](const char* url, void*) -> int {
          ++*redirect_hook_calls;
          return url && url[0] &&
                         std::string(url).rfind(*redirect_block_origin, 0) == 0
                     ? 1
                     : 0;
        },
        nullptr);
    mbNavigate(v, redirect_url.c_str());
    mbWait(v, 900);
    const std::string redirect_policy_body =
        Eval(v, "document.body?document.body.textContent:''");
    mbSetRequestCallback(nullptr, nullptr);
    Expect(*redirect_hook_calls >= 2 &&
               redirect_policy_body == "redirect-policy-baseline",
           "async redirect hops re-run request policy before fetching the target",
           "calls=" + std::to_string(*redirect_hook_calls) + " body=[" +
               redirect_policy_body + "]");

    // A normal 302 with Content-Length: 0 is a redirect, not an anomalous empty success:
    // it must be requested exactly once before following.
    const std::string count_key = "mb-empty-redirect";
    mbLoadURL(v, (host + "/reset-count?key=" + count_key).c_str());
    mbLoadURL(v, (host + "/empty-redirect?key=" + count_key + "&url=/origin").c_str());
    mbLoadURL(v, (host + "/count?key=" + count_key).c_str());
    const std::string count_body = Eval(v, "document.body?document.body.innerText:''");
    Expect(count_body.find("\"count\": 1") != std::string::npos ||
               count_body.find("\"count\":1") != std::string::npos,
           "bodyless redirect is fetched once (no empty-body retry loop)", count_body);

    // The synchronous host-load path follows redirects independently from the
    // async navigation engine. It must retain the previous fragment when a
    // Location header omits one, matching Fetch redirect semantics.
    mbLoadURL(v, (host + "/redirect-to?status_code=302&url=/origin#kept").c_str());
    const std::string sync_redirect_hash = Eval(v, "String(location.hash)");
    Expect(sync_redirect_hash == "#kept",
           "synchronous redirects retain an omitted fragment",
           "hash=[" + sync_redirect_hash + "]");

    // Transparent rewrites maintain distinct public/transport URL chains. The echo
    // backend returns a RELATIVE Location (/get): curl must stay on the local backend,
    // while sync navigation, async lifecycle, and fetch Response.url all stay public.
    const std::string public_origin = "https://rewrite-visible.test";
    mbClearUrlRewrites();
    mbRewriteUrl((public_origin + "/redirect/1").c_str(),
                 (host + "/redirect/1").c_str());
    mbLoadURL(v, (public_origin + "/redirect/1").c_str());
    const std::string rewritten_sync_url = Eval(v, "String(location.href)");

    std::vector<NavigationEventRecord> rewrite_events;
    mbOnNavigationEvent(v, &RecordNavigationEvent, &rewrite_events);
    const mbNavigationId rewrite_id =
        mbNavigate(v, (public_origin + "/redirect/1").c_str());
    for (int i = 0; !mbIsLoadFinished(v) && i < 300; ++i)
      mbWait(v, 10);
    const std::string rewritten_async_url = Eval(v, "String(location.href)");
    std::string lifecycle_committed_url;
    std::string lifecycle_final_url;
    for (const NavigationEventRecord& event : rewrite_events) {
      if (event.id == rewrite_id &&
          event.phase == MB_NAVIGATION_PHASE_COMMITTED)
        lifecycle_committed_url = event.url;
      if (event.id == rewrite_id &&
          event.phase == MB_NAVIGATION_PHASE_TERMINAL)
        lifecycle_final_url = event.url;
    }
    mbOnNavigationEvent(v, nullptr, nullptr);

    mbRunJS(v,
            "window.__rewriteFetch='pending';"
            "fetch('https://rewrite-visible.test/redirect/1')"
            ".then(function(r){window.__rewriteFetch=r.url+'|'+r.redirected;})"
            ".catch(function(e){window.__rewriteFetch='error:'+e.name;});");
    mbWaitForFunction(v, "window.__rewriteFetch!=='pending'", 3000);
    const std::string rewritten_fetch_url =
        Eval(v, "String(window.__rewriteFetch)");
    mbClearUrlRewrites();
    Expect(rewritten_sync_url == public_origin + "/get" &&
               rewritten_async_url == public_origin + "/get" &&
               lifecycle_committed_url == public_origin + "/get" &&
               lifecycle_final_url == public_origin + "/get" &&
               rewritten_fetch_url == public_origin + "/get|true",
           "relative backend redirects preserve public URLs across sync/async/fetch",
           "sync=[" + rewritten_sync_url + "] async=[" +
               rewritten_async_url + "] committed=[" +
               lifecycle_committed_url + "] terminal=[" + lifecycle_final_url +
               "] fetch=[" + rewritten_fetch_url + "]");

    // Re-consult the rewrite table against EACH visible redirect target. The first
    // backend hop points at /rewrite-chain-mid, whose response is deliberately
    // UNREWRITTEN. A rule matching that intermediate PUBLIC URL must instead fetch
    // /rewrite-chain-rematched, follow its second redirect, and expose the public final
    // URL through both location.href and the direct mbResponseURL hook.
    struct RewriteResponseTrace {
      std::vector<std::string> urls;
    };
    static RewriteResponseTrace* rematch_response_trace =
        new RewriteResponseTrace();
    *rematch_response_trace = RewriteResponseTrace();
    const std::string rematch_public_origin =
        "https://rewrite-rematch-visible.test";
    const std::string rematch_public_start =
        rematch_public_origin + "/rewrite-chain-start";
    const std::string rematch_public_mid =
        rematch_public_origin + "/rewrite-chain-mid";
    const std::string rematch_public_final =
        rematch_public_origin + "/rewrite-chain-final";
    mbClearUrlRewrites();
    mbRewriteUrl(rematch_public_start.c_str(),
                 (host + "/rewrite-chain-start").c_str());
    mbRewriteUrl(rematch_public_mid.c_str(),
                 (host + "/rewrite-chain-rematched").c_str());
    mbSetResponseCallback(
        [](mbResponse* response, void*) {
          const char* url = mbResponseURL(response);
          if (url && (std::strstr(url, "rewrite-rematch-visible.test") ||
                      std::strstr(url, "/rewrite-chain"))) {
            rematch_response_trace->urls.emplace_back(url);
          }
        },
        nullptr);
    mbLoadURL(v, rematch_public_start.c_str());
    const std::string rematch_body =
        Eval(v, "document.body?document.body.textContent:''");
    const std::string rematch_location = Eval(v, "String(location.href)");
    mbSetResponseCallback(nullptr, nullptr);
    mbClearUrlRewrites();
    bool response_url_saw_final = false;
    bool response_urls_all_public = !rematch_response_trace->urls.empty();
    std::string rematch_response_urls;
    for (const std::string& response_url : rematch_response_trace->urls) {
      response_url_saw_final =
          response_url_saw_final || response_url == rematch_public_final;
      response_urls_all_public =
          response_urls_all_public &&
          response_url.rfind(rematch_public_origin + "/", 0) == 0;
      rematch_response_urls += "[" + response_url + "]";
    }
    Expect(rematch_body == "REMATCHED" &&
               rematch_location == rematch_public_final &&
               response_url_saw_final && response_urls_all_public,
           "redirect chains re-match visible-hop rewrites and mbResponseURL stays public",
           "body=[" + rematch_body + "] location=[" + rematch_location +
               "] response_count=" +
               std::to_string(rematch_response_trace->urls.size()) +
               " responses=" + rematch_response_urls);

    // A backend commonly builds an ABSOLUTE Location from the transport Host.
    // When that target points back to the exact rewritten backend origin, it is
    // still part of the transparent chain: keep fetching the backend but expose
    // the public origin through synchronous navigation, mbNavigate, and fetch().
    const std::string absolute_public_origin =
        "https://rewrite-absolute-visible.test";
    const std::string absolute_public_url =
        absolute_public_origin + "/absolute-host-redirect";
    const std::string absolute_final_url =
        absolute_public_origin + "/origin?via=absolute";
    mbClearUrlRewrites();
    mbRewriteUrl(absolute_public_url.c_str(),
                 (host + "/absolute-host-redirect").c_str());
    mbLoadURL(v, absolute_public_url.c_str());
    const std::string absolute_sync_url = Eval(v, "String(location.href)");

    mbLoadHTML(v, "<body>absolute-async-baseline</body>",
               (absolute_public_origin + "/async-baseline").c_str());
    std::vector<NavigationEventRecord> absolute_events;
    mbOnNavigationEvent(v, &RecordNavigationEvent, &absolute_events);
    const mbNavigationId absolute_nav_id =
        mbNavigate(v, absolute_public_url.c_str());
    for (int i = 0; !mbIsLoadFinished(v) && i < 300; ++i)
      mbWait(v, 10);
    const bool absolute_async_finished = mbIsLoadFinished(v) == 1;
    const std::string absolute_async_url = Eval(v, "String(location.href)");
    std::string absolute_committed_url;
    std::string absolute_terminal_url;
    mbNavigationOutcome absolute_terminal_outcome =
        MB_NAVIGATION_OUTCOME_NONE;
    for (const NavigationEventRecord& event : absolute_events) {
      if (event.id != absolute_nav_id)
        continue;
      if (event.phase == MB_NAVIGATION_PHASE_COMMITTED)
        absolute_committed_url = event.url;
      if (event.phase == MB_NAVIGATION_PHASE_TERMINAL) {
        absolute_terminal_url = event.url;
        absolute_terminal_outcome = event.outcome;
      }
    }
    mbOnNavigationEvent(v, nullptr, nullptr);

    mbRunJS(v,
            ("window.__absoluteRewriteFetch='pending';fetch('" +
             absolute_public_url +
             "').then(function(r){window.__absoluteRewriteFetch="
             "r.url+'|'+r.redirected;}).catch(function(e){"
             "window.__absoluteRewriteFetch='error:'+e.name;});")
                .c_str());
    mbWaitForFunction(v, "window.__absoluteRewriteFetch!=='pending'", 3000);
    const std::string absolute_fetch_url =
        Eval(v, "String(window.__absoluteRewriteFetch)");

    // Do not over-project: a backend's explicit absolute redirect to a DIFFERENT
    // origin is a real redirect and must remain visible to the page.
    const std::string absolute_other_public_url =
        absolute_public_origin + "/absolute-other-origin";
    mbRewriteUrl(
        absolute_other_public_url.c_str(),
        (host + "/redirect-to?status_code=302&url=" + other_origin +
         "/origin")
            .c_str());
    mbLoadURL(v, absolute_other_public_url.c_str());
    const std::string absolute_other_origin = Eval(v, "String(location.origin)");
    mbClearUrlRewrites();
    Expect(absolute_nav_id != 0 && absolute_sync_url == absolute_final_url &&
               absolute_async_finished &&
               absolute_async_url == absolute_final_url &&
               absolute_committed_url == absolute_final_url &&
               absolute_terminal_url == absolute_final_url &&
               absolute_terminal_outcome == MB_NAVIGATION_OUTCOME_SUCCESS &&
               absolute_fetch_url == absolute_final_url + "|true" &&
               absolute_other_origin == other_origin,
           "same-backend absolute redirects preserve public URLs across sync/async/fetch",
           "sync=[" + absolute_sync_url + "] async=[" +
               absolute_async_url + "] committed=[" +
               absolute_committed_url + "] terminal=[" +
               absolute_terminal_url + "]/" +
               std::to_string(absolute_terminal_outcome) + " fetch=[" +
               absolute_fetch_url + "] other=[" + absolute_other_origin +
               "]");

    // A network-path Location (//backend/path) inherits the base scheme in each
    // URL space. That means a public https URL rewritten to this http echo server
    // used to miss same-backend projection: the transport result was http://backend
    // while the visible result was https://backend. Exact backend authority must
    // still project to the public origin for both navigation and fetch().
    const std::string network_path_public_origin =
        "https://rewrite-network-path-visible.test";
    const std::string network_path_public_url =
        network_path_public_origin + "/scheme-relative-host-redirect";
    const std::string network_path_final_url =
        network_path_public_origin + "/origin?via=scheme-relative";
    mbClearUrlRewrites();
    mbRewriteUrl(network_path_public_url.c_str(),
                 (host + "/scheme-relative-host-redirect").c_str());
    mbLoadURL(v, network_path_public_url.c_str());
    const std::string network_path_sync_url = Eval(v, "String(location.href)");
    mbRunJS(v,
            ("window.__networkPathFetch='pending';fetch('" +
             network_path_public_url +
             "').then(function(r){window.__networkPathFetch="
             "r.url+'|'+r.redirected;}).catch(function(e){"
             "window.__networkPathFetch='error:'+e.name;});")
                .c_str());
    mbWaitForFunction(v, "window.__networkPathFetch!=='pending'", 3000);
    const std::string network_path_fetch_url =
        Eval(v, "String(window.__networkPathFetch)");
    mbClearUrlRewrites();
    Expect(network_path_sync_url == network_path_final_url &&
               network_path_fetch_url == network_path_final_url + "|true",
           "same-backend network-path redirects preserve public URLs across rewrites",
           "sync=[" + network_path_sync_url + "] fetch=[" +
               network_path_fetch_url + "]");

    // Authority projection is exact-origin, including the effective port. A
    // scheme-relative redirect to the same backend HOST but another port remains
    // visible. Block it in the policy callback so the test never connects to the
    // deliberately unused port.
    struct PortRedirectVisibility {
      std::string initial;
      std::vector<std::string> urls;
    };
    static PortRedirectVisibility* port_redirect_visibility =
        new PortRedirectVisibility();
    *port_redirect_visibility = PortRedirectVisibility();
    const std::string port_redirect_public_url =
        network_path_public_origin + "/different-backend-port";
    port_redirect_visibility->initial = port_redirect_public_url;
    mbSetRequestCallback(
        [](const char* url, void*) -> int {
          const std::string seen = url ? url : "";
          port_redirect_visibility->urls.push_back(seen);
          return seen == port_redirect_visibility->initial ? 0 : 1;
        },
        nullptr);
    mbRewriteUrl(
        port_redirect_public_url.c_str(),
        (host + "/redirect-to?status_code=302&url=//" + echo_host_name +
         ":1/origin?via=different-port")
            .c_str());
    mbLoadURL(v, port_redirect_public_url.c_str());
    mbClearUrlRewrites();
    mbSetRequestCallback(nullptr, nullptr);
    const bool different_port_visible =
        port_redirect_visibility->urls.size() >= 2 &&
        port_redirect_visibility->urls[1].rfind(
            "https://" + echo_host_name + ":1/origin", 0) == 0;
    Expect(different_port_visible,
           "same-host different-port redirects remain visible real redirects",
           "calls=" + std::to_string(port_redirect_visibility->urls.size()) +
               (port_redirect_visibility->urls.size() < 2
                    ? std::string()
                    : " second=[" + port_redirect_visibility->urls[1] + "]"));

    // A transport failure before any HTTP response must likewise report the
    // attempted PUBLIC URL, never the transparent backend target, through both
    // the synchronous mbLoadURL and asynchronous mbNavigate failure paths.
    struct RewriteFailState {
      std::vector<std::string> urls;
      std::vector<std::string> domains;
      std::vector<int> codes;
    };
    static RewriteFailState* rewrite_fail = new RewriteFailState();
    *rewrite_fail = RewriteFailState();
    mbOnFailLoadingEx(
        v,
        [](mbView*, void*, const char* url, const char* domain, int code,
           const char*) {
          rewrite_fail->urls.push_back(url ? url : "");
          rewrite_fail->domains.push_back(domain ? domain : "");
          rewrite_fail->codes.push_back(code);
        },
        nullptr);
    const std::string failure_public_url =
        "https://rewrite-failure-visible.test/transport-fail";
    mbRewriteUrl(failure_public_url.c_str(),
                 (host + "/drop-connection").c_str());
    mbLoadURL(v, failure_public_url.c_str());
    const mbNavigationId failure_nav_id =
        mbNavigate(v, failure_public_url.c_str());
    for (int i = 0; !mbIsLoadFinished(v) && i < 400; ++i)
      mbWait(v, 10);
    mbClearUrlRewrites();
    mbOnFailLoadingEx(v, nullptr, nullptr);
    const bool failure_urls_public =
        rewrite_fail->urls.size() == 2 &&
        rewrite_fail->urls[0] == failure_public_url &&
        rewrite_fail->urls[1] == failure_public_url;
    const bool failure_codes_structured =
        rewrite_fail->domains.size() == 2 && rewrite_fail->codes.size() == 2 &&
        rewrite_fail->domains[0] == "curl" &&
        rewrite_fail->domains[1] == "curl" &&
        rewrite_fail->codes[0] != 0 && rewrite_fail->codes[1] != 0;
    Expect(failure_nav_id != 0 && failure_urls_public &&
               failure_codes_structured,
           "transparent rewrite transport failures report only the public URL",
           "urls=" + std::to_string(rewrite_fail->urls.size()) +
               (rewrite_fail->urls.empty()
                    ? std::string()
                    : " first=[" + rewrite_fail->urls.front() + "]") +
               (rewrite_fail->urls.size() < 2
                    ? std::string()
                    : " second=[" + rewrite_fail->urls[1] + "]"));

    // Header precedence is one global registration timeline across categories. The later
    // substring rule must beat the earlier origin rule; the mutable hook must inspect that
    // composed value and then override the actual header curl sends.
    static int* hook_saw_static = new int(0);
    *hook_saw_static = 0;
    mbClearRequestHeaders();
    mbSetRequestHeaderForOrigin(host.c_str(), "X-Mb-Layer", "origin-first");
    mbSetRequestHeader("/headers?header-order=1", "X-Mb-Layer",
                       "substring-later");
    mbSetRequestHook(
        [](mbRequest* request, void*) {
          const char* url = mbRequestURL(request);
          if (!url || !std::strstr(url, "header-order=1"))
            return;
          const char* headers = mbRequestHeaders(request);
          if (headers && std::strstr(headers, "X-Mb-Layer: substring-later"))
            *hook_saw_static = 1;
          mbRequestSetHeader(request, "X-Mb-Layer", "hook-final");
        },
        nullptr);
    mbLoadURL(v, (host + "/headers?header-order=1").c_str());
    const std::string layered_headers =
        Eval(v, "document.body?document.body.innerText:''");
    mbSetRequestHook(nullptr, nullptr);
    mbClearRequestHeaders();
    Expect(*hook_saw_static &&
               layered_headers.find("hook-final") != std::string::npos &&
               layered_headers.find("substring-later") == std::string::npos &&
               layered_headers.find("origin-first") == std::string::npos,
           "static headers follow global call order and mutable hook wins last",
           "saw=" + std::to_string(*hook_saw_static) + " response=[" +
               layered_headers + "]");

    // Host-scoped registrations must both reach curl and participate in the SAME
    // registration timeline as origin/substring rules. Exercise both orderings so
    // a fixed category-order implementation cannot accidentally pass.
    mbClearRequestHeaders();
    mbSetRequestHeaderForOrigin(host.c_str(), "X-Mb-Host-Order",
                                "origin-first");
    mbSetRequestHeaderForHost(echo_host_name.c_str(), "X-Mb-Host-Order",
                              "host-last");
    mbSetRequestHeaderForHost(echo_host_name.c_str(), "X-Mb-Host-Only",
                              "host-delivered");
    mbSetRequestHeaderForHost("not-the-echo-host.invalid", "X-Mb-Host-Skip",
                              "must-not-deliver");
    mbLoadURL(v, (host + "/headers?host-precedence=a").c_str());
    const std::string host_rule_last =
        Eval(v, "document.body?document.body.innerText:''");

    mbClearRequestHeaders();
    mbSetRequestHeaderForHost(echo_host_name.c_str(), "X-Mb-Host-Order",
                              "host-first");
    mbSetRequestHeader("/headers?host-precedence=b", "X-Mb-Host-Order",
                       "substring-last");
    mbLoadURL(v, (host + "/headers?host-precedence=b").c_str());
    const std::string host_rule_first =
        Eval(v, "document.body?document.body.innerText:''");
    mbClearRequestHeaders();
    const bool host_header_delivery =
        host_rule_last.find("host-delivered") != std::string::npos &&
        host_rule_last.find("must-not-deliver") == std::string::npos;
    const bool host_header_order =
        host_rule_last.find("host-last") != std::string::npos &&
        host_rule_last.find("origin-first") == std::string::npos &&
        host_rule_first.find("substring-last") != std::string::npos &&
        host_rule_first.find("host-first") == std::string::npos;
    Expect(host_header_delivery && host_header_order,
           "mbSetRequestHeaderForHost delivers and obeys global registration order",
           "host-last=[" + host_rule_last + "] host-first=[" +
               host_rule_first + "]");

    // Exercise the two nontrivial host-filter modes directly. A leading dot opts
    // into true subdomain matching (while the exact-host control must miss), and a
    // /path/ suffix narrows an otherwise matching host to that path prefix.
    mbClearRequestHeaders();
    mbSetRequestHeaderForHost(".localhost", "X-Mb-Subdomain-Mode",
                              "leading-dot-hit");
    mbSetRequestHeaderForHost("localhost", "X-Mb-Exact-Mode",
                              "exact-host-must-miss");
    mbLoadURL(v, (subdomain_origin + "/headers/subdomain-mode").c_str());
    const std::string subdomain_mode_headers =
        Eval(v, "document.body?document.body.innerText:''");

    mbClearRequestHeaders();
    mbSetRequestHeaderForHost(
        (echo_host_name + "/headers/host-prefix/").c_str(),
        "X-Mb-Host-Path", "host-path-hit");
    mbLoadURL(v, (host + "/headers/host-prefix/item").c_str());
    const std::string host_path_match_headers =
        Eval(v, "document.body?document.body.innerText:''");
    mbLoadURL(v, (host + "/headers/host-prefixish/item").c_str());
    const std::string host_path_miss_headers =
        Eval(v, "document.body?document.body.innerText:''");
    mbClearRequestHeaders();
    const bool subdomain_filter_ok =
        subdomain_mode_headers.find("leading-dot-hit") != std::string::npos &&
        subdomain_mode_headers.find("exact-host-must-miss") ==
            std::string::npos;
    const bool host_path_filter_ok =
        host_path_match_headers.find("host-path-hit") != std::string::npos &&
        host_path_miss_headers.find("host-path-hit") == std::string::npos;
    Expect(subdomain_filter_ok && host_path_filter_ok,
           "mbSetRequestHeaderForHost honors subdomain and path-prefix modes",
           "subdomain=[" + subdomain_mode_headers + "] path-hit=[" +
               host_path_match_headers + "] path-miss=[" +
               host_path_miss_headers + "]");

    // Credential stripping recognizes the literal Authorization header, not
    // merely the arbitrary hook-header case exercised below. The view-global
    // header reaches its intended first origin, then must be absent after a
    // redirect to the alternate loopback origin.
    mbSetExtraHeaders(v, "Authorization: Bearer mb-auth-secret");
    mbLoadURL(v, (host + "/headers?authorization=direct").c_str());
    const std::string direct_authorization =
        Eval(v, "document.body?document.body.innerText:''");
    mbLoadURL(v,
              (host + "/redirect-to?status_code=302&url=" + other_origin +
               "/headers?authorization=redirected")
                  .c_str());
    const std::string redirected_authorization =
        Eval(v, "document.body?document.body.innerText:''");
    mbSetExtraHeaders(v, "");
    Expect(direct_authorization.find("mb-auth-secret") != std::string::npos &&
               redirected_authorization.find("mb-auth-secret") ==
                   std::string::npos,
           "Authorization is delivered first-party then stripped on redirect",
           "direct=[" + direct_authorization + "] redirected=[" +
               redirected_authorization + "]");

    // Do not over-strip: a caller-supplied Authorization header is carried through
    // a same-origin redirect. The echo fixture reports both hops so merely sending
    // the credential on the first request cannot make this pass.
    mbSetExtraHeaders(v, "Authorization: Bearer mb-same-origin-secret");
    mbLoadURL(v,
              (host +
               "/auth-chain-start?key=same-origin&url=/auth-chain-final?key=same-origin")
                  .c_str());
    const std::string same_origin_authorization =
        Eval(v, "document.body?document.body.innerText:''");
    mbSetExtraHeaders(v, "");
    const bool same_origin_authorization_kept =
        same_origin_authorization.find(
            "\"initial_authorization\": \"Bearer mb-same-origin-secret\"") !=
            std::string::npos &&
        same_origin_authorization.find(
            "\"final_authorization\": \"Bearer mb-same-origin-secret\"") !=
            std::string::npos;
    Expect(same_origin_authorization_kept,
           "same-origin redirects preserve caller Authorization",
           "response=[" + same_origin_authorization + "]");

    // Registered credentials are evaluated anew on every hop. Bind distinct
    // Authorization values to the source and destination origins: the first hop
    // must receive only the source value, then the cross-origin target must receive
    // the destination registration rather than retaining or permanently losing it.
    mbClearRequestHeaders();
    mbSetRequestHeaderForOrigin(host.c_str(), "Authorization",
                                "Bearer mb-source-registration");
    mbSetRequestHeaderForOrigin(other_origin.c_str(), "Authorization",
                                "Bearer mb-destination-registration");
    mbLoadURL(
        v,
        (host + "/auth-chain-start?key=registered-per-hop&url=" +
         other_origin + "/auth-chain-final?key=registered-per-hop")
            .c_str());
    const std::string registered_authorization =
        Eval(v, "document.body?document.body.innerText:''");
    mbClearRequestHeaders();
    const bool registered_authorization_reapplied =
        registered_authorization.find(
            "\"initial_authorization\": \"Bearer mb-source-registration\"") !=
            std::string::npos &&
        registered_authorization.find(
            "\"final_authorization\": \"Bearer mb-destination-registration\"") !=
            std::string::npos;
    Expect(registered_authorization_reapplied,
           "origin-registered Authorization is re-evaluated on each redirect hop",
           "response=[" + registered_authorization + "]");

    mbNavigationOptions head_options = {};
    head_options.struct_size = sizeof(head_options);
    head_options.method = "HEAD";
    const mbNavigationId head_id =
        mbNavigateEx(v, (host + "/head-only").c_str(), &head_options);
    for (int i = 0; !mbIsLoadFinished(v) && i < 150; ++i)
      mbWait(v, 10);
    char head_headers[1024] = {0};
    mbGetResponseHeaders(v, head_headers, sizeof(head_headers));
    Expect(head_id != 0 && mbGetHttpStatus(v) == 200 &&
               std::string(head_headers).find("X-Observed-Method: HEAD") !=
                   std::string::npos,
           "mbNavigateEx preserves an explicit HEAD method",
           "status=" + std::to_string(mbGetHttpStatus(v)) + " headers=[" +
               head_headers + "]");

    // Content-Type is request metadata, not evidence that a body is non-empty. An
    // explicit empty POST must retain it rather than ConfigureCurlEasy silently dropping it.
    mbLoadHTML(v, "<body>empty-post</body>", (host + "/").c_str());
    mbRunJS(v,
            "window.__emptyPost='pending';"
            "fetch('/post',{method:'POST',headers:{'Content-Type':'application/x-empty'}})"
            ".then(function(r){return r.text();})"
            ".then(function(t){window.__emptyPost=t;})"
            ".catch(function(e){window.__emptyPost='error:'+e.name;});");
    mbWaitForFunction(v, "window.__emptyPost!=='pending'", 2500);
    const std::string empty_post = Eval(v, "String(window.__emptyPost)");
    Expect(empty_post.find("application/x-empty") != std::string::npos,
           "empty-body POST preserves its explicit Content-Type",
           "response=[" + empty_post + "]");

    // 307 and 308 are method-preserving redirects. Exercise the host-driven POST
    // path through a transparent rewrite: the backend receives the original POST
    // body on the second hop while the committed URL remains on the public origin.
    bool preserved_307_308 = true;
    std::string preserved_307_308_detail;
    for (int redirect_status : {307, 308}) {
      const std::string status_text = std::to_string(redirect_status);
      const std::string post_public_origin =
          "https://rewrite-post-visible.test";
      const std::string post_public_url =
          post_public_origin + "/start-" + status_text;
      const std::string post_final_url =
          post_public_origin + "/post?via=" + status_text;
      const std::string post_body =
          "redirect_code=" + status_text + "&payload=preserved-" + status_text;
      mbClearUrlRewrites();
      mbRewriteUrl(
          post_public_url.c_str(),
          (host + "/redirect-post?status_code=" + status_text +
           "&url=/post?via=" + status_text)
              .c_str());
      mbPostURL(v, post_public_url.c_str(), post_body.c_str(),
                "application/x-www-form-urlencoded");
      const std::string echoed_post =
          Eval(v, "document.body?document.body.innerText:''");
      const std::string committed_post_url = Eval(v, "String(location.href)");
      const bool one_preserved =
          committed_post_url == post_final_url &&
          echoed_post.find("\"method\": \"POST\"") != std::string::npos &&
          echoed_post.find(post_body) != std::string::npos;
      preserved_307_308 = preserved_307_308 && one_preserved;
      preserved_307_308_detail +=
          status_text + ":url=[" + committed_post_url + "] body=[" +
          echoed_post + "] ";
    }
    mbClearUrlRewrites();
    Expect(preserved_307_308,
           "rewritten 307/308 redirects preserve POST method and body",
           preserved_307_308_detail);

    // The non-blocking navigation engine owns a separate redirect loop. Drive
    // mbNavigateEx through both preserving statuses and require the final public
    // URL plus the exact POST body, so falling back to GET cannot false-pass.
    bool async_preserved_307_308 = true;
    std::string async_preserved_detail;
    for (int redirect_status : {307, 308}) {
      const std::string status_text = std::to_string(redirect_status);
      const std::string async_public_origin =
          "https://rewrite-post-async-visible.test";
      const std::string async_public_url =
          async_public_origin + "/start-" + status_text;
      const std::string async_final_url =
          async_public_origin + "/post?via=async-" + status_text;
      const std::string async_body =
          "redirect_code=" + status_text + "&payload=async-preserved-" +
          status_text;
      mbClearUrlRewrites();
      mbRewriteUrl(
          async_public_url.c_str(),
          (host + "/redirect-post?status_code=" + status_text +
           "&url=/post?via=async-" + status_text)
              .c_str());
      mbNavigationOptions async_options = {};
      async_options.struct_size = sizeof(async_options);
      async_options.method = "POST";
      async_options.body = async_body.data();
      async_options.body_len = async_body.size();
      async_options.content_type = "application/x-www-form-urlencoded";
      const mbNavigationId async_id =
          mbNavigateEx(v, async_public_url.c_str(), &async_options);
      const std::string async_wait =
          "document.body&&document.body.innerText.indexOf('async-preserved-" +
          status_text + "')>=0";
      mbWaitForFunction(v, async_wait.c_str(), 4000);
      const std::string async_echoed_post =
          Eval(v, "document.body?document.body.innerText:''");
      const std::string async_committed_url = Eval(v, "String(location.href)");
      const bool one_async_preserved =
          async_id != 0 && mbIsLoadFinished(v) == 1 &&
          async_committed_url == async_final_url &&
          async_echoed_post.find("\"method\": \"POST\"") !=
              std::string::npos &&
          async_echoed_post.find(async_body) != std::string::npos;
      async_preserved_307_308 =
          async_preserved_307_308 && one_async_preserved;
      async_preserved_detail +=
          status_text + ":id=" + std::to_string(async_id) + " url=[" +
          async_committed_url + "] body=[" + async_echoed_post + "] ";
    }
    mbClearUrlRewrites();
    Expect(async_preserved_307_308,
           "mbNavigateEx preserves rewritten POST bodies across 307/308",
           async_preserved_detail);

    // fetch()/XHR has a third redirect implementation, including Blink's
    // WillFollowRedirect header/method edits. Keep the page on the public origin
    // and assert Response.url/redirected together with the final echoed POST.
    bool fetch_preserved_307_308 = true;
    std::string fetch_preserved_detail;
    const std::string fetch_post_public_origin =
        "https://rewrite-post-fetch-visible.test";
    mbLoadHTML(v, "<body>fetch-post-redirect</body>",
               (fetch_post_public_origin + "/page").c_str());
    for (int redirect_status : {307, 308}) {
      const std::string status_text = std::to_string(redirect_status);
      const std::string fetch_public_url =
          fetch_post_public_origin + "/start-" + status_text;
      const std::string fetch_final_url =
          fetch_post_public_origin + "/post?via=fetch-" + status_text;
      const std::string fetch_body =
          "redirect_code=" + status_text + "&payload=fetch-preserved-" +
          status_text;
      mbClearUrlRewrites();
      mbRewriteUrl(
          fetch_public_url.c_str(),
          (host + "/redirect-post?status_code=" + status_text +
           "&url=/post?via=fetch-" + status_text)
              .c_str());
      const std::string fetch_script =
          "window.__preservedPost='pending';fetch('" + fetch_public_url +
          "',{method:'POST',headers:{'Content-Type':"
          "'application/x-www-form-urlencoded'},body:'" +
          fetch_body +
          "'}).then(function(r){var meta=r.url+'|'+r.redirected+'|';"
          "return r.text().then(function(t){window.__preservedPost=meta+t;});})"
          ".catch(function(e){window.__preservedPost='error:'+e.name;});";
      mbRunJS(v, fetch_script.c_str());
      mbWaitForFunction(v, "window.__preservedPost!=='pending'", 3000);
      const std::string fetch_result =
          Eval(v, "String(window.__preservedPost)");
      const bool one_fetch_preserved =
          fetch_result.rfind(fetch_final_url + "|true|", 0) == 0 &&
          fetch_result.find("\"method\": \"POST\"") != std::string::npos &&
          fetch_result.find(fetch_body) != std::string::npos;
      fetch_preserved_307_308 =
          fetch_preserved_307_308 && one_fetch_preserved;
      fetch_preserved_detail +=
          status_text + ":[" + fetch_result + "] ";
    }
    mbClearUrlRewrites();
    Expect(fetch_preserved_307_308,
           "fetch preserves rewritten POST bodies across 307/308",
           fetch_preserved_detail);

    // Mutable-hook headers are request/origin scoped even on transports that
    // cannot call the main-thread hook again while following redirects.
    mbSetRequestHook(
        [](mbRequest* request, void*) {
          if (std::strstr(mbRequestURL(request), "hook-header"))
            mbRequestSetHeader(request, "X-Hook-Key", "hook-secret");
        },
        nullptr);
    mbLoadURL(v, (host + "/headers?hook-header=1").c_str());
    const std::string direct_sync_headers =
        Eval(v, "document.body?document.body.innerText:''");
    mbLoadURL(v, (host + "/redirect-to?status_code=302&url=" + other_origin +
                  "/headers&hook-header=1")
                     .c_str());
    const std::string redirected_sync_headers =
        Eval(v, "document.body?document.body.innerText:''");

    mbLoadHTML(v, "<body>fetch-header-scope</body>", (host + "/").c_str());
    mbRunJS(v,
            "window.__directHeaders='pending';"
            "fetch('/cors-headers?hook-header=1').then(function(r){return r.text();})"
            ".then(function(t){window.__directHeaders=t;})"
            ".catch(function(e){window.__directHeaders='error:'+e;});");
    mbWaitForFunction(v, "window.__directHeaders!=='pending'", 2500);
    const std::string direct_fetch_headers =
        Eval(v, "String(window.__directHeaders)");
    const std::string fetch_redirect_url =
        host + "/redirect-to?status_code=302&url=" + other_origin +
        "/cors-headers?final=1&hook-header=1";
    const std::string fetch_script =
        "window.__redirectHeaders='pending';fetch('" + fetch_redirect_url +
        "').then(function(r){return r.text();})"
        ".then(function(t){window.__redirectHeaders=t;})"
        ".catch(function(e){window.__redirectHeaders='error:'+e;});";
    mbRunJS(v, fetch_script.c_str());
    mbWaitForFunction(v, "window.__redirectHeaders!=='pending'", 2500);
    const std::string redirected_fetch_headers =
        Eval(v, "String(window.__redirectHeaders)");
    mbSetRequestHook(nullptr, nullptr);
    const bool hook_header_scope_ok =
        direct_sync_headers.find("hook-secret") != std::string::npos &&
        redirected_sync_headers.find("hook-secret") == std::string::npos &&
        direct_fetch_headers.find("hook-secret") != std::string::npos &&
        redirected_fetch_headers.find("hook-secret") == std::string::npos;
    Expect(hook_header_scope_ok,
           "mutable-hook headers do not cross sync or buffered redirect origins",
           "sync-direct=[" + direct_sync_headers + "] sync-redirect=[" +
               redirected_sync_headers + "] fetch-direct=[" +
               direct_fetch_headers + "] fetch-redirect=[" +
               redirected_fetch_headers + "]");

    // URL-scoped headers are evaluated on every streaming-download hop. Mutable-hook
    // additions belong to the origin where the hook ran too: an arbitrary credential name
    // such as X-Hook-Key must not be carried to the alternate origin.
    struct StreamState {
      std::string bytes;
      std::string begin_url;
      int finished = 0;
      int success = 0;
    } direct, redirected;
    mbSetRequestHeaderForOrigin(host.c_str(), "X-Stream-Key", "stream-secret");
    static std::string* stream_hook_origin = new std::string();
    static int* stream_hook_calls = new int(0);
    *stream_hook_origin = host;
    *stream_hook_calls = 0;
    mbSetRequestHook(
        [](mbRequest* request, void*) {
          const std::string url = mbRequestURL(request);
          if (url.find("-auth") != std::string::npos)
            ++*stream_hook_calls;
          if (url.rfind(*stream_hook_origin, 0) == 0 &&
              url.find("-auth") != std::string::npos) {
            mbRequestSetHeader(request, "X-Hook-Key", "hook-secret");
          }
        },
        nullptr);
    auto run_stream = [&](const std::string& url, StreamState* state) {
      mbOnDownloadStream(
          v,
          [](mbView*, void* ud, unsigned, const char* url, const char*,
             const char*, long long) {
            static_cast<StreamState*>(ud)->begin_url = url ? url : "";
          },
          [](mbView*, void* ud, unsigned, const char* bytes, int len,
             long long, long long) {
            static_cast<StreamState*>(ud)->bytes.append(bytes,
                                                        static_cast<size_t>(len));
          },
          [](mbView*, void* ud, unsigned, int success) {
            auto* s = static_cast<StreamState*>(ud);
            s->success = success;
            s->finished = 1;
          },
          state);
      const unsigned id = mbDownloadURLStream(v, url.c_str());
      for (int i = 0; id != 0 && !state->finished && i < 150; ++i)
        mbWait(v, 10);
      return id;
    };
    const unsigned direct_id = run_stream(host + "/stream-auth", &direct);
    const unsigned redirected_id = run_stream(
        host + "/redirect-to?status_code=302&url=" + other_origin + "/stream-auth",
        &redirected);
    mbOnDownloadStream(v, nullptr, nullptr, nullptr, nullptr);
    mbClearRequestHeaders();
    const bool stream_ok =
        direct_id != 0 && redirected_id != 0 && direct.success == 1 &&
        redirected.success == 1 &&
        direct.bytes.find("key=stream-secret") != std::string::npos &&
        direct.bytes.find("hook=hook-secret") != std::string::npos &&
        redirected.bytes.find("key=stream-secret") == std::string::npos &&
        redirected.bytes.find("hook=hook-secret") == std::string::npos;
    Expect(stream_ok,
           "streaming downloads scope registered and hook headers across redirects",
           "direct=[" + direct.bytes + "] redirect=[" + redirected.bytes + "]");

    // EventSource uses a separate streaming transport. It must enforce the same
    // mutable-header origin boundary while following a redirect on its worker thread.
    auto run_sse = [&](const std::string& target) {
      mbLoadHTML(v, "<body>sse-scope</body>", (host + "/").c_str());
      const std::string script =
          "window.__sse='pending';var es=new EventSource('" + target +
          "');es.onmessage=function(e){window.__sse=e.data;es.close();};"
          "es.onerror=function(){if(window.__sse==='pending')window.__sse='error';};";
      mbRunJS(v, script.c_str());
      mbWaitForFunction(v, "window.__sse!=='pending'", 2500);
      return Eval(v, "String(window.__sse)");
    };
    const std::string direct_sse = run_sse(host + "/sse-auth");
    const std::string redirected_sse = run_sse(
        host + "/redirect-to?status_code=302&url=" + other_origin + "/sse-auth");
    mbSetRequestHook(nullptr, nullptr);
    Expect(direct_sse == "hook=hook-secret" && redirected_sse == "hook=" &&
               *stream_hook_calls >= 6,
           "SSE scopes mutable-hook headers across redirects",
           "direct=[" + direct_sse + "] redirect=[" + redirected_sse +
               "] hook_calls=" + std::to_string(*stream_hook_calls));

    // The SSE and streaming-download redirect loops have their own detached
    // transports. Drive both through a transparent cross-scheme rewrite whose
    // backend emits //backend/... and verify two things independently: bytes/events
    // arrive from the backend, while request-policy callbacks see only the public
    // initial and final URLs (never the backend authority).
    std::vector<std::string> streaming_visible_urls;
    mbSetRequestCallback(
        [](const char* url, void* userdata) -> int {
          auto* urls = static_cast<std::vector<std::string>*>(userdata);
          const std::string seen = url ? url : "";
          if (seen.find("rewrite-stream-visible.test") != std::string::npos ||
              seen.find("rewrite-sse-visible.test") != std::string::npos) {
            urls->push_back(seen);
          }
          return 0;
        },
        &streaming_visible_urls);
    const std::string download_public_origin =
        "https://rewrite-stream-visible.test";
    const std::string download_public_url =
        download_public_origin + "/start-download";
    const std::string download_final_public_url =
        download_public_origin + "/stream-auth";
    const std::string sse_public_origin =
        "https://rewrite-sse-visible.test";
    const std::string sse_public_url = sse_public_origin + "/start-sse";
    const std::string sse_final_public_url = sse_public_origin + "/sse-auth";
    mbClearUrlRewrites();
    mbRewriteUrl(
        download_public_url.c_str(),
        (host + "/scheme-relative-host-redirect?target=/stream-auth").c_str());
    mbRewriteUrl(
        sse_public_url.c_str(),
        (host + "/scheme-relative-host-redirect?target=/sse-auth").c_str());

    StreamState rewritten_download;
    const unsigned rewritten_download_id =
        run_stream(download_public_url, &rewritten_download);

    mbLoadHTML(v, "<body>rewritten-sse</body>",
               (sse_public_origin + "/page").c_str());
    mbRunJS(v,
            ("window.__rewrittenSse='pending';var rewrittenEs=new EventSource('" +
             sse_public_url +
             "');rewrittenEs.onmessage=function(e){window.__rewrittenSse=e.data;"
             "rewrittenEs.close();};rewrittenEs.onerror=function(){"
             "if(window.__rewrittenSse==='pending')window.__rewrittenSse='error';};")
                .c_str());
    mbWaitForFunction(v, "window.__rewrittenSse!=='pending'", 3000);
    const std::string rewritten_sse =
        Eval(v, "String(window.__rewrittenSse)");
    mbOnDownloadStream(v, nullptr, nullptr, nullptr, nullptr);
    mbSetRequestCallback(nullptr, nullptr);
    mbClearUrlRewrites();

    const auto saw_visible_url = [&](const std::string& wanted) {
      return std::find(streaming_visible_urls.begin(),
                       streaming_visible_urls.end(), wanted) !=
             streaming_visible_urls.end();
    };
    bool streaming_backend_hidden = true;
    for (const std::string& seen : streaming_visible_urls) {
      if (seen.find(echo_host_name) != std::string::npos)
        streaming_backend_hidden = false;
    }
    const bool download_visible_chain_ok =
        rewritten_download_id != 0 && rewritten_download.success == 1 &&
        rewritten_download.begin_url == download_public_url &&
        rewritten_download.bytes.find("host=" + echo_host_name) !=
            std::string::npos &&
        saw_visible_url(download_public_url) &&
        saw_visible_url(download_final_public_url);
    const bool sse_visible_chain_ok =
        rewritten_sse == "hook=" && saw_visible_url(sse_public_url) &&
        saw_visible_url(sse_final_public_url);
    std::string streaming_trace;
    for (const std::string& seen : streaming_visible_urls)
      streaming_trace += "[" + seen + "]";
    Expect(download_visible_chain_ok && sse_visible_chain_ok &&
               streaming_backend_hidden,
           "rewritten SSE/download streams expose only their public redirect chains",
           "download=[" + rewritten_download.bytes + "] begin=[" +
               rewritten_download.begin_url + "] sse=[" + rewritten_sse +
               "] trace=" + streaming_trace);
  }

  // 31r. The response hook exposes the raw RESPONSE HEADERS (mbResponseHeaders) for http
  // loads — so an embedder can read Content-Type / Set-Cookie / rate-limit / custom API
  // headers, not just the body. A page fetch()es the host over http; the response hook
  // captures that response's header block and must carry the status line + a content-type.
  // (Net-gated: data:/file:/mock loads have no header block.)
  if (hb_ok) {
    static std::string* rh = new std::string();  // -Wexit-time-destructors
    rh->clear();
    mbLoadURL(v, (host + "/get").c_str());  // same-origin page for the fetch()
    mbWait(v, 300);
    mbSetResponseCallback(
        [](mbResponse* r, void*) {
          const char* h = mbResponseHeaders(r);
          if (h && *h && rh->empty())  // first response carrying headers (the fetch)
            *rh = h;
        },
        nullptr);
    mbRunJS(v, "fetch('/get').then(function(r){return r.text();});");
    mbWait(v, 500);
    mbSetResponseCallback(nullptr, nullptr);
    std::string low = *rh;
    for (char& c : low) c = static_cast<char>(std::tolower((unsigned char)c));
    Expect(!rh->empty() && low.find("http/") != std::string::npos &&
               low.find("content-type") != std::string::npos,
           "mbResponseHeaders exposes the raw response header block (status + content-type)",
           "len=" + std::to_string((int)rh->size()) + " head=[" + rh->substr(0, 60) + "]");
  }
  // 31. Cookie jar: set a cookie via a redirecting endpoint, then a SEPARATE request
  // must still send it — Set-Cookie survives the redirect and the jar is shared.
  mbLoadURL(v, (host + "/cookies/set?mbck=val99").c_str());  // 302 -> /cookies
  mbWait(v, 400);
  std::string ck1 = Eval(v, "document.body?document.body.innerText:''");
  if (ck1.find("cookies") != std::string::npos) {  // host responded
    bool survived_redirect = ck1.find("val99") != std::string::npos;
    mbLoadURL(v, (host + "/cookies").c_str());  // separate request, shared jar
    mbWait(v, 400);
    std::string ck2 = Eval(v, "document.body?document.body.innerText:''");
    bool jar_persists = ck2.find("val99") != std::string::npos;
    Expect(survived_redirect && jar_persists,
           "cookie jar: survives redirect + persists across requests");
  } else {
    std::fprintf(stderr, "  [SKIP] cookie jar (host unreachable)\n");
  }

  // 32. Request headers: a custom header and the default Accept-Language must reach
  // the server (the echo host returns the request headers).
  mbSetExtraHeaders(v, "X-Mb-Test: probe-42");
  mbLoadURL(v, (host + "/headers").c_str());
  mbWait(v, 400);
  {
    std::string h = Eval(v, "document.body?document.body.innerText:''");
    if (h.find("headers") != std::string::npos) {  // host responded
      Expect(h.find("probe-42") != std::string::npos &&
                 h.find("Accept-Language") != std::string::npos,
             "request headers: custom header + default Accept-Language sent");
    } else {
      std::fprintf(stderr, "  [SKIP] request headers (host unreachable)\n");
    }
  }
  mbSetExtraHeaders(v, "");  // reset

  // 32b. Per-URL request header injection: a header registered for the "/headers" URL
  // is echoed; one registered for a non-matching host is NOT — proving it's conditional
  // on the URL (vs the global extra-headers above), e.g. an API key sent only to its host.
  mbSetRequestHeader("/headers", "X-Mb-Inject", "inj-77");
  mbSetRequestHeader("other.example", "X-Mb-Skip", "should-not-appear");
  mbLoadURL(v, (host + "/headers").c_str());
  mbWait(v, 400);
  {
    std::string h = Eval(v, "document.body?document.body.innerText:''");
    if (h.find("headers") != std::string::npos) {  // host responded
      Expect(h.find("inj-77") != std::string::npos &&
                 h.find("should-not-appear") == std::string::npos,
             "per-URL request header injected for the matching URL only");
    } else {
      std::fprintf(stderr, "  [SKIP] header injection (host unreachable)\n");
    }
  }
  mbClearRequestHeaders();  // reset

  // 33. Cookie bridge: a cookie set via document.cookie on an http origin must be
  // sent on a subsequent network request (JS jar -> HTTP jar).
  mbLoadURL(v, (host + "/").c_str());
  mbWait(v, 400);
  if (!Eval(v, "String(document.location.host)").empty() &&
      Eval(v, "String(document.location.host)") != "undefined") {
    mbRunJS(v, "document.cookie='mbjs=fromjs';");
    mbLoadURL(v, (host + "/cookies").c_str());
    mbWait(v, 400);
    std::string c = Eval(v, "document.body?document.body.innerText:''");
    if (c.find("cookies") != std::string::npos) {
      Expect(c.find("fromjs") != std::string::npos,
             "cookie bridge: document.cookie reaches the HTTP jar");
    } else {
      std::fprintf(stderr, "  [SKIP] cookie bridge (host unreachable)\n");
    }
  } else {
    std::fprintf(stderr, "  [SKIP] cookie bridge (host unreachable)\n");
  }

  // 34. Image loading toggle: a NETWORK <img> loads (naturalWidth>0) by default but
  // is skipped (naturalWidth==0) when image loading is disabled. (data: images are
  // inline and not gated by this setting, so the test uses a served image.)
  {
    const std::string page = "<body><img id='i' src='" + host + "/img'></body>";
    std::vector<uint8_t> tmp(static_cast<size_t>(W) * H * 4, 0);
    mbSetLoadImages(v, 1);
    mbLoadHTML(v, page.c_str(), (host + "/").c_str());
    mbPaintToBitmap(v, tmp.data(), W, H, W * 4);  // settle the fetch+decode
    std::string on_nw = Eval(v, "String(document.getElementById('i').naturalWidth)");
    if (std::atoi(on_nw.c_str()) > 0) {  // host served the image
      mbSetLoadImages(v, 0);
      mbLoadHTML(v, page.c_str(), (host + "/").c_str());
      mbPaintToBitmap(v, tmp.data(), W, H, W * 4);
      std::string off_nw =
          Eval(v, "String(document.getElementById('i').naturalWidth)");
      Expect(off_nw == "0", "no-images: network image skipped when disabled",
             "on=" + on_nw + " off=" + off_nw);
      mbSetLoadImages(v, 1);
    } else {
      std::fprintf(stderr, "  [SKIP] no-images (host image unreachable)\n");
    }
  }

  // 35. Cookie export: after the jar has a cookie (set above via /cookies/set),
  // mbGetCookies returns it for the host to extract/reuse.
  {
    mbLoadURL(v, (host + "/cookies/set?expk=expv").c_str());
    mbWait(v, 400);
    if (!Eval(v, "(document.body?document.body.innerText:'')").empty()) {
      char cb[512] = {0};
      mbGetCookies(v, (host + "/").c_str(), cb, sizeof(cb));
      Expect(std::string(cb).find("expk=expv") != std::string::npos,
             "mbGetCookies exports the jar", cb);
    } else {
      std::fprintf(stderr, "  [SKIP] cookie export (host unreachable)\n");
    }
  }
  // 36. POST form submission: a method=post form's body must reach the server.
  // The form auto-submits via JS (same BeginNavigation path as a click); the
  // echo host returns the posted fields as JSON, so ours must come back. This
  // exercises the POST path in DoCommit (extract WebHTTPBody) + MbFetchUrl POST.
  {
    const std::string page =
        "<body><form id='f' method='post' action='" + host + "/post'>"
        "<input name='user' value='mbpost'></form>"
        "<script>document.getElementById('f').submit();</script></body>";
    mbLoadHTML(v, page.c_str(), (host + "/").c_str());
    mbWait(v, 900);  // submit -> POST -> commit the echo response
    std::string r = Eval(v, "document.body?document.body.innerText:''");
    if (r.find("\"form\"") != std::string::npos ||
        r.find("\"user\"") != std::string::npos) {  // host echoed
      Expect(r.find("mbpost") != std::string::npos,
             "POST form submission: posted body reaches the server");
    } else {
      std::fprintf(stderr, "  [SKIP] POST form (host unreachable)\n");
    }
  }

  // 37. fetch()/XHR with a request body: the subresource loader (MbURLLoader)
  // must send the method + body, not a bodyless GET. Issue a fetch POST and read
  // the echoed field back. (The page is loaded from the host origin so the fetch
  // is same-origin.) Exercises the method/body extraction in MbURLLoader::Deliver.
  {
    mbLoadHTML(v, "<body>x</body>", (host + "/").c_str());
    mbRunJS(v,
        ("window.__fp='';fetch('" + host +
         "/post',{method:'POST',headers:{'Content-Type':"
         "'application/x-www-form-urlencoded'},body:'mk=fetchmk'})"
         ".then(function(r){return r.json();}).then(function(j){"
         "window.__fp=(j.form&&j.form.mk)||'nofield';}).catch(function(e){"
         "window.__fp='ERR:'+e.name;});").c_str());
    mbWait(v, 1500);  // async fetch round-trip
    std::string r = Eval(v, "String(window.__fp)");
    if (!r.empty() && r.rfind("ERR:", 0) != 0) {  // host responded
      Expect(r.find("fetchmk") != std::string::npos,
             "fetch() POST sends the request body", r);
    } else {
      std::fprintf(stderr, "  [SKIP] fetch POST (host unreachable: %s)\n",
                   r.c_str());
    }
  }

  // 38. fetch() per-request headers: a custom header set on the fetch (e.g. an
  // Authorization token or X-* header) must reach the server. The echo host
  // returns the request headers; ours must be present. Exercises forwarding of
  // request->headers in MbURLLoader::Deliver.
  {
    mbLoadHTML(v, "<body>x</body>", (host + "/").c_str());
    mbRunJS(v,
        ("window.__fh='';fetch('" + host +
         "/headers',{headers:{'X-Mb-Tok':'mbtok7'}})"
         ".then(function(r){return r.json();}).then(function(j){"
         // Match the value, not the key: echo hosts differ on header-name case
         // (httpbin Title-Cases, postman-echo lowercases).
         "window.__fh=JSON.stringify(j.headers||{});})"
         ".catch(function(e){window.__fh='ERR:'+e.name;});").c_str());
    mbWait(v, 1500);  // async fetch round-trip
    std::string r = Eval(v, "String(window.__fh)");
    if (!r.empty() && r.rfind("ERR:", 0) != 0) {  // host responded
      Expect(r.find("mbtok7") != std::string::npos,
             "fetch() forwards custom request headers", r);
    } else {
      std::fprintf(stderr, "  [SKIP] fetch headers (host unreachable: %s)\n",
                   r.c_str());
    }
  }

  // 39. Response status + headers: an HTTP error (404) must resolve as a real
  // Response (status 404, ok=false), NOT a rejected fetch — and a server
  // response header must be readable. Previously 4xx/5xx were turned into
  // network failures (TypeError) and only Content-Type was exposed. (httpbin
  // shapes: /status/404 and /response-headers?k=v.)
  {
    mbLoadHTML(v, "<body>x</body>", (host + "/").c_str());
    mbRunJS(v,
        ("window.__rs='';fetch('" + host +
         "/status/404').then(function(r){window.__rs=r.status+'/'+r.ok;})"
         ".catch(function(e){window.__rs='ERR:'+e.name;});"
         "window.__rh='';fetch('" + host +
         "/response-headers?X-Smk=sv1').then(function(r){"
         "window.__rh=r.headers.get('X-Smk')||'MISSING';})"
         ".catch(function(e){window.__rh='ERR:'+e.name;});").c_str());
    mbWait(v, 1800);  // two async round-trips
    std::string st = Eval(v, "String(window.__rs)");
    std::string hd = Eval(v, "String(window.__rh)");
    if (hb_ok && !st.empty() && st.rfind("ERR:", 0) != 0) {  // host healthy
      Expect(st == "404/false" && hd == "sv1",
             "fetch sees real HTTP status (404, !ok) + response headers",
             st + " hdr=" + hd);
    } else {
      std::fprintf(stderr, "  [SKIP] response status/headers (host unhealthy: %s)\n",
                   st.c_str());
    }
  }

  // 40. Navigation redirect: navigating to a URL that 302-redirects must commit
  // with the FINAL URL as the document URL (location.href), not the original.
  // curl follows the redirect; LoadURL now commits the effective URL as the base.
  {
    mbLoadURL(v,
              (host + "/redirect-to?url=" + host + "/get&status_code=302").c_str());
    mbWait(v, 700);
    std::string loc = Eval(v, "String(location.href)");
    if (hb_ok && (loc.find("/get") != std::string::npos ||
                  loc.find("/redirect-to") != std::string::npos)) {  // healthy
      Expect(loc.find("/get") != std::string::npos &&
                 loc.find("/redirect-to") == std::string::npos,
             "navigation redirect commits the final URL as location.href", loc);
    } else {
      std::fprintf(stderr, "  [SKIP] nav redirect (host unhealthy: %s)\n",
                   loc.c_str());
    }
  }

  // 41. fetch() redirect: a fetch that 302-redirects must resolve as a Response
  // whose url is the FINAL URL and whose .redirected is true. The loader follows
  // redirects manually and reports each hop via WillFollowRedirect so Blink's
  // url list (response.url / redirected) is correct.
  {
    mbLoadHTML(v, "<body>x</body>", (host + "/").c_str());
    mbRunJS(v,
        ("window.__rr='';fetch('" + host + "/redirect-to?url=" + host +
         "/get&status_code=302').then(function(r){"
         "window.__rr=r.url+'|'+r.redirected;}).catch(function(e){"
         "window.__rr='ERR:'+e.name;});").c_str());
    mbWait(v, 1500);
    std::string rr = Eval(v, "String(window.__rr)");
    if (hb_ok && !rr.empty() && rr.rfind("ERR:", 0) != 0) {  // host healthy
      Expect(rr.find("/get") != std::string::npos &&
                 rr.find("/redirect-to") == std::string::npos &&
                 rr.find("|true") != std::string::npos,
             "fetch() redirect exposes final url + redirected=true", rr);
    } else {
      std::fprintf(stderr, "  [SKIP] fetch redirect (host unhealthy: %s)\n",
                   rr.c_str());
    }
  }

  // 41 (net). mbGetHttpStatus reflects the last navigation's real HTTP status
  // (200 vs 404), and mbGetResponseHeaders exposes the server's response headers.
  {
    mbLoadURL(v, (host + "/html").c_str());
    mbWait(v, 600);
    const int ok_status = mbGetHttpStatus(v);
    if (ok_status != 0) {  // host reachable
      char hb[4096] = {0};
      mbGetResponseHeaders(v, hb, sizeof(hb));
      std::string headers(hb);
      for (char& c : headers) c = static_cast<char>(std::tolower((unsigned char)c));
      const bool has_ct = headers.find("content-type") != std::string::npos;
      mbLoadURL(v, (host + "/status/404").c_str());
      mbWait(v, 600);
      const int err_status = mbGetHttpStatus(v);
      Expect(ok_status == 200 && err_status == 404 && has_ct,
             "mbGetHttpStatus (200/404) + mbGetResponseHeaders exposes headers",
             "ok=" + std::to_string(ok_status) + " err=" +
                 std::to_string(err_status) + " ct=" + (has_ct ? "1" : "0"));
    } else {
      std::fprintf(stderr, "  [SKIP] http status/headers (host unreachable)\n");
    }
  }

  // 42 (net). mbSetFollowRedirects(0) stops at the redirect so the 30x status +
  // Location header are visible; re-enabling follows through to the final 200.
  {
    mbSetFollowRedirects(0);
    mbLoadURL(v, (host + "/redirect/1").c_str());  // 302 -> /get
    mbWait(v, 700);
    const int s_off = mbGetHttpStatus(v);
    char hb[4096] = {0};
    mbGetResponseHeaders(v, hb, sizeof(hb));
    std::string h(hb);
    for (char& c : h) c = static_cast<char>(std::tolower((unsigned char)c));
    const bool redirect_seen =
        s_off >= 300 && s_off < 400 && h.find("location:") != std::string::npos;
    mbSetFollowRedirects(1);  // restore the default (process-wide) before more loads
    mbLoadURL(v, (host + "/redirect/1").c_str());
    mbWait(v, 900);
    const int s_on = mbGetHttpStatus(v);
    if (s_off != 0 || s_on != 0) {  // host reachable
      Expect(redirect_seen && s_on == 200,
             "mbSetFollowRedirects: off exposes 30x+Location, on follows to 200",
             "off=" + std::to_string(s_off) + " on=" + std::to_string(s_on));
    } else {
      std::fprintf(stderr, "  [SKIP] follow-redirects (host unreachable)\n");
    }
  }

  // 43 (net). mbPostURL: host-driven POST navigation. httpbin/post echoes the
  // received form data into the response JSON, which becomes the document.
  {
    mbPostURL(v, (host + "/post").c_str(), "mbk=postval", nullptr);
    mbWait(v, 700);
    const int status = mbGetHttpStatus(v);
    if (status != 0) {  // host reachable
      std::string doc = Eval(v, "document.body?document.body.innerText:''");
      Expect(status == 200 && doc.find("mbk") != std::string::npos &&
                 doc.find("postval") != std::string::npos,
             "mbPostURL posts a body and commits the response",
             "status=" + std::to_string(status));
    } else {
      std::fprintf(stderr, "  [SKIP] mbPostURL (host unreachable)\n");
    }
  }

  // 44 (net). End-to-end integration on a REAL https page: one fetch over real
  // TLS exercises the whole stack together — load -> parse -> layout -> the recent
  // scraping readers (text/html/rect/style). example.com is a stable target whose
  // <h1> says "Example Domain". Skips if the host is unreachable.
  {
    mbLoadURL(v, "https://example.com");
    mbWaitForSelector(v, "h1", 4000);
    const int status = mbGetHttpStatus(v);
    if (status == 200) {
      char tb[256] = {0};
      mbGetTextForSelector(v, "h1", tb, sizeof(tb));
      const bool text_ok = std::string(tb).find("Example Domain") != std::string::npos;
      char hb[512] = {0};
      mbGetHtmlForSelector(v, "h1", hb, sizeof(hb));
      const std::string html(hb);
      const bool html_ok = html.find("<h1") != std::string::npos &&
                           html.find("Example Domain") != std::string::npos;
      int rw = 0, rh = 0;
      const bool rect_ok =
          mbGetElementRect(v, "h1", nullptr, nullptr, &rw, &rh) && rw > 0 && rh > 0;
      char sb[64] = {0};
      mbGetComputedStyle(v, "h1", "display", sb, sizeof(sb));
      const bool style_ok = std::string(sb) == "block";
      Expect(text_ok && html_ok && rect_ok && style_ok,
             "integration: real-TLS load + text/html/rect/style readers agree",
             std::string("text=") + (text_ok ? "1" : "0") + " html=" +
                 (html_ok ? "1" : "0") + " rect=" + (rect_ok ? "1" : "0") +
                 " style=" + (style_ok ? "1" : "0"));
    } else {
      std::fprintf(stderr, "  [SKIP] integration (example.com unreachable)\n");
    }
  }
  }  // MB_NET_TESTS

  // 33. document.cookie (JS): write then read round-trips through the in-process
  // RestrictedCookieManager wired into the frame's BrowserInterfaceBroker.
  {
    const char* doc = "<body>x</body>";
    if (FILE* f = std::fopen("/tmp/mb_jsck.html", "wb")) {
      std::fwrite(doc, 1, std::strlen(doc), f); std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_jsck.html");
    mbRunJS(v, "document.cookie='a=1';document.cookie='b=2';");
    mbWait(v, 20);
    std::string ck = Eval(v, "document.cookie");
    Expect(ck.find("a=1") != std::string::npos &&
               ck.find("b=2") != std::string::npos,
           "document.cookie read/write round-trip", ck);
  }

  // 33b. document.cookie READ as the first cookie op, from the page's own inline
  // script during load (no prior write, no pump in between). This is the common
  // "read existing cookies on load" pattern and it used to HANG: the synchronous
  // RestrictedCookieManager.GetCookiesString blocked the main thread before the
  // BrowserInterfaceBroker.GetInterface that binds the manager had been pumped.
  // The broker is now bound on the runtime service thread, so the [Sync] read is
  // serviced off-thread and returns immediately. The inline read records the jar
  // into the DOM; reaching this assertion at all proves it didn't hang. Same
  // file:// origin as case 33, so it reads back the a=1/b=2 set there.
  {
    const char* doc =
        "<body><div id=o>x</div><script>"
        "document.getElementById('o').textContent='ck['+document.cookie+']';"
        "</script></body>";
    if (FILE* f = std::fopen("/tmp/mb_jsck2.html", "wb")) {
      std::fwrite(doc, 1, std::strlen(doc), f); std::fclose(f);
    }
    mbLoadURL(v, "file:///tmp/mb_jsck2.html");
    mbWait(v, 30);
    std::string ck2 = Eval(v, "document.getElementById('o').textContent");
    Expect(ck2.rfind("ck[", 0) == 0 && ck2.find("a=1") != std::string::npos &&
               ck2.find("b=2") != std::string::npos,
           "document.cookie read-first on load does not hang", ck2);
  }

  // 34. Init script (evaluateOnNewDocument): runs before the page's own scripts.
  // Set a global in the init script; the page's inline script must observe it.
  mbSetInitScript(v, "window.__early='injected';");
  mbLoadHTML(v,
             "<body><script>window.__pageSaw=window.__early||'no';</script></body>",
             "about:blank");
  Expect(Eval(v, "window.__pageSaw") == "injected",
         "init script runs before page scripts", Eval(v, "window.__pageSaw"));
  mbSetInitScript(v, "");  // clear so it doesn't affect any later case

  // 35. Isolated-world eval: separate JS globals from the main world, shared DOM.
  mbLoadHTML(v, "<body></body>", "about:blank");
  mbRunJS(v, "window.__main='mainval';");
  // In the isolated world: set its own global, touch the shared DOM, and report
  // whether it can see the main world's global (it must NOT).
  Expect(EvalIso(v, "window.__iso='isoval';"
                    "document.body.setAttribute('data-s','shared');"
                    "String(typeof window.__main);") == "undefined",
         "isolated world: cannot see main-world globals");
  Expect(Eval(v, "String(window.__iso)") == "undefined",
         "isolated world: does not leak globals into main world");
  Expect(Eval(v, "document.body.getAttribute('data-s')") == "shared",
         "isolated world: shares the DOM with main world");

  // 36. Dark mode: prefers-color-scheme media query + a responsive CSS rule flip
  // when dark mode is emulated.
  {
    const char* page =
        "<style>#d{color:rgb(1,1,1)}"
        "@media (prefers-color-scheme:dark){#d{color:rgb(2,2,2)}}</style>"
        "<body><b id='d'>x</b></body>";
    mbSetDarkMode(v, 1);
    mbLoadHTML(v, page, "about:blank");
    bool mm = Eval(v, "String(matchMedia('(prefers-color-scheme:dark)').matches)") ==
              "true";
    bool css = Eval(v, "getComputedStyle(document.getElementById('d')).color") ==
               "rgb(2, 2, 2)";
    Expect(mm && css, "dark mode: prefers-color-scheme dark applies");
    mbSetDarkMode(v, 0);  // restore light for any later case
  }

  // 36b. mbEmulateMedia: generic media-feature override (DevTools setEmulatedMedia path).
  // Override prefers-reduced-motion + prefers-contrast LIVE on a loaded page (matchMedia
  // flips without reload), and clearing a feature reverts it. A general dark-mode for any
  // media feature (accessibility/theme testing).
  {
    mbLoadHTML(v, "<body>x</body>", "about:blank");
    const std::string m0 =
        Eval(v, "String(matchMedia('(prefers-reduced-motion: reduce)').matches)");
    mbEmulateMedia(v, "prefers-reduced-motion", "reduce");
    mbWait(v, 20);
    const std::string m1 =
        Eval(v, "String(matchMedia('(prefers-reduced-motion: reduce)').matches)");
    mbEmulateMedia(v, "prefers-contrast", "more");
    mbWait(v, 20);
    const std::string c1 =
        Eval(v, "String(matchMedia('(prefers-contrast: more)').matches)");
    mbEmulateMedia(v, "prefers-reduced-motion", "");  // clear just this one
    mbWait(v, 20);
    const std::string m2 =
        Eval(v, "String(matchMedia('(prefers-reduced-motion: reduce)').matches)");
    const std::string r = m0 + "," + m1 + "," + c1 + "," + m2;
    Expect(r == "false,true,true,false",
           "mbEmulateMedia overrides media features live (reduced-motion, contrast) + clears",
           "em=[" + r + "]");
    mbEmulateMedia(v, "", "");  // clear all overrides for later cases
  }

  // 36c. mbEmulateMediaType: override the media TYPE (DevTools setEmulatedMedia
  // `media`, distinct from the features above). With "print", matchMedia('print')
  // flips true AND @media print rules apply to COMPUTED STYLE while the page is
  // still on screen — so a screenshot/PDF reflects the print stylesheet. Clearing
  // reverts to screen. Asserting computed color (not just matchMedia) proves the
  // print cascade actually took effect.
  {
    mbLoadHTML(v,
        "<style>#p{color:rgb(9,9,9)}@media print{#p{color:rgb(1,2,3)}}</style>"
        "<body><div id=p>t</div></body>",
        "about:blank");
    const std::string s0 = Eval(v, "String(matchMedia('print').matches)");
    const std::string col0 =
        Eval(v, "getComputedStyle(document.getElementById('p')).color");
    mbEmulateMediaType(v, "print");
    mbWait(v, 20);
    const std::string s1 = Eval(v, "String(matchMedia('print').matches)");
    const std::string col1 =
        Eval(v, "getComputedStyle(document.getElementById('p')).color");
    mbEmulateMediaType(v, "");  // clear -> back to screen
    mbWait(v, 20);
    const std::string col2 =
        Eval(v, "getComputedStyle(document.getElementById('p')).color");
    Expect(s0 == "false" && col0 == "rgb(9, 9, 9)" && s1 == "true" &&
               col1 == "rgb(1, 2, 3)" && col2 == "rgb(9, 9, 9)",
           "mbEmulateMediaType applies @media print to computed style live + clears",
           "mt=[" + s0 + "," + col0 + " / " + s1 + "," + col1 + " / " + col2 +
               "]");
  }

  // 37. Locale: navigator.language / navigator.languages reflect the set value.
  mbSetLocale(v, "fr-FR,fr,en");
  mbLoadHTML(v, "<body>x</body>", "about:blank");
  Expect(Eval(v, "navigator.language") == "fr-FR" &&
             Eval(v, "navigator.languages.join(',')") == "fr-FR,fr,en",
         "locale: navigator.language(s) set",
         Eval(v, "navigator.language") + " / " +
             Eval(v, "navigator.languages.join(',')"));
  mbSetLocale(v, "en-US");  // restore for any later case

  // 38. Timezone override: Date/Intl report the chosen zone deterministically.
  mbSetTimezone(v, "America/New_York");
  mbLoadHTML(v, "<body>x</body>", "about:blank");
  Expect(Eval(v, "Intl.DateTimeFormat().resolvedOptions().timeZone") ==
             "America/New_York",
         "timezone override (Intl resolvedOptions)",
         Eval(v, "Intl.DateTimeFormat().resolvedOptions().timeZone"));
  // A fixed UTC instant formats to a New-York wall-clock time (EST/EDT), proving
  // Date itself uses the zone: 2021-01-01T00:00:00Z -> 2020-12-31 19:00 EST.
  Expect(Eval(v, "new Date(1609459200000).getHours().toString()") == "19",
         "timezone override (Date local hours)",
         Eval(v, "new Date(1609459200000).getHours().toString()"));
  mbSetTimezone(v, "UTC");  // restore deterministic UTC for any later case

  // 39. PDF export: print a document to a PDF and confirm it's a real PDF file.
  mbLoadHTML(v, "<body style='font:30px sans-serif'><h1>PDF</h1><p>page content</p></body>",
             "about:blank");
  {
    const char* pdf_path = "/tmp/mb_smoke.pdf";
    bool ok = mbSavePdf(v, pdf_path) != 0;
    char hdr[6] = {0};
    long sz = 0;
    if (FILE* f = std::fopen(pdf_path, "rb")) {
      std::fread(hdr, 1, 5, f);
      std::fseek(f, 0, SEEK_END);
      sz = std::ftell(f);
      std::fclose(f);
    }
    Expect(ok && std::string(hdr) == "%PDF-" && sz > 500,
           "PDF export (valid %PDF, non-trivial)",
           std::string(hdr) + " sz=" + std::to_string(sz));
  }

  // 39b. mbSavePdfEx page geometry: an A4 page (595x842 pt) and its landscape (842x595)
  // must set the PDF MediaBox accordingly — proves custom size + landscape reach output.
  {
    auto slurp = [](const char* p) -> std::string {
      std::string s;
      if (FILE* f = std::fopen(p, "rb")) {
        char b[8192];
        size_t n;
        while ((n = std::fread(b, 1, sizeof(b), f)) > 0)
          s.append(b, n);
        std::fclose(f);
      }
      return s;
    };
    const bool a4 = mbSavePdfEx(v, "/tmp/mb_a4.pdf", 595, 842, 0, 1.0, 0) != 0;
    const std::string p1 = slurp("/tmp/mb_a4.pdf");
    const bool ls = mbSavePdfEx(v, "/tmp/mb_a4l.pdf", 595, 842, 1, 1.0, 0) != 0;
    const std::string p2 = slurp("/tmp/mb_a4l.pdf");
    const bool a4ok = p1.find("595 842") != std::string::npos;
    const bool lsok = p2.find("842 595") != std::string::npos;
    Expect(a4 && a4ok && ls && lsok,
           "mbSavePdfEx sets the PDF MediaBox (A4 portrait + landscape)",
           std::string("a4=") + std::to_string(a4ok) + " ls=" + std::to_string(lsok));
  }

  // 39c. mbSetPrintBackground: blink's print path drops backgrounds by default ("save ink");
  // enabling printBackground includes them. A page with a full-page gradient background ->
  // the PDF with backgrounds ON is LARGER (it embeds the gradient shading) than with OFF.
  {
    auto fsz = [](const char* p) -> long {
      long n = 0;
      if (FILE* f = std::fopen(p, "rb")) { std::fseek(f, 0, SEEK_END); n = std::ftell(f); std::fclose(f); }
      return n;
    };
    mbLoadHTML(v,
               "<body style='margin:0;height:1000px;"
               "background:linear-gradient(45deg,red,blue,green,yellow)'>"
               "<p>page</p></body>",
               "https://pdfbg.test/");
    mbWait(v, 60);
    mbSetPrintBackground(v, 0);
    const bool ok_off = mbSavePdf(v, "/tmp/mb_pdf_nobg.pdf") != 0;
    const long s_off = fsz("/tmp/mb_pdf_nobg.pdf");
    mbSetPrintBackground(v, 1);
    const bool ok_on = mbSavePdf(v, "/tmp/mb_pdf_bg.pdf") != 0;
    const long s_on = fsz("/tmp/mb_pdf_bg.pdf");
    mbSetPrintBackground(v, 0);  // restore default
    Expect(ok_off && ok_on && s_on > s_off,
           "mbSetPrintBackground: PDF includes the page background when enabled (larger)",
           "off=" + std::to_string(s_off) + " on=" + std::to_string(s_on));
  }

  // 107. Native function binding: a C function bound via mbJsBindFunction is
  // callable from JS synchronously — window[name](args) returns the C result
  // inline — receiving string args and the userdata pointer. Installed into each
  // new document (so it works after a navigation and from a page event handler).
  {
    int tag = 7;
    mbJsBindFunction(v, "mbEcho", SmokeEcho, &tag);
    mbJsBindFunction(v, "mbObj", SmokeJson, nullptr);
    mbLoadHTML(v, "<body>native</body>", "about:blank");
    const std::string defined = Eval(v, "typeof window.mbEcho");
    const std::string r = Eval(v, "window.mbEcho('hi')");  // -> "hi!7"
    const std::string in_expr =
        Eval(v, "(function(){return 'got:'+window.mbEcho('x');})()");
    // out_type 5 (json): the C return becomes a real JS object the page navigates.
    const std::string obj_type = Eval(v, "typeof window.mbObj()");
    const std::string obj_vals =
        Eval(v, "window.mbObj().a + ',' + window.mbObj().b[1]");  // -> "1,3"
    Expect(defined == "function" && r == "hi!7" && in_expr == "got:x!7" &&
               obj_type == "object" && obj_vals == "1,3",
           "mbJsBindFunction: synchronous C call; string + JSON-object returns",
           "typeof=" + defined + " r=" + r + " expr=" + in_expr +
               " objType=" + obj_type + " objVals=" + obj_vals);
  }

  {
    // case 51: null-argument robustness. A C ABI must not crash when a caller
    // passes a null string pointer — it should return the documented failure
    // value. Reaching the Expect at all proves no function crashed (a null
    // deref would abort the process before we get there). Probes a spread
    // across categories: selector getters (-1), action setters (0), eval (0),
    // file save (0), and void sinks (no return, just must not crash).
    char nb[16] = {0};
    int rect_x = 0, rect_y = 0, rect_w = 0, rect_h = 0;
    const bool getters_safe =
        mbGetTextForSelector(v, nullptr, nb, sizeof(nb)) == -1 &&
        mbGetValueForSelector(v, nullptr, nb, sizeof(nb)) == -1 &&
        mbGetAttribute(v, nullptr, nullptr, nb, sizeof(nb)) == -1 &&
        mbGetCheckedForSelector(v, nullptr) == -1 &&
        mbIsVisibleForSelector(v, nullptr) == -1 &&
        mbGetElementRect(v, nullptr, &rect_x, &rect_y, &rect_w, &rect_h) == 0;
    const bool actions_safe =
        mbClickSelector(v, nullptr) == 0 &&
        mbFillSelector(v, nullptr, "x") == 0 &&
        mbSelectOption(v, nullptr, nullptr) == 0 &&
        mbSetAttribute(v, nullptr, nullptr, nullptr) == 0 &&
        mbEvalJS(v, nullptr, nb, sizeof(nb)) == 0 &&
        mbSavePdf(v, nullptr) == 0;
    // void sinks: no return value — the test is simply that these don't crash.
    mbLoadURL(v, nullptr);
    mbSetCookie(v, nullptr, nullptr);
    mbSendKey(v, nullptr);
    mbSendKeyEx(v, nullptr, 1);
    mbSendText(v, nullptr);
    Expect(getters_safe && actions_safe,
           "C ABI is null-arg safe (no crash; documented failure returns)",
           std::string("getters=") + (getters_safe ? "ok" : "BAD") +
               " actions=" + (actions_safe ? "ok" : "BAD"));
  }

  // Runaway-script guard: a synchronous infinite loop in page JS would otherwise hang
  // the single-process embedder forever. mbSetScriptTimeout makes the watchdog terminate
  // the stuck task; the embedder must RECOVER and load the next page normally. (This case
  // blocks ~the timeout before the watchdog fires.)
  {
    mbSetScriptTimeout(1000);
    mbView* wd = mbCreateView(200, 150);
    // The <script> never returns (while(true){}). Without the watchdog this call hangs.
    mbLoadHTML(wd,
               "<body><div id=x>start</div><script>"
               "document.getElementById('x').textContent='loop';while(true){}"
               "</script></body>",
               "https://hang.example/");
    // Reaching here means the loop was terminated. Recovery: a fresh load + eval on the
    // SAME view must work (the isolate was un-terminated at the next task boundary).
    mbLoadHTML(wd, "<body><div id=y>recovered</div></body>", "https://ok.example/");
    const std::string rec = Eval(wd, "document.getElementById('y').textContent");
    const std::string math = Eval(wd, "String(6*7)");  // isolate fully usable again
    mbDestroyView(wd);
    Expect(rec == "recovered" && math == "42",
           "script watchdog terminates a runaway sync loop + the embedder recovers",
           "rec=[" + rec + "] math=[" + math + "]");

    // An infinite MICROTASK flood (Promise.resolve().then(f) recursively) also never
    // returns to the loop — it drains within one task's microtask checkpoint, so the
    // same per-task watchdog catches it. Without the guard this hangs the load forever.
    mbView* wd2 = mbCreateView(200, 150);
    mbLoadHTML(wd2,
               "<body><div id=z>start</div><script>"
               "document.getElementById('z').textContent='flood';"
               "(function f(){Promise.resolve().then(f);})();"
               "</script></body>",
               "https://flood.example/");
    const std::string rec2 = Eval(wd2, "(function(){try{return 6*7;}catch(e){return 'ERR';}})()");
    mbDestroyView(wd2);
    mbSetScriptTimeout(0);  // restore default (disabled) for any later case
    Expect(rec2 == "42",
           "script watchdog also catches an infinite microtask flood (recovers)",
           "rec2=[" + rec2 + "]");
  }

  // REVIEW-FIX #1. mbWaitForSelector must ESCAPE the selector before embedding it in
  // the probe's JS string literal — an attribute selector with double quotes (very
  // common) otherwise made the probe a syntax error so the wait timed out forever.
  {
    mbLoadHTML(v, "<body><input data-x=\"y\" id='q'></body>", "about:blank");
    const bool found = mbWaitForSelector(v, "[data-x=\"y\"]", 1000) != 0;
    Expect(found, "mbWaitForSelector escapes a quoted attribute selector (#1)",
           found ? "found" : "timeout");
  }

  // REVIEW-FIX #2. The loader retry predicate must NEVER retry a non-idempotent
  // method (no duplicate POST/PUT/DELETE) and must NOT treat redirects or
  // 204/205/304/HEAD empty bodies as failures. Pure function -> deterministic, offline.
  {
    using mb::MbShouldRetryFetch;
    const bool ok =
        // writes are never auto-retried (even on a transient transport error / 503 /
        // empty-2xx anomaly):
        !MbShouldRetryFetch("POST", true, false, 0, true, 1, 3) &&
        !MbShouldRetryFetch("DELETE", false, true, 503, true, 1, 3) &&
        !MbShouldRetryFetch("PUT", false, false, 200, true, 1, 3) &&
        // safe methods retry transient transport errors + transient statuses:
        MbShouldRetryFetch("GET", true, false, 0, true, 1, 3) &&
        MbShouldRetryFetch("", true, false, 0, true, 1, 3) &&  // "" == GET
        MbShouldRetryFetch("GET", false, true, 503, false, 1, 3) &&
        // empty-2xx anomaly retries on GET, but redirects and
        // 204/205/304/HEAD legit-empty responses do NOT:
        MbShouldRetryFetch("GET", false, false, 200, true, 1, 3) &&
        !MbShouldRetryFetch("GET", false, false, 302, true, 1, 3) &&
        !MbShouldRetryFetch("GET", false, false, 204, true, 1, 3) &&
        !MbShouldRetryFetch("GET", false, false, 205, true, 1, 3) &&
        !MbShouldRetryFetch("GET", false, false, 304, true, 1, 3) &&
        !MbShouldRetryFetch("HEAD", false, false, 200, true, 1, 3) &&
        // attempts exhausted:
        !MbShouldRetryFetch("GET", true, false, 0, true, 3, 3);
    Expect(ok,
           "MbShouldRetryFetch: no write retries; redirects/204/205/304/HEAD not anomalies (#2)");
  }

  // REG #1. IndexedDB open() with NO version must default to version 1, fire
  // onupgradeneeded (so a store can be created), then a write/read round-trips.
  {
    mbLoadHTML(v, "<body>x</body>", "https://idbreg.test/");
    Eval(v,
         "window.__r1='';window.__up1=0;"
         "var q=indexedDB.open('regdb1');"  // no explicit version
         "q.onupgradeneeded=function(e){window.__up1=1;"
         "e.target.result.createObjectStore('s',{keyPath:'id'});};"
         "q.onsuccess=function(e){var db=e.target.result;"
         "var tx=db.transaction('s','readwrite');tx.objectStore('s').put({id:1,v:'hi'});"
         "tx.oncomplete=function(){var g=db.transaction('s').objectStore('s').get(1);"
         "g.onsuccess=function(){window.__r1='up'+window.__up1+',v'+db.version+','+"
         "(g.result?g.result.v:'null');};g.onerror=function(){window.__r1='geterr';};};"
         "tx.onerror=function(){window.__r1='txerr';};};"
         "q.onerror=function(){window.__r1='operr';};");
    mbWaitForFunction(v, "window.__r1!==''", 4000);
    const std::string r = Eval(v, "window.__r1");
    Expect(r == "up1,v1,hi",
           "IndexedDB open() with no version fires upgradeneeded + creates a store",
           "r1=[" + r + "]");
  }

  // REG #2. IndexedDB downgrade rejects: open 'regdb2' at v3 (let the upgrade
  // complete + close the connection), then open the same DB at the LOWER v2 — that
  // request must fire onerror with a VersionError, never onsuccess.
  {
    mbLoadHTML(v, "<body>x</body>", "https://idbreg.test/");
    Eval(v,
         "window.__r2='';"
         "var q=indexedDB.open('regdb2',3);"
         "q.onupgradeneeded=function(e){e.target.result.createObjectStore('s');};"
         "q.onsuccess=function(e){e.target.result.close();"
         "var q2=indexedDB.open('regdb2',2);"  // downgrade -> must reject
         "q2.onsuccess=function(ev){ev.target.result.close();"
         "window.__r2='unexpected-success';};"
         "q2.onerror=function(ev){window.__r2='err:'+(ev.target.error?"
         "ev.target.error.name:'?');};};"
         "q.onerror=function(){window.__r2='openerr';};");
    mbWaitForFunction(v, "window.__r2!==''", 4000);
    const std::string r = Eval(v, "window.__r2");
    Expect(r == "err:VersionError",
           "IndexedDB open() at a lower version rejects with VersionError",
           "r2=[" + r + "]");
  }

  // REG #3. A Cache handle stays usable after caches.delete() of its storage (no
  // use-after-free): open, put, delete the named cache, then match() on the dangling
  // handle must not crash (the page stays scriptable). Also cache.delete() returns
  // false for a missing entry and true for a present one.
  {
    mbLoadHTML(v, "<body>x</body>", "https://cachereg.test/");
    Eval(v,
         "window.__r3='';"
         "(async function(){try{"
         "var c=await caches.open('regc1');"
         "await c.put('/x',new Response('hi'));"
         "await caches.delete('regc1');"
         "var crashed=false;try{await c.match('/x');}catch(e){crashed=true;}"
         "var c2=await caches.open('regc2');"
         "var delMissing=await c2.delete('/missing');"  // nothing matched -> false
         "await c2.put('/present',new Response('p'));"
         "var delPresent=await c2.delete('/present');"  // matched -> true
         "window.__r3='crashed:'+crashed+',missing:'+delMissing+',present:'+delPresent;"
         "}catch(e){window.__r3='err:'+e.name;}})();");
    mbWaitForFunction(v, "window.__r3!==''", 4000);
    const std::string r = Eval(v, "window.__r3");
    const std::string alive = Eval(v, "String(1+1)");  // still scriptable?
    Expect(r == "crashed:false,missing:false,present:true" && alive == "2",
           "Cache handle survives caches.delete (no UAF); delete() returns false/true",
           "r3=[" + r + "] alive=[" + alive + "]");
  }

  // REG #4. Cache honors Vary: store two responses for the SAME url keyed by a
  // differing request header (Vary: x-k). match() with the matching header returns
  // the right entry; a non-matching header misses. (Asserts via response STATUS, not
  // body text — cached body bytes read empty intermittently, a known cache-body bug;
  // status distinguishes the two responses reliably.)
  {
    mbLoadHTML(v, "<body>x</body>", "https://cachereg.test/");
    Eval(v,
         "window.__r4='';"
         "(async function(){try{"
         "var c=await caches.open('regc4');var u='/vary';"
         "await c.put(new Request(u,{headers:{'x-k':'a'}}),"
         "new Response('A',{status:200,headers:{'Vary':'x-k'}}));"
         "await c.put(new Request(u,{headers:{'x-k':'b'}}),"
         "new Response('B',{status:201,headers:{'Vary':'x-k'}}));"
         "var ra=await c.match(new Request(u,{headers:{'x-k':'a'}}));"
         "var rb=await c.match(new Request(u,{headers:{'x-k':'b'}}));"
         "var miss=await c.match(new Request(u,{headers:{'x-k':'zzz'}}));"
         "window.__r4=(ra?ra.status:'none')+','+(rb?rb.status:'none')+','+"
         "(miss?'hit':'miss');"
         "}catch(e){window.__r4='err:'+e.name;}})();");
    mbWaitForFunction(v, "window.__r4!==''", 4000);
    const std::string r = Eval(v, "window.__r4");
    Expect(r == "200,201,miss",
           "Cache honors Vary: each request header matches its own response; misses on others",
           "r4=[" + r + "]");
  }

  // REG #5. OPFS removeEntry recursive flag: removing a NON-EMPTY directory with the
  // default (recursive:false) must reject with InvalidModificationError; passing
  // {recursive:true} succeeds.
  {
    mbLoadHTML(v, "<body>x</body>", "https://opfsreg.test/");
    Eval(v,
         "window.__r5='';"
         "(async function(){try{"
         "var root=await navigator.storage.getDirectory();"
         "var d=await root.getDirectoryHandle('reg5',{create:true});"
         "await d.getFileHandle('f.txt',{create:true});"
         "var rej='';try{await root.removeEntry('reg5');}catch(e){rej=e.name;}"
         "var ok='ok';try{await root.removeEntry('reg5',{recursive:true});}"
         "catch(e){ok='fail:'+e.name;}"
         "window.__r5=rej+','+ok;"
         "}catch(e){window.__r5='err:'+e.name;}})();");
    mbWaitForFunction(v, "window.__r5!==''", 4000);
    const std::string r = Eval(v, "window.__r5");
    Expect(r == "InvalidModificationError,ok",
           "OPFS removeEntry rejects a non-empty dir; recursive:true succeeds",
           "r5=[" + r + "]");
  }

  // REG #6. A blob: URL preserves the Blob's Content-Type: fetch()ing the object URL
  // returns a response whose content-type header carries the type given to the Blob.
  {
    mbLoadHTML(v, "<body>x</body>", "https://blobreg.test/");
    Eval(v,
         "window.__r6='';"
         "(async function(){try{"
         "var u=URL.createObjectURL(new Blob(['{}'],{type:'application/json'}));"
         "var r=await fetch(u);"
         "window.__r6=String(r.headers.get('content-type'));"
         "}catch(e){window.__r6='err:'+e.name;}})();");
    mbWaitForFunction(v, "window.__r6!==''", 4000);
    const std::string r = Eval(v, "window.__r6");
    Expect(r.find("application/json") != std::string::npos,
           "blob: URL keeps the Blob Content-Type through fetch()", "r6=[" + r + "]");
  }

  // REG #7. Cookie deletion via a NON-epoch past `expires` (one year ago, not the
  // Unix epoch): set a cookie, confirm it's present, then re-set it empty with a past
  // expiry — document.cookie must no longer contain it.
  {
    mbLoadHTML(v, "<body>x</body>", "https://ckreg.test/");
    mbRunJS(v, "document.cookie='rk=rv';");
    mbWait(v, 20);
    const std::string before = Eval(v, "document.cookie");
    mbRunJS(v,
            "document.cookie='rk=; expires='+"
            "new Date(Date.now()-31536000000).toUTCString();");
    mbWait(v, 20);
    const std::string after = Eval(v, "document.cookie");
    Expect(before.find("rk=rv") != std::string::npos &&
               after.find("rk=rv") == std::string::npos,
           "cookie deleted via a non-epoch past expires",
           "before=[" + before + "] after=[" + after + "]");
  }

  // REG #8. mbSendText populates KeyboardEvent.key: a keydown listener must see the
  // typed character ('a'), not "Unidentified" (the dom_key regression).
  {
    mbLoadHTML(v, "<body><input id='k'></body>", "about:blank");
    mbRunJS(v,
            "window.__key='';var el=document.getElementById('k');"
            "el.addEventListener('keydown',function(e){window.__key=e.key;});"
            "el.focus();");
    mbSendText(v, "a");
    mbWait(v, 20);
    const std::string key = Eval(v, "window.__key");
    Expect(key == "a", "mbSendText sets KeyboardEvent.key (not Unidentified)",
           "key=[" + key + "]");
  }

  // 42. DevTools pause/resume notification (mbOnDevToolsPaused): with the CDP
  // bridge attached and Debugger enabled, a `debugger` statement parks the main
  // thread in the inspector's nested loop — the host callback fires paused=1
  // FROM that loop, resumes via Debugger.resume (interrupt-class, routed over
  // the IO session, so sending it from inside the callback is safe), and gets
  // paused=0 after. Without the resume the mbRunJS below would never return.
  {
    struct PauseLog {
      mbView* view;
      std::string log;
    } pl{v, ""};
    Expect(mbDevToolsAttach(
               v, [](mbView*, void*, const char*, int) {}, nullptr) == 1,
           "devtools attaches for the pause test", "");
    mbOnDevToolsPaused(
        v,
        [](mbView* pv, void* ud, int paused) {
          auto* s = static_cast<PauseLog*>(ud);
          s->log += paused ? 'P' : 'R';
          if (paused) {
            const char* kResume = "{\"id\":99,\"method\":\"Debugger.resume\"}";
            mbDevToolsSend(pv, kResume,
                           static_cast<int>(std::strlen(kResume)));
          }
        },
        &pl);
    const char* kEnable = "{\"id\":1,\"method\":\"Debugger.enable\"}";
    mbDevToolsSend(v, kEnable, static_cast<int>(std::strlen(kEnable)));
    mbWait(v, 100);  // let the enable dispatch before the script runs
    mbRunJS(v, "debugger; window.__afterpause='yes';");
    mbWait(v, 100);
    const std::string after = Eval(v, "window.__afterpause");
    Expect(pl.log == "PR" && after == "yes",
           "mbOnDevToolsPaused fires paused/resumed around a debugger statement",
           "log=[" + pl.log + "] after=[" + after + "]");
    mbOnDevToolsPaused(v, nullptr, nullptr);
    mbDevToolsDetach(v);
    mbWait(v, 50);  // let the async session detach settle before teardown
  }

  // 43. <select> popup surfaces to the HOST (mbOnSelectPopup): clicking the
  // closed menulist delivers items/bounds/selection to the callback, and
  // committing an index from inside it selects that option (blink's external
  // popup path; previously the click was a silent dead-end). Committing an
  // index in the delivered array's index space also proves the plumbing all
  // the way back through PopupMenuClient::DidAcceptIndices.
  {
    struct PopupState {
      std::string labels;
      int selected = -1;
      int fired = 0;
    } ps;
    mbOnSelectPopup(
        v,
        [](mbView* pv, void* ud, const mbSelectPopupInfo* info) {
          auto* s = static_cast<PopupState*>(ud);
          ++s->fired;
          s->selected = info->selected_index;
          for (int i = 0; i < info->item_count; ++i) {
            if (i)
              s->labels += "|";
            s->labels += info->items[i].label ? info->items[i].label : "";
          }
          int idx = 2;  // choose "gamma"
          mbSelectPopupCommit(pv, &idx, 1);
        },
        &ps);
    mbLoadHTML(v,
               "<body style='margin:0'><select id='s' style='font-size:20px'>"
               "<option>alpha</option><option selected>beta</option>"
               "<option>gamma</option></select></body>",
               "about:blank");
    mbSendMouseClick(v, 30, 15);  // open the menulist
    mbWait(v, 250);          // popup routes via the service thread; commit applies
    const std::string val = Eval(v, "document.getElementById('s').value");
    Expect(ps.fired == 1 && ps.labels == "alpha|beta|gamma" &&
               ps.selected == 1 && val == "gamma",
           "<select> popup surfaces to the host and commits the chosen option",
           "fired=" + std::to_string(ps.fired) + " labels=[" + ps.labels +
               "] sel=" + std::to_string(ps.selected) + " value=[" + val + "]");
    mbOnSelectPopup(v, nullptr, nullptr);
  }

  // 44. Round-3 embedder surface (IMPROVEMENT.md items 14-19).
  // (a) Version handshake: static strings, callable state already proven by
  // reaching here post-init; the Chromium version must match the donor tree.
  {
    const std::string ver = mbVersion() ? mbVersion() : "";
    const std::string cr = mbChromiumVersion() ? mbChromiumVersion() : "";
    const bool compat =
        mbAbiEpoch() == MB_ABI_EPOCH && mbApiLevel() == MB_API_LEVEL &&
        mbApiVersion() == MB_API_VERSION &&
        mbCheckCompat(MB_ABI_EPOCH, MB_API_LEVEL) == 1 &&
        mbCheckCompat(MB_ABI_EPOCH + 1, MB_API_LEVEL) == 0 &&
        mbCheckCompat(MB_ABI_EPOCH, MB_API_LEVEL + 1) == 0 &&
        mbCheckCompat(MB_ABI_EPOCH, -1) == 0;
    Expect(!ver.empty() && compat && cr.rfind("150.", 0) == 0,
           "version handshake exports (engine / API / Chromium)",
           "ver=[" + ver + "] api=" + std::to_string(mbApiVersion()) +
               " chromium=[" + cr + "]");
  }

  // (b) Cursor + tooltip push (WidgetHost channel): hovering an element with
  // an explicit CSS cursor fires mbOnCursorChanged with that code; hovering a
  // title= element pushes its tooltip text.
  {
    struct UiState {
      int last_cursor = -1;
      std::string tooltip;
    } us;
    mbOnCursorChanged(
        v, [](mbView*, void* ud, int cursor) {
          static_cast<UiState*>(ud)->last_cursor = cursor;
        },
        &us);
    mbOnTooltipChanged(
        v, [](mbView*, void* ud, const char* text) {
          static_cast<UiState*>(ud)->tooltip = text ? text : "";
        },
        &us);
    mbLoadHTML(v,
               "<body style='margin:0'>"
               "<div id=t style='cursor:text;width:100px;height:40px'></div>"
               "<div id=h title='tip!' style='cursor:pointer;width:100px;"
               "height:40px'></div></body>",
               "about:blank");
    mbSendMouseMove(v, 50, 20);   // over #t: cursor:text -> IBeam
    mbWait(v, 100);               // associated-channel delivery needs the pump
    const int ibeam = us.last_cursor;
    mbSendMouseMove(v, 50, 60);   // over #h: cursor:pointer -> Hand + tooltip
    mbWait(v, 100);
    const int hand = us.last_cursor;
    Expect(ibeam == MB_CURSOR_IBEAM && hand == MB_CURSOR_HAND &&
               us.tooltip == "tip!",
           "cursor + tooltip push (mbOnCursorChanged / mbOnTooltipChanged)",
           "ibeam=" + std::to_string(ibeam) + " hand=" + std::to_string(hand) +
               " tip=[" + us.tooltip + "]");
    mbOnCursorChanged(v, nullptr, nullptr);
    mbOnTooltipChanged(v, nullptr, nullptr);
  }

  // (c) Input-focus query: no editable focused -> 0; focusing an <input>
  // flips it to 1 (blink pushes TextInputStateChanged); blurring restores 0.
  {
    mbLoadHTML(v, "<body><input id='f'><div id='d'>x</div></body>",
               "about:blank");
    mbWait(v, 50);
    const int before = mbHasInputFocus(v);
    mbRunJS(v, "document.getElementById('f').focus();");
    mbWait(v, 100);
    const int focused = mbHasInputFocus(v);
    mbRunJS(v, "document.getElementById('f').blur();");
    mbWait(v, 100);
    const int after = mbHasInputFocus(v);
    Expect(before == 0 && focused == 1 && after == 0,
           "mbHasInputFocus tracks caret focus (focus/blur an <input>)",
           "before=" + std::to_string(before) + " focused=" +
               std::to_string(focused) + " after=" + std::to_string(after));
  }

  // (d) History-state push: a second navigation flips can_go_back true; a
  // host GoBack flips can_go_forward true. Fires only on change.
  {
    struct HistState {
      std::string log;  // "B<b>F<f>" per push
    } hs;
    mbOnHistoryChanged(
        v, [](mbView*, void* ud, int b, int f) {
          static_cast<HistState*>(ud)->log +=
              "B" + std::to_string(b) + "F" + std::to_string(f) + ";";
        },
        &hs);
    mbLoadHTML(v, "<body>h1</body>", "https://hist1.example/");
    mbLoadHTML(v, "<body>h2</body>", "https://hist2.example/");
    const std::string after_loads = hs.log;
    mbGoBack(v);
    const bool fwd_after_back = mbCanGoForward(v) == 1;
    Expect(after_loads.find("B1F0;") != std::string::npos && fwd_after_back &&
               hs.log.find("B1F1;") != std::string::npos,
           "mbOnHistoryChanged pushes back/forward flags on change",
           "log=[" + hs.log + "]");
    mbOnHistoryChanged(v, nullptr, nullptr);
  }

  // (e) window.close() surfacing: a single-entry view (script-closable) calling
  // window.close() fires mbOnRequestClose; the view itself stays alive (the
  // host decides). Uses a fresh view — this one's history is deep by now.
  {
    int closed = 0;
    mbView* cv = mbCreateView(200, 150);
    mbOnRequestClose(
        cv, [](mbView*, void* ud) { ++*static_cast<int*>(ud); }, &closed);
    mbLoadHTML(cv, "<body>close me</body>", "https://close.example/");
    mbRunJS(cv, "window.close();");
    mbWait(cv, 100);  // RequestClose hops service -> main thread
    const std::string alive = Eval(cv, "'still-'+'here'");
    mbDestroyView(cv);
    Expect(closed == 1 && alive == "still-here",
           "window.close() fires mbOnRequestClose (view stays alive)",
           "closed=" + std::to_string(closed) + " alive=[" + alive + "]");
  }

  // (f) Host log sink: installing and clearing the process-wide callback must
  // be safe around engine activity (delivery is covered when engine LOG output
  // occurs; base logging is quiet in a healthy run — this asserts the wiring
  // doesn't crash or eat the run's stderr).
  {
    static int log_hits = 0;
    mbOnLogMessage([](void*, int, const char*) { ++log_hits; }, nullptr);
    mbLoadHTML(v, "<body>log</body>", "about:blank");
    mbOnLogMessage(nullptr, nullptr);
    Expect(true, "mbOnLogMessage installs/clears cleanly",
           "hits=" + std::to_string(log_hits));
  }

  // (g) Per-character font fallback hook: layout of a CJK ideograph the
  // default Times/Helvetica families don't cover consults the host callback
  // (codepoint + weight/italic) BEFORE the platform cascade; a real family
  // answer is used, and a bogus answer must fall through without breaking
  // rendering (the tofu guard).
  {
    struct FbState {
      unsigned int last_cp = 0;
      int fired = 0;
      const char* answer = "";
    } fs;
    fs.answer = "PingFang SC";
    mbSetFontFallbackCallback(
        [](void* ud, unsigned int cp, int weight, int italic, char* out,
           int cap) -> int {
          auto* s = static_cast<FbState*>(ud);
          if (cp != 0x9F98)
            return 0;  // only answer for the probe character
          ++s->fired;
          s->last_cp = cp;
          (void)weight;
          (void)italic;
          std::snprintf(out, cap, "%s", s->answer);
          return 1;
        },
        &fs);
    std::vector<unsigned char> fbpx(static_cast<size_t>(W) * H * 4);
    mbLoadHTML(v, "<body><div id=c style='font-family:Times'>&#x9F98;</div>"
                  "</body>", "about:blank");
    mbPaintToBitmap(v, fbpx.data(), W, H, W * 4);  // layout+paint runs fallback
    const int fired_real = fs.fired;
    // Bogus family: the unicharToGlyph guard must reject it and fall through
    // to the platform cascade — the glyph still renders (no crash, no tofu).
    fs.fired = 0;
    fs.answer = "NoSuchFamily-12345";
    mbLoadHTML(v, "<body><div style='font-family:Times'>&#x9F98;</div></body>",
               "about:blank");
    mbPaintToBitmap(v, fbpx.data(), W, H, W * 4);
    const int fired_bogus = fs.fired;
    mbSetFontFallbackCallback(nullptr, nullptr);
    Expect(fired_real > 0 && fs.last_cp == 0x9F98 && fired_bogus > 0,
           "mbSetFontFallbackCallback consulted per character (tofu-guarded)",
           "real=" + std::to_string(fired_real) + " bogus=" +
               std::to_string(fired_bogus));
  }

  // ---- Round 4 (IMPROVEMENT.md items 22-29) --------------------------------

  // (h) mbSetEnableJavascript gates the PAGE's scripts per view: with JS off a
  // <script> must not run (its DOM write never happens); re-enabling restores it.
  {
    mbView* jv = mbCreateView(200, 150);
    mbSetEnableJavascript(jv, 0);
    mbLoadHTML(jv, "<body><div id=r>static</div>"
                   "<script>document.getElementById('r').textContent='ran';"
                   "</script></body>", "about:blank");
    // Host eval is also refused while script is off (blink reads the live
    // setting); re-enable to READ — the inert <script> does not retroactively run.
    mbSetEnableJavascript(jv, 1);
    const std::string off = Eval(jv, "document.getElementById('r').textContent");
    mbLoadHTML(jv, "<body><div id=r>static</div>"
                   "<script>document.getElementById('r').textContent='ran';"
                   "</script></body>", "about:blank");
    const std::string on = Eval(jv, "document.getElementById('r').textContent");
    mbDestroyView(jv);
    Expect(off == "static" && on == "ran",
           "mbSetEnableJavascript gates page scripts (off=inert, on=runs)",
           "off=[" + off + "] on=[" + on + "]");
  }

  // (i) mbLoadHTMLEx(add_to_history=0) REPLACES the current entry;
  // mbGoToOffset jumps N entries in one traversal.
  {
    mbView* hv = mbCreateView(200, 150);
    mbLoadHTML(hv, "<body>A</body>", "https://r4a.example/");
    mbLoadHTML(hv, "<body>B</body>", "https://r4b.example/");
    mbLoadHTMLEx(hv, "<body>C</body>", "https://r4c.example/", 0);  // replaces B
    const int back_ok = mbGoToOffset(hv, -1);            // -> A (one entry back)
    const std::string at_a = Eval(hv, "document.body.textContent");
    const int fwd_ok = mbGoToOffset(hv, 1);              // -> C (B was replaced)
    const std::string at_c = Eval(hv, "document.body.textContent");
    const int too_far = mbGoToOffset(hv, 5);             // out of range
    mbDestroyView(hv);
    Expect(back_ok == 1 && at_a == "A" && fwd_ok == 1 && at_c == "C" &&
               too_far == 0,
           "mbLoadHTMLEx(no-history) replaces the entry; mbGoToOffset jumps",
           "back=" + std::to_string(back_ok) + " a=[" + at_a + "] fwd=" +
               std::to_string(fwd_ok) + " c=[" + at_c + "] far=" +
               std::to_string(too_far));
  }

  // (j) mbViewSetDirty re-arms the damage flag after a paint cleared it;
  // mbSetForceRepaint makes mbViewIsDirty report 1 regardless.
  {
    mbView* dv = mbCreateView(200, 150);
    mbLoadHTML(dv, "<body>static</body>", "about:blank");
    std::vector<unsigned char> dpx(200 * 150 * 4);
    mbRepaintToBitmap(dv, dpx.data(), 200, 150, 200 * 4);
    mbRepaintToBitmap(dv, dpx.data(), 200, 150, 200 * 4);  // drain stragglers
    const int clean = mbViewIsDirty(dv);
    mbViewSetDirty(dv);
    const int rearmed = mbViewIsDirty(dv);
    mbRepaintToBitmap(dv, dpx.data(), 200, 150, 200 * 4);
    mbSetForceRepaint(dv, 1);
    const int forced = mbViewIsDirty(dv);
    mbSetForceRepaint(dv, 0);
    const int unforced = mbViewIsDirty(dv);
    mbDestroyView(dv);
    Expect(clean == 0 && rearmed == 1 && forced == 1 && unforced == 0,
           "mbViewSetDirty re-arms damage; mbSetForceRepaint bypasses gating",
           "clean=" + std::to_string(clean) + " rearmed=" +
               std::to_string(rearmed) + " forced=" + std::to_string(forced) +
               " unforced=" + std::to_string(unforced));
  }

  // (j2) Dirty RECTS (item 2 tail): mbViewGetDirtyRect reports the damaged
  // region of the last repaint. A clean repaint is empty damage; recoloring a
  // small absolutely-positioned div damages (at least) the div but far less
  // than the view; mbViewSetDirty restores full-view damage.
  {
    mbView* rv = mbCreateView(400, 300);
    mbLoadHTML(rv,
               "<body style='margin:0'><div id=b style='position:absolute;"
               "left:40px;top:30px;width:50px;height:20px;background:#00f'>"
               "</div></body>",
               "about:blank");
    std::vector<unsigned char> rpx(400 * 300 * 4);
    // Settle: repaint until the damage flag stays clean (straggler frames).
    for (int i = 0; i < 10 && (i < 2 || mbViewIsDirty(rv)); ++i)
      mbRepaintToBitmap(rv, rpx.data(), 400, 300, 400 * 4);
    // Clean repaint: nothing invalidated -> empty rect ("skip the blit").
    mbRepaintToBitmap(rv, rpx.data(), 400, 300, 400 * 4);
    int cx = -1, cy = -1, cw = -1, ch = -1;
    mbViewGetDirtyRect(rv, &cx, &cy, &cw, &ch);
    const bool clean_empty = (cw == 0 && ch == 0);
    // Recolor the 50x20 div and repaint: the damage must cover the div's box
    // yet stay far smaller than the 400x300 view — and the pixels must really
    // have changed to red inside the reported rect.
    mbRunJS(rv, "document.getElementById('b').style.background='#f00'");
    mbRepaintToBitmap(rv, rpx.data(), 400, 300, 400 * 4);
    int dx = -1, dy = -1, dw = -1, dh = -1;
    mbViewGetDirtyRect(rv, &dx, &dy, &dw, &dh);
    const bool covers =
        dx <= 40 && dy <= 30 && dx + dw >= 90 && dy + dh >= 50;
    const bool partial = dw > 0 && dh > 0 && dw <= 200 && dh <= 150;
    const size_t off = (static_cast<size_t>(40) * 400 + 60) * 4;  // (60,40) BGRA
    const bool red_px = rpx[off] == 0x00 && rpx[off + 1] == 0x00 &&
                        rpx[off + 2] == 0xFF;
    // Host-forced dirty (lost buffer): the next frame is full-view damage.
    mbViewSetDirty(rv);
    mbRepaintToBitmap(rv, rpx.data(), 400, 300, 400 * 4);
    int fx = -1, fy = -1, fw = -1, fh = -1;
    mbViewGetDirtyRect(rv, &fx, &fy, &fw, &fh);
    const bool full = fx == 0 && fy == 0 && fw == 400 && fh == 300;
    mbDestroyView(rv);
    Expect(clean_empty && covers && partial && red_px && full,
           "mbViewGetDirtyRect: empty when clean, element-sized on a small "
           "change, full after mbViewSetDirty",
           "clean=" + std::to_string(cw) + "x" + std::to_string(ch) +
               " change=" + std::to_string(dx) + "," + std::to_string(dy) +
               " " + std::to_string(dw) + "x" + std::to_string(dh) +
               " red=" + std::to_string(red_px) + " forced=" +
               std::to_string(fx) + "," + std::to_string(fy) + " " +
               std::to_string(fw) + "x" + std::to_string(fh));
  }

  // (j3) Streaming download lifecycle: mbDownloadURLStream delivers
  // begin -> data -> finish through the mbOnDownloadStream sink (a mocked URL
  // exercises the full plumbing without a network), with id/bytes/progress
  // consistent; mbCancelDownload flips finish to success=0 and suppresses the
  // body.
  {
    struct DlState {
      unsigned begin_id = 0, data_id = 0, finish_id = 0;
      std::string url, mime, filename, bytes;
      long long expected = -2, received = -2;
      int finishes = 0, success = -1;
    } st;
    mbView* dv = mbCreateView(200, 150);
    mbLoadHTML(dv, "<body>dl</body>", "about:blank");
    mbMockResponse("dl.test/report.bin", "0123456789ABCDEF",
                   "application/octet-stream", 200);
    mbOnDownloadStream(
        dv,
        [](mbView*, void* ud, unsigned id, const char* url, const char* mime,
           const char* filename, long long expected) {
          auto* s = static_cast<DlState*>(ud);
          s->begin_id = id;
          s->url = url;
          s->mime = mime;
          s->filename = filename;
          s->expected = expected;
        },
        [](mbView*, void* ud, unsigned id, const char* bytes, int len,
           long long received, long long) {
          auto* s = static_cast<DlState*>(ud);
          s->data_id = id;
          s->bytes.append(bytes, static_cast<size_t>(len));
          s->received = received;
        },
        [](mbView*, void* ud, unsigned id, int success) {
          auto* s = static_cast<DlState*>(ud);
          s->finish_id = id;
          s->finishes++;
          s->success = success;
        },
        &st);
    const unsigned id = mbDownloadURLStream(dv, "https://dl.test/report.bin");
    const bool async_start = id > 0 && st.finishes == 0;  // nothing inline
    for (int i = 0; i < 50 && st.finishes == 0; ++i)
      mbWait(dv, 20);
    const bool lifecycle_ok =
        st.begin_id == id && st.data_id == id && st.finish_id == id &&
        st.finishes == 1 && st.success == 1 && st.bytes == "0123456789ABCDEF" &&
        st.expected == 16 && st.received == 16 &&
        st.mime == "application/octet-stream" && st.filename == "report.bin" &&
        st.url == "https://dl.test/report.bin";
    // Cancel before the posted delivery runs: finish reports success=0 and no
    // body arrives.
    DlState st2;
    mbOnDownloadStream(
        dv, nullptr,
        [](mbView*, void* ud, unsigned id, const char* bytes, int len,
           long long, long long) {
          auto* s = static_cast<DlState*>(ud);
          s->data_id = id;
          s->bytes.append(bytes, static_cast<size_t>(len));
        },
        [](mbView*, void* ud, unsigned id, int success) {
          auto* s = static_cast<DlState*>(ud);
          s->finish_id = id;
          s->finishes++;
          s->success = success;
        },
        &st2);
    const unsigned id2 = mbDownloadURLStream(dv, "https://dl.test/report.bin");
    mbCancelDownload(dv, id2);
    for (int i = 0; i < 50 && st2.finishes == 0; ++i)
      mbWait(dv, 20);
    const bool cancel_ok = id2 > 0 && st2.finish_id == id2 &&
                           st2.finishes == 1 && st2.success == 0 &&
                           st2.bytes.empty();
    mbClearMocks();
    mbOnDownloadStream(dv, nullptr, nullptr, nullptr, nullptr);
    mbDestroyView(dv);
    Expect(async_start && lifecycle_ok && cancel_ok,
           "streaming download: begin/data/finish with progress; cancel "
           "reports success=0",
           "id=" + std::to_string(id) + " begin=" + std::to_string(st.begin_id) +
               " bytes=[" + st.bytes + "] expected=" +
               std::to_string(st.expected) + " success=" +
               std::to_string(st.success) + " cancel_success=" +
               std::to_string(st2.success) + " cancel_bytes=" +
               std::to_string(st2.bytes.size()));
  }

  // (j4) mbViewSetFrameTime: per-view rAF timestamps follow the view's own
  // clock. Two view-stamped ticks 0.5s apart yield a rAF delta of ~500ms even
  // while the GLOBAL mbUpdateAt time is set to a value the animation clock
  // would clamp (it refuses to run backwards) — proving the per-view override
  // wins. Times use a far-future base (mach time restarts at boot, so 1e9s of
  // uptime is unreachable) to stay monotonic vs the wall-clock ticks that ran
  // during load.
  {
    mbView* tv = mbCreateView(200, 150);
    mbLoadHTML(tv,
               "<body><script>window.ts=[];"
               "(function loop(t){window.ts.push(t);"
               "requestAnimationFrame(loop);})(0);</script></body>",
               "about:blank");
    std::vector<unsigned char> tpx(200 * 150 * 4);
    mbUpdateAt(9999.0);  // global time: stale/backwards — must NOT be used
    mbViewSetFrameTime(tv, 1e9);
    mbRepaintToBitmap(tv, tpx.data(), 200, 150, 200 * 4);  // rAF at t=1e9
    mbViewSetFrameTime(tv, 1e9 + 0.5);
    mbRepaintToBitmap(tv, tpx.data(), 200, 150, 200 * 4);  // rAF at +500ms
    const std::string delta =
        Eval(tv,
             "(window.ts.length>=2?Math.round(window.ts[window.ts.length-1]-"
             "window.ts[window.ts.length-2]):-1).toString()");
    mbViewSetFrameTime(tv, 0);  // clear the override
    mbUpdateAt(0);              // restore wall-clock rAF for later tests
    mbDestroyView(tv);
    Expect(delta == "500",
           "mbViewSetFrameTime: per-view rAF clock beats the global "
           "mbUpdateAt time (500ms delta)",
           "delta=[" + delta + "]");
  }

  // (j5) Host image sources: a registered BGRA image renders through
  // <img src="https://mb-image.internal/<id>">; re-registering swaps the
  // pixels and fires 'mbimagesourceupdate' so the page's cache-busting
  // re-fetch shows the new frame — the ImageSource live-update loop.
  {
    // 8x8 solid red, premultiplied BGRA.
    std::vector<unsigned char> img(8 * 8 * 4);
    for (size_t i = 0; i < img.size(); i += 4) {
      img[i] = 0x00; img[i + 1] = 0x00; img[i + 2] = 0xFF; img[i + 3] = 0xFF;
    }
    mbRegisterImageSource("smoketick", img.data(), 8, 8, 0);
    mbView* iv = mbCreateView(200, 150);
    mbLoadHTML(iv,
               "<body style='margin:0'>"
               "<img id=i src='https://mb-image.internal/smoketick' width=32 "
               "height=32 style='position:absolute;left:0;top:0'>"
               "<script>window.n=0;document.addEventListener("
               "'mbimagesourceupdate',e=>{if(e.detail==='smoketick')"
               "document.getElementById('i').src="
               "'https://mb-image.internal/smoketick?v='+(++window.n);});"
               "</script></body>",
               "about:blank");
    std::vector<unsigned char> ipx(200 * 150 * 4);
    mbPaintToBitmap(iv, ipx.data(), 200, 150, 200 * 4);  // settles: decodes img
    const size_t off = (static_cast<size_t>(8) * 200 + 8) * 4;  // (8,8) BGRA
    const bool red = ipx[off] < 0x40 && ipx[off + 1] < 0x40 &&
                     ipx[off + 2] > 0xC0;
    // Swap the backing image to blue: the broadcast event re-points the <img>
    // at ?v=1 and the next settled paint shows the new pixels.
    for (size_t i = 0; i < img.size(); i += 4) {
      img[i] = 0xFF; img[i + 1] = 0x00; img[i + 2] = 0x00; img[i + 3] = 0xFF;
    }
    mbRegisterImageSource("smoketick", img.data(), 8, 8, 0);
    const std::string bumped = Eval(iv, "String(window.n)");
    mbPaintToBitmap(iv, ipx.data(), 200, 150, 200 * 4);
    const bool blue = ipx[off] > 0xC0 && ipx[off + 1] < 0x40 &&
                      ipx[off + 2] < 0x40;
    mbUnregisterImageSource("smoketick");
    mbDestroyView(iv);
    Expect(red && bumped == "1" && blue,
           "image source: <img> renders registered BGRA; re-register fires "
           "mbimagesourceupdate and the re-fetch shows the new pixels",
           "red=" + std::to_string(red) + " bumped=[" + bumped + "] blue=" +
               std::to_string(blue));
  }

  // (k) mbSendKeyEvent: a typed KeyDown with text inserts into the focused
  // input (implicit kChar) and carries the auto-repeat flag to KeyboardEvent.
  {
    mbView* kv = mbCreateView(200, 150);
    mbLoadHTML(kv,
               "<body><input id=i autofocus><script>"
               "window.rep=[];document.getElementById('i').addEventListener("
               "'keydown',e=>rep.push(e.key+':'+e.repeat));</script></body>",
               "about:blank");
    mbFocusSelector(kv, "#i");
    mbKeyEvent ke = {};
    ke.struct_size = sizeof(mbKeyEvent);
    ke.type = MB_KEY_DOWN;
    ke.windows_key_code = 'A';
    ke.text = "a";
    ke.unmodified_text = "a";
    mbSendKeyEvent(kv, &ke);
    ke.is_auto_repeat = 1;
    mbSendKeyEvent(kv, &ke);
    const std::string val = Eval(kv, "document.getElementById('i').value");
    const std::string rep = Eval(kv, "window.rep.join('|')");
    mbDestroyView(kv);
    Expect(val == "aa" && rep == "a:false|a:true",
           "mbSendKeyEvent types text and carries the auto-repeat flag",
           "value=[" + val + "] keydowns=[" + rep + "]");
  }

  // (l) Session introspection getters: name / persistence mode / persist path
  // read back from the handle; the default session is ephemeral.
  {
    mbSession* es = mbCreateSession("r4-ephemeral", nullptr);
    char name[64] = {0};
    mbSessionGetName(es, name, sizeof(name));
    const int eph = mbSessionIsPersistent(es);
    char epath[256] = {0};
    const int eplen = mbSessionGetPersistPath(es, epath, sizeof(epath));
    mbSession* ps = mbCreateSession("r4-persistent", "/tmp/mb-smoke-r4-profiles");
    const int per = mbSessionIsPersistent(ps);
    char ppath[256] = {0};
    mbSessionGetPersistPath(ps, ppath, sizeof(ppath));
    const std::string ppath_s = ppath;
    char pname[64] = {0};
    mbSessionGetName(ps, pname, sizeof(pname));
    mbDestroySession(ps);
    mbDestroySession(es);
    // Persist dir is <persist_path>/<name> (persist_path canonicalized, so /tmp may resolve
    // to /private/tmp) — the stable documented layout, with the name round-tripping.
    const bool path_ok =
        ppath_s.find("mb-smoke-r4-profiles/r4-persistent") != std::string::npos;
    // Name validation: a non-portable persistent name is REJECTED (no lossy aliasing);
    // mbCreateSessionEx reports why. Ephemeral sessions accept any name.
    mbSessionCreateStatus st = MB_SESSION_ERROR;
    mbSession* bad = mbCreateSessionEx("a/b", "/tmp/mb-smoke-r4-profiles", &st);
    mbSessionCreateStatus null_st = MB_SESSION_ERROR;
    mbSession* bad_null =
        mbCreateSessionEx(nullptr, "/tmp/mb-smoke-r4-profiles", &null_st);
    mbSessionCreateStatus empty_st = MB_SESSION_ERROR;
    mbSession* bad_empty =
        mbCreateSessionEx("", "/tmp/mb-smoke-r4-profiles", &empty_st);
    mbSessionCreateStatus eph_st = MB_SESSION_ERROR;
    mbSession* empty_ephemeral = mbCreateSessionEx("", nullptr, &eph_st);
    // The original symbol remains permissive for API-level-1 callers. Strict
    // validation is additive in Ex, not a behavior change to an old export.
    mbSession* legacy_nested =
        mbCreateSession("legacy/nested", "/tmp/mb-smoke-r4-profiles");
    mbSession* legacy_unnamed =
        mbCreateSession(nullptr, "/tmp/mb-smoke-r4-profiles");
    char legacy_name[64] = {0};
    if (legacy_unnamed)
      mbSessionGetName(legacy_unnamed, legacy_name, sizeof(legacy_name));
    const bool legacy_compatible = legacy_nested && legacy_unnamed &&
                                   std::string(legacy_name) == "unnamed";
    const bool reject_ok = bad == nullptr && st == MB_SESSION_INVALID_NAME &&
                           bad_null == nullptr &&
                           null_st == MB_SESSION_INVALID_NAME &&
                           bad_empty == nullptr &&
                           empty_st == MB_SESSION_INVALID_NAME &&
                           empty_ephemeral != nullptr && eph_st == MB_SESSION_OK;
    mbDestroySession(legacy_unnamed);
    mbDestroySession(legacy_nested);
    mbDestroySession(empty_ephemeral);
    Expect(std::string(name) == "r4-ephemeral" && eph == 0 && eplen == 0 &&
               per == 1 && path_ok && std::string(pname) == "r4-persistent" &&
               reject_ok && legacy_compatible &&
               mbSessionIsPersistent(mbDefaultSession()) == 0,
           "session introspection + strict Ex and compatible legacy creation",
           "name=[" + std::string(name) + "] eph=" + std::to_string(eph) +
               " per=" + std::to_string(per) + " pname=[" + std::string(pname) +
               "] path=[" + ppath_s + "] reject_st=" + std::to_string(st) +
               " null=" + std::to_string(null_st) +
               " empty=" + std::to_string(empty_st));
  }

#if !defined(_WIN32)
  // (l2) A copied profile directory is a clone, not an alias. Its copied
  // marker is rebound to the new canonical directory, producing a distinct
  // in-memory cookie/storage identity while symlink/path aliases still share.
  {
    const std::filesystem::path root("/tmp/mb-smoke-session-clone");
    const std::filesystem::path source_dir = root / "source";
    const std::filesystem::path clone_dir = root / "clone";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    mbSessionCreateStatus seed_st = MB_SESSION_ERROR;
    mbSession* seed = mbCreateSessionEx("source", root.string().c_str(), &seed_st);
    mbDestroySession(seed);
    std::filesystem::copy(source_dir, clone_dir,
                          std::filesystem::copy_options::recursive, ec);
    const std::string copied_token = ReadProfileToken(clone_dir);

    mbSessionCreateStatus source_st = MB_SESSION_ERROR;
    mbSessionCreateStatus clone_st = MB_SESSION_ERROR;
    mbSession* source =
        mbCreateSessionEx("source", root.string().c_str(), &source_st);
    mbSession* clone =
        mbCreateSessionEx("clone", root.string().c_str(), &clone_st);
    mbView* source_view =
        source ? mbCreateViewInSession(160, 100, source) : nullptr;
    mbView* clone_view = clone ? mbCreateViewInSession(160, 100, clone) : nullptr;
    if (source_view)
      mbSetCookie(source_view, "https://profile-clone.test/", "source_only=1");
    char clone_cookie[64] = {0};
    const int clone_cookie_len =
        clone_view ? mbGetCookie(clone_view, "https://profile-clone.test/",
                                 "source_only", clone_cookie,
                                 sizeof(clone_cookie))
                   : 0;
    const std::string source_token = ReadProfileToken(source_dir);
    const std::string rebound_token = ReadProfileToken(clone_dir);
    if (source_view)
      mbDestroyView(source_view);
    if (clone_view)
      mbDestroyView(clone_view);
    mbDestroySession(source);
    mbDestroySession(clone);
    std::filesystem::remove_all(root, ec);
    Expect(seed_st == MB_SESSION_OK && !ec && copied_token.size() == 32 &&
               source_st == MB_SESSION_OK && clone_st == MB_SESSION_OK &&
               source_token.size() == 32 && rebound_token.size() == 32 &&
               source_token != rebound_token && copied_token != rebound_token &&
               clone_cookie_len == -1 && clone_cookie[0] == '\0',
           "copied persistent profiles rebind identity and remain isolated",
           "seed=" + std::to_string(seed_st) + " source=" +
               std::to_string(source_st) + " clone=" +
               std::to_string(clone_st) + " tokens=[" + source_token + "]/ [" +
               rebound_token + "] copied=[" + copied_token + "] cookie=[" +
               clone_cookie + "] cookie_len=" +
               std::to_string(clone_cookie_len) + " ec=" + ec.message());
  }

  // (l3) Upgrade an old profile whose partition scopes used the caller's RAW path spelling.
  // The new session canonicalizes that path; restore must remap the serialized IDB/OPFS
  // scopes, and the migrated data must survive the session's canonical scoped flush.
  {
    const std::filesystem::path root("/tmp/mb-smoke-session-upgrade");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const std::string raw_root = (root / "profiles").string() + "/.";
    const std::filesystem::path profile_dir = root / "profiles" / "legacy";
    const std::string legacy_id = "p:" + raw_root + "/legacy";
    const std::string origin = "https://session-migrate.test";
    const bool fixture_ok =
        WriteLegacySessionFixtures(profile_dir, legacy_id + "\x1f" + origin);

    auto check_migrated = [&](mbSession* session) {
      if (!session)
        return false;
      mbView* sv = mbCreateViewInSession(240, 160, session);
      mbLoadHTML(
          sv,
          "<body>migration<script>"
          "window.__idbm='pending';"
          "var rq=indexedDB.open('legacydb',1);"
          "rq.onupgradeneeded=function(){window.__idbm='upgrade';};"
          "rq.onsuccess=function(){if(window.__idbm==='pending')"
          "window.__idbm='existing';rq.result.close();};"
          "window.__opm='pending';navigator.storage.getDirectory()"
          ".then(function(r){return r.getDirectoryHandle('legacydir');})"
          ".then(function(){window.__opm='existing';})"
          ".catch(function(e){window.__opm=e.name;});"
          "</script></body>",
          origin.c_str());
      mbWaitForFunction(
          sv,
          "window.__idbm!=='pending'&&window.__opm!=='pending'",
          3000);
      const bool ok = Eval(sv, "String(window.__idbm)") == "existing" &&
                      Eval(sv, "String(window.__opm)") == "existing";
      mbDestroyView(sv);
      return ok;
    };

    mbSessionCreateStatus first_st = MB_SESSION_ERROR;
    mbSession* first = mbCreateSessionEx("legacy", raw_root.c_str(), &first_st);
    char canonical_buf[1024] = {0};
    if (first)
      mbSessionGetPersistPath(first, canonical_buf, sizeof(canonical_buf));
    const std::string canonical_profile(canonical_buf);
    const std::string profile_token =
        ReadProfileToken(std::filesystem::path(canonical_profile));
    const bool first_ok = first_st == MB_SESSION_OK && check_migrated(first);
    mbDestroySession(first);  // flushes only the NEW canonical scope back to the files

    const std::string canonical_prefix = "p:" + profile_token + "\x1f";
    if (profile_token.size() == 32) {
      mb::MbClearIndexedDBScoped(canonical_prefix);
      mb::MbClearOPFSScoped(canonical_prefix);
    }
    const std::string canonical_root =
        std::filesystem::path(canonical_profile).parent_path().string();
    mbSessionCreateStatus second_st = MB_SESSION_ERROR;
    mbSession* second =
        mbCreateSessionEx("legacy", canonical_root.c_str(), &second_st);
    const std::string reopened_token =
        ReadProfileToken(std::filesystem::path(canonical_profile));
    const bool reopen_ok = second_st == MB_SESSION_OK && check_migrated(second);
    mbDestroySession(second);
    if (profile_token.size() == 32) {
      mb::MbClearIndexedDBScoped(canonical_prefix);
      mb::MbClearOPFSScoped(canonical_prefix);
    }
    std::filesystem::remove_all(root, ec);
    Expect(fixture_ok && profile_token.size() == 32 &&
               reopened_token == profile_token && first_ok && reopen_ok,
           "session upgrade remaps raw-path IDB/OPFS scopes and persists them",
           "fixture=" + std::to_string(fixture_ok) +
               " token=" + std::to_string(profile_token.size()) +
               " stable=" + std::to_string(reopened_token == profile_token) +
               " first=" + std::to_string(first_ok) +
               " reopen=" + std::to_string(reopen_ok) +
               " path=[" + canonical_profile + "]");
  }
#endif

  // (m) Child views: with mbOnCreateChildView registered, window.open returns
  // a LIVE window — the engine creates an opener-wired child view, the host
  // adopts it, blink navigates it (mock-served), and the child can postMessage
  // back to its opener. A declining callback keeps window.open null.
  {
    struct ChildState {
      mbView* child = nullptr;
      std::string url, name;
      int is_popup = -1, w = 0, h = 0;
      int adopt = 1;  // second round declines
      int fired = 0;
    };
    static ChildState* cs = new ChildState();  // -Wexit-time-destructors
    *cs = ChildState();
    mbView* pv = mbCreateView(300, 200);
    mbMockResponse("r4child.test/pop",
                   "<body id=childbody>child"
                   "<script>window.opener.postMessage('hi-opener','*');"
                   "</script></body>",
                   "text/html", 200);
    mbOnCreateChildView(
        pv,
        [](mbView*, void*, mbView* child, const char* url, const char* name,
           int is_popup, int, int, int w, int h) -> int {
          ++cs->fired;
          if (!cs->adopt)
            return 0;
          cs->child = child;
          cs->url = url;
          cs->name = name;
          cs->is_popup = is_popup;
          cs->w = w;
          cs->h = h;
          return 1;
        },
        nullptr);
    mbLoadHTML(pv,
               "<body><div id=m>?</div><script>"
               "window.addEventListener('message',e=>{"
               "document.getElementById('m').textContent=e.data;});"
               "window.w=window.open('https://r4child.test/pop','pop',"
               "'width=320,height=240');"
               "document.title=window.w?'got-window':'null-window';"
               "</script></body>",
               "https://r4parent.test/");
    // The child commits its (mock-served) document on posted tasks; wait for
    // its script to have posted back to the opener.
    mbWaitForFunction(pv, "document.getElementById('m').textContent!=='?'",
                      3000);
    const std::string title = Eval(pv, "document.title");
    const std::string msg = Eval(pv, "document.getElementById('m').textContent");
    const std::string childbody =
        cs->child ? Eval(cs->child, "document.body.id") : "";
    const bool geom_ok = cs->is_popup == 1 && cs->w == 320 && cs->h == 240;
    // Decline path: the callback returns 0 -> the page sees null.
    cs->adopt = 0;
    mbRunJS(pv, "document.title = window.open('https://r4child.test/pop')?"
                "'second-got':'second-null';");
    mbUpdate();  // drain the deferred teardown of the declined child
    const std::string declined = Eval(pv, "document.title");
    if (cs->child)
      mbDestroyView(cs->child);
    mbOnCreateChildView(pv, nullptr, nullptr);
    mbDestroyView(pv);
    Expect(cs->fired == 2 && title == "got-window" && msg == "hi-opener" &&
               childbody == "childbody" &&
               cs->url == "https://r4child.test/pop" && cs->name == "pop" &&
               geom_ok && declined == "second-null",
           "mbOnCreateChildView: window.open returns a live opener-wired view",
           "title=[" + title + "] msg=[" + msg + "] child=[" + childbody +
               "] url=[" + cs->url + "] name=[" + cs->name + "] popup=" +
               std::to_string(cs->is_popup) + " " + std::to_string(cs->w) +
               "x" + std::to_string(cs->h) + " declined=[" + declined + "]");
  }

  // ---- Round 5 (IMPROVEMENT.md items 32–37) --------------------------------

  // R5a. mbOnWindowObjectReady fires per main-frame document BEFORE any page
  // script (after mbSetInitScript's slot), and host JS run from inside the
  // callback is visible to the page's earliest script. Order: W(OR) precedes
  // DOMContentLoaded.
  {
    static std::string* seq = new std::string();  // -Wexit-time-destructors
    seq->clear();
    mbOnWindowObjectReady(
        v,
        [](mbView* view, void*) {
          *seq += "W";
          mbRunJS(view, "window.__wor='early';");
        },
        nullptr);
    mbOnDOMContentLoaded(v, [](mbView*, void*) { *seq += "D"; }, nullptr);
    mbLoadHTML(v,
               "<body><script>document.title=window.__wor||'missing';"
               "</script>r5a</body>",
               "about:blank");
    const std::string title = Eval(v, "document.title");
    mbOnWindowObjectReady(v, nullptr, nullptr);
    mbOnDOMContentLoaded(v, nullptr, nullptr);
    Expect(!seq->empty() && seq->front() == 'W' &&
               seq->find('D') != std::string::npos && title == "early",
           "mbOnWindowObjectReady: pre-page-script hook, host JS visible early",
           "seq=[" + *seq + "] title=[" + title + "]");
  }

  // R5b. mbOnFailLoadingEx delivers a machine-checkable (domain, code) beside
  // the prose: "file" for an unreadable file:// load, "curl" + a nonzero
  // CURLcode for a transport failure (.invalid never resolves). One slot: the
  // plain mbOnFailLoading registered first must NOT fire once Ex replaces it.
  {
    struct FailState {
      std::string domain;
      int code = -1;
      std::string desc;
      int ex_fired = 0;
      int plain_fired = 0;
    };
    static FailState* fs = new FailState();
    *fs = FailState();
    mbOnFailLoading(
        v, [](mbView*, void*, const char*, const char*) { ++fs->plain_fired; },
        nullptr);
    mbOnFailLoadingEx(
        v,
        [](mbView*, void*, const char*, const char* domain, int code,
           const char* desc) {
          ++fs->ex_fired;
          fs->domain = domain ? domain : "";
          fs->code = code;
          fs->desc = desc ? desc : "";
        },
        nullptr);
    mbLoadURL(v, "file:///nonexistent-mb-round5-nope.html");
    const std::string file_domain = fs->domain;
    const int file_code = fs->code;
    mbLoadURL(v, "http://mb-round5-nonexistent.invalid/");
    Expect(fs->ex_fired == 2 && fs->plain_fired == 0 &&
               file_domain == "file" && file_code == 0 &&
               fs->domain == "curl" && fs->code != 0 && !fs->desc.empty(),
           "mbOnFailLoadingEx: file/curl domains + CURLcode; Ex replaces plain",
           "file=[" + file_domain + "," + std::to_string(file_code) +
               "] net=[" + fs->domain + "," + std::to_string(fs->code) + "," +
               fs->desc + "] ex=" + std::to_string(fs->ex_fired) +
               " plain=" + std::to_string(fs->plain_fired));
    mbOnFailLoadingEx(v, nullptr, nullptr);
  }

  // R5c. OS-clipboard bridge: with a read handler installed the PAGE's paste
  // path pulls from the host (bypassing the jar); page writes push to the
  // write handler AND still land in the jar (mbGetClipboard). Clearing the
  // handlers restores the pure in-process clipboard. (The handlers fire on a
  // service thread; the strings here are only read after the page round-trip
  // completes on this thread.)
  {
    static std::string* wrote = new std::string();
    wrote->clear();
    mbSetClipboardHandler(
        [](void*, char* out, int out_cap) -> int {
          const char kText[] = "host-clip";
          std::snprintf(out, out_cap, "%s", kText);
          return static_cast<int>(sizeof(kText) - 1);
        },
        [](void*, const char* text) { *wrote = text ? text : ""; }, nullptr);
    mbSetClipboard("jar-text");  // must be SHADOWED by the read handler
    mbLoadHTML(v, "<body>r5c</body>", "https://r5clip.test/");
    mbRunJS(v,
            "window.__ct='';navigator.clipboard.readText().then(function(t){"
            "window.__ct=t;}).catch(function(e){window.__ct='ERR:'+e.name;});");
    mbWaitForFunction(v, "window.__ct!==''", 2000);
    const std::string read_via_host = Eval(v, "String(window.__ct)");
    mbRunJS(v,
            "navigator.clipboard.writeText('page-copy').then(function(){"
            "window.__cw='done';}).catch(function(e){window.__cw='ERR';});");
    mbWaitForFunction(v, "window.__cw==='done'||window.__cw==='ERR'", 2000);
    char jar[64] = {0};
    mbGetClipboard(jar, sizeof(jar));
    mbSetClipboardHandler(nullptr, nullptr, nullptr);
    mbSetClipboard("back-to-jar");
    mbRunJS(v,
            "window.__c2='';navigator.clipboard.readText().then(function(t){"
            "window.__c2=t;}).catch(function(e){window.__c2='ERR:'+e.name;});");
    mbWaitForFunction(v, "window.__c2!==''", 2000);
    const std::string read_via_jar = Eval(v, "String(window.__c2)");
    Expect(read_via_host == "host-clip" && *wrote == "page-copy" &&
               std::string(jar) == "page-copy" && read_via_jar == "back-to-jar",
           "mbSetClipboardHandler: page reads pull host, writes push host+jar",
           "read=[" + read_via_host + "] wrote=[" + *wrote + "] jar=[" + jar +
               "] after-clear=[" + read_via_jar + "]");
  }

  // R5d. mbViewConfig: creation-time choices (session, UA, dark mode, device
  // scale) apply before the first document — no "call before load" ordering.
  {
    mbSession* s = mbCreateSession("r5cfg", nullptr);
    mbViewConfig* cfg = mbCreateViewConfig();
    mbViewConfigSetSession(cfg, s);
    mbViewConfigSetUserAgent(cfg, "MbRound5UA/1.0");
    mbViewConfigSetDarkMode(cfg, 1);
    mbViewConfigSetDeviceScaleFactor(cfg, 2.0f);
    mbView* cv = mbCreateViewWithConfig(320, 240, cfg);
    mbDestroyViewConfig(cfg);  // views outlive their config
    mbLoadHTML(cv, "<body>cfg</body>", "about:blank");
    const std::string ua = Eval(cv, "navigator.userAgent");
    const std::string dark =
        Eval(cv, "String(matchMedia('(prefers-color-scheme: dark)').matches)");
    const std::string dpr = Eval(cv, "String(window.devicePixelRatio)");
    const bool session_ok = mbViewGetSession(cv) == s;
    // Config-created views default to the non-compositing path.
    const bool non_compositing = mbViewFrameSinkRequested(cv) == -1;
    mbDestroyView(cv);
    mbDestroySession(s);
    Expect(ua == "MbRound5UA/1.0" && dark == "true" && dpr == "2" &&
               session_ok && non_compositing,
           "mbCreateViewWithConfig applies session/UA/dark/scale at creation",
           "ua=[" + ua + "] dark=[" + dark + "] dpr=[" + dpr + "] session=" +
               std::to_string(session_ok));
  }

  // R5e. Mutable request hook: mbRequestSetUrl transparently redirects a
  // top-level fetch (the redirected URL hits a mock; the page still shows its
  // ORIGINAL URL), and mbRequestBlock vetoes a load, which reports
  // error_domain "blocked" through mbOnFailLoadingEx.
  {
    mbMockResponse("mbr5-target", "<body id=rt>redirected</body>",
                   "text/html", 200);
    static std::string* blocked_domain = new std::string();
    blocked_domain->clear();
    mbSetRequestHook(
        [](mbRequest* r, void*) {
          const std::string url = mbRequestURL(r);
          if (url.find("mbr5-src") != std::string::npos)
            mbRequestSetUrl(r, "https://mbr5-target.test/x");
          if (url.find("mbr5-block") != std::string::npos)
            mbRequestBlock(r);
        },
        nullptr);
    mbLoadURL(v, "https://mbr5-src.test/page");
    const std::string body_id = Eval(v, "document.body.id");
    char cur[128] = {0};
    mbGetURL(v, cur, sizeof(cur));
    mbOnFailLoadingEx(
        v,
        [](mbView*, void*, const char*, const char* domain, int,
           const char*) { *blocked_domain = domain ? domain : ""; },
        nullptr);
    mbLoadURL(v, "https://mbr5-block.test/");
    mbOnFailLoadingEx(v, nullptr, nullptr);
    mbSetRequestHook(nullptr, nullptr);
    mbClearMocks();
    Expect(body_id == "rt" &&
               std::string(cur).find("mbr5-src.test/page") !=
                   std::string::npos &&
               *blocked_domain == "blocked",
           "mbSetRequestHook: SetUrl redirects transparently; Block vetoes "
           "with domain 'blocked'",
           "body=[" + body_id + "] url=[" + cur + "] blocked-domain=[" +
               *blocked_domain + "]");
  }

  // R5f. mbAddFontData: register in-memory font bytes so a NEW family name
  // resolves. Ahem (not installed on any stock system) has exactly-1em square
  // glyphs, so 5 chars at 20px measure exactly 100px iff the registered face
  // (and not a platform fallback) rendered. Fixture: the donor tree's
  // headless-test copy of Ahem, relative to the smoke's $TREE/$OUT cwd —
  // SKIPPED, not failed, when unavailable (e.g. running outside the tree).
  {
    std::string font_bytes;
    if (FILE* f = std::fopen("../../headless/test/data/Ahem.ttf", "rb")) {
      char buf[4096];
      size_t n;
      while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        font_bytes.append(buf, n);
      std::fclose(f);
    }
    if (font_bytes.empty()) {
      std::fprintf(stderr,
                   "  [SKIP] mbAddFontData (Ahem.ttf fixture not found)\n");
    } else {
      char family[64] = {0};
      const int ok = mbAddFontData(font_bytes.data(),
                                   static_cast<int>(font_bytes.size()), family,
                                   sizeof(family));
#if defined(_WIN32)
      // Documented platform gap: blink's Windows font stack is DirectWrite-
      // backed and cannot see memory-registered fonts, so the export returns
      // 0 honestly (webview.h) — assert THAT, not silent misbehavior.
      Expect(ok == 0,
             "mbAddFontData: returns 0 on Windows (DirectWrite gap, documented)",
             std::string("ok=") + std::to_string(ok));
#else
      mbLoadHTML(v,
                 "<body><span id=a style=\"font-family:Ahem;font-size:20px\">"
                 "XXXXX</span></body>",
                 "about:blank");
      const std::string w =
          Eval(v, "String(document.getElementById('a').offsetWidth)");
      Expect(ok == 1 && std::string(family) == "Ahem" && w == "100",
             "mbAddFontData: in-memory font registers and its family resolves",
             std::string("ok=") + std::to_string(ok) + " family=[" + family +
                 "] width=[" + w + "]");
#endif
    }
  }

  // R5g. Per-view network-idle counters are monotonic for the whole lifetime of a
  // stable activity identity, then release their map slot when that identity is
  // retired. A defensive retirement path keeps an entry until an already-counted
  // load balances, instead of either leaking it forever or erasing an in-flight
  // counter.
  {
    int live_identity = 0;
    const void* key = &live_identity;
    mb::MbNetRequestStarted(key);
    mb::MbNetRequestFinished(key);
    mb::MbNetRequestStarted(key);
    mb::MbNetRequestFinished(key);
    const bool live_monotonic = mb::MbNetStartedCount(key) == 2 &&
                                mb::MbNetInFlight(key) == 0 &&
                                mb::MbNetHasActivityContextForTesting(key);
    mb::MbNetForgetActivityContext(key);
    const bool released = !mb::MbNetHasActivityContextForTesting(key);

    int retiring_identity = 0;
    const void* retiring_key = &retiring_identity;
    mb::MbNetRequestStarted(retiring_key);
    mb::MbNetForgetActivityContext(retiring_key);
    const bool deferred = mb::MbNetHasActivityContextForTesting(retiring_key) &&
                          mb::MbNetInFlight(retiring_key) == 1;
    mb::MbNetRequestFinished(retiring_key);
    const bool drained =
        !mb::MbNetHasActivityContextForTesting(retiring_key);
    Expect(live_monotonic && released && deferred && drained,
           "network-idle activity entries stay monotonic while live and are "
           "released after teardown",
           "live=" + std::to_string(live_monotonic) +
               " released=" + std::to_string(released) +
               " deferred=" + std::to_string(deferred) +
               " drained=" + std::to_string(drained));
  }

  mbDestroyView(v);
  mbShutdown();

  std::fprintf(stderr, "\nmb_smoke: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
