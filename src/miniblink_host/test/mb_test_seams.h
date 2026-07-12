// mb_test_seams — the mb::*ForTesting hooks the smoke suites arm.
//
// This is the ONLY place these functions are declared. The defining TUs
// (loader/mb_url_loader.cc, worker/mb_worker_script.cc) include this header
// too, so a signature change is a compile error in the definer instead of a
// link-time surprise in a test binary. Production code must not call these.
#ifndef MINIBLINK_HOST_TEST_MB_TEST_SEAMS_H_
#define MINIBLINK_HOST_TEST_MB_TEST_SEAMS_H_

#include <string>

namespace mb {

// Number of worker-sequence request-policy calls currently parked waiting for
// the engine sequence for this exact page-visible URL. Becomes nonzero only
// after the worker has genuinely spent one wait interval in the rendezvous.
int MbRequestPolicyWaiterCountForTesting(const std::string& visible_url);

// Same rendezvous diagnostic for a worker-owned response hook, keyed by the
// exact page-visible response URL.
int MbResponseHookWaiterCountForTesting(const std::string& visible_url);

// Same diagnostic for an off-engine worker-main-script fetch, keyed by the
// exact script URL.
int MbWorkerScriptWaiterCountForTesting(const std::string& url);

// Whether the per-view network-activity map still holds an entry for this
// stable attribution key (live or retired-but-draining).
bool MbNetHasActivityContextForTesting(const void* view_ctx);

// Deterministic test seam for local (file:/data:) asynchronous navigation.
// Arm installs a one-shot worker barrier for the exact page-visible URL. Once
// the worker has started, it reports whether it is off the engine sequence and
// waits for Release before materializing the local payload. The bounded wait
// helpers and reply counter let tests prove that a pool task/reply straddled
// view teardown without relying on wall-clock duration or stack userdata.
void MbArmLocalNavigationForTesting(const std::string& visible_url);
void MbReleaseLocalNavigationForTesting(const std::string& visible_url);
void MbClearLocalNavigationForTesting(const std::string& visible_url);
bool MbWaitForLocalNavigationWorkerStartForTesting(
    const std::string& visible_url,
    int timeout_ms);
bool MbWaitForLocalNavigationWorkerFinishForTesting(
    const std::string& visible_url,
    int timeout_ms);
bool MbLocalNavigationRanOffEngineSequenceForTesting(
    const std::string& visible_url);
bool MbLocalNavigationBarrierTimedOutForTesting(
    const std::string& visible_url);
int MbLocalNavigationReplyCountForTesting(const std::string& visible_url);

}  // namespace mb

#endif  // MINIBLINK_HOST_TEST_MB_TEST_SEAMS_H_
