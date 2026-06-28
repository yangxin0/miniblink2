// mb_retry_policy — the fetch-retry decision, isolated in a dependency-free header
// (only <string>) so it can be unit-tested from the smoke executables without
// pulling the loader's heavy base/blink includes (which a test target's narrower
// include config can't resolve). Header-only inline = one definition, no cross-
// module linkage / symbol-visibility concerns.

#ifndef MINIBLINK_HOST_LOADER_MB_RETRY_POLICY_H_
#define MINIBLINK_HOST_LOADER_MB_RETRY_POLICY_H_

#include <string>

namespace mb {

// Decide whether a failed/empty fetch attempt should be retried. Rules:
//  - Only SAFE methods are retried (GET/HEAD, or empty == GET). A write
//    (POST/PUT/PATCH/DELETE) is NEVER auto-retried — re-sending after an
//    ambiguous failure can duplicate a server-side side effect.
//  - Transient transport errors and transient HTTP statuses (429/5xx) retry.
//  - An empty body on a 2xx/3xx is treated as a throttled-connection anomaly and
//    retried, EXCEPT for responses that legitimately carry no body: 204, 304, and
//    any HEAD (so a real 204/304 is not mistaken for a failure).
// `transient_transport_error` = IsTransientCurlError(rc); `transient_http_status`
// = (rc == CURLE_OK && IsTransientHttpCode(http_code)). attempt is 1-based.
inline bool MbShouldRetryFetch(const std::string& method,
                               bool transient_transport_error,
                               bool transient_http_status, long http_code,
                               bool body_empty, int attempt, int max_attempts) {
  if (attempt >= max_attempts)
    return false;
  // Only retry SAFE methods. A write (POST/PUT/PATCH/DELETE) must never be
  // re-sent automatically — a retry after an ambiguous failure can duplicate a
  // server-side side effect (double charge / double insert). Empty == GET.
  const bool safe = method.empty() || method == "GET" || method == "HEAD";
  if (!safe)
    return false;
  if (transient_transport_error || transient_http_status)
    return true;
  // An empty body on an otherwise-OK response is the shape a throttled/half-open
  // connection produces (it once caused blank renders), so retry it — but NOT for
  // responses that legitimately carry no body: 204 No Content, 304 Not Modified,
  // and any HEAD. Otherwise a real 204/304 looks like a failure and is retried.
  const bool body_expected =
      method != "HEAD" && http_code != 204 && http_code != 304;
  const bool success_code = http_code >= 200 && http_code < 400;
  if (success_code && body_empty && body_expected)
    return true;
  return false;
}

}  // namespace mb

#endif  // MINIBLINK_HOST_LOADER_MB_RETRY_POLICY_H_
