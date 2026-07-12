// mb_url_loader.cc — file-backed blink::URLLoader. Status: Phase 2 (subresources).
#include "miniblink_host/loader/mb_url_loader.h"

#include "miniblink_host/session/mb_session.h"
#include "miniblink_host/loader/mb_retry_policy.h"
#include "miniblink_host/runtime/mb_runtime.h"
#include "miniblink_host/test/mb_test_seams.h"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "ui/gfx/codec/png_codec.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/synchronization/lock.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/threading/platform_thread.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "net/base/data_url.h"
#include "net/cookies/cookie_util.h"
#include "net/cookies/site_for_cookies.h"
#include "base/strings/escape.h"
#include "net/base/filename_util.h"
#include "net/base/net_errors.h"
#include "net/http/http_request_headers.h"
#include "services/network/public/mojom/referrer_policy.mojom-shared.h"
#include "services/network/public/cpp/data_element.h"
#include "services/network/public/cpp/request_destination.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/resource_request_body.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/platform/web_url.h"
#include "third_party/blink/public/platform/web_url_error.h"
#include "third_party/blink/public/platform/web_url_response.h"
#include "third_party/blink/public/platform/resource_load_info_notifier_wrapper.h"
#include "third_party/blink/renderer/platform/loader/fetch/url_loader/url_loader_client.h"
#include "base/containers/span.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/data_pipe_producer.h"
#include "mojo/public/cpp/system/string_data_source.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "url/gurl.h"

namespace {
blink::WebURL ToWebURL(const GURL& url) {
  return blink::WebURL(blink::KURL(url.spec().c_str()));
}

size_t CurlWrite(char* ptr, size_t size, size_t nmemb, void* userdata) {
  static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
  return size * nmemb;
}

// Captures the response header block. Resets on each "HTTP/..." status line so
// that after retries and redirects the buffer holds only the FINAL response's
// headers (the ones the caller should expose to JS).
size_t CurlHeaderWrite(char* ptr, size_t size, size_t nmemb, void* userdata) {
  const size_t n = size * nmemb;
  std::string* buf = static_cast<std::string*>(userdata);
  if (std::string_view(ptr, n).rfind("HTTP/", 0) == 0)
    buf->clear();
  buf->append(ptr, n);
  return n;
}

// A transient failure is worth retrying: it's a network-layer hiccup (DNS/TLS/
// connect/timeout/dropped-connection) or a server backpressure code (429/5xx),
// not a deterministic answer like 404/403 that a retry can't change.
bool IsTransientCurlError(CURLcode rc) {
  switch (rc) {
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_PARTIAL_FILE:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
      return true;
    default:
      return false;
  }
}
bool IsTransientHttpCode(long code) {
  return code == 429 || (code >= 500 && code <= 599);
}

// One process-wide registry of per-session in-memory cookie jars. Within a session,
// Set-Cookie is honored across redirect chains and separate requests (main documents,
// subresources, workers, and successive navigations). Each curl share is touched from
// more than one
// thread now (document.cookie writes are serviced on the runtime service thread
// — see MbCookieManager — while network fetches read it on the calling thread),
// so it carries lock callbacks. One coarse lock is fine: cookie access is rare
// relative to layout/paint.
base::Lock& CookieShareLock() {
  static base::NoDestructor<base::Lock> lock;
  return *lock;
}
void CookieShareLockCb(CURL*, curl_lock_data, curl_lock_access, void*)
    NO_THREAD_SAFETY_ANALYSIS {
  CookieShareLock().Acquire();
}
void CookieShareUnlockCb(CURL*, curl_lock_data, void*)
    NO_THREAD_SAFETY_ANALYSIS {
  CookieShareLock().Release();
}
CURLSH* NewCookieShare() {
  CURLSH* s = curl_share_init();
  if (s) {
    curl_share_setopt(s, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
    curl_share_setopt(s, CURLSHOPT_LOCKFUNC, CookieShareLockCb);
    curl_share_setopt(s, CURLSHOPT_UNLOCKFUNC, CookieShareUnlockCb);
  }
  return s;
}

base::Lock& CookieShareRegistryLock() {
  static base::NoDestructor<base::Lock> lock;
  return *lock;
}

// One cookie share per session id (leaked; sessions are few). "" and unknown
// ids alias the default session's jar — the pre-session behavior.
CURLSH* CookieShareForKey(const std::string& key) {
  static auto* shares = new std::map<std::string, CURLSH*>();
  std::string k = key.empty() ? mb::MbSession::Default()->id() : key;
  base::AutoLock guard(CookieShareRegistryLock());
  auto it = shares->find(k);
  if (it != shares->end())
    return it->second;
  CURLSH* share = NewCookieShare();
  (*shares)[k] = share;
  return share;
}

// Fetch-path resolution: opaque host ctx (the owning view) -> session key.
std::map<const void*, std::string>& LoaderSessionKeys() {
  static auto* m = new std::map<const void*, std::string>();
  return *m;
}



// Synchronous HTTP(S) fetch via the system libcurl (HTTPS via SecureTransport on macOS).
// Blocks the calling task; fine for the headless/synchronous render model. Retries
// transient failures with linear backoff so a single network hiccup (which produced
// indistinguishable blank renders before) doesn't doom the whole page.
// Lowercase a copy for case-insensitive header-name checks.
std::string ToLower(const std::string& s) {
  std::string r = s;
  for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return r;
}

// Value of a header (case-insensitive `lname`) in a raw "Name: Value\r\n" block.
std::string HeaderValueFromBlock(const std::string& block,
                                 const std::string& lname) {
  // Split into logical header lines, unfolding obs-fold continuations (a line
  // starting with SP/HTAB continues the previous header's value).
  std::vector<std::string> headers;
  std::string line;
  std::string buf = block;
  buf.push_back('\n');  // flush sentinel
  for (char c : buf) {
    if (c == '\n') {
      if (!line.empty() && (line.front() == ' ' || line.front() == '\t') &&
          !headers.empty())
        headers.back() += line;  // continuation -> fold into previous header
      else if (!line.empty())
        headers.push_back(line);
      line.clear();
    } else if (c != '\r') {
      line.push_back(c);
    }
  }
  for (const std::string& h : headers) {
    std::string::size_type colon = h.find(':');
    if (colon != std::string::npos && ToLower(h.substr(0, colon)) == lname) {
      std::string v = h.substr(colon + 1);
      while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
        v.erase(v.begin());
      while (!v.empty() && (v.back() == ' ' || v.back() == '\t'))
        v.pop_back();
      return v;
    }
  }
  return {};
}

// Configure an already-initialized curl easy handle for one HTTP(S) transfer: sets every
// option EXCEPT the perform / multi-add. The caller owns `curl`, `header_block` and `body`
// (wired as HEADERDATA / WRITEDATA — they must outlive the transfer) and must
// curl_slist_free_all the returned list afterwards. Shared by the blocking FetchHttp
// (top-level navigation, MbFetchUrl) and the async curl_multi reactor (subresources/fetch).
// follow_redirects=false on the reactor path (the loader follows redirects per-hop on the
// main thread so each is reported to WillFollowRedirect).
curl_slist* ConfigureCurlEasy(CURL* curl,
                              const std::string& url,
                              const std::string& user_agent,
                              const std::string& extra_headers,
                              const std::string& post_body,
                              const std::string& post_content_type,
                              const std::string& http_method,
                              bool follow_redirects,
                              std::string* header_block,
                              std::string* body,
                              const std::string& cookie_session_key,
                              const std::string& pinned_pubkey = std::string()) {
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  // Per-request TLS public-key pinning (mbRequestPinPublicKey, item 44):
  // curl-native "sha256//BASE64[;...]" pin syntax passes through verbatim; a
  // mismatched server key fails the transfer with CURLE_SSL_PINNEDPUBKEYNOTMATCH.
  if (!pinned_pubkey.empty())
    curl_easy_setopt(curl, CURLOPT_PINNEDPUBLICKEY, pinned_pubkey.c_str());
  // Restrict to http(s) on BOTH the initial request and any libcurl-followed
  // redirect, so a "302 Location: file:///etc/passwd" (or other non-web scheme)
  // can never reach curl's FILE/SCP/etc. handlers — a local-file read / SSRF.
  // The string setopts exist since libcurl 7.85.0; older libcurl falls back to
  // the deprecated bitmask form.
#if CURL_AT_LEAST_VERSION(7, 85, 0)
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#elif defined(CURLOPT_REDIR_PROTOCOLS)
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS,
                   CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
  // Host-configured proxy (mbSetProxy). When set, applies to every request — an
  // explicit "" forces a direct connection (overriding *_proxy env vars). When
  // never set, libcurl's default proxy resolution (env vars) is left untouched.
  std::string proxy;
  if (mb::MbProxyConfigured(&proxy))
    curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());
  // Host-configured TLS verification bypass (mbSetIgnoreCertErrors) — like
  // curl -k. Off by default (verify); only disabled when explicitly requested,
  // for scraping/testing internal or dev sites with self-signed/expired certs.
  if (mb::MbIgnoreCertErrors()) {
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  }
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, CurlHeaderWrite);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, header_block);
  // Method + body. A non-GET verb (form submit, fetch/XHR POST/PUT/DELETE…) or a
  // non-empty body makes this a write request. COPYPOSTFIELDS copies the body so
  // it survives retries; CUSTOMREQUEST sets the exact verb (POST is otherwise
  // implied by POSTFIELDS, but set it explicitly so empty-body POST and
  // PUT/PATCH/DELETE are correct too).
  std::string method = http_method;
  if (method.empty() && !post_body.empty())
    method = "POST";
  if (method == "HEAD") {
    // CURLOPT_CUSTOMREQUEST alone only changes the request token; without
    // NOBODY libcurl would still perform a GET and read a response body.
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
  } else {
    if (!post_body.empty()) {
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                       static_cast<curl_off_t>(post_body.size()));
      curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, post_body.c_str());
    }
    // Preserve the caller's exact verb, including an explicit GET carrying a
    // body and an empty-body POST/PUT/PATCH/DELETE.
    if (!method.empty())
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
  }
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWrite);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, follow_redirects ? 1L : 0L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT,
                   (user_agent.empty() ? mb::MbDefaultUserAgent() : user_agent).c_str());
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");  // allow gzip, auto-decode
  curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");  // enable the in-memory cookie engine
  if (CURLSH* share = CookieShareForKey(cookie_session_key))
    curl_easy_setopt(curl, CURLOPT_SHARE, share);  // shared jar across all fetches

  // Request headers: the host's extra "Name: Value" lines, plus a default
  // Accept-Language (so sites serve localized content) unless the host set one.
  curl_slist* header_list = nullptr;
  bool has_accept_language = false;
  bool has_content_type = false;
  std::string line;
  std::string lines = extra_headers;
  // `lines` is already the finalized per-hop request header block. Policy is evaluated
  // on the engine sequence before transport submission, in this order: caller headers,
  // matching static registrations, legacy inspection/block callback, mutable hook. Do
  // not re-run static registrations here: doing so would make the mutable hook unable to
  // inspect or reliably override what curl actually sends.
  lines.push_back('\n');  // sentinel so the last line flushes
  for (char c : lines) {
    if (c == '\n' || c == '\r') {
      if (!line.empty()) {
        const std::string lline = ToLower(line);
        if (lline.rfind("accept-language:", 0) == 0)
          has_accept_language = true;
        if (lline.rfind("content-type:", 0) == 0)
          has_content_type = true;
        // Credential headers (Authorization/Cookie/...) are sent AS-IS to this hop's
        // origin. Cross-origin scoping is NOT done here by dropping headers before the
        // first request (the old, over-broad mitigation that also stripped them from the
        // intended first-party origin); it is done PER-HOP by the manual redirect drivers
        // (FetchHttp for navigation/API, MbURLLoader::OnHopComplete for subresources),
        // which strip them only when a 3xx actually crosses to a different origin. Both
        // configure curl with FOLLOWLOCATION off, so curl never re-sends a header across a
        // redirect on its own.
        header_list = curl_slist_append(header_list, line.c_str());
        line.clear();
      }
    } else {
      line.push_back(c);
    }
  }
  if (!has_accept_language)
    header_list = curl_slist_append(header_list, "Accept-Language: en-US,en;q=0.9");
  if (!has_content_type && (!post_content_type.empty() || !post_body.empty())) {
    // An explicit content type belongs on an empty-body POST/PUT too. Otherwise forms
    // with bytes default to urlencoded. A finalized header supplied by the hook/static
    // policy wins and is never duplicated here.
    const std::string ct_line =
        "Content-Type: " +
        (post_content_type.empty() ? std::string("application/x-www-form-urlencoded")
                                   : post_content_type);
    header_list = curl_slist_append(header_list, ct_line.c_str());
  }
  if (header_list)
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
  return header_list;
}

// Two URLs share an ORIGIN iff scheme, host, and effective port all match — the correct
// boundary for deciding whether a credential may cross a redirect (host-only or substring
// comparisons are not safe).
bool MbSameOrigin(const GURL& a, const GURL& b) {
  return a.scheme() == b.scheme() && a.host() == b.host() &&
         a.EffectiveIntPort() == b.EffectiveIntPort();
}

// Drop credential-bearing request headers (Authorization / Cookie / Proxy-Authorization)
// from a '\n'-joined header block. Applied when a redirect crosses origins so a credential
// set for origin A does not ride to origin B. (Cookies for the new origin are still sent —
// curl's origin-scoped shared jar handles those; this only strips an explicit Cookie header
// the caller injected for the previous origin.)
void MbStripCredentialHeaders(std::string* block) {
  if (block->empty())
    return;
  std::string out;
  size_t start = 0;
  while (start <= block->size()) {
    const size_t nl = block->find('\n', start);
    std::string line = block->substr(
        start, nl == std::string::npos ? std::string::npos : nl - start);
    std::string lname = ToLower(line.substr(0, line.find(':')));
    while (!lname.empty() && lname.back() == ' ')
      lname.pop_back();
    if (lname != "authorization" && lname != "cookie" &&
        lname != "proxy-authorization" && !line.empty()) {
      if (!out.empty())
        out += '\n';
      out += line;
    }
    if (nl == std::string::npos)
      break;
    start = nl + 1;
  }
  *block = std::move(out);
}

// Turn a '\n'/'\r'-separated "Name: Value" header block into a curl_slist (caller frees).
curl_slist* BuildHeaderSlist(const std::string& block) {
  curl_slist* headers = nullptr;
  std::string line, lines = block;
  lines.push_back('\n');  // sentinel so the last line flushes
  for (char ch : lines) {
    if (ch == '\n' || ch == '\r') {
      if (!line.empty()) {
        headers = curl_slist_append(headers, line.c_str());
        line.clear();
      }
    } else {
      line.push_back(ch);
    }
  }
  return headers;
}

// Resolve one server Location against a particular representation of the current URL.
// Transparent rewrites deliberately have TWO representations: the page-visible public URL
// and the transport/backend URL. A relative Location must therefore be resolved twice.
GURL ResolveRedirectTarget(const GURL& previous, const std::string& location) {
  GURL next = previous.Resolve(location);
  // Fetch redirect semantics retain the previous fragment when Location omits it.
  if (next.is_valid() && !next.has_ref() && previous.has_ref()) {
    GURL::Replacements replacements;
    replacements.SetRefStr(previous.ref());
    next = next.ReplaceComponents(replacements);
  }
  return next;
}

struct RedirectTargets {
  GURL fetch;
  GURL visible;
};

// Resolve one Location in both URL spaces. In addition to ordinary relative
// redirects, hide the common reverse-proxy/backend forms where a transparently
// rewritten server constructs either an absolute URL from the transport Host or
// a network-path reference ("//host/path") to that same backend authority.
// Projection is deliberately origin-strict: under the transport scheme the
// resolved target must retain the exact backend host and effective port. A
// different host or port remains a real, visible redirect.
RedirectTargets ResolveRedirectTargets(const GURL& visible_previous,
                                       const GURL& fetch_previous,
                                       const std::string& location) {
  RedirectTargets targets{ResolveRedirectTarget(fetch_previous, location),
                          ResolveRedirectTarget(visible_previous, location)};
  const bool network_path_reference =
      location.size() >= 2 && location[0] == '/' && location[1] == '/';
  // An absolute backend URL resolves to the transport origin in BOTH URL
  // spaces. A network-path reference keeps each base URL's scheme, so under a
  // cross-scheme rewrite only the transport-space result can be same-origin;
  // the explicit "//" syntax is what proves that its authority came from
  // Location rather than from the visible base URL.
  const bool names_backend_authority =
      network_path_reference || MbSameOrigin(targets.visible, fetch_previous);
  if (!targets.fetch.is_valid() || !targets.visible.is_valid() ||
      !visible_previous.SchemeIsHTTPOrHTTPS() ||
      !fetch_previous.SchemeIsHTTPOrHTTPS() ||
      MbSameOrigin(visible_previous, fetch_previous) ||
      !MbSameOrigin(targets.fetch, fetch_previous) ||
      !names_backend_authority) {
    return targets;
  }

  // `targets.visible` already has the Location's path/query/ref and the visible
  // chain's fragment-retention semantics. Replace only its backend authority.
  GURL::Replacements replacements;
  replacements.SetSchemeStr(visible_previous.scheme());
  if (visible_previous.has_username())
    replacements.SetUsernameStr(visible_previous.username());
  else
    replacements.ClearUsername();
  if (visible_previous.has_password())
    replacements.SetPasswordStr(visible_previous.password());
  else
    replacements.ClearPassword();
  replacements.SetHostStr(visible_previous.host());
  if (visible_previous.has_port())
    replacements.SetPortStr(visible_previous.port());
  else
    replacements.ClearPort();
  targets.visible = targets.visible.ReplaceComponents(replacements);
  return targets;
}

// The complete request-policy result for ONE hop. `baseline_headers` contains only
// caller/request-owned headers that may be carried (and scrubbed) to a later redirect.
// Static registrations are applied in their global registration order, then the legacy
// callback inspects that composed block, then the mutable callback gets the last word.
struct RequestPolicyDecision {
  bool blocked = false;
  bool mocked = false;
  std::string fetch_url;
  std::string headers;
  std::string pin_pubkey;
  std::string mock_body;
  std::string mock_content_type;
  int mock_status = 0;
};

std::string RedirectFetchCandidate(const GURL& visible_target,
                                   const GURL& backend_target);

void RunRequestPolicyCallbacks(const std::string& visible_url,
                               const std::string& default_fetch_url,
                               const std::string& method,
                               const std::string& body,
                               RequestPolicyDecision* decision) {
  if (mb::MbRequestHookBlocks(visible_url, method, decision->headers, body)) {
    decision->blocked = true;
    return;
  }

  mb::MbRequestMutation mutation;
  if (!mb::MbRunRequestMutateHook(default_fetch_url, method,
                                  decision->headers, body, &mutation)) {
    return;
  }
  if (mutation.block) {
    decision->blocked = true;
    return;
  }
  if (!mutation.set_url.empty() && GURL(mutation.set_url).is_valid())
    decision->fetch_url = mutation.set_url;
  mb::MbMergeRequestHeaders(&decision->headers, mutation.add_headers);
  decision->pin_pubkey = std::move(mutation.pin_pubkey);
}

base::Lock& RequestPolicyWaiterLock() {
  static base::NoDestructor<base::Lock> lock;
  return *lock;
}

std::map<std::string, int>& RequestPolicyWaitersForTesting() {
  static auto* waiters = new std::map<std::string, int>();
  return *waiters;
}

class ScopedRequestPolicyWaiterForTesting {
 public:
  explicit ScopedRequestPolicyWaiterForTesting(std::string visible_url)
      : visible_url_(std::move(visible_url)) {
    base::AutoLock guard(RequestPolicyWaiterLock());
    ++RequestPolicyWaitersForTesting()[visible_url_];
  }

  ~ScopedRequestPolicyWaiterForTesting() {
    base::AutoLock guard(RequestPolicyWaiterLock());
    auto it = RequestPolicyWaitersForTesting().find(visible_url_);
    if (it == RequestPolicyWaitersForTesting().end())
      return;
    if (--it->second == 0)
      RequestPolicyWaitersForTesting().erase(it);
  }

 private:
  const std::string visible_url_;
};

base::Lock& ResponseHookWaiterLock() {
  static base::NoDestructor<base::Lock> lock;
  return *lock;
}

std::map<std::string, int>& ResponseHookWaitersForTesting() {
  static auto* waiters = new std::map<std::string, int>();
  return *waiters;
}

class ScopedResponseHookWaiterForTesting {
 public:
  explicit ScopedResponseHookWaiterForTesting(std::string visible_url)
      : visible_url_(std::move(visible_url)) {
    base::AutoLock guard(ResponseHookWaiterLock());
    ++ResponseHookWaitersForTesting()[visible_url_];
  }

  ~ScopedResponseHookWaiterForTesting() {
    base::AutoLock guard(ResponseHookWaiterLock());
    auto it = ResponseHookWaitersForTesting().find(visible_url_);
    if (it == ResponseHookWaitersForTesting().end())
      return;
    if (--it->second == 0)
      ResponseHookWaitersForTesting().erase(it);
  }

 private:
  const std::string visible_url_;
};

RequestPolicyDecision EvaluateRequestPolicy(
    const std::string& visible_url,
    const std::string& backend_fetch_url,
    const std::string& method,
    const std::string& baseline_headers,
    const std::string& body,
    scoped_refptr<mb::MbLoaderViewContext> view_context = nullptr,
    const void* host_ctx = nullptr,
    const std::string& resource_type = std::string()) {
  // Worker-owned Blink loaders execute Deliver/redirect continuations on their worker
  // sequence, while the public request-callback contract is engine-main-thread-only.
  // Marshal the WHOLE policy lookup (rewrite/block/header/mock registries plus callbacks),
  // not only the callbacks: those process-global registries are engine-sequence APIs too.
  if (view_context) {
    const scoped_refptr<base::SingleThreadTaskRunner>& engine_runner =
        view_context->engine_task_runner();
    if (engine_runner && !engine_runner->RunsTasksInCurrentSequence()) {
      struct PolicyCall {
        base::WaitableEvent done;
        std::string visible_url;
        std::string backend_fetch_url;
        std::string method;
        std::string baseline_headers;
        std::string body;
        std::string resource_type;
        RequestPolicyDecision decision;
      };
      auto call = std::make_shared<PolicyCall>();
      call->visible_url = visible_url;
      call->backend_fetch_url = backend_fetch_url;
      call->method = method;
      call->baseline_headers = baseline_headers;
      call->body = body;
      call->resource_type = resource_type;
      if (!engine_runner->PostTask(
              FROM_HERE,
              base::BindOnce(
                  [](scoped_refptr<mb::MbLoaderViewContext> context,
                     std::shared_ptr<PolicyCall> policy_call) {
                    if (context->is_alive()) {
                      policy_call->decision = EvaluateRequestPolicy(
                          policy_call->visible_url,
                          policy_call->backend_fetch_url,
                          policy_call->method,
                          policy_call->baseline_headers, policy_call->body,
                          nullptr, context->host_ctx(),
                          policy_call->resource_type);
                    } else {
                      policy_call->decision.blocked = true;
                    }
                    policy_call->done.Signal();
                  },
                  view_context, call))) {
        call->decision.blocked = true;
        return std::move(call->decision);
      }
      std::optional<ScopedRequestPolicyWaiterForTesting> waiter;
      while (!call->done.TimedWait(base::Milliseconds(10))) {
        // Publish only after the worker has actually spent one interval in the
        // wait. Tests can then distinguish a queued request from a rendezvous
        // that teardown genuinely has to release.
        if (!waiter)
          waiter.emplace(call->visible_url);
        // Abandon when the owning view is gone OR the host has begun mbShutdown
        // (after which the engine loop is never pumped again, so the posted task
        // can never signal us).
        if (!view_context->is_alive() || mb::MbRuntime::ShutdownStarted()) {
          RequestPolicyDecision abandoned;
          abandoned.blocked = true;
          return abandoned;
        }
      }
      return std::move(call->decision);
    }
    if (!view_context->is_alive()) {
      RequestPolicyDecision decision;
      decision.blocked = true;
      return decision;
    }
    host_ctx = view_context->host_ctx();
  }

  RequestPolicyDecision decision;
  decision.fetch_url = RedirectFetchCandidate(GURL(visible_url),
                                               GURL(backend_fetch_url));
  decision.headers = baseline_headers;
  mb::MbApplyRequestHeaders(decision.fetch_url, &decision.headers,
                            /*follow_redirects=*/false);
  if (mb::MbIsUrlBlocked(decision.fetch_url) ||
      (!resource_type.empty() && mb::MbIsResourceTypeBlocked(resource_type))) {
    decision.blocked = true;
    return decision;
  }
  RunRequestPolicyCallbacks(visible_url, decision.fetch_url, method, body,
                            &decision);
  if (!decision.blocked) {
    decision.mocked = mb::MbFindMock(
        decision.fetch_url, &decision.mock_body, &decision.mock_content_type,
        &decision.mock_status, host_ctx);
  }
  return decision;
}

// A transparent rewrite for the page-visible redirect target takes precedence when one
// matches. Otherwise preserve the backend redirect chain, which is essential when the
// backend returns a relative Location and only the initial public URL had a rewrite rule.
std::string RedirectFetchCandidate(const GURL& visible_target,
                                   const GURL& backend_target) {
  const std::string visible = visible_target.spec();
  std::string rewritten = mb::MbApplyUrlRewrites(visible);
  return rewritten == visible ? backend_target.spec() : rewritten;
}

bool FetchHttp(const std::string& url, std::string* body, std::string* content_type,
               const std::string& user_agent, const std::string& extra_headers,
               const std::string& post_body = "",
               const std::string& post_content_type = "",
               const std::string& http_method = "", int* out_status = nullptr,
               std::string* out_headers = nullptr,
               std::string* out_final_url = nullptr,
               bool follow_redirects = true,
               std::string* out_error = nullptr,
               int* out_error_code = nullptr,
               const void* host_ctx = nullptr,
               const std::string& pinned_pubkey = std::string(),
               const std::string& initial_request_headers = std::string(),
               const std::string& initial_visible_url = std::string(),
               bool* out_redirected = nullptr,
               const std::string& cookie_session_key = std::string()) {
  if (out_redirected)
    *out_redirected = false;
  // Redirects are followed MANUALLY, one hop at a time (CURLOPT_FOLLOWLOCATION off in
  // ConfigureCurlEasy), so we can apply proper per-hop credential scoping: send the
  // caller's Authorization/Cookie/etc. to the INTENDED first origin, then strip them only
  // when a 3xx crosses to a different origin. curl's own auto-follow re-sends custom
  // CURLOPT_HTTPHEADER lines verbatim across origins with no scrub — which is exactly the
  // leak (and the over-broad "drop before the first request" mitigation) this replaces.
  std::string cur = url;                 // current hop URL (advances on each redirect)
  std::string method = http_method;      // may be rewritten to GET on 301/302/303
  if (method.empty())
    method = post_body.empty() ? "GET" : "POST";
  std::string cur_body = post_body;      // cleared when a redirect downgrades to GET
  std::string cur_extra = extra_headers; // credential headers stripped on cross-origin
  std::string cur_content_type = post_content_type;
  std::string visible_cur =
      initial_visible_url.empty() ? url : initial_visible_url;
  std::string current_headers = initial_request_headers;
  std::string current_pin = pinned_pubkey;

  constexpr int kMaxAttempts = 3;
  constexpr int kMaxHops = 20;
  CURLcode rc = CURLE_OK;
  long http_code = 0;
  std::string header_block;  // the FINAL response's raw header lines (for the caller)
  std::string final_ct;
  int hop = 0;
  bool redirected = false;
  for (;;) {
    CURL* curl = curl_easy_init();
    if (!curl)
      return false;
    header_block.clear();
    curl_slist* header_list = ConfigureCurlEasy(
        curl, cur, user_agent, current_headers, cur_body, cur_content_type, method,
        /*follow_redirects=*/false, &header_block, body,
        cookie_session_key.empty() ? mb::MbLoaderSessionKeyFor(host_ctx)
                                   : cookie_session_key,
        current_pin);

    // Per-hop transient-failure retry loop (only SAFE methods are retried; 204/304/HEAD
    // empty bodies are not treated as anomalies).
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
      body->clear();  // CurlWrite appends; start each attempt fresh
      header_block.clear();  // never expose headers from a prior failed attempt
      rc = curl_easy_perform(curl);
      http_code = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
      const bool retryable = mb::MbShouldRetryFetch(
          method, IsTransientCurlError(rc),
          rc == CURLE_OK && IsTransientHttpCode(http_code), http_code,
          body->empty(), attempt, kMaxAttempts);
      if (!retryable)
        break;
      if (std::getenv("MB_VERBOSE")) {
        std::fprintf(stderr,
                     "[mb_url_loader] transient failure (curl=%d http=%ld) on %s "
                     "— retry %d/%d\n",
                     rc, http_code, cur.c_str(), attempt, kMaxAttempts - 1);
      }
      base::PlatformThread::Sleep(base::Milliseconds(250 * attempt));
    }

    const char* ct = nullptr;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
    final_ct = ct ? ct : std::string();

    // Decide whether this is a redirect we should follow.
    const bool is_redirect = follow_redirects && rc == CURLE_OK &&
                             http_code >= 300 && http_code < 400;
    const std::string loc =
        is_redirect ? HeaderValueFromBlock(header_block, "location") : std::string();
    GURL next_fetch_raw;
    GURL next_visible;
    bool follow = false;
    if (is_redirect && !loc.empty()) {
      RedirectTargets targets = ResolveRedirectTargets(
          GURL(visible_cur), GURL(cur), loc);
      next_fetch_raw = std::move(targets.fetch);
      next_visible = std::move(targets.visible);
      follow = next_fetch_raw.is_valid() && next_visible.is_valid() &&
               hop < kMaxHops;
    }

    curl_easy_cleanup(curl);
    if (header_list)
      curl_slist_free_all(header_list);

    if (!follow)
      break;  // final response (or an unfollowable/invalid redirect) — deliver it

    // Method rewrite: 303 (and POST on 301/302) becomes a bodyless GET; 307/308 preserve
    // the method and body.
    if (http_code == 303 ||
        ((http_code == 301 || http_code == 302) && method == "POST")) {
      method = "GET";
      cur_body.clear();
      cur_content_type.clear();
    }
    const std::string next_fetch_candidate =
        RedirectFetchCandidate(next_visible, next_fetch_raw);
    std::string next_extra = cur_extra;
    if (!MbSameOrigin(GURL(cur), GURL(next_fetch_candidate)))
      MbStripCredentialHeaders(&next_extra);
    std::string next_baseline = next_extra;
    if (!cur_content_type.empty())
      mb::MbMergeRequestHeaders(&next_baseline,
                                "Content-Type: " + cur_content_type);
    RequestPolicyDecision policy = EvaluateRequestPolicy(
        next_visible.spec(), next_fetch_raw.spec(), method, next_baseline,
        cur_body, nullptr, host_ctx);
    if (policy.blocked) {
      if (out_status)
        *out_status = 0;
      if (out_headers)
        out_headers->clear();
      if (out_final_url)
        *out_final_url = next_visible.spec();
      if (out_redirected)
        *out_redirected = true;
      if (out_error)
        *out_error = mb::kMbBlockedByHookError;
      if (out_error_code)
        *out_error_code = 0;
      return false;
    }
    std::string next_fetch = std::move(policy.fetch_url);
    if (!policy.pin_pubkey.empty())
      current_pin = std::move(policy.pin_pubkey);

    if (policy.mocked) {
      *body = std::move(policy.mock_body);
      final_ct = std::move(policy.mock_content_type);
      http_code = policy.mock_status > 0 ? policy.mock_status : 200;
      header_block.clear();
      visible_cur = next_visible.spec();
      redirected = true;
      rc = CURLE_OK;
      break;
    }

    // A raw non-web Location is never fetched. Interception above may block it,
    // mock it, or explicitly rewrite it back to http(s); otherwise expose the
    // original 3xx response without granting local-protocol access.
    const GURL next_fetch_url(next_fetch);
    if (!next_fetch_url.SchemeIsHTTPOrHTTPS())
      break;

    // Cross-origin transition: strip the caller's credential headers so they don't ride to
    // a different origin. Same-origin hops keep them.
    if (!MbSameOrigin(GURL(cur), next_fetch_url))
      MbStripCredentialHeaders(&next_extra);
    cur_extra = std::move(next_extra);
    cur = next_fetch;
    visible_cur = next_visible.spec();
    current_headers = std::move(policy.headers);
    hop++;
    redirected = true;
  }

  if (content_type)
    *content_type = final_ct;
  if (out_status)
    *out_status = static_cast<int>(http_code);
  if (out_headers)
    *out_headers = std::move(header_block);
  if (out_final_url)
    *out_final_url = visible_cur;
  if (out_redirected)
    *out_redirected = redirected;
  // Success means we got a complete HTTP response — ANY status, incl. 4xx/5xx. Those are
  // real answers the caller must deliver (a 404 page should render), NOT network failures.
  // Only a transport error (no response at all) returns false.
  const bool ok = rc == CURLE_OK && http_code > 0;
  if (out_error) {
    if (ok)
      out_error->clear();
    else if (rc != CURLE_OK)
      *out_error = curl_easy_strerror(rc);  // "Couldn't resolve host name", etc.
    else
      *out_error = "no response from server";
  }
  if (out_error_code)  // the CURLcode, for machine-checkable failure handling
    *out_error_code = (!ok && rc != CURLE_OK) ? static_cast<int>(rc) : 0;
  return ok;
}

// ---------------------------------------------------------------------------
// MbCurlReactor — a libcurl curl_multi event loop on ONE dedicated IO thread.
//
// THE architecture match with miniblink49's WebURLLoaderManager: instead of one blocking
// curl_easy_perform per thread-pool worker, a single dedicated thread runs curl_multi and
// MULTIPLEXES many concurrent transfers (non-blocking). The main thread never blocks on a
// socket; each transfer's result posts back to the submitter's task runner — mirroring
// WebURLLoaderManagerMainTask. (miniblink49 spreads jobs over 4 such IO threads; one
// curl_multi loop already drives hundreds of sockets, so we start with one reactor thread
// and could round-robin across N later for CPU parallelism.)
//
// A whole-response buffer is accumulated per transfer (like the prior blocking path), so
// the loader's existing per-hop redirect chain (OnHopComplete) and DeliverResponse are
// reused unchanged — only HOW one hop is transferred moves from a blocking pool task to
// this reactor.
class MbCurlReactor {
 public:
  struct Result {
    bool ok = false;
    long http_status = 0;
    int curl_code = 0;          // CURLcode of the transfer (0 == CURLE_OK); nonzero on a
                                // transport failure (DNS/TLS/timeout/aborted), for diagnostics
    std::string body;           // accumulated response body (decoded)
    std::string headers;        // raw final-response header block
    std::string content_type;   // curl CONTENT_TYPE (may be empty)
    std::string effective_url;  // curl EFFECTIVE_URL
  };
  struct Request {
    std::string url, method, body, post_content_type, user_agent, extra_headers;
    std::string cookie_session_key;  // the submitting view's session jar
    std::string pin_pubkey;  // per-request TLS public-key pin (may be empty)
    // Optional cancellation token: setting *cancel aborts the transfer promptly (via the
    // xferinfo callback) — used by the async navigation engine (MbCancelNavigation).
    std::shared_ptr<std::atomic<bool>> cancel;
  };

  // curl progress callback: return nonzero to abort this transfer when its cancel token is
  // set. curl then reports the easy handle DONE with CURLE_ABORTED_BY_CALLBACK, reaped and
  // posted like any other completion (ok=false).
  static int CancelXfer(void* data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* cancel = static_cast<std::atomic<bool>*>(data);
    return (cancel && cancel->load()) ? 1 : 0;
  }

  static MbCurlReactor* Get() {
    static MbCurlReactor* instance = new MbCurlReactor();  // leaked (process lifetime)
    return instance;
  }

  // Thread-safe; returns immediately. `on_done` runs on `reply_runner` with the Result.
  void Submit(Request req,
              scoped_refptr<base::SequencedTaskRunner> reply_runner,
              base::OnceCallback<void(Result)> on_done) {
    // Round-robin across the IO lanes — miniblink49 spreads jobs over its 4 IO threads
    // the same way. Each lane is an independent curl_multi loop on its own thread.
    Lane* lane = lanes_[next_lane_.fetch_add(1) % lanes_.size()].get();
    {
      base::AutoLock l(lane->lock);
      lane->pending.push_back(std::make_unique<Pending>(
          std::move(req), std::move(reply_runner), std::move(on_done)));
    }
    // curl_multi_wakeup is the ONLY curl_multi function safe to call off the owning
    // thread — it breaks this lane's curl_multi_poll so the new job is adopted now.
    if (lane->multi)
      curl_multi_wakeup(lane->multi);
  }

  // Break every lane out of its poll so a just-set cancel token is observed now (its
  // xferinfo callback then aborts the transfer) instead of after the ~1s poll timeout.
  void Wake() {
    for (auto& lane : lanes_) {
      if (lane->multi)
        curl_multi_wakeup(lane->multi);
    }
  }

 private:
  struct Pending {
    Request req;
    scoped_refptr<base::SequencedTaskRunner> reply_runner;
    base::OnceCallback<void(Result)> on_done;
    Pending(Request r,
            scoped_refptr<base::SequencedTaskRunner> run,
            base::OnceCallback<void(Result)> cb)
        : req(std::move(r)), reply_runner(std::move(run)), on_done(std::move(cb)) {}
  };
  struct Job {
    CURL* easy = nullptr;
    curl_slist* headers = nullptr;
    std::string body, header_block;  // WRITEDATA / HEADERDATA targets (outlive transfer)
    scoped_refptr<base::SequencedTaskRunner> reply_runner;
    base::OnceCallback<void(Result)> on_done;
    std::shared_ptr<std::atomic<bool>> cancel;  // kept alive while the transfer runs
  };
  // One IO lane: an independent curl_multi event loop on its own dedicated thread.
  // Each lane owns its multi handle + active jobs (touched only on its thread) and a
  // lock-guarded pending queue (pushed to by Submit on the main thread).
  struct Lane {
    CURLM* multi = nullptr;
    std::thread thread;
    base::Lock lock;
    std::vector<std::unique_ptr<Pending>> pending;  // GUARDED_BY(lock)
    std::vector<std::unique_ptr<Job>> active;       // lane-thread only
  };

  // miniblink49 uses 4 dedicated IO threads (BlinkPlatformImpl::getIoThreads). Match it:
  // one curl_multi loop already multiplexes many sockets, and 4 spread the per-transfer
  // curl work (TLS, header/body callbacks) across cores under heavy fan-out (YouTube).
  static constexpr size_t kLaneCount = 4;

  MbCurlReactor() {
    for (size_t i = 0; i < kLaneCount; ++i) {
      auto lane = std::make_unique<Lane>();
      lane->multi = curl_multi_init();
      Lane* raw = lane.get();
      // The lane threads run for the process lifetime; the reactor singleton is leaked,
      // so these std::thread members are never destructed (no join/terminate). Mirrors
      // the engine's "globals leak, OS reclaims at exit" model (MbRuntime::Shutdown).
      lane->thread = std::thread([raw] { RunLane(raw); });
      lanes_.push_back(std::move(lane));
    }
  }

  static void RunLane(Lane* lane) {
    for (;;) {
      // 1. Adopt newly-submitted requests into this lane's multi handle.
      std::vector<std::unique_ptr<Pending>> batch;
      {
        base::AutoLock l(lane->lock);
        batch.swap(lane->pending);
      }
      for (auto& p : batch) {
        auto job = std::make_unique<Job>();
        job->reply_runner = std::move(p->reply_runner);
        job->on_done = std::move(p->on_done);
        job->cancel = std::move(p->req.cancel);
        // Already cancelled before we even started — report failure and drop.
        if (job->cancel && job->cancel->load()) {
          job->reply_runner->PostTask(
              FROM_HERE, base::BindOnce(std::move(job->on_done), Result{}));
          continue;
        }
        job->easy = curl_easy_init();
        if (!job->easy) {
          // Report the failure to the caller and drop the job.
          job->reply_runner->PostTask(
              FROM_HERE, base::BindOnce(std::move(job->on_done), Result{}));
          continue;
        }
        // follow_redirects=false: the loader follows per-hop on the main thread.
        job->headers = ConfigureCurlEasy(
            job->easy, p->req.url, p->req.user_agent, p->req.extra_headers,
            p->req.body, p->req.post_content_type, p->req.method,
            /*follow_redirects=*/false, &job->header_block, &job->body,
            p->req.cookie_session_key, p->req.pin_pubkey);
        // Wire prompt cancellation for a job that carries a token.
        if (job->cancel) {
          curl_easy_setopt(job->easy, CURLOPT_NOPROGRESS, 0L);
          curl_easy_setopt(job->easy, CURLOPT_XFERINFOFUNCTION, &CancelXfer);
          curl_easy_setopt(job->easy, CURLOPT_XFERINFODATA, job->cancel.get());
        }
        curl_easy_setopt(job->easy, CURLOPT_PRIVATE, job.get());
        curl_multi_add_handle(lane->multi, job->easy);
        lane->active.push_back(std::move(job));
      }

      // 2. Drive all in-flight transfers on this lane (non-blocking).
      int running = 0;
      curl_multi_perform(lane->multi, &running);

      // 3. Reap completed transfers and deliver each result to its submitter.
      int in_queue = 0;
      while (CURLMsg* m = curl_multi_info_read(lane->multi, &in_queue)) {
        if (m->msg != CURLMSG_DONE)
          continue;
        CURL* easy = m->easy_handle;
        Job* job = nullptr;
        curl_easy_getinfo(easy, CURLINFO_PRIVATE, &job);
        if (job) {
          Result r;
          long code = 0;
          curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code);
          r.http_status = code;
          r.curl_code = static_cast<int>(m->data.result);  // CURLcode (for diagnostics)
          // A complete HTTP response of ANY status (incl. 4xx/5xx) is success; only a
          // transport error (no response) is a failure (matches FetchHttp).
          r.ok = (m->data.result == CURLE_OK && code > 0);
          const char* ct = nullptr;
          if (curl_easy_getinfo(easy, CURLINFO_CONTENT_TYPE, &ct) == CURLE_OK && ct)
            r.content_type = ct;
          const char* eu = nullptr;
          if (curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &eu) == CURLE_OK && eu)
            r.effective_url = eu;
          r.body = std::move(job->body);
          r.headers = std::move(job->header_block);
          job->reply_runner->PostTask(
              FROM_HERE, base::BindOnce(std::move(job->on_done), std::move(r)));
        }
        curl_multi_remove_handle(lane->multi, easy);
        curl_easy_cleanup(easy);
        if (job && job->headers)
          curl_slist_free_all(job->headers);
        if (job) {
          for (auto it = lane->active.begin(); it != lane->active.end(); ++it) {
            if (it->get() == job) {
              lane->active.erase(it);
              break;
            }
          }
        }
      }

      // 4. Block until socket activity, the timeout, or a Submit() wakeup.
      int numfds = 0;
      curl_multi_poll(lane->multi, nullptr, 0, /*timeout_ms=*/1000, &numfds);
    }
  }

  std::vector<std::unique_ptr<Lane>> lanes_;
  std::atomic<unsigned> next_lane_{0};
};
}  // namespace


namespace {
// file:// URL -> readable FilePath. net::FileURLToFilePath (percent-decodes,
// handles drive letters) first; on Windows it rejects the drive-less unix-style
// paths ("file:///tmp/x") the test fixtures use, so fall back to the decoded
// URL path, which the CRT resolves against the current drive.
bool MbReadFileURL(const GURL& url, std::string* body) {
  base::FilePath fp;
  if (net::FileURLToFilePath(url, &fp) && base::ReadFileToString(fp, body))
    return true;
  const std::string decoded = base::UnescapeBinaryURLComponent(url.path());
  return !decoded.empty() &&
         base::ReadFileToString(base::FilePath::FromUTF8Unsafe(decoded), body);
}
}  // namespace

namespace mb {
namespace {

blink::WebString MimeFromPath(const std::string& path) {
  auto ends = [&](const char* s) {
    size_t n = std::char_traits<char>::length(s);
    return path.size() >= n && path.compare(path.size() - n, n, s) == 0;
  };
  if (ends(".html") || ends(".htm")) return blink::WebString::FromUtf8("text/html");
  if (ends(".css")) return blink::WebString::FromUtf8("text/css");
  if (ends(".js") || ends(".mjs")) return blink::WebString::FromUtf8("text/javascript");
  if (ends(".svg")) return blink::WebString::FromUtf8("image/svg+xml");
  if (ends(".png")) return blink::WebString::FromUtf8("image/png");
  if (ends(".jpg") || ends(".jpeg")) return blink::WebString::FromUtf8("image/jpeg");
  if (ends(".gif")) return blink::WebString::FromUtf8("image/gif");
  if (ends(".json")) return blink::WebString::FromUtf8("application/json");
  return blink::WebString::FromUtf8("application/octet-stream");
}

}  // namespace

MbLoaderViewContext::MbLoaderViewContext(
    const void* host_ctx,
    scoped_refptr<base::SingleThreadTaskRunner> engine_task_runner,
    std::string session_key)
    : host_ctx_(host_ctx),
      engine_task_runner_(std::move(engine_task_runner)),
      session_key_(std::move(session_key)) {}

MbLoaderViewContext::MbLoaderViewContext(
    scoped_refptr<MbLoaderViewContext> owner_context)
    : host_ctx_(nullptr),
      engine_task_runner_(owner_context ? owner_context->engine_task_runner()
                                        : nullptr),
      session_key_(owner_context ? owner_context->session_key() : std::string()),
      owner_context_(std::move(owner_context)) {}

MbLoaderViewContext::~MbLoaderViewContext() {
  MbNetForgetActivityContext(activity_key());
}

const void* MbLoaderViewContext::host_ctx() const {
  if (!alive_.load(std::memory_order_acquire))
    return nullptr;
  if (!owner_context_)
    return host_ctx_;
  base::AutoLock guard(activity_lock_);
  if (!activity_contexts_registered_)
    return owner_context_->host_ctx();
  for (const auto& context : activity_contexts_) {
    if (const void* host = context->host_ctx())
      return host;
  }
  return nullptr;
}

bool MbLoaderViewContext::is_alive() const {
  if (!alive_.load(std::memory_order_acquire))
    return false;
  if (!owner_context_)
    return true;
  base::AutoLock guard(activity_lock_);
  if (!activity_contexts_registered_)
    return owner_context_->is_alive();
  for (const auto& context : activity_contexts_) {
    if (context->is_alive())
      return true;
  }
  return false;
}

void MbLoaderViewContext::Invalidate() {
  alive_.store(false, std::memory_order_release);
}

void MbLoaderViewContext::AddActivityContext(
    scoped_refptr<MbLoaderViewContext> context) {
  if (!context)
    return;
  base::AutoLock guard(activity_lock_);
  activity_contexts_registered_ = true;
  activity_contexts_.push_back(std::move(context));
}

void MbLoaderViewContext::RemoveActivityContext(
    const MbLoaderViewContext* context) {
  if (!context)
    return;
  base::AutoLock guard(activity_lock_);
  auto it = std::find_if(
      activity_contexts_.begin(), activity_contexts_.end(),
      [context](const scoped_refptr<MbLoaderViewContext>& candidate) {
        return candidate.get() == context;
      });
  if (it != activity_contexts_.end())
    activity_contexts_.erase(it);
}

std::vector<scoped_refptr<MbLoaderViewContext>>
MbLoaderViewContext::activity_contexts() const {
  std::vector<scoped_refptr<MbLoaderViewContext>> contexts;
  base::AutoLock guard(activity_lock_);
  if (!owner_context_ || !activity_contexts_registered_) {
    contexts.push_back(owner_context_
                           ? owner_context_
                           : base::WrapRefCounted(
                                 const_cast<MbLoaderViewContext*>(this)));
  }
  for (const auto& context : activity_contexts_) {
    auto it = std::find_if(
        contexts.begin(), contexts.end(),
        [&context](const scoped_refptr<MbLoaderViewContext>& candidate) {
          return candidate.get() == context.get();
        });
    if (it == contexts.end())
      contexts.push_back(context);
  }
  if (contexts.empty()) {
    // A SharedWorker with no attributable live client may still be unwinding one load.
    // Give that load a stable teardown-only bucket rather than a null/global key.
    contexts.push_back(
        base::WrapRefCounted(const_cast<MbLoaderViewContext*>(this)));
  }
  return contexts;
}

const std::string& MbDefaultUserAgent() {
  // M150 desktop Chrome on macOS — current enough that UA-sniffing sites serve
  // their modern desktop layout/JS. NoDestructor: no exit-time dtor (-Werror).
  static const base::NoDestructor<std::string> kUa(
      "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/150.0.0.0 Safari/537.36");
  return *kUa;
}

namespace {
// Process-wide proxy. A NoDestructor string (no exit-time dtor) plus a flag
// distinguishing "never set" (honor libcurl defaults) from "set to ''" (force
// direct). Reactor and streaming workers snapshot it under this lock.
std::string& ProxyStorage() {
  static base::NoDestructor<std::string> s;
  return *s;
}
base::Lock& ProxyLock() {
  static base::NoDestructor<base::Lock> lock;
  return *lock;
}
bool g_proxy_set = false;
}  // namespace

void MbSetProxy(const std::string& proxy) {
  base::AutoLock guard(ProxyLock());
  ProxyStorage() = proxy;
  g_proxy_set = true;
}

bool MbProxyConfigured(std::string* out) {
  base::AutoLock guard(ProxyLock());
  if (g_proxy_set && out)
    *out = ProxyStorage();
  return g_proxy_set;
}

namespace {
// When true, TLS peer/host verification is disabled for network fetches (the
// equivalent of curl -k / --no-check-certificate). Default false — verify, the
// secure default. Set on the main thread before navigating; read on the fetch
// path (same single-thread model as the proxy flag).
std::atomic<bool> g_ignore_cert_errors{false};

// When false, a top-level/subresource fetch does NOT follow 3xx redirects, so the
// caller sees the redirect response itself (status 30x + Location header) — for
// resolving URL shorteners / inspecting redirect chains. Default true (follow).
std::atomic<bool> g_follow_redirects{true};
}  // namespace

void MbSetIgnoreCertErrors(bool ignore) {
  g_ignore_cert_errors.store(ignore, std::memory_order_release);
}

bool MbIgnoreCertErrors() {
  return g_ignore_cert_errors.load(std::memory_order_acquire);
}

void MbSetFollowRedirects(bool follow) {
  g_follow_redirects.store(follow, std::memory_order_release);
}

bool MbFollowRedirects() {
  return g_follow_redirects.load(std::memory_order_acquire);
}

namespace {
// Trim ASCII spaces/tabs from both ends.
void TrimWs(std::string* s) {
  while (!s->empty() && (s->front() == ' ' || s->front() == '\t'))
    s->erase(s->begin());
  while (!s->empty() && (s->back() == ' ' || s->back() == '\t'))
    s->pop_back();
}
// Extract a cookie attribute's value from "; name=value; ..." (case-insensitive
// name). Returns "" if absent; for valueless flags (e.g. "secure") use Contains.
std::string AttrValue(const std::string& attrs_lower, const std::string& attrs,
                      const std::string& name) {
  const std::string key = name + "=";
  std::string::size_type p = attrs_lower.find(key);
  if (p == std::string::npos)
    return {};
  p += key.size();
  std::string::size_type end = attrs.find(';', p);
  std::string v = attrs.substr(p, end == std::string::npos ? end : end - p);
  TrimWs(&v);
  return v;
}
}  // namespace

void MbAddCookieToJar(const std::string& url, const std::string& cookie,
                      const std::string& session_key) {
  GURL gurl(url);
  if (!gurl.SchemeIsHTTPOrHTTPS())
    return;  // cookies only make sense for network origins
  // Split "name=value; attr; attr". document.cookie assigns one cookie.
  std::string pair = cookie, attrs;
  if (std::string::size_type semi = cookie.find(';'); semi != std::string::npos) {
    pair = cookie.substr(0, semi);
    attrs = cookie.substr(semi + 1);
  }
  std::string::size_type eq = pair.find('=');
  if (eq == std::string::npos)
    return;
  std::string name = pair.substr(0, eq), value = pair.substr(eq + 1);
  TrimWs(&name);
  TrimWs(&value);
  if (name.empty())
    return;
  const std::string lattrs = ToLower(attrs);
  // domain: explicit attr (subdomain-matching) else host-only.
  std::string domain = AttrValue(lattrs, attrs, "domain");
  std::string tailmatch = "FALSE";
  if (!domain.empty()) {
    tailmatch = "TRUE";  // an explicit Domain applies to subdomains
  } else {
    domain = gurl.host();
  }
  std::string path = AttrValue(lattrs, attrs, "path");
  if (path.empty())
    path = "/";
  const std::string secure =
      lattrs.find("secure") != std::string::npos ? "TRUE" : "FALSE";
  // Netscape expiry field (unix epoch seconds; 0 = session). A deletion — Max-Age<=0
  // or an Expires in the PAST (ANY past date, not just the 1970 epoch) — becomes a
  // past timestamp ("1") so curl drops the cookie; a future Expires/Max-Age persists
  // until then. Max-Age takes precedence over Expires per RFC 6265.
  std::string expiry = "0";
  if (std::string ma = AttrValue(lattrs, attrs, "max-age"); !ma.empty()) {
    int64_t secs = 0;
    if (base::StringToInt64(ma, &secs)) {
      expiry = secs <= 0
                   ? "1"
                   : base::NumberToString(
                         (base::Time::Now() - base::Time::UnixEpoch()).InSeconds() +
                         secs);
    }
  } else if (std::string exp = AttrValue(lattrs, attrs, "expires"); !exp.empty()) {
    base::Time t = net::cookie_util::ParseCookieExpirationTime(exp);
    if (!t.is_null()) {
      int64_t secs = (t - base::Time::UnixEpoch()).InSeconds();
      expiry = secs <= 0 ? "1" : base::NumberToString(secs);
    } else if (lattrs.find("expires=thu, 01 jan 1970") != std::string::npos) {
      expiry = "1";  // fallback for the canonical epoch deletion string
    }
  }
  // Netscape TSV (domain tailmatch path secure expiry name value) — injected via
  // COOKIELIST with an explicit domain, so it needs no active transfer to land in
  // the shared jar (a "Set-Cookie:"-format line without a domain is dropped).
  const std::string ns = domain + "\t" + tailmatch + "\t" + path + "\t" + secure +
                         "\t" + expiry + "\t" + name + "\t" + value;
  CURL* curl = curl_easy_init();
  if (!curl)
    return;
  curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");  // enable the cookie engine
  if (CURLSH* share = CookieShareForKey(session_key))
    curl_easy_setopt(curl, CURLOPT_SHARE, share);
  curl_easy_setopt(curl, CURLOPT_COOKIELIST, ns.c_str());
  curl_easy_cleanup(curl);
}

void MbClearCookieJar(const std::string& session_key) {
  CURL* curl = curl_easy_init();
  if (!curl)
    return;
  curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");  // enable the cookie engine
  if (CURLSH* share = CookieShareForKey(session_key))
    curl_easy_setopt(curl, CURLOPT_SHARE, share);
  curl_easy_setopt(curl, CURLOPT_COOKIELIST, "ALL");  // erase every cookie
  curl_easy_cleanup(curl);
}

std::string MbGetAllCookies(const std::string& session_key) {
  // Snapshot this browsing session's whole jar (every host and cookie lifetime) via
  // CURLINFO_COOKIELIST — the same list MbGetCookiesForUrl reads — formatted as a
  // Netscape cookie file in memory. We format it ourselves rather than relying on
  // CURLOPT_COOKIEJAR flushing a shared store, which is unreliable without a
  // transfer.
  CURL* curl = curl_easy_init();
  if (!curl)
    return std::string();
  curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");  // enable the cookie engine
  if (CURLSH* share = CookieShareForKey(session_key))
    curl_easy_setopt(curl, CURLOPT_SHARE, share);
  struct curl_slist* list = nullptr;
  curl_easy_getinfo(curl, CURLINFO_COOKIELIST, &list);
  std::string out = "# Netscape HTTP Cookie File\n";
  for (struct curl_slist* it = list; it; it = it->next) {
    if (it->data) {
      out += it->data;
      out += "\n";
    }
  }
  curl_slist_free_all(list);
  curl_easy_cleanup(curl);
  return out;
}

bool MbSaveCookies(const std::string& path, const std::string& session_key) {
  if (path.empty())
    return false;
  // Write THIS session's jar snapshot (a Netscape cookie file, curl's native
  // format, reloadable by curl itself) to `path`. session_key must be forwarded
  // to MbGetAllCookies — the no-arg form snapshots the DEFAULT session's jar, so
  // a custom session would otherwise persist the wrong cookies (they would not
  // round-trip against MbLoadCookies(path, id_)).
  return base::WriteFile(base::FilePath::FromUTF8Unsafe(path),
                         MbGetAllCookies(session_key));
}

bool MbLoadCookies(const std::string& path, const std::string& session_key) {
  std::string contents;
  if (path.empty() ||
      !base::ReadFileToString(base::FilePath::FromUTF8Unsafe(path), &contents))
    return false;
  CURL* curl = curl_easy_init();
  if (!curl)
    return false;
  curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");  // enable the cookie engine
  if (CURLSH* share = CookieShareForKey(session_key))
    curl_easy_setopt(curl, CURLOPT_SHARE, share);
  // Inject each Netscape line via COOKIELIST (same path as MbAddCookieToJar), so
  // no transfer is needed. Keep "#HttpOnly_..." lines (curl's HttpOnly marker);
  // skip blank lines and ordinary "# ..." comments (e.g. the header).
  std::string::size_type start = 0;
  for (std::string::size_type i = 0; i <= contents.size(); ++i) {
    if (i == contents.size() || contents[i] == '\n' || contents[i] == '\r') {
      std::string line = contents.substr(start, i - start);
      start = i + 1;
      if (line.empty() ||
          (line[0] == '#' && line.rfind("#HttpOnly_", 0) != 0))
        continue;
      curl_easy_setopt(curl, CURLOPT_COOKIELIST, line.c_str());
    }
  }
  curl_easy_cleanup(curl);
  return true;
}

std::string MbGetCookiesForUrl(const std::string& url,
                               const std::string& session_key) {
  GURL gurl(url);
  if (!gurl.SchemeIsHTTPOrHTTPS())
    return {};
  const std::string host(gurl.host());
  CURL* curl = curl_easy_init();
  if (!curl)
    return {};
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");  // enable engine
  if (CURLSH* share = CookieShareForKey(session_key))
    curl_easy_setopt(curl, CURLOPT_SHARE, share);
  struct curl_slist* list = nullptr;
  curl_easy_getinfo(curl, CURLINFO_COOKIELIST, &list);
  std::string out;
  for (struct curl_slist* it = list; it; it = it->next) {
    // Netscape TSV: domain \t flag \t path \t secure \t expiry \t name \t value.
    // "#HttpOnly_" domain prefix marks HttpOnly cookies — exclude them.
    std::string line = it->data ? it->data : "";
    if (line.rfind("#HttpOnly_", 0) == 0)
      continue;
    std::vector<std::string> f;
    size_t start = 0;
    for (size_t i = 0; i <= line.size(); ++i) {
      if (i == line.size() || line[i] == '\t') {
        f.push_back(line.substr(start, i - start));
        start = i + 1;
      }
    }
    if (f.size() < 7)
      continue;
    std::string dom = f[0];
    if (!dom.empty() && dom[0] == '.')
      dom = dom.substr(1);
    const bool match =
        host == dom ||
        (host.size() > dom.size() &&
         host.compare(host.size() - dom.size() - 1, dom.size() + 1, "." + dom) == 0);
    if (!match)
      continue;
    if (!out.empty())
      out += "; ";
    out += f[5] + "=" + f[6];
  }
  if (list)
    curl_slist_free_all(list);
  curl_easy_cleanup(curl);
  return out;
}

// --- Request log -------------------------------------------------------------
namespace {
// Leaked-new (avoids an exit-time destructor on a function-local static). Capped
// so a long-lived process can't grow it without bound; oldest entries drop first.
std::vector<std::string>& RequestLog() {
  static std::vector<std::string>* log = new std::vector<std::string>();
  return *log;
}
base::Lock& RequestLogLock() {
  static base::NoDestructor<base::Lock> lock;
  return *lock;
}
constexpr size_t kRequestLogCap = 2048;
}  // namespace

void MbRecordRequest(const std::string& url) {
  if (url.empty())
    return;
  base::AutoLock guard(RequestLogLock());
  auto& log = RequestLog();
  if (log.size() >= kRequestLogCap)
    log.erase(log.begin(), log.begin() + (log.size() - kRequestLogCap + 1));
  log.push_back(url);
}

std::string MbGetRequestLog() {
  base::AutoLock guard(RequestLogLock());
  std::string out;
  for (const std::string& u : RequestLog()) {
    out += u;
    out += "\n";
  }
  return out;
}

void MbClearRequestLog() {
  base::AutoLock guard(RequestLogLock());
  RequestLog().clear();
}

size_t MbRequestCount() {
  base::AutoLock guard(RequestLogLock());
  return RequestLog().size();
}

int MbRequestPolicyWaiterCountForTesting(const std::string& visible_url) {
  base::AutoLock guard(RequestPolicyWaiterLock());
  auto it = RequestPolicyWaitersForTesting().find(visible_url);
  return it == RequestPolicyWaitersForTesting().end() ? 0 : it->second;
}

int MbResponseHookWaiterCountForTesting(const std::string& visible_url) {
  base::AutoLock guard(ResponseHookWaiterLock());
  auto it = ResponseHookWaitersForTesting().find(visible_url);
  return it == ResponseHookWaitersForTesting().end() ? 0 : it->second;
}

// --- Per-view network activity (backs mbWaitForNetworkIdleEx) -----------------
// See the header for why the capped request-log size is the wrong idle signal.
namespace {
struct ViewNetActivity {
  uint64_t started = 0;  // monotonic; never pinned, so ongoing traffic always moves it
  int in_flight = 0;     // loads currently outstanding for this view
  bool retired = false;  // attribution token died; erase when in_flight reaches zero
};
// Worker fetch contexts create/destroy their loaders on worker sequences while the
// engine sequence polls for idle, so every access is protected by the same lock.
base::Lock& ViewNetLock() {
  static base::NoDestructor<base::Lock> lock;
  return *lock;
}
// Leaked-new to avoid an exit-time destructor on a function-local static.
std::map<const void*, ViewNetActivity>& ViewNetMap() {
  static auto* m = new std::map<const void*, ViewNetActivity>();
  return *m;
}
}  // namespace

void MbNetRequestStarted(const void* view_ctx) {
  if (!view_ctx)
    return;
  base::AutoLock guard(ViewNetLock());
  ViewNetActivity& a = ViewNetMap()[view_ctx];
  // A stable token is retained by each counted load. Reaching this branch after
  // retirement therefore means a new token legitimately reused an already-quiescent
  // raw key; start a fresh lifetime rather than inheriting the old monotonic count.
  if (a.retired && a.in_flight == 0)
    a = ViewNetActivity();
  ++a.started;
  ++a.in_flight;
}

void MbNetRequestFinished(const void* view_ctx) {
  if (!view_ctx)
    return;
  base::AutoLock guard(ViewNetLock());
  auto it = ViewNetMap().find(view_ctx);
  if (it != ViewNetMap().end() && it->second.in_flight > 0) {
    --it->second.in_flight;
    if (it->second.in_flight == 0 && it->second.retired)
      ViewNetMap().erase(it);
  }
}

uint64_t MbNetStartedCount(const void* view_ctx) {
  if (!view_ctx)
    return 0;
  base::AutoLock guard(ViewNetLock());
  auto it = ViewNetMap().find(view_ctx);
  return it == ViewNetMap().end() ? 0u : it->second.started;
}

int MbNetInFlight(const void* view_ctx) {
  if (!view_ctx)
    return 0;
  base::AutoLock guard(ViewNetLock());
  auto it = ViewNetMap().find(view_ctx);
  return it == ViewNetMap().end() ? 0 : it->second.in_flight;
}

void MbNetForgetActivityContext(const void* view_ctx) {
  if (!view_ctx)
    return;
  base::AutoLock guard(ViewNetLock());
  auto it = ViewNetMap().find(view_ctx);
  if (it == ViewNetMap().end())
    return;
  if (it->second.in_flight == 0) {
    ViewNetMap().erase(it);
  } else {
    // Defensive for legacy raw-context loaders: their host may retire before the
    // loader destructor balances the counter. Keep the entry until that finish.
    it->second.retired = true;
  }
}

bool MbNetHasActivityContextForTesting(const void* view_ctx) {
  base::AutoLock guard(ViewNetLock());
  return ViewNetMap().find(view_ctx) != ViewNetMap().end();
}

// --- Request blocking --------------------------------------------------------
namespace {
std::vector<std::string>& BlockList() {
  static std::vector<std::string>* bl = new std::vector<std::string>();
  return *bl;
}
}  // namespace

void MbBlockUrl(const std::string& substring) {
  if (!substring.empty())
    BlockList().push_back(substring);
}

void MbClearUrlBlocks() {
  BlockList().clear();
}

bool MbIsUrlBlocked(const std::string& url) {
  for (const std::string& s : BlockList())
    if (url.find(s) != std::string::npos)
      return true;
  return false;
}

// --- Block by resource TYPE (fetch destination: image/font/style/script/media/...) ---
// Lets a scrape skip whole resource classes (block "image"+"font"+"media" to load text-only
// pages fast) without enumerating URLs. Keyed by the standard fetch destination string.
namespace {
std::set<std::string>& BlockedTypes() {
  static std::set<std::string>* t = new std::set<std::string>();
  return *t;
}
}  // namespace

void MbSetResourceTypeBlocked(const std::string& type, bool blocked) {
  if (type.empty())
    return;
  if (blocked)
    BlockedTypes().insert(type);
  else
    BlockedTypes().erase(type);
}

bool MbIsResourceTypeBlocked(const std::string& type) {
  return !type.empty() && BlockedTypes().count(type) != 0;
}

// --- Dynamic per-request hook ------------------------------------------------
namespace {
MbRequestHook& RequestHook() {
  static base::NoDestructor<MbRequestHook> h;
  return *h;
}
}  // namespace

void MbSetRequestHook(MbRequestHook hook) {
  RequestHook() = std::move(hook);
}

bool MbRequestHookBlocks(const std::string& url, const std::string& method,
                         const std::string& headers, const std::string& body) {
  const MbRequestHook& h = RequestHook();
  return h && h(url, method, headers, body) != 0;
}

const char kMbBlockedByHookError[] = "blocked by request hook";

namespace {
MbRequestMutateHook& RequestMutateHook() {
  static MbRequestMutateHook* h = new MbRequestMutateHook();
  return *h;
}

// Merge "Name: Value" lines from `add` into the newline-joined `lines`,
// replacing any existing same-name line (case-insensitive) so an added header
// OVERRIDES rather than duplicates — curl would send both copies otherwise.
void MergeHeaderLines(std::string* lines, const std::string& add) {
  std::vector<std::string> kept;
  auto split = [](const std::string& block, std::vector<std::string>* out) {
    size_t start = 0;
    while (start <= block.size()) {
      size_t nl = block.find('\n', start);
      std::string line = block.substr(
          start, nl == std::string::npos ? std::string::npos : nl - start);
      if (!line.empty())
        out->push_back(std::move(line));
      if (nl == std::string::npos)
        break;
      start = nl + 1;
    }
  };
  std::vector<std::string> add_lines;
  split(add, &add_lines);
  if (add_lines.empty())
    return;
  auto name_of = [](const std::string& line) {
    std::string n = line.substr(0, line.find(':'));
    for (char& c : n)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return n;
  };
  std::vector<std::string> existing;
  split(*lines, &existing);
  for (const std::string& line : existing) {
    bool replaced = false;
    for (const std::string& al : add_lines)
      if (name_of(al) == name_of(line)) {
        replaced = true;
        break;
      }
    if (!replaced)
      kept.push_back(line);
  }
  for (const std::string& al : add_lines)
    kept.push_back(al);
  lines->clear();
  for (const std::string& line : kept) {
    if (!lines->empty())
      *lines += "\n";
    *lines += line;
  }
}
}  // namespace

void MbMergeRequestHeaders(std::string* lines, const std::string& add) {
  MergeHeaderLines(lines, add);
}

void MbSetRequestMutateHook(MbRequestMutateHook hook) {
  RequestMutateHook() = std::move(hook);
}

bool MbRunRequestMutateHook(const std::string& url, const std::string& method,
                            const std::string& headers, const std::string& body,
                            MbRequestMutation* out) {
  const MbRequestMutateHook& h = RequestMutateHook();
  if (!h)
    return false;
  h(url, method, headers, body, out);
  return true;
}

// --- Response hook -----------------------------------------------------------
namespace {
MbResponseHook& ResponseHook() {
  static MbResponseHook* h = new MbResponseHook();
  return *h;
}
}  // namespace

void MbSetResponseHook(MbResponseHook hook) {
  ResponseHook() = std::move(hook);
}

void MbInvokeResponseHook(const void* host_ctx, const std::string& url, int* status,
                          std::string* headers, std::string* body) {
  if (ResponseHook())
    ResponseHook()(host_ctx, url, status, headers, body);
}

// --- Response mocking --------------------------------------------------------
// Serve a canned body for any request whose URL contains a registered substring,
// WITHOUT a real fetch — run a page offline or substitute an API response. The
// signature miniblink/automation feature; pure in-loader data, like the blocklist.
namespace {
struct MockEntry {
  std::string substring;
  std::string body;
  std::string content_type;
  int status;
};
std::vector<MockEntry>& MockList() {
  static std::vector<MockEntry>* m = new std::vector<MockEntry>();
  return *m;
}
}  // namespace

void MbAddMock(const std::string& substring, const std::string& body,
               const std::string& content_type, int status) {
  if (!substring.empty())
    MockList().push_back({substring, body, content_type, status});
}

void MbClearMocks() {
  MockList().clear();
}

// Dynamic request mock: a hook that may COMPUTE a response for any URL (one the caller
// could not pre-register as a static substring) without a real fetch. Returns true to serve.
namespace {
MbRequestMockHook& RequestMockHook() {
  static MbRequestMockHook* h = new MbRequestMockHook();
  return *h;
}
}  // namespace

void MbSetRequestMockHook(MbRequestMockHook hook) {
  RequestMockHook() = std::move(hook);
}

namespace {
// Per-context hooks, keyed by the opaque owner pointer. Leaked (no exit-time
// destructor); entries are erased when their view unregisters.
std::map<const void*, MbRequestMockHook>& ContextMockHooks() {
  static auto* m = new std::map<const void*, MbRequestMockHook>();
  return *m;
}
}  // namespace

std::string MbLoaderSessionKeyFor(const void* ctx) {
  if (!ctx)
    return std::string();
  auto it = LoaderSessionKeys().find(ctx);
  return it != LoaderSessionKeys().end() ? it->second : std::string();
}

void MbSetLoaderSessionKey(const void* ctx, const std::string& key) {
  if (!ctx)
    return;
  if (key.empty())
    LoaderSessionKeys().erase(ctx);
  else
    LoaderSessionKeys()[ctx] = key;
}

std::string MbSessionKeyFromScope(const std::string& scope) {
  auto pos = scope.find('\x1f');
  // Session ids start "e:" or "p:" (mb_session.cc); anything else predates
  // sessions or is a worker's raw origin - default jar.
  if (pos == std::string::npos)
    return std::string();
  const std::string head = scope.substr(0, pos);
  if (head.rfind("e:", 0) == 0 || head.rfind("p:", 0) == 0)
    return head;
  return std::string();
}

void MbSetRequestMockHookForContext(const void* ctx, MbRequestMockHook hook) {
  if (!ctx)
    return;
  if (hook)
    ContextMockHooks()[ctx] = std::move(hook);
  else
    ContextMockHooks().erase(ctx);
}

namespace {
// Host image sources (see MbSetImageSource / MbSetImageSourceBuffer). An entry
// is either eagerly-encoded PNG bytes (the copying registration) or a BORROWED
// caller pixel buffer encoded lazily on first fetch (the zero-copy
// registration, item 43) — `png` empty + `borrowed` set means "not encoded
// yet". Main thread only (MbFindMock's thread).
struct ImageSourceEntry {
  std::string png;                 // encoded bytes; empty until first fetch
  const void* borrowed = nullptr;  // caller-owned BGRA (zero-copy variant)
  int width = 0, height = 0, stride = 0;
  MbImageSourceRelease release = nullptr;  // fired on replace/unregister
  void* release_ud = nullptr;
};
std::map<std::string, ImageSourceEntry>& ImageSources() {
  static std::map<std::string, ImageSourceEntry>* sources =
      new std::map<std::string, ImageSourceEntry>();
  return *sources;
}
constexpr char kImageSourcePrefix[] = "https://mb-image.internal/";

bool ImageSourceIdValid(const std::string& id) {
  if (id.empty())
    return false;
  for (char c : id) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok)
      return false;  // the id embeds in URLs and page-visible events verbatim
  }
  return true;
}

// PNG-encode a BGRA buffer (blink needs a decodable image format; PNG is
// lossless and keeps alpha). Shared by the eager and lazy paths.
bool EncodeBgraPng(const void* bgra, int width, int height, int stride,
                   std::string* out) {
  SkBitmap bitmap;
  if (!bitmap.installPixels(
          SkImageInfo::Make(width, height, kBGRA_8888_SkColorType,
                            kPremul_SkAlphaType),
          const_cast<void*>(bgra), static_cast<size_t>(stride))) {
    return false;
  }
  std::optional<std::vector<uint8_t>> png =
      gfx::PNGCodec::EncodeBGRASkBitmap(bitmap, /*discard_transparency=*/false);
  if (!png)
    return false;
  out->assign(png->begin(), png->end());
  return true;
}

// Erase `id`, firing the release callback if the entry borrowed caller pixels.
void ReleaseImageSourceEntry(const std::string& id) {
  auto it = ImageSources().find(id);
  if (it == ImageSources().end())
    return;
  ImageSourceEntry old = std::move(it->second);
  ImageSources().erase(it);
  if (old.borrowed && old.release)
    old.release(old.release_ud, old.borrowed);
}
}  // namespace

bool MbSetImageSource(const std::string& id, const void* bgra, int width,
                      int height, int stride) {
  if (!ImageSourceIdValid(id) || !bgra || width <= 0 || height <= 0)
    return false;
  if (stride <= 0)
    stride = width * 4;
  // Encode ONCE at registration; the caller's buffer is free the moment this
  // returns. Serving is then a string copy per fetch.
  ImageSourceEntry entry;
  if (!EncodeBgraPng(bgra, width, height, stride, &entry.png))
    return false;
  ReleaseImageSourceEntry(id);  // replacing a borrowed entry releases it
  ImageSources()[id] = std::move(entry);
  return true;
}

bool MbSetImageSourceBuffer(const std::string& id, const void* bgra, int width,
                            int height, int stride,
                            MbImageSourceRelease release, void* release_ud) {
  if (!ImageSourceIdValid(id) || !bgra || width <= 0 || height <= 0)
    return false;
  // ZERO-COPY: borrow the caller's pixels (no copy, no eager encode); the PNG
  // is produced lazily on first fetch and cached. The buffer must stay valid
  // and unchanged until `release` fires (on replace or unregister).
  ImageSourceEntry entry;
  entry.borrowed = bgra;
  entry.width = width;
  entry.height = height;
  entry.stride = stride > 0 ? stride : width * 4;
  entry.release = release;
  entry.release_ud = release_ud;
  ReleaseImageSourceEntry(id);
  ImageSources()[id] = std::move(entry);
  return true;
}

void MbRemoveImageSource(const std::string& id) {
  ReleaseImageSourceEntry(id);
}

bool MbFindImageSource(const std::string& url, std::string* out_png) {
  if (url.rfind(kImageSourcePrefix, 0) != 0)
    return false;
  std::string id = url.substr(sizeof(kImageSourcePrefix) - 1);
  id = id.substr(0, id.find_first_of("?#"));  // ?v=N cache-buster is ignored
  auto it = ImageSources().find(id);
  if (it == ImageSources().end())
    return false;
  ImageSourceEntry& entry = it->second;
  if (entry.png.empty() && entry.borrowed) {
    // Lazy first-fetch encode of a borrowed buffer (cached for later fetches).
    if (!EncodeBgraPng(entry.borrowed, entry.width, entry.height, entry.stride,
                       &entry.png))
      return false;
  }
  if (out_png)
    *out_png = entry.png;
  return true;
}

// --- Custom URL schemes (item 48) --------------------------------------------
// Host-served schemes ("app", ...): standard-parsed, secure, fetch-capable,
// served exclusively through the interception stack. The list is set before
// engine init and read-only afterwards, so no locking.
namespace {
std::vector<std::string>& CustomSchemes() {
  static std::vector<std::string>* s = new std::vector<std::string>();
  return *s;
}
}  // namespace

void MbRegisterCustomScheme(const std::string& scheme) {
  if (scheme.empty())
    return;
  std::string lower;
  for (char c : scheme) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'))
      return;  // not a legal scheme token
    lower.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if ((lower[0] >= '0' && lower[0] <= '9') || MbIsCustomScheme(lower))
    return;  // schemes start with a letter; ignore duplicates
  CustomSchemes().push_back(lower);
}

bool MbIsCustomScheme(const std::string& scheme) {
  for (const std::string& s : CustomSchemes())
    if (s == scheme)
      return true;
  return false;
}

const std::vector<std::string>& MbCustomSchemes() {
  return CustomSchemes();
}

bool MbFindMock(const std::string& url, std::string* body,
                std::string* content_type, int* status,
                const void* host_ctx) {
  // Host image sources own their reserved host — answered before the mock
  // layer so a broad mock substring can't shadow them. Every fetch path
  // (subresource, top-level, downloads, workers) funnels through here.
  {
    std::string png;
    if (MbFindImageSource(url, &png)) {
      if (body)
        *body = std::move(png);
      if (content_type)
        *content_type = "image/png";
      if (status)
        *status = 200;
      return true;
    }
  }
  // Static mocks first. Last matching entry wins, so a later mbMockResponse overrides an
  // earlier overlapping one (intuitive "re-mock to replace").
  const MockEntry* hit = nullptr;
  for (const MockEntry& e : MockList())
    if (url.find(e.substring) != std::string::npos)
      hit = &e;
  if (hit) {
    if (body)
      *body = hit->body;
    if (content_type)
      *content_type = hit->content_type;
    if (status)
      *status = hit->status;
    return true;
  }
  // Per-context hook next: the requesting view's own mock, if registered.
  if (host_ctx) {
    auto it = ContextMockHooks().find(host_ctx);
    if (it != ContextMockHooks().end() && it->second) {
      std::string b, ct;
      int st = 0;
      if (it->second(url, &b, &ct, &st)) {
        if (body)
          *body = std::move(b);
        if (content_type)
          *content_type = std::move(ct);
        if (status)
          *status = st > 0 ? st : 200;
        return true;
      }
    }
  }
  // Then the dynamic hook (computed per URL). It runs for every otherwise-unmocked load.
  if (RequestMockHook()) {
    std::string b, ct;
    int s = 0;
    if (RequestMockHook()(url, &b, &ct, &s)) {
      if (body)
        *body = std::move(b);
      if (content_type)
        *content_type = std::move(ct);
      if (status)
        *status = s > 0 ? s : 200;
      return true;
    }
  }
  return false;
}

// --- Request URL rewriting ---------------------------------------------------
// Redirect a request to a different URL before any fetch: replace the first
// occurrence of `from` with `to` in the request URL (host swap, scheme upgrade,
// CDN -> local mock, API repointing). The request-side counterpart to mocking.
namespace {
struct RewriteEntry {
  std::string from;
  std::string to;
};
std::vector<RewriteEntry>& RewriteList() {
  static std::vector<RewriteEntry>* r = new std::vector<RewriteEntry>();
  return *r;
}
}  // namespace

void MbAddUrlRewrite(const std::string& from, const std::string& to) {
  if (!from.empty())
    RewriteList().push_back({from, to});
}

void MbClearUrlRewrites() {
  RewriteList().clear();
}

std::string MbApplyUrlRewrites(const std::string& url) {
  std::string out = url;
  for (const RewriteEntry& e : RewriteList()) {
    const std::string::size_type at = out.find(e.from);
    if (at != std::string::npos)
      out.replace(at, e.from.size(), e.to);  // first occurrence; in registration order
  }
  return out;
}

// --- Per-URL request header injection ----------------------------------------
// Two registries, applied by MbApplyRequestHeaders, both conditional on the URL (unlike
// the global extra-headers):
//
//  1) SUBSTRING (mbSetRequestHeader) — the ORIGINAL behavior, preserved for ABI/behavior
//     compatibility: the header rides any request whose full URL contains the registered
//     substring. Flexible (matches path/query too) but a footgun for credentials — a
//     substring like "api.example.com" also matches "https://evil.test/?next=api.example.com".
//
//  2) HOST-scoped (mbSetRequestHeaderForHost) — the SAFE alternative for credentials: the
//     header rides a request only when the request URL's parsed HOST matches the filter,
//     and (crucially) it is applied PER-HOP on the manual-redirect path so it does not
//     follow a cross-origin redirect. Filter syntax:
//       "api.example.com"     exact host
//       ".example.com"        that host OR any subdomain (leading dot opts in)
//       "api.example.com/v2/" exact host AND request path starts with "/v2/"
namespace {
struct SubstringHeaderEntry {
  std::string substring;
  std::string name;
  std::string value;
  uint64_t sequence = 0;
};
struct HostHeaderEntry {
  std::string host;          // lowercased registrable host (no leading dot)
  bool match_subdomains;     // true if the filter began with '.'
  std::string path_prefix;   // "" or a "/..."-rooted prefix the path must start with
  std::string name;
  std::string value;
  uint64_t sequence = 0;
};
// A FULL-ORIGIN filter: scheme + host + effective port must all match (the truly
// cross-origin-safe form). "https://api.example.com" and "https://api.example.com:443" are
// the same origin (default port); "http://..." and ":8443" are NOT.
struct OriginHeaderEntry {
  std::string scheme;        // lowercased ("https")
  std::string host;          // lowercased host
  int effective_port;        // scheme default filled in (443/80) if unspecified
  std::string path_prefix;   // "" or a "/..."-rooted prefix the path must start with
  std::string name;
  std::string value;
  uint64_t sequence = 0;
};
std::vector<SubstringHeaderEntry>& SubstringHeaderList() {
  static auto* h = new std::vector<SubstringHeaderEntry>();
  return *h;
}
std::vector<HostHeaderEntry>& HostHeaderList() {
  static auto* h = new std::vector<HostHeaderEntry>();
  return *h;
}
std::vector<OriginHeaderEntry>& OriginHeaderList() {
  static auto* h = new std::vector<OriginHeaderEntry>();
  return *h;
}
// Guards both lists. MbApplyRequestHeaders runs on the thread-pool worker (inside FetchHttp
// on the async HTTP path) while the setters run on the main thread — so the registries must
// be locked against that cross-thread access.
base::Lock& HeaderListLock() {
  static base::NoDestructor<base::Lock> lock;
  return *lock;
}
uint64_t& HeaderRegistrationSequence() {
  static uint64_t sequence = 0;
  return sequence;
}

// Parse a host filter into (host, match_subdomains, path_prefix). Empty host = unusable.
HostHeaderEntry ParseHostFilter(const std::string& filter) {
  HostHeaderEntry e;
  e.match_subdomains = false;
  std::string f = filter;
  if (!f.empty() && f.front() == '.') {
    e.match_subdomains = true;
    f.erase(f.begin());
  }
  const size_t slash = f.find('/');
  if (slash == std::string::npos) {
    e.host = f;
  } else {
    e.host = f.substr(0, slash);
    e.path_prefix = f.substr(slash);  // keeps the leading '/'
  }
  e.host = base::ToLowerASCII(e.host);
  return e;
}

bool HostFilterMatches(const HostHeaderEntry& e, const std::string& host,
                       const std::string& path) {
  bool host_ok = (host == e.host);
  if (!host_ok && e.match_subdomains) {
    // ".example.com" matches "a.example.com" (a true subdomain), not "notexample.com" —
    // require a '.' boundary.
    host_ok = host.size() > e.host.size() + 1 &&
              host.compare(host.size() - e.host.size(), e.host.size(), e.host) == 0 &&
              host[host.size() - e.host.size() - 1] == '.';
  }
  if (!host_ok)
    return false;
  if (!e.path_prefix.empty() &&
      path.compare(0, e.path_prefix.size(), e.path_prefix) != 0)
    return false;
  return true;
}

// Parse an origin filter "scheme://host[:port][/path/prefix]". Empty host = unusable.
OriginHeaderEntry ParseOriginFilter(const std::string& origin) {
  OriginHeaderEntry e;
  e.effective_port = -1;
  const GURL g(origin);
  if (!g.is_valid() || !g.has_scheme() || !g.has_host())
    return e;  // host stays empty -> caller drops
  e.scheme = std::string(g.scheme());       // GURL lowercases scheme
  e.host = std::string(g.host());           // GURL lowercases host
  e.effective_port = g.EffectiveIntPort();  // scheme default (443/80) if unspecified
  std::string p(g.path());
  if (p != "/")  // a bare origin ("https://h") yields "/", which constrains nothing
    e.path_prefix = std::move(p);
  return e;
}

bool OriginFilterMatches(const OriginHeaderEntry& e, const GURL& gurl,
                         const std::string& path) {
  if (gurl.scheme() != e.scheme)
    return false;
  if (gurl.host() != e.host)
    return false;
  if (gurl.EffectiveIntPort() != e.effective_port)
    return false;
  if (!e.path_prefix.empty() &&
      path.compare(0, e.path_prefix.size(), e.path_prefix) != 0)
    return false;
  return true;
}
}  // namespace

void MbAddRequestHeader(const std::string& substring, const std::string& name,
                        const std::string& value) {
  base::AutoLock guard(HeaderListLock());
  if (!substring.empty() && !name.empty())
    SubstringHeaderList().push_back(
        {substring, name, value, ++HeaderRegistrationSequence()});
}

void MbAddRequestHeaderForHost(const std::string& host_filter,
                               const std::string& name, const std::string& value) {
  base::AutoLock guard(HeaderListLock());
  if (host_filter.empty() || name.empty())
    return;
  HostHeaderEntry e = ParseHostFilter(host_filter);
  if (e.host.empty())  // e.g. "/foo" or "." alone — no host to bind to
    return;
  e.name = name;
  e.value = value;
  e.sequence = ++HeaderRegistrationSequence();
  HostHeaderList().push_back(std::move(e));
}

void MbAddRequestHeaderForOrigin(const std::string& origin, const std::string& name,
                                 const std::string& value) {
  base::AutoLock guard(HeaderListLock());
  if (origin.empty() || name.empty())
    return;
  OriginHeaderEntry e = ParseOriginFilter(origin);
  if (e.host.empty())  // unparseable origin
    return;
  e.name = name;
  e.value = value;
  e.sequence = ++HeaderRegistrationSequence();
  OriginHeaderList().push_back(std::move(e));
}

void MbClearRequestHeaders() {
  base::AutoLock guard(HeaderListLock());
  SubstringHeaderList().clear();
  HostHeaderList().clear();
  OriginHeaderList().clear();
}

void MbApplyRequestHeaders(const std::string& url, std::string* req_headers,
                           bool follow_redirects) {
  // OVERRIDE (last-write-wins), not append: MergeHeaderLines drops any existing line with
  // the same (case-insensitive) name before adding, so re-registering a name — or an
  // injected header colliding with an extra_header — replaces rather than duplicates it.
  // All three registries share one monotonically increasing sequence, so the true latest
  // matching registration wins even when callers interleave substring/host/origin APIs.
  // (We use MergeHeaderLines rather than net::HttpRequestHeaders on purpose: SetHeader
  // DCHECKs header validity, which would abort a dev build on an unusual embedder value.)
  (void)follow_redirects;  // retained in the internal signature for source compatibility
  struct Match {
    uint64_t sequence;
    std::string name;
    std::string value;
  };
  std::vector<Match> matches;
  base::AutoLock guard(HeaderListLock());
  // Substring headers: original semantics — matched against the whole URL spec.
  for (const SubstringHeaderEntry& e : SubstringHeaderList()) {
    if (url.find(e.substring) != std::string::npos)
      matches.push_back({e.sequence, e.name, e.value});
  }
  const GURL gurl(url);
  if (gurl.is_valid() && gurl.has_host()) {
    const std::string host(gurl.host());  // GURL already lowercases the host
    const std::string path(gurl.path());
    for (const HostHeaderEntry& e : HostHeaderList()) {
      if (HostFilterMatches(e, host, path))
        matches.push_back({e.sequence, e.name, e.value});
    }
    // Origin-scoped (scheme + host + effective port) — the strict form.
    for (const OriginHeaderEntry& e : OriginHeaderList()) {
      if (OriginFilterMatches(e, gurl, path))
        matches.push_back({e.sequence, e.name, e.value});
    }
  }
  std::sort(matches.begin(), matches.end(),
            [](const Match& a, const Match& b) {
              return a.sequence < b.sequence;
            });
  for (const Match& match : matches)
    MergeHeaderLines(req_headers, match.name + ": " + match.value);
}

bool MbFetchUrl(const std::string& url_spec, std::string* body,
                std::string* content_type, const std::string& user_agent,
                const std::string& extra_headers, const std::string& post_body,
                const std::string& post_content_type,
                const std::string& http_method, std::string* out_final_url,
                int* out_status, std::string* out_headers,
                std::string* out_error, int* out_error_code,
                const void* host_ctx, bool run_response_hook,
                const std::string& cookie_session_key) {
  if (out_error)
    out_error->clear();
  if (out_error_code)
    *out_error_code = 0;
  // Resolve the complete first-hop policy once. The transport receives this finalized
  // header block unchanged, so both request callbacks inspect the headers curl will send.
  const std::string method = !http_method.empty()
                                 ? http_method
                                 : (post_body.empty() ? "GET" : "POST");
  std::string baseline_headers = extra_headers;
  if (!post_content_type.empty())
    MergeHeaderLines(&baseline_headers,
                     "Content-Type: " + post_content_type);
  RequestPolicyDecision policy = EvaluateRequestPolicy(
      url_spec, url_spec, method, baseline_headers, post_body, nullptr,
      host_ctx);
  if (policy.blocked) {
    if (out_error)
      *out_error = kMbBlockedByHookError;
    return false;
  }
  std::string fetch_spec = std::move(policy.fetch_url);
  GURL url(fetch_spec);
  // Response hook for a SUCCESSFUL top-level / navigation / worker-script fetch — the
  // MAIN-document counterpart of the subresource hook (MbURLLoader::DeliverResponse).
  // Every synchronous navigation path funnels through here (LoadURL, PostURL,
  // page-initiated navigation in MbFrameClient, worker main scripts), so firing it once at
  // MbFetchUrl's success return gives the embedder inspect/rewrite of the main document too
  // — previously it saw only subresources and downloads. `visible_url` is the FINAL URL the
  // bytes came from (post-redirect for http; the requested URL for mock/file/data). status
  // and headers may be absent for local schemes, so synthesize a mutable local and
  // propagate any rewrite back. `run_response_hook` lets a caller that runs the hook itself
  // (FetchDownloadBody) opt out, so the hook fires exactly once per result.
  auto run_hook = [&](const std::string& visible_url) {
    if (!run_response_hook)
      return;
    int status = out_status ? *out_status : 200;
    std::string headers = out_headers ? *out_headers : std::string();
    MbInvokeResponseHook(host_ctx, visible_url, &status, &headers, body);
    if (out_status)
      *out_status = status;
    if (out_headers)
      *out_headers = headers;
  };
  auto run_local_hook = [&](const std::string& visible_url,
                            int synthetic_status = 200) {
    if (!run_response_hook)
      return;
    // Local bytes have a successful synthetic response for interception, but
    // no HTTP status/header state for mbGetHttpStatus/mbGetResponseHeaders.
    int hook_status = synthetic_status;
    std::string hook_headers;
    MbInvokeResponseHook(host_ctx, visible_url, &hook_status, &hook_headers,
                         body);
  };
  // Response mocking: a registered URL substring serves its canned body without a real fetch.
  // Checked before any scheme — matching the async loader (Deliver) — so worker scripts,
  // iframes, and top-level navigations (all of which fetch through here) can be mocked too.
  {
    if (policy.mocked) {
      *body = std::move(policy.mock_body);
      if (content_type)
        *content_type = policy.mock_content_type;
      const int status =
          policy.mock_status > 0 ? policy.mock_status : 200;
      if (out_final_url)
        *out_final_url = url_spec;  // the page-visible URL, not the redirected fetch
      if (GURL(url_spec).SchemeIsHTTPOrHTTPS()) {
        if (out_status)
          *out_status = status;
        run_hook(url_spec);
      } else {
        if (out_status)
          *out_status = 0;
        if (out_headers)
          out_headers->clear();
        run_local_hook(url_spec, status);
      }
      return true;
    }
  }
  if (url.SchemeIsFile()) {
    // Convert via net (percent-decodes the path; "Andale%20Mono.ttf" -> a space)
    // — a raw url.path() leaves it encoded and ReadFileToString fails.
    const bool ok = MbReadFileURL(url, body);
    if (!ok) {
      if (out_error)
        *out_error = "file not found or unreadable";
      return false;
    }
    run_local_hook(url_spec);
    return true;
  }
  if (url.SchemeIsHTTPOrHTTPS()) {
    bool redirected = false;
    std::string fetched_final_url;
    std::string* final_url = out_final_url ? out_final_url : &fetched_final_url;
    const bool ok = FetchHttp(fetch_spec, body, content_type, user_agent,
                              extra_headers, post_body, post_content_type,
                              http_method, out_status, out_headers, final_url,
                              MbFollowRedirects(), out_error, out_error_code,
                              host_ctx, policy.pin_pubkey, policy.headers,
                              url_spec,
                              &redirected, cookie_session_key);
    if (ok) {
      // URL rewrites and mutate-hook set_url are transparent: absent a real
      // server redirect, the document keeps the URL the page requested. Once a
      // 3xx is followed, expose the resolved target so callers commit the
      // response under the origin that actually supplied it.
      if (!redirected)
        *final_url = url_spec;
      run_hook(final_url->empty() ? fetch_spec : *final_url);
    }
    return ok;
  }
  if (url.SchemeIs("data")) {
    std::string mime, charset;
    if (!net::DataURL::Parse(url, &mime, &charset, body))
      return false;
    if (content_type)
      *content_type = mime;
    run_local_hook(url_spec);
    return true;
  }
  // A registered custom scheme (item 48) is served ONLY by the interception
  // stack, which already ran above — reaching here means nothing answered.
  if (MbIsCustomScheme(std::string(url.scheme())) && out_error)
    *out_error = "no mock/handler answered the custom-scheme request";
  return false;
}

// ---- Asynchronous top-level navigation engine (see header) ----------------------------
namespace {
// Per-id cancellation tokens for in-flight navigations, so MbCancelNavigation(id) can reach
// the token the reactor transfer is watching. Main-thread only.
std::map<uint64_t, std::shared_ptr<std::atomic<bool>>>& NavCancelTokens() {
  static auto* m = new std::map<uint64_t, std::shared_ptr<std::atomic<bool>>>();
  return *m;
}

// Exact-URL, test-only barrier around local navigation materialization. Keeping
// the state on the heap makes observations safe after the owning view has died;
// the pool task and its reply each retain a strong reference until completion.
struct LocalNavigationProbeForTesting {
  base::WaitableEvent worker_started;
  base::WaitableEvent release_worker;
  base::WaitableEvent worker_finished;
  std::atomic<bool> ran_off_engine_sequence{false};
  std::atomic<bool> barrier_timed_out{false};
  std::atomic<int> reply_count{0};
};

base::Lock& LocalNavigationProbeLockForTesting() {
  static base::NoDestructor<base::Lock> lock;
  return *lock;
}

std::map<std::string, std::shared_ptr<LocalNavigationProbeForTesting>>&
LocalNavigationProbesForTesting() {
  static auto* probes =
      new std::map<std::string,
                   std::shared_ptr<LocalNavigationProbeForTesting>>();
  return *probes;
}

std::shared_ptr<LocalNavigationProbeForTesting>
FindLocalNavigationProbeForTesting(const std::string& visible_url) {
  base::AutoLock guard(LocalNavigationProbeLockForTesting());
  auto it = LocalNavigationProbesForTesting().find(visible_url);
  return it == LocalNavigationProbesForTesting().end() ? nullptr : it->second;
}

struct NavChain {
  MbNavigationRequest req;
  // `visible_cur` is the URL the page navigated to. Transparent rewrite-table
  // and mutate-hook targets never replace it; a real server redirect does.
  // `fetch_cur` is the actual URL handed to curl for the active hop and is the
  // base against which that hop's Location header is resolved.
  std::string visible_cur;
  std::string fetch_cur;
  std::string method;
  std::string body;
  // Only caller-owned headers are carried between hops. Mutable-hook additions
  // are rebuilt for each hop, so a URL-specific X-Api-Key does not silently
  // persist onto a different redirect target. Credential-bearing carried
  // headers are additionally scrubbed on a real cross-origin transition.
  std::string extra_headers;
  // A request pin applies to the redirect chain (matching subresource fetches).
  // A later hop may replace it by returning another non-empty pin.
  std::string pin_pubkey;
  int hop = 0;
  // Initial local URLs are allowed. For a server redirect whose raw Location
  // uses a non-http(s) scheme, interception/mocking still gets a chance to veto
  // or answer it, but the loader must never read the local target itself.
  bool allow_local_resolution = true;
  std::shared_ptr<std::atomic<bool>> cancel;
  MbNavigationCallback on_complete;
  scoped_refptr<base::SequencedTaskRunner> runner;
};

const void* NavHostContext(const MbNavigationRequest& req) {
  return req.view_context ? req.view_context->host_ctx() : req.host_ctx;
}

const void* NavActivityContext(const MbNavigationRequest& req) {
  return req.view_context ? req.view_context->activity_key() : req.host_ctx;
}

std::string NavSessionKey(const MbNavigationRequest& req) {
  return req.view_context ? req.view_context->session_key()
                          : MbLoaderSessionKeyFor(req.host_ctx);
}

void NavFinish(std::shared_ptr<NavChain> chain, MbNavigationResult result) {
  NavCancelTokens().erase(chain->req.id);
  MbNetRequestFinished(
      NavActivityContext(chain->req));  // navigation is no longer outstanding
  std::move(chain->on_complete).Run(std::move(result));
}

void NavStartHop(std::shared_ptr<NavChain> chain);

void NavPostFinish(std::shared_ptr<NavChain> chain, MbNavigationResult result) {
  auto runner = chain->runner;
  runner->PostTask(FROM_HERE,
                   base::BindOnce(&NavFinish, std::move(chain),
                                  std::move(result)));
}

void NavFinishBlocked(std::shared_ptr<NavChain> chain) {
  MbNavigationResult res;
  res.final_url = chain->visible_cur;
  res.error = kMbBlockedByHookError;
  res.error_domain = "blocked";
  NavPostFinish(std::move(chain), std::move(res));
}

// Build the FINAL navigation result from a completed (or unfollowable) hop, carrying the
// real transport failure domain/code (curl / network) so the async path classifies errors
// the same way the synchronous path does.
MbNavigationResult NavFinalResult(const std::string& final_url,
                                  MbCurlReactor::Result& r) {
  MbNavigationResult res;
  res.ok = r.ok;
  res.status = static_cast<int>(r.http_status);
  res.body = std::move(r.body);
  res.headers = std::move(r.headers);
  res.content_type = std::move(r.content_type);
  res.final_url = final_url;
  if (!r.ok) {
    res.error_code = r.curl_code;
    if (r.curl_code != 0) {  // a real CURLcode -> transport failure
      res.error_domain = "curl";
      res.error = curl_easy_strerror(static_cast<CURLcode>(r.curl_code));
    } else {
      res.error_domain = "network";
      res.error = "no response from server";
    }
  }
  return res;
}

MbNavigationResult ResolveLocalNavigation(
    const GURL& fetch_url,
    const std::string& visible_url,
    scoped_refptr<base::SequencedTaskRunner> engine_runner,
    std::shared_ptr<LocalNavigationProbeForTesting> probe) {
  if (probe) {
    probe->ran_off_engine_sequence.store(
        engine_runner && !engine_runner->RunsTasksInCurrentSequence());
    probe->worker_started.Signal();
    // Bound the test seam so a failed assertion cannot permanently consume a
    // blocking-pool worker. Production navigations never install this probe.
    base::ScopedAllowBaseSyncPrimitivesForTesting allow_probe_wait;
    if (!probe->release_worker.TimedWait(base::Seconds(5)))
      probe->barrier_timed_out.store(true);
  }

  MbNavigationResult res;
  res.final_url = visible_url;
  if (fetch_url.SchemeIs("data")) {
    std::string charset;
    res.ok = net::DataURL::Parse(fetch_url, &res.content_type, &charset,
                                 &res.body);
    res.error = res.ok ? std::string() : "invalid data: URL";
    res.error_domain = res.ok ? std::string() : "network";
  } else {
    res.ok = MbReadFileURL(fetch_url, &res.body);
    res.error = res.ok ? std::string() : "file not found or unreadable";
    res.error_domain = res.ok ? std::string() : "file";
  }
  res.local_response = res.ok;
  res.status = res.ok ? 200 : 0;
  if (probe)
    probe->worker_finished.Signal();
  return res;
}

void NavOnLocalResolved(std::shared_ptr<NavChain> chain,
                        std::shared_ptr<LocalNavigationProbeForTesting> probe,
                        MbNavigationResult result) {
  if (probe)
    probe->reply_count.fetch_add(1);
  if (chain->cancel->load()) {
    result = MbNavigationResult();
    result.final_url = chain->visible_cur;
    result.error = "cancelled";
    result.error_domain = "cancelled";
  }
  NavFinish(std::move(chain), std::move(result));
}

// Reactor completion for one hop (runs on the main thread).
void NavOnHop(std::shared_ptr<NavChain> chain, MbCurlReactor::Result r) {
  if (chain->cancel->load()) {
    MbNavigationResult res;
    res.error = "cancelled";
    res.error_domain = "cancelled";
    NavFinish(std::move(chain), std::move(res));
    return;
  }
  // Follow a 3xx only when redirect-following is enabled (mbSetFollowRedirects) — otherwise
  // deliver the 3xx as the final response, matching the synchronous path.
  const bool is_redirect = MbFollowRedirects() && r.ok && r.http_status >= 300 &&
                           r.http_status < 400;
  const std::string loc =
      is_redirect ? HeaderValueFromBlock(r.headers, "location") : std::string();
  if (is_redirect && !loc.empty()) {
    const GURL prev_fetch(chain->fetch_cur);
    RedirectTargets targets = ResolveRedirectTargets(
        GURL(chain->visible_cur), prev_fetch, loc);
    const GURL& next_fetch_raw = targets.fetch;
    const GURL& next_visible = targets.visible;
    // A valid target gets one full main-thread interception pass before any
    // next-hop fetch. This lets block/inspect hooks veto redirect targets and
    // lets a mock answer a registered custom-scheme target without granting
    // curl (or the file loader) access to an arbitrary non-web scheme.
    if (next_fetch_raw.is_valid() && next_visible.is_valid() &&
        chain->hop < 20) {
      if (r.http_status == 303 ||
          ((r.http_status == 301 || r.http_status == 302) && chain->method == "POST")) {
        chain->method = "GET";
        chain->body.clear();
        chain->req.content_type.clear();
      }
      const std::string next_fetch_candidate =
          RedirectFetchCandidate(next_visible, next_fetch_raw);
      if (!MbSameOrigin(prev_fetch, GURL(next_fetch_candidate)))
        MbStripCredentialHeaders(&chain->extra_headers);
      chain->visible_cur = next_visible.spec();
      chain->fetch_cur = next_fetch_raw.spec();
      chain->allow_local_resolution = next_fetch_raw.SchemeIsHTTPOrHTTPS();
      chain->hop++;
      NavStartHop(std::move(chain));
      return;
    }
  }
  MbNavigationResult res = NavFinalResult(chain->visible_cur, r);
  NavFinish(std::move(chain), std::move(res));
}

void NavStartHop(std::shared_ptr<NavChain> chain) {
  if (chain->cancel->load()) {
    MbNavigationResult res;
    res.error = "cancelled";
    res.error_domain = "cancelled";
    NavFinish(std::move(chain), std::move(res));
    return;
  }

  // Every hop gets the complete interception policy on the main sequence:
  // transparent rewrites, static blocks, the legacy inspect/block callback,
  // the mutable callback, and mocks. In particular this runs again after each
  // Location response, before curl can contact the redirect target.
  const GURL backend_url(chain->fetch_cur.empty() ? chain->visible_cur
                                                  : chain->fetch_cur);
  std::string baseline_headers = chain->extra_headers;
  if (!chain->req.content_type.empty())
    MergeHeaderLines(&baseline_headers,
                     "Content-Type: " + chain->req.content_type);
  RequestPolicyDecision policy = EvaluateRequestPolicy(
      chain->visible_cur, backend_url.spec(), chain->method, baseline_headers,
      chain->body, nullptr, NavHostContext(chain->req));
  if (policy.blocked) {
    NavFinishBlocked(std::move(chain));
    return;
  }

  std::string fetch_spec = std::move(policy.fetch_url);
  std::string hop_headers = std::move(policy.headers);
  if (!policy.pin_pubkey.empty())
    chain->pin_pubkey = std::move(policy.pin_pubkey);

  const GURL fetch_url(fetch_spec);
  // Re-check mocks on every hop, after transparent rewrite/mutation, just like
  // the initial request. A mocked redirect target never reaches curl.
  {
    if (policy.mocked) {
      MbNavigationResult res;
      res.ok = true;
      res.status = policy.mock_status > 0 ? policy.mock_status : 200;
      res.local_response =
          !GURL(chain->visible_cur).SchemeIsHTTPOrHTTPS();
      res.body = std::move(policy.mock_body);
      res.content_type = std::move(policy.mock_content_type);
      res.final_url = chain->visible_cur;
      NavPostFinish(std::move(chain), std::move(res));
      return;
    }
  }

  // A server redirect may be inspected or mocked above, but it may not directly
  // pivot an http(s) response into file:/data:/another curl protocol. An explicit
  // host rewrite/mutation of an http(s) target remains trusted and keeps the
  // initial-navigation behavior.
  if (!chain->allow_local_resolution && !fetch_url.SchemeIsHTTPOrHTTPS()) {
    MbNavigationResult res;
    res.final_url = chain->visible_cur;
    res.error = "redirect target uses a disallowed scheme";
    res.error_domain = "network";
    NavPostFinish(std::move(chain), std::move(res));
    return;
  }

  if (fetch_url.SchemeIs("data") || fetch_url.SchemeIsFile()) {
    // Local payloads can be large too: percent/base64 decoding and file IO must not
    // monopolize the engine update immediately after mbNavigate returns. Resolve them on
    // the shared blocking pool and reply on this navigation's engine sequence.
    std::shared_ptr<LocalNavigationProbeForTesting> probe =
        FindLocalNavigationProbeForTesting(chain->visible_cur);
    const bool posted = base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&ResolveLocalNavigation, fetch_url,
                       chain->visible_cur, chain->runner, probe),
        base::BindOnce(&NavOnLocalResolved, chain, probe));
    if (!posted) {
      MbNavigationResult res;
      res.final_url = chain->visible_cur;
      res.error = "local navigation task unavailable";
      res.error_domain = fetch_url.SchemeIsFile() ? "file" : "network";
      NavPostFinish(std::move(chain), std::move(res));
    }
    return;
  }
  if (!fetch_url.SchemeIsHTTPOrHTTPS()) {
    MbNavigationResult res;
    res.final_url = chain->visible_cur;
    res.error = "no mock/handler answered the request";
    res.error_domain = "network";
    NavPostFinish(std::move(chain), std::move(res));
    return;
  }

  chain->fetch_cur = fetch_spec;
  MbCurlReactor::Request req;
  req.url = chain->fetch_cur;
  req.method = chain->method;
  req.body = chain->body;
  req.post_content_type = chain->req.content_type;
  req.user_agent = chain->req.user_agent;
  req.extra_headers = std::move(hop_headers);
  req.cookie_session_key = NavSessionKey(chain->req);
  req.pin_pubkey = chain->pin_pubkey;
  req.cancel = chain->cancel;
  auto runner = chain->runner;
  MbCurlReactor::Get()->Submit(std::move(req), runner,
                               base::BindOnce(&NavOnHop, std::move(chain)));
}
}  // namespace

void MbArmLocalNavigationForTesting(const std::string& visible_url) {
  auto probe = std::make_shared<LocalNavigationProbeForTesting>();
  std::shared_ptr<LocalNavigationProbeForTesting> old;
  {
    base::AutoLock guard(LocalNavigationProbeLockForTesting());
    auto& slot = LocalNavigationProbesForTesting()[visible_url];
    old = std::move(slot);
    slot = std::move(probe);
  }
  if (old)
    old->release_worker.Signal();
}

void MbReleaseLocalNavigationForTesting(const std::string& visible_url) {
  std::shared_ptr<LocalNavigationProbeForTesting> probe =
      FindLocalNavigationProbeForTesting(visible_url);
  if (probe)
    probe->release_worker.Signal();
}

void MbClearLocalNavigationForTesting(const std::string& visible_url) {
  std::shared_ptr<LocalNavigationProbeForTesting> probe;
  {
    base::AutoLock guard(LocalNavigationProbeLockForTesting());
    auto it = LocalNavigationProbesForTesting().find(visible_url);
    if (it == LocalNavigationProbesForTesting().end())
      return;
    probe = std::move(it->second);
    LocalNavigationProbesForTesting().erase(it);
  }
  probe->release_worker.Signal();
}

bool MbWaitForLocalNavigationWorkerStartForTesting(
    const std::string& visible_url,
    int timeout_ms) {
  std::shared_ptr<LocalNavigationProbeForTesting> probe =
      FindLocalNavigationProbeForTesting(visible_url);
  return probe && probe->worker_started.TimedWait(
                      base::Milliseconds(std::max(timeout_ms, 0)));
}

bool MbWaitForLocalNavigationWorkerFinishForTesting(
    const std::string& visible_url,
    int timeout_ms) {
  std::shared_ptr<LocalNavigationProbeForTesting> probe =
      FindLocalNavigationProbeForTesting(visible_url);
  return probe && probe->worker_finished.TimedWait(
                      base::Milliseconds(std::max(timeout_ms, 0)));
}

bool MbLocalNavigationRanOffEngineSequenceForTesting(
    const std::string& visible_url) {
  std::shared_ptr<LocalNavigationProbeForTesting> probe =
      FindLocalNavigationProbeForTesting(visible_url);
  return probe && probe->ran_off_engine_sequence.load();
}

bool MbLocalNavigationBarrierTimedOutForTesting(
    const std::string& visible_url) {
  std::shared_ptr<LocalNavigationProbeForTesting> probe =
      FindLocalNavigationProbeForTesting(visible_url);
  return probe && probe->barrier_timed_out.load();
}

int MbLocalNavigationReplyCountForTesting(const std::string& visible_url) {
  std::shared_ptr<LocalNavigationProbeForTesting> probe =
      FindLocalNavigationProbeForTesting(visible_url);
  return probe ? probe->reply_count.load() : 0;
}

uint64_t MbNextNavigationId() {
  static std::atomic<uint64_t> g{1};
  return g.fetch_add(1, std::memory_order_relaxed);
}

void MbStartNavigation(MbNavigationRequest req, MbNavigationCallback on_complete) {
  auto cancel = std::make_shared<std::atomic<bool>>(false);
  NavCancelTokens()[req.id] = cancel;
  // Count the whole navigation as one outstanding request for this view, so
  // mbWaitForNetworkIdleEx sees the async main resource in flight (balanced in NavFinish,
  // which every terminal path — success, failure, cancel, local — funnels through).
  MbNetRequestStarted(NavActivityContext(req));
  auto chain = std::make_shared<NavChain>();
  chain->visible_cur = req.url;
  chain->method =
      !req.method.empty() ? req.method : (req.body.empty() ? "GET" : "POST");
  chain->body = req.body;
  chain->extra_headers = req.extra_headers;
  chain->cancel = std::move(cancel);
  chain->on_complete = std::move(on_complete);
  chain->runner = base::SequencedTaskRunner::GetCurrentDefault();
  chain->req = std::move(req);
  chain->runner->PostTask(FROM_HERE,
                          base::BindOnce(&NavStartHop, std::move(chain)));
}

void MbCancelNavigation(uint64_t id) {
  auto it = NavCancelTokens().find(id);
  if (it == NavCancelTokens().end())
    return;
  it->second->store(true);       // CancelXfer aborts the transfer; NavOnHop then finalizes
  MbCurlReactor::Get()->Wake();  // observe the cancel now, not after the ~1s poll timeout
}

namespace {

// Streaming curl transfers live on detached workers, but the interception
// callbacks and mock registries are main-sequence APIs. A worker posts one of
// these decisions between redirect hops and waits without touching any hook
// state itself. The wait is cancellation-aware so tearing down a view does not
// strand a detached stream behind a main-sequence task.
enum class StreamHopAction { kStop, kFetch, kMock };

struct StreamHopDecision {
  base::WaitableEvent ready;
  StreamHopAction action = StreamHopAction::kStop;
  std::string fetch_url;
  std::string add_headers;
  std::string request_headers;
  std::string replacement_pin;
  std::string mock_body;
  std::string mock_content_type;
  int mock_status = 0;
};

void EvaluateStreamRedirectTargetOnMain(
    std::shared_ptr<StreamHopDecision> decision,
    std::shared_ptr<std::atomic<bool>> stop,
    std::string visible_url,
    std::string backend_url,
    std::string policy_headers,
    const void* host_ctx) {
  auto finish = [&] { decision->ready.Signal(); };
  if (stop->load()) {
    finish();
    return;
  }

  // Match the normal request entry order for every followed Location target:
  // rewrite -> static/legacy block -> mutable hook -> mock -> network.
  std::string fetch_url = RedirectFetchCandidate(GURL(visible_url),
                                                  GURL(backend_url));
  MbApplyRequestHeaders(fetch_url, &policy_headers,
                        /*follow_redirects=*/false);
  if (MbIsUrlBlocked(fetch_url) ||
      MbRequestHookBlocks(visible_url, "GET", policy_headers, std::string()) ||
      stop->load()) {
    finish();
    return;
  }

  MbRequestMutation mutation;
  const bool mutated = MbRunRequestMutateHook(
      fetch_url, "GET", policy_headers, std::string(), &mutation);
  if (stop->load() || (mutated && mutation.block)) {
    finish();
    return;
  }
  if (mutated) {
    if (!mutation.set_url.empty() && GURL(mutation.set_url).is_valid())
      fetch_url = mutation.set_url;
    decision->add_headers = std::move(mutation.add_headers);
    MergeHeaderLines(&policy_headers, decision->add_headers);
    if (!mutation.pin_pubkey.empty())
      decision->replacement_pin = std::move(mutation.pin_pubkey);
  }

  if (MbFindMock(fetch_url, &decision->mock_body,
                 &decision->mock_content_type, &decision->mock_status,
                 host_ctx)) {
    if (!stop->load())
      decision->action = StreamHopAction::kMock;
    finish();
    return;
  }
  if (stop->load()) {
    finish();
    return;
  }

  // A raw non-web Location still gets the full interception pass above, so a
  // rewrite or mock can answer it. It is never handed directly to curl.
  if (GURL(fetch_url).SchemeIsHTTPOrHTTPS()) {
    decision->fetch_url = std::move(fetch_url);
    decision->request_headers = std::move(policy_headers);
    decision->action = StreamHopAction::kFetch;
  }
  finish();
}

std::shared_ptr<StreamHopDecision> WaitForStreamRedirectPolicy(
    const scoped_refptr<base::SequencedTaskRunner>& hook_runner,
    const std::shared_ptr<std::atomic<bool>>& stop,
    const std::string& visible_url,
    const std::string& backend_url,
    std::string policy_headers,
    const void* host_ctx) {
  auto decision = std::make_shared<StreamHopDecision>();
  if (!hook_runner ||
      !hook_runner->PostTask(
          FROM_HERE,
          base::BindOnce(&EvaluateStreamRedirectTargetOnMain, decision, stop,
                         visible_url, backend_url, std::move(policy_headers),
                         host_ctx))) {
    return nullptr;
  }
  while (!decision->ready.TimedWait(base::Milliseconds(50))) {
    if (stop->load())
      return nullptr;
  }
  return decision;
}

std::string StreamingMimeType(std::string content_type) {
  const size_t semicolon = content_type.find(';');
  if (semicolon != std::string::npos)
    content_type.resize(semicolon);
  while (!content_type.empty() && content_type.back() == ' ')
    content_type.pop_back();
  return content_type;
}

}  // namespace

// A long-lived streaming HTTP GET over libcurl on a DETACHED worker thread, for
// EventSource / SSE. The buffered FetchHttp would block until EOF (which a live
// SSE stream never reaches) AND has a 30s timeout — both fatal here. We stream
// the body via the curl write callback as chunks arrive, with NO read timeout,
// and abort promptly via the progress callback (called even while idle) when the
// page drops the loader. The worker holds a shared_ptr to itself, so it outlives
// the loader if it's mid-call during teardown; chunks hop to the loader through
// BindPostTask + a WeakPtr (dropped after the loader dies). Mirrors the WebSocket
// transport's lifecycle.
class MbSseStream : public std::enable_shared_from_this<MbSseStream> {
 public:
  using ChunkCb = base::RepeatingCallback<void(std::string)>;
  using DoneCb = base::OnceCallback<void()>;
  MbSseStream(std::string url,
              std::string visible_url,
              std::string cookie_session_key,
              std::string req_headers,
              std::string first_request_headers,
              std::string pinned_pubkey,
              std::string user_agent,
              const void* host_ctx,
              scoped_refptr<base::SequencedTaskRunner> hook_runner,
              ChunkCb chunk,
              DoneCb done)
      : url_(std::move(url)),
        visible_url_(std::move(visible_url)),
        cookie_session_key_(std::move(cookie_session_key)),
        req_headers_(std::move(req_headers)),
        first_request_headers_(std::move(first_request_headers)),
        pinned_pubkey_(std::move(pinned_pubkey)),
        user_agent_(std::move(user_agent)),
        host_ctx_(host_ctx),
        hook_runner_(hook_runner ? std::move(hook_runner)
                                 : base::SequencedTaskRunner::GetCurrentDefault()),
        chunk_(std::move(chunk)),
        done_(std::move(done)),
        stop_(std::make_shared<std::atomic<bool>>(false)) {}

  void Start() {
    auto self = shared_from_this();  // keep alive across the worker's life
    std::thread([self] { self->Run(); }).detach();
  }
  void Stop() { stop_->store(true); }

 private:
  static size_t HeaderThunk(char* p, size_t s, size_t n, void* ud) {
    static_cast<MbSseStream*>(ud)->hop_headers_.append(p, s * n);
    return s * n;  // capture this hop's header block (for the redirect Location)
  }
  static size_t WriteThunk(char* p, size_t s, size_t n, void* ud) {
    auto* self = static_cast<MbSseStream*>(ud);
    if (self->stop_->load())
      return 0;  // returning <bytes aborts curl_easy_perform
    // Determine once per hop whether this is a redirect whose (small) body must be swallowed
    // rather than streamed to the page — the real stream is the final, non-3xx hop.
    if (!self->hop_probed_) {
      long status = 0;
      curl_easy_getinfo(self->curl_, CURLINFO_RESPONSE_CODE, &status);
      self->hop_redirect_ = status >= 300 && status < 400;
      self->hop_probed_ = true;
    }
    if (self->hop_redirect_)
      return s * n;  // swallow; the next hop delivers the actual response
    self->chunk_.Run(std::string(p, s * n));
    return s * n;
  }
  static int XferThunk(void* ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    // Called periodically even while the stream is idle -> prompt cancellation.
    return static_cast<MbSseStream*>(ud)->stop_->load() ? 1 : 0;
  }

  void Run() {
    std::string cur = url_;
    std::string visible_cur = visible_url_;
    std::string carried = req_headers_;  // base headers; credential-scrubbed per cross-origin hop
    std::string request_headers = first_request_headers_;
    int hop = 0;
    for (;;) {
      CURL* c = curl_easy_init();
      if (!c)
        break;
      curl_ = c;
      hop_headers_.clear();
      hop_probed_ = false;
      hop_redirect_ = false;
      curl_easy_setopt(c, CURLOPT_URL, cur.c_str());
      std::string proxy;
      if (MbProxyConfigured(&proxy))
        curl_easy_setopt(c, CURLOPT_PROXY, proxy.c_str());
      if (MbIgnoreCertErrors()) {
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
      }
      if (!pinned_pubkey_.empty())
        curl_easy_setopt(c, CURLOPT_PINNEDPUBLICKEY, pinned_pubkey_.c_str());
      curl_easy_setopt(
          c, CURLOPT_USERAGENT,
          (user_agent_.empty() ? MbDefaultUserAgent() : user_agent_).c_str());
      curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
      curl_easy_setopt(c, CURLOPT_COOKIEFILE, "");  // in-memory cookie engine
      if (CURLSH* share = CookieShareForKey(cookie_session_key_))
        curl_easy_setopt(c, CURLOPT_SHARE, share);
      // Manual per-hop redirect following (NOT CURLOPT_FOLLOWLOCATION): we resolve the chain
      // ourselves so a credential set for one origin is stripped before a cross-origin hop
      // (curl auto-follow re-sends custom headers verbatim across origins) AND the injected
      // host/origin headers are re-evaluated against each hop's URL — including the first, so
      // a credential registered for the SSE endpoint's own origin IS delivered.
      curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
      curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
      // NO CURLOPT_TIMEOUT — an SSE stream is intentionally long-lived.
      curl_slist* headers = BuildHeaderSlist(request_headers);
      if (headers)
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, &HeaderThunk);
      curl_easy_setopt(c, CURLOPT_HEADERDATA, this);
      curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, &WriteThunk);
      curl_easy_setopt(c, CURLOPT_WRITEDATA, this);
      curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, &XferThunk);
      curl_easy_setopt(c, CURLOPT_XFERINFODATA, this);
      const CURLcode rc =
          curl_easy_perform(c);  // final hop streams until EOF/error/abort
      long status = 0;
      curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
      if (headers)
        curl_slist_free_all(headers);
      curl_easy_cleanup(c);
      curl_ = nullptr;
      // Follow a redirect manually (bounded). The target policy is evaluated
      // on the main sequence before another worker fetch can begin.
      if (MbFollowRedirects() && rc == CURLE_OK && status >= 300 &&
          status < 400 && hop < 20 && !stop_->load()) {
        const std::string loc = HeaderValueFromBlock(hop_headers_, "location");
        const GURL prev_fetch(cur);
        RedirectTargets targets;
        if (!loc.empty()) {
          targets = ResolveRedirectTargets(GURL(visible_cur), prev_fetch,
                                           loc);
        }
        const GURL& next_fetch_raw = targets.fetch;
        const GURL& next_visible = targets.visible;
        if (next_fetch_raw.is_valid() && next_visible.is_valid()) {
          std::string next_carried = carried;
          if (!MbSameOrigin(prev_fetch, next_fetch_raw))
            MbStripCredentialHeaders(&next_carried);
          auto decision = WaitForStreamRedirectPolicy(
              hook_runner_, stop_, next_visible.spec(), next_fetch_raw.spec(),
              next_carried, host_ctx_);
          if (decision && !stop_->load()) {
            if (!decision->replacement_pin.empty())
              pinned_pubkey_ = std::move(decision->replacement_pin);
            if (decision->action == StreamHopAction::kMock) {
              if (decision->mock_status < 400 &&
                  !decision->mock_body.empty()) {
                chunk_.Run(std::move(decision->mock_body));
              }
              break;
            }
            if (decision->action == StreamHopAction::kFetch) {
              const GURL fetch_target(decision->fetch_url);
              if (!MbSameOrigin(prev_fetch, fetch_target))
                MbStripCredentialHeaders(&next_carried);
              carried = std::move(next_carried);
              cur = std::move(decision->fetch_url);
              visible_cur = next_visible.spec();
              request_headers = std::move(decision->request_headers);
              hop++;
              continue;
            }
          }
        }
      }
      break;  // final (streamed) or unfollowable redirect
    }
    std::move(done_).Run();  // -> OnSseDone on the loader thread (if alive)
  }

  std::string url_;
  std::string visible_url_;

  std::string cookie_session_key_;
  std::string req_headers_;
  // Headers supplied by the initial mutable request hook apply to that exact
  // fetch. Every followed target gets a fresh main-sequence mutation pass.
  std::string first_request_headers_;
  // Request-hook TLS pins apply to the entire SSE redirect chain, like every
  // other fetch path.
  std::string pinned_pubkey_;
  std::string user_agent_;
  const void* host_ctx_ = nullptr;
  scoped_refptr<base::SequencedTaskRunner> hook_runner_;
  ChunkCb chunk_;
  DoneCb done_;
  std::shared_ptr<std::atomic<bool>> stop_;
  CURL* curl_ = nullptr;       // current hop handle (for status getinfo in WriteThunk)
  std::string hop_headers_;    // current hop's raw header block (redirect Location)
  bool hop_probed_ = false;    // did we classify this hop (redirect vs final) yet
  bool hop_redirect_ = false;  // is the current hop a 3xx whose body we swallow
};

// The streaming download transport (mb download lifecycle; see the header).
// MbSseStream's shape — detached worker, self-owning via shared_ptr, prompt
// abort through the progress callback — plus response metadata: `begin` fires
// at the FIRST body byte of the final hop (FOLLOWLOCATION never hands 3xx
// bodies to the write callback, so "first write" IS the post-redirect
// response), when curl can report the effective status / Content-Type /
// Content-Length. A >= 400 status aborts before `begin` and fails the stream:
// an error page's body is not the requested download. Unlike SSE this stream
// EXPECTS EOF, so the normal 30s-idle protection stays off but completion is
// success only when curl reports CURLE_OK (or the abort was ours).
class MbDownloadStream : public std::enable_shared_from_this<MbDownloadStream> {
 public:
  using BeginCb = base::OnceCallback<
      void(std::string mime, std::string disposition_filename, int64_t expected)>;
  using DataCb = base::RepeatingCallback<void(std::string chunk)>;
  using DoneCb = base::OnceCallback<void(bool success)>;
  MbDownloadStream(std::string url,
                   std::string visible_url,
                   std::string cookie_session_key,
                   std::string req_headers,
                   std::string user_agent,
                   BeginCb begin,
                   DataCb data,
                   DoneCb done,
                   std::string pinned_pubkey,
                   std::string first_request_headers,
                   const void* host_ctx)
      : url_(std::move(url)),
        visible_url_(std::move(visible_url)),
        cookie_session_key_(std::move(cookie_session_key)),
        req_headers_(std::move(req_headers)),
        user_agent_(std::move(user_agent)),
        begin_(std::move(begin)),
        data_(std::move(data)),
        done_(std::move(done)),
        pinned_pubkey_(std::move(pinned_pubkey)),
        first_request_headers_(std::move(first_request_headers)),
        host_ctx_(host_ctx),
        hook_runner_(base::SequencedTaskRunner::GetCurrentDefault()),
        stop_(std::make_shared<std::atomic<bool>>(false)) {}

  void Start() {
    auto self = shared_from_this();  // keep alive across the worker's life
    std::thread([self] { self->Run(); }).detach();
  }
  void Stop() { stop_->store(true); }

 private:
  static size_t HeaderThunk(char* p, size_t s, size_t n, void* ud) {
    auto* self = static_cast<MbDownloadStream*>(ud);
    self->hop_headers_.append(p, s * n);  // this hop's block (for the redirect Location)
    // Track the LAST hop's Content-Disposition (each redirect hop delivers its
    // own header block; later hops overwrite earlier ones).
    std::string line(p, s * n);
    constexpr char kName[] = "content-disposition:";
    if (line.size() > sizeof(kName) - 1) {
      std::string lower = line;
      for (char& ch : lower)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      if (lower.rfind(kName, 0) == 0) {
        std::string v = line.substr(sizeof(kName) - 1);
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
          v.erase(v.begin());
        while (!v.empty() && (v.back() == '\r' || v.back() == '\n'))
          v.pop_back();
        self->content_disposition_ = v;
      }
    }
    return s * n;
  }

  static size_t WriteThunk(char* p, size_t s, size_t n, void* ud) {
    auto* self = static_cast<MbDownloadStream*>(ud);
    if (self->stop_->load())
      return 0;  // returning <bytes aborts curl_easy_perform
    if (self->hop_redirect_)
      return s * n;  // this hop is a 3xx: swallow its body; Run() follows manually
    if (!self->began_) {
      long status = 0;
      curl_easy_getinfo(self->curl_, CURLINFO_RESPONSE_CODE, &status);
      if (status >= 300 && status < 400) {
        self->hop_redirect_ = true;
        return s * n;  // redirect body; the real download is the post-redirect hop
      }
      if (status >= 400) {
        self->http_error_ = true;
        return 0;  // fail the stream; the error body is not the download
      }
      char* ct = nullptr;
      curl_easy_getinfo(self->curl_, CURLINFO_CONTENT_TYPE, &ct);
      std::string mime = ct ? ct : "";
      mime = mime.substr(0, mime.find(';'));
      while (!mime.empty() && mime.back() == ' ')
        mime.pop_back();
      curl_off_t len = -1;
      curl_easy_getinfo(self->curl_, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &len);
      self->began_ = true;
      std::move(self->begin_)
          .Run(std::move(mime), ParseDispositionFilename(self->content_disposition_),
               len >= 0 ? static_cast<int64_t>(len) : -1);
    }
    self->data_.Run(std::string(p, s * n));
    return s * n;
  }

  static int XferThunk(void* ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    // Called periodically even while idle -> prompt cancellation.
    return static_cast<MbDownloadStream*>(ud)->stop_->load() ? 1 : 0;
  }

  // "attachment; filename=\"report.pdf\"" -> "report.pdf" (quoted or token
  // form; the RFC 5987 filename*= form is rare on downloads and skipped).
  static std::string ParseDispositionFilename(const std::string& disposition) {
    std::string lower = disposition;
    for (char& ch : lower)
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    std::string::size_type p = lower.find("filename=");
    if (p == std::string::npos)
      return std::string();
    p += sizeof("filename=") - 1;
    if (p < disposition.size() && disposition[p] == '"') {
      std::string::size_type end = disposition.find('"', p + 1);
      return end == std::string::npos ? std::string()
                                      : disposition.substr(p + 1, end - p - 1);
    }
    std::string::size_type end = disposition.find_first_of("; \t\r\n", p);
    return disposition.substr(p, end == std::string::npos ? end : end - p);
  }

  void Run() {
    std::string cur = url_;
    std::string visible_cur = visible_url_;
    std::string carried = req_headers_;  // credential-scrubbed per cross-origin hop
    std::string request_headers = first_request_headers_;
    int hop = 0;
    CURLcode rc = CURLE_OK;
    for (;;) {
      CURL* c = curl_easy_init();
      if (!c) {
        std::move(done_).Run(false);
        return;
      }
      curl_ = c;
      hop_headers_.clear();
      hop_redirect_ = false;
      curl_easy_setopt(c, CURLOPT_URL, cur.c_str());
      std::string proxy;
      if (MbProxyConfigured(&proxy))
        curl_easy_setopt(c, CURLOPT_PROXY, proxy.c_str());
      if (MbIgnoreCertErrors()) {
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
      }
      if (!pinned_pubkey_.empty())
        curl_easy_setopt(c, CURLOPT_PINNEDPUBLICKEY, pinned_pubkey_.c_str());
      curl_easy_setopt(
          c, CURLOPT_USERAGENT,
          (user_agent_.empty() ? MbDefaultUserAgent() : user_agent_).c_str());
      curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
      curl_easy_setopt(c, CURLOPT_COOKIEFILE, "");  // in-memory cookie engine
      if (CURLSH* share = CookieShareForKey(cookie_session_key_))
        curl_easy_setopt(c, CURLOPT_SHARE, share);
      // Manual per-hop redirects (NOT CURLOPT_FOLLOWLOCATION): resolve the chain ourselves so
      // a credential set for one origin is stripped before a cross-origin hop instead of curl
      // re-sending custom headers verbatim across origins.
      curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
      curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
      // NO CURLOPT_TIMEOUT — a large download legitimately runs for minutes;
      // cancellation comes through XferThunk.
      // The engine sequence finalized this hop's entire header block before the worker
      // fetch began. Never read mutable policy registries from this detached thread.
      curl_slist* headers = BuildHeaderSlist(request_headers);
      if (headers)
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, &HeaderThunk);
      curl_easy_setopt(c, CURLOPT_HEADERDATA, this);
      curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, &WriteThunk);
      curl_easy_setopt(c, CURLOPT_WRITEDATA, this);
      curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, &XferThunk);
      curl_easy_setopt(c, CURLOPT_XFERINFODATA, this);
      rc = curl_easy_perform(c);
      long status = 0;
      curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
      if (headers)
        curl_slist_free_all(headers);
      curl_easy_cleanup(c);
      curl_ = nullptr;
      // Follow a redirect (only when redirect-following is enabled), scrubbing credentials
      // across origins. The final hop's Content-Disposition wins.
      if (MbFollowRedirects() && rc == CURLE_OK && status >= 300 &&
          status < 400 && hop < 20 && !stop_->load()) {
        const std::string loc = HeaderValueFromBlock(hop_headers_, "location");
        const GURL prev_fetch(cur);
        RedirectTargets targets;
        if (!loc.empty()) {
          targets = ResolveRedirectTargets(GURL(visible_cur), prev_fetch,
                                           loc);
        }
        const GURL& next_fetch_raw = targets.fetch;
        const GURL& next_visible = targets.visible;
        if (next_fetch_raw.is_valid() && next_visible.is_valid()) {
          std::string next_carried = carried;
          if (!MbSameOrigin(prev_fetch, next_fetch_raw))
            MbStripCredentialHeaders(&next_carried);
          auto decision = WaitForStreamRedirectPolicy(
              hook_runner_, stop_, next_visible.spec(), next_fetch_raw.spec(),
              next_carried, host_ctx_);
          if (decision && !stop_->load()) {
            if (!decision->replacement_pin.empty())
              pinned_pubkey_ = std::move(decision->replacement_pin);
            if (decision->action == StreamHopAction::kMock) {
              if (decision->mock_status >= 400)
                break;
              std::string body = std::move(decision->mock_body);
              std::string mime =
                  StreamingMimeType(std::move(decision->mock_content_type));
              began_ = true;
              std::move(begin_).Run(std::move(mime), std::string(),
                                    static_cast<int64_t>(body.size()));
              if (!body.empty())
                data_.Run(std::move(body));
              std::move(done_).Run(!stop_->load());
              return;
            }
            if (decision->action == StreamHopAction::kFetch) {
              const GURL fetch_target(decision->fetch_url);
              if (!MbSameOrigin(prev_fetch, fetch_target))
                MbStripCredentialHeaders(&next_carried);
              carried = std::move(next_carried);
              cur = std::move(decision->fetch_url);
              visible_cur = next_visible.spec();
              request_headers = std::move(decision->request_headers);
              hop++;
              content_disposition_.clear();
              continue;
            }
          }
        }
      }
      break;
    }
    // Success = clean EOF after a non-error response began. A cancel
    // (stop_/http_error_ abort through the write/progress callbacks) surfaces
    // as CURLE_WRITE_ERROR / CURLE_ABORTED_BY_CALLBACK -> false.
    std::move(done_).Run(rc == CURLE_OK && began_ && !http_error_ &&
                         !stop_->load());
  }

  std::string url_;
  std::string visible_url_;
  std::string cookie_session_key_;
  std::string req_headers_;
  std::string user_agent_;
  BeginCb begin_;
  DataCb data_;
  DoneCb done_;
  std::string pinned_pubkey_;
  std::string first_request_headers_;
  const void* host_ctx_ = nullptr;
  scoped_refptr<base::SequencedTaskRunner> hook_runner_;
  CURL* curl_ = nullptr;             // valid only inside Run()
  std::string content_disposition_;  // last hop's header value
  std::string hop_headers_;          // current hop's header block (redirect Location)
  bool began_ = false;               // begin_ fired (worker thread only)
  bool http_error_ = false;          // aborted on a >= 400 status
  bool hop_redirect_ = false;        // current hop is a 3xx whose body we swallow
  std::shared_ptr<std::atomic<bool>> stop_;
};

std::shared_ptr<MbDownloadStream> MbStartDownloadStream(
    std::string url,
    std::string visible_url,
    std::string cookie_session_key,
    std::string req_headers,
    std::string user_agent,
    base::OnceCallback<void(std::string, std::string, int64_t)> begin,
    base::RepeatingCallback<void(std::string)> data,
    base::OnceCallback<void(bool)> done,
    std::string pinned_pubkey,
    std::string first_request_headers,
    const void* host_ctx) {
  auto stream = std::make_shared<MbDownloadStream>(
      std::move(url), std::move(visible_url), std::move(cookie_session_key),
      std::move(req_headers),
      std::move(user_agent), std::move(begin), std::move(data), std::move(done),
      std::move(pinned_pubkey), std::move(first_request_headers), host_ctx);
  stream->Start();
  return stream;
}

void MbStopDownloadStream(const std::shared_ptr<MbDownloadStream>& stream) {
  if (stream)
    stream->Stop();
}

MbURLLoader::MbURLLoader(std::string user_agent, std::string extra_headers,
                         const void* host_ctx)
    : user_agent_(std::move(user_agent)),
      extra_headers_(std::move(extra_headers)),
      host_ctx_(host_ctx) {}

MbURLLoader::MbURLLoader(
    std::string user_agent,
    std::string extra_headers,
    scoped_refptr<MbLoaderViewContext> view_context)
    : user_agent_(std::move(user_agent)),
      extra_headers_(std::move(extra_headers)),
      view_context_(std::move(view_context)) {}

MbURLLoader::~MbURLLoader() {
  if (net_counted_) {
    for (const void* activity_key : net_activity_keys_)
      MbNetRequestFinished(activity_key);
  }
  if (sse_stream_)
    sse_stream_->Stop();  // detached worker observes stop_ and exits
}

const void* MbURLLoader::HostContext() const {
  return view_context_ ? view_context_->host_ctx() : host_ctx_;
}

std::vector<scoped_refptr<MbLoaderViewContext>>
MbURLLoader::ActivityContexts() const {
  if (view_context_)
    return view_context_->activity_contexts();
  return {};
}

std::string MbURLLoader::SessionKey() const {
  return view_context_ ? view_context_->session_key()
                       : MbLoaderSessionKeyFor(host_ctx_);
}

void MbURLLoader::InvokeResponseHook(const std::string& url, int* status,
                                     std::string* headers, std::string* body) {
  if (!view_context_) {
    MbInvokeResponseHook(host_ctx_, url, status, headers, body);
    return;
  }

  scoped_refptr<MbLoaderViewContext> context = view_context_;
  const scoped_refptr<base::SingleThreadTaskRunner>& engine_runner =
      context->engine_task_runner();
  if (!engine_runner || engine_runner->RunsTasksInCurrentSequence()) {
    if (context->is_alive())
      MbInvokeResponseHook(context->host_ctx(), url, status, headers, body);
    return;
  }

  // Worker Blink delivery cannot continue until the response mutation is known, but the
  // public callback is engine-main-thread-only. Copy the mutable response into a shared
  // call record, synchronously rendezvous with the engine sequence, then copy the result
  // back before the worker's URLLoaderClient sees the response.
  struct HookCall {
    base::WaitableEvent done;
    std::string url;
    int status = 0;
    std::string headers;
    std::string body;
  };
  auto call = std::make_shared<HookCall>();
  call->url = url;
  call->status = *status;
  call->headers = *headers;
  call->body = *body;
  if (!engine_runner->PostTask(
          FROM_HERE,
          base::BindOnce(
              [](scoped_refptr<MbLoaderViewContext> hook_context,
                 std::shared_ptr<HookCall> hook_call) {
                if (hook_context->is_alive()) {
                  MbInvokeResponseHook(hook_context->host_ctx(), hook_call->url,
                                       &hook_call->status, &hook_call->headers,
                                       &hook_call->body);
                }
                hook_call->done.Signal();
              },
              context, call))) {
    return;
  }
  std::optional<ScopedResponseHookWaiterForTesting> waiter;
  while (!call->done.TimedWait(base::Milliseconds(10))) {
    // Publish only after the worker really had to rendezvous with the engine
    // sequence. This mirrors the request-policy diagnostic and lets teardown
    // tests distinguish a queued load from the response-hook wait itself.
    if (!waiter)
      waiter.emplace(url);
    // View teardown may synchronously wait for this worker to stop. Do not strand it
    // behind a main-sequence callback that can no longer be serviced; the posted task
    // owns `call` and will simply observe the invalidated context if it runs later.
    // Same escape once mbShutdown has begun: the engine loop will never pump again.
    if (!context->is_alive() || mb::MbRuntime::ShutdownStarted())
      return;
  }
  *status = call->status;
  *headers = std::move(call->headers);
  *body = std::move(call->body);
}

void MbURLLoader::LoadAsynchronously(
    std::unique_ptr<network::ResourceRequest> request,
    scoped_refptr<const blink::SecurityOrigin> top_frame_origin,
    bool no_mime_sniffing,
    std::unique_ptr<blink::ResourceLoadInfoNotifierWrapper>
        resource_load_info_notifier_wrapper,
    blink::CodeCacheHost* code_cache_host,
    blink::URLLoaderClient* client) {
  client_ = client;
  // Count this load as outstanding for its view (paired with the decrement in the
  // destructor via net_counted_) so mbWaitForNetworkIdleEx sees a real in-flight
  // request until the loader is torn down — success, error, or cancel.
  net_activity_contexts_ = ActivityContexts();
  net_activity_keys_.reserve(net_activity_contexts_.size() + 1);
  for (const auto& activity_context : net_activity_contexts_)
    net_activity_keys_.push_back(activity_context->activity_key());
  if (net_activity_keys_.empty() && host_ctx_)
    net_activity_keys_.push_back(host_ctx_);  // legacy raw-context constructor
  for (const void* activity_key : net_activity_keys_)
    MbNetRequestStarted(activity_key);
  net_counted_ = true;
  // Deliver on the main thread, async (blink expects no reentrancy here).
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&MbURLLoader::Deliver, weak_factory_.GetWeakPtr(),
                                std::move(request)));
}

// ASYNC HTTP(S) state carried between the main thread and the thread-pool worker.
// Moved (by unique_ptr) into the worker closure and back via PostTask; all members are
// movable value types, and the worker only touches the request-side fields (never the
// blink client). Mirrors miniblink49's WebURLLoaderInternal job state.
struct MbURLLoader::FetchState {
  GURL original_url;   // the page's request URL (response.url, errors)
  GURL fetch_url;      // post-rewrite target (mime fallback)
  std::string cur;     // current hop URL (advances on each followed redirect)
  std::string visible_cur;  // page-visible URL after real redirects
  std::string method;  // current hop method (303 / POST->GET rewrites it)
  std::string body;    // request body (cleared on a method->GET rewrite)
  std::string post_ct; // request Content-Type (carried separately from headers)
  net::HttpRequestHeaders hop_headers;   // request's own headers (redirect-stripped)
  // Finalized header block for the active hop: caller/request baseline, static
  // registrations in global order, then mutable-hook overrides.
  std::string request_headers;
  // The embedder's extra_headers_ CARRIED across hops (not re-read fresh each hop) so a
  // credential set via mbSetExtraHeaders can be stripped on a cross-origin redirect just
  // like the request's own headers — otherwise it would be re-prepended verbatim and leak.
  std::string carried_extra_headers;
  net::SiteForCookies site_for_cookies;  // for WillFollowRedirect on the main thread
  std::string pin_pubkey;  // per-request TLS public-key pin (may be empty)
  int hop = 0;           // redirect hop count (capped at 20)
  bool redirected = false;  // a real 3xx hop was followed (vs. a transparent rewrite)
};

// One hop's raw transfer output, produced on the worker and posted back to the main
// thread. (FetchHttp fills body/content_type/status/headers/effective-url.)
struct MbURLLoader::HopResult {
  bool ok = false;
  std::string contents;
  std::string content_type;
  std::string resp_headers;
  std::string final_url;  // curl effective URL of this hop
  int status = 0;
};

void MbURLLoader::Deliver(std::unique_ptr<network::ResourceRequest> request) {
  if (!client_)
    return;
  // The synchronous-fetch path here is now local/instant (mock / file: / data:); the
  // HTTP(S) path runs async (StartHttpFetch -> worker -> OnHopComplete), where the
  // reentrancy guard against blink synchronously canceling+deleting this loader during
  // WillFollowRedirect / DidReceiveResponse lives (a stack-local WeakPtr in OnHopComplete
  // and DeliverResponse). The local branches below never reenter blink before returning.
  const GURL& url = request->url;     // the page's URL (response.url, log, errors)
  MbRecordRequest(url.spec());        // log the URL the page actually requested

  // URL rewriting: TRANSPARENTLY redirect the request to a different URL — the
  // block/mock/fetch act on `fetch_url`, but the response is still reported as the
  // page's original `url` (so fetch()'s url_list_ DCHECK holds; host swap, scheme
  // upgrade, CDN -> local mock).
  GURL fetch_url = url;

  // Compose the request-owned baseline (view extra headers, then Blink's request
  // headers) before static registrations and callbacks are evaluated.
  std::string baseline_headers = extra_headers_;
  for (const auto& kv : request->headers.GetHeaderVector()) {
    if (!baseline_headers.empty())
      baseline_headers += "\n";
    baseline_headers += kv.key + ": " + kv.value;
  }
  std::string hook_body;
  if (request->request_body) {
    for (const auto& el : *request->request_body->elements())
      if (el.type() == network::DataElement::Tag::kBytes) {
        std::string_view sv = el.As<network::DataElementBytes>().AsStringView();
        hook_body.append(sv.data(), sv.size());
      }
  }
  RequestPolicyDecision policy = EvaluateRequestPolicy(
      url.spec(), fetch_url.spec(), request->method, baseline_headers,
      hook_body, view_context_, HostContext(),
      network::RequestDestinationToString(request->destination));
  if (policy.blocked) {
    client_->DidFail(
        blink::WebURLError(net::ERR_BLOCKED_BY_CLIENT, ToWebURL(url)),
        base::TimeTicks::Now(), blink::URLLoaderClient::kUnknownEncodedDataLength,
        0, 0);
    return;
  }

  fetch_url = GURL(policy.fetch_url);

  // EventSource / SSE: a request with `Accept: text/event-stream` to http(s) is a
  // long-lived stream. The buffered path below would block forever (no EOF) and
  // freeze the engine. Stream it on a worker thread instead — UNLESS it's mocked
  // (an offline test serves a complete event-stream body the buffered way).
  if (fetch_url.SchemeIsHTTPOrHTTPS()) {
    std::string accept;
    for (const auto& kv : request->headers.GetHeaderVector())
      if (ToLower(kv.key) == "accept")
        accept = kv.value;
    if (accept.find("text/event-stream") != std::string::npos &&
        !policy.mocked) {
      std::string req_headers = extra_headers_;
      for (const auto& kv : request->headers.GetHeaderVector()) {
        if (ToLower(kv.key) == "content-type")
          continue;
        if (!req_headers.empty())
          req_headers += "\n";
        req_headers += kv.key + ": " + kv.value;
      }
      // SSE now follows redirects MANUALLY per-hop (MbSseStream::Run), so the injected
      // host/origin headers are applied there against each hop's URL — including the first —
      // rather than here. Pass the base headers separately from mutable-hook
      // additions: the latter belong to this origin and must not ride a
      // cross-origin redirect merely because their name is not one of the
      // built-in Authorization/Cookie credential names.
      StartSse(fetch_url.spec(), req_headers, policy.headers,
               policy.pin_pubkey, url.spec());
      return;
    }
  }

  std::string contents;
  std::string http_content_type;  // from the server, may be "text/html; charset=..."
  int http_status = 0;            // real HTTP status (0 -> treat as 200)
  std::string resp_headers;       // raw final-response header block (http only)
  std::string final_url;          // URL after manual redirect following (http)
  bool redirected = false;        // an actual 3xx hop was followed (not a rewrite)
  bool ok = false;
  // Response mocking: a registered URL substring serves its canned body without a
  // real fetch (offline tests, API substitution). Checked before any scheme fetch.
  if (policy.mocked) {
    contents = std::move(policy.mock_body);
    http_content_type = policy.mock_content_type.empty()
                            ? std::string("text/html")
                            : std::move(policy.mock_content_type);
    http_status = policy.mock_status > 0 ? policy.mock_status : 200;
    ok = true;
  } else if (fetch_url.SchemeIsFile()) {
    ok = MbReadFileURL(fetch_url, &contents);
  } else if (fetch_url.SchemeIsHTTPOrHTTPS()) {
    // Carry the request method + body so fetch()/XHR POST/PUT/etc. send their
    // payload (the dominant API pattern), not a bodyless GET. The body's bytes
    // live in kBytes DataElements; pull the Content-Type off the request headers.
    std::string post_body, post_ct;
    if (request->request_body) {
      for (const auto& el : *request->request_body->elements()) {
        if (el.type() == network::DataElement::Tag::kBytes) {
          std::string_view sv = el.As<network::DataElementBytes>().AsStringView();
          post_body.append(sv.data(), sv.size());
        }
      }
    }
    // Forward the request's own headers (fetch headers:{}, XHR setRequestHeader)
    // — Authorization, X-*, Accept, etc. — so API calls authenticate/negotiate.
    // Content-Type is carried separately (post_ct) to avoid a duplicate header.
    // Kept STRUCTURED (not pre-joined) so a redirect can apply blink's removed_headers/
    // modified_headers between hops — otherwise Authorization/Cookie/etc. would be re-sent
    // to a cross-origin redirect target (a credential leak blink asks us to prevent).
    net::HttpRequestHeaders hop_headers;
    for (const auto& kv : request->headers.GetHeaderVector()) {
      if (ToLower(kv.key) == "content-type") {
        post_ct = kv.value;
        continue;
      }
      hop_headers.SetHeader(kv.key, kv.value);
    }
    // ASYNC: run the (blocking) curl transfer on the thread pool, NOT inline on the
    // main thread — this is the input-freeze fix and the architecture alignment with
    // miniblink49 (whose WebURLLoaderManager runs curl off-main on IO threads). Each
    // redirect hop is fetched on a worker; WillFollowRedirect is replayed on the MAIN
    // thread BETWEEN hops (in OnHopComplete) so url_list_ / response.url / .redirected
    // stay correct and CORS-redirect + cancel checks are preserved. (Per-URL injected
    // headers from mbSetRequestHeader are applied inside FetchHttp, the shared
    // chokepoint, so the top-level navigation gets them too.)
    auto state = std::make_unique<FetchState>();
    state->original_url = url;
    state->fetch_url = fetch_url;
    state->cur = fetch_url.spec();
    state->visible_cur = url.spec();
    state->method = request->method;
    state->body = std::move(post_body);
    state->post_ct = std::move(post_ct);
    state->hop_headers = std::move(hop_headers);
    state->request_headers = std::move(policy.headers);
    state->carried_extra_headers = extra_headers_;  // scrubbed per-hop (see OnHopComplete)
    state->site_for_cookies = request->site_for_cookies;
    state->pin_pubkey = std::move(policy.pin_pubkey);
    StartHttpFetch(std::move(state));
    return;  // async — OnHopComplete continues on the main thread when the hop returns
  } else if (fetch_url.SchemeIs("data")) {
    // Decode the data: URL in-process (libcurl doesn't serve it); the parsed
    // mime flows into the response Content-Type below via http_content_type.
    std::string charset;
    ok = net::DataURL::Parse(fetch_url, &http_content_type, &charset, &contents);
  }

  // Fetch complete — hand the result to the main-thread delivery path. When the
  // blocking fetch above moves to a worker thread (to match miniblink49's async
  // IO-thread network model, where curl_multi runs off-main and results post back via
  // WebURLLoaderManagerMainTask), THIS is the base::BindPostTask seam: the curl I/O
  // runs off-main and DeliverResponse runs back on the main thread (blink client calls
  // must never happen off-main).
  FetchResult fr;
  fr.ok = ok;
  fr.contents = std::move(contents);
  fr.http_content_type = std::move(http_content_type);
  fr.http_status = http_status;
  fr.resp_headers = std::move(resp_headers);
  fr.final_url = std::move(final_url);
  fr.redirected = redirected;
  DeliverResponse(url, fetch_url, std::move(fr));
}

void MbURLLoader::StartHttpFetch(std::unique_ptr<FetchState> state) {
  // Policy already finalized THIS hop's headers on the loader sequence. The reactor must
  // transmit the exact block inspected/overridden by the callbacks, without reapplying
  // static rules on its IO thread.
  MbCurlReactor::Request req;
  req.url = state->cur;
  req.method = state->method;
  req.body = state->body;
  req.post_content_type = state->post_ct;
  req.user_agent = user_agent_;
  req.extra_headers = std::move(state->request_headers);
  req.cookie_session_key = SessionKey();
  req.pin_pubkey = state->pin_pubkey;  // survives across redirect hops

  auto main_runner = base::SingleThreadTaskRunner::GetCurrentDefault();
  auto weak = weak_factory_.GetWeakPtr();
  // Submit ONE hop to the curl_multi reactor (non-blocking, off-main). When it completes,
  // the reactor posts the result back to the MAIN thread; we map it into a HopResult and
  // resume OnHopComplete (per-hop redirect chain). If the loader was destroyed meanwhile
  // the weak ptr no-ops and `state` is freed — the reactor never touched `this`.
  MbCurlReactor::Get()->Submit(
      std::move(req), main_runner,
      base::BindOnce(
          [](base::WeakPtr<MbURLLoader> weak_self, std::unique_ptr<FetchState> st,
             MbCurlReactor::Result r) {
            if (!weak_self)
              return;
            HopResult hr;
            hr.ok = r.ok;
            hr.status = static_cast<int>(r.http_status);
            hr.contents = std::move(r.body);
            hr.resp_headers = std::move(r.headers);
            hr.content_type = std::move(r.content_type);
            hr.final_url = std::move(r.effective_url);
            weak_self->OnHopComplete(std::move(st), std::move(hr));
          },
          weak, std::move(state)));
}

void MbURLLoader::OnHopComplete(std::unique_ptr<FetchState> state, HopResult hop) {
  if (!client_)
    return;
  // Blink can SYNCHRONOUSLY cancel + delete this loader from inside WillFollowRedirect
  // below (it returns false and tears the load down). weak_factory_ guards the posted
  // task, not reentrancy DURING this call, so capture a weak ptr and bail before
  // touching any member after that callback.
  const base::WeakPtr<MbURLLoader> alive = weak_factory_.GetWeakPtr();

  // Helper: finalize this load with the given outcome (success or ERR_FAILED).
  auto finalize = [&](bool ok) {
    FetchResult fr;
    fr.ok = ok;
    if (ok) {
      fr.contents = std::move(hop.contents);
      fr.http_content_type = std::move(hop.content_type);
      fr.http_status = hop.status;
      fr.resp_headers = std::move(hop.resp_headers);
      fr.final_url = state->redirected ? state->visible_cur
                                       : std::move(hop.final_url);
    }
    fr.redirected = state->redirected;
    DeliverResponse(state->original_url, state->fetch_url, std::move(fr));
  };

  // A transport error or a final (non-3xx) response -> deliver it as-is.
  const bool is_redirect = hop.ok && hop.status >= 300 && hop.status < 400;
  const std::string loc =
      is_redirect ? HeaderValueFromBlock(hop.resp_headers, "location") : std::string();
  if (!is_redirect || loc.empty()) {
    finalize(hop.ok);  // 3xx without Location is treated as the final response
    return;
  }

  // Resolve the redirect separately in transport/backend and page-visible space. A
  // relative Location from a rewritten local backend must fetch relative to that backend
  // while Blink sees it relative to the public URL/origin.
  RedirectTargets targets = ResolveRedirectTargets(
      GURL(state->visible_cur), GURL(state->cur), loc);
  const GURL& next_fetch_raw = targets.fetch;
  const GURL& next_visible = targets.visible;
  if (!next_fetch_raw.is_valid() || !next_visible.is_valid() ||
      state->hop >= 20) {
    finalize(false);
    return;
  }
  // Method rewrite: 303 (and POST on 301/302) becomes a bodyless GET.
  std::string new_method = state->method;
  if (hop.status == 303 ||
      ((hop.status == 301 || hop.status == 302) && state->method == "POST")) {
    new_method = "GET";
    state->body.clear();
    state->post_ct.clear();
  }
  // Report the hop to the client on the MAIN thread (updates url_list_; enforces CORS
  // redirect mode + lets the client cancel) BEFORE fetching the next hop.
  blink::WebURLResponse redirect_response;
  redirect_response.SetCurrentRequestUrl(ToWebURL(GURL(state->visible_cur)));
  redirect_response.SetHttpStatusCode(hop.status);
  bool report_raw_headers = false;
  std::vector<std::string> removed_headers;
  net::HttpRequestHeaders modified_headers;
  const bool accepted = client_->WillFollowRedirect(
      ToWebURL(next_visible), state->site_for_cookies, blink::WebString(),
      network::mojom::ReferrerPolicy::kDefault,
      blink::WebString::FromUtf8(new_method), redirect_response, report_raw_headers,
      &removed_headers, modified_headers, /*insecure_scheme_was_upgraded=*/false);
  // CRITICAL: a declined redirect usually means the client is CANCELING the load, which
  // synchronously DELETES this loader — so bail before touching client_ again.
  if (!alive)
    return;
  if (!accepted) {
    finalize(false);  // client declined (and is still alive) -> ERR_FAILED
    return;
  }
  // Apply blink's header edits for the next hop: drop sensitive headers it wants removed
  // on a cross-origin redirect (Authorization, Cookie, …) and take any overrides
  // (updated Referer/Origin/sec-fetch-*).
  for (const std::string& rm : removed_headers) {
    state->hop_headers.RemoveHeader(rm);
    if (ToLower(rm) == "content-type")
      state->post_ct.clear();
  }
  state->hop_headers.MergeFrom(modified_headers);

  std::string next_extra = state->carried_extra_headers;
  if (!MbSameOrigin(GURL(state->cur), next_fetch_raw))
    MbStripCredentialHeaders(&next_extra);
  std::string baseline_headers = next_extra;
  if (!state->post_ct.empty())
    MergeHeaderLines(&baseline_headers, "Content-Type: " + state->post_ct);
  for (const auto& kv : state->hop_headers.GetHeaderVector()) {
    MergeHeaderLines(&baseline_headers, kv.key + ": " + kv.value);
  }
  RequestPolicyDecision policy = EvaluateRequestPolicy(
      next_visible.spec(), next_fetch_raw.spec(), new_method,
      baseline_headers, state->body, view_context_, HostContext());
  if (!alive)
    return;
  if (policy.blocked) {
    finalize(false);
    return;
  }
  std::string next_fetch = std::move(policy.fetch_url);
  if (!policy.pin_pubkey.empty())
    state->pin_pubkey = std::move(policy.pin_pubkey);

  if (policy.mocked) {
    FetchResult fr;
    fr.ok = true;
    fr.contents = std::move(policy.mock_body);
    fr.http_content_type = std::move(policy.mock_content_type);
    fr.http_status = policy.mock_status > 0 ? policy.mock_status : 200;
    fr.final_url = next_visible.spec();
    fr.redirected = true;
    DeliverResponse(state->original_url, GURL(next_fetch), std::move(fr));
    return;
  }
  const GURL next_fetch_url(next_fetch);
  if (!next_fetch_url.SchemeIsHTTPOrHTTPS()) {
    finalize(false);
    return;
  }

  // Blink's removals cover the request's own headers (hop_headers); the embedder's CARRIED
  // extra headers are ours to scrub. On a cross-origin hop, strip their credentials too so
  // an Authorization/Cookie set via mbSetExtraHeaders doesn't ride to a different origin.
  if (!MbSameOrigin(GURL(state->cur), next_fetch_url))
    MbStripCredentialHeaders(&next_extra);
  state->carried_extra_headers = std::move(next_extra);
  state->cur = next_fetch;
  state->visible_cur = next_visible.spec();
  state->fetch_url = next_fetch_url;
  state->request_headers = std::move(policy.headers);
  state->method = new_method;
  state->redirected = true;  // a real 3xx hop was followed
  state->hop++;
  StartHttpFetch(std::move(state));  // fetch the next hop on the thread pool
}

void MbURLLoader::DeliverResponse(const GURL& url,
                                  const GURL& fetch_url,
                                  FetchResult result) {
  if (!client_)
    return;
  // Host callbacks and Blink can SYNCHRONOUSLY cancel + delete this loader from
  // inside the response hook or DidReceiveResponse below. weak_factory_ guards
  // posted tasks, not reentrancy DURING this call, so validate it after each.
  const base::WeakPtr<MbURLLoader> alive = weak_factory_.GetWeakPtr();
  // Unpack into locals so the delivery body below reads unchanged. contents/resp_headers/
  // http_content_type/final_url are aliased (the response hook rewrites them in place);
  // ok/http_status/redirected are plain copies (http_status is rewritten via &http_status).
  const bool ok = result.ok;
  std::string& contents = result.contents;
  std::string& http_content_type = result.http_content_type;
  int http_status = result.http_status;
  std::string& resp_headers = result.resp_headers;
  std::string& final_url = result.final_url;
  const bool redirected = result.redirected;

  if (std::getenv("MB_VERBOSE")) {
    std::fprintf(stderr, "[mb_url_loader] %s -> %s (%zu bytes)\n", url.spec().c_str(),
                 ok ? "OK" : "FAIL", contents.size());
  }
  if (!ok) {
    client_->DidFail(
        blink::WebURLError(net::ERR_FAILED, ToWebURL(url)),
        base::TimeTicks::Now(), blink::URLLoaderClient::kUnknownEncodedDataLength,
        0, 0);
    return;
  }

  // Response hook: let an embedder inspect the headers / rewrite the body before delivery.
  // Report the FINAL URL the bytes came from (post-redirect), matching the documented
  // mbResponseURL contract and the page-visible response.url set below. Any replacement
  // flows into the mime/header/content-length build below (SetExpectedContentLength uses
  // contents.size()). resp_headers is the final response's header lines (empty for non-http).
  const std::string hook_url =
      (redirected && !result.final_url.empty()) ? result.final_url : url.spec();
  InvokeResponseHook(hook_url, &http_status, &resp_headers, &contents);
  if (!alive)
    return;

  // If the hook set/overrode Content-Type in the header block, honor it for the delivered
  // MIME (Content-Type is special-cased below, so a block edit alone wouldn't take effect).
  {
    const std::string ct = HeaderValueFromBlock(resp_headers, "content-type");
    if (!ct.empty())
      http_content_type = ct;
  }

  // Response mime: from the server's Content-Type (http), else the file extension.
  std::string mime_str;
  std::string content_type_header;
  if (!http_content_type.empty()) {
    content_type_header = http_content_type;
    mime_str = http_content_type.substr(0, http_content_type.find(';'));
    while (!mime_str.empty() && mime_str.back() == ' ')
      mime_str.pop_back();
  } else {
    mime_str = MimeFromPath(std::string(fetch_url.path())).Utf8();
    content_type_header = mime_str;
  }

  blink::WebURLResponse response;
  // Response URL. Only override with the final URL when a REAL redirect hop was
  // followed — then it equals Blink's url_list_.back() (advanced by the
  // WillFollowRedirect calls above), so fetch's response.url/redirected are
  // correct and the fetch_manager URL DCHECK holds. A transparent URL rewrite
  // (MbApplyUrlRewrites) is NOT a redirect: report the page's ORIGINAL url so the
  // rewrite stays invisible and url_list_.back() still matches.
  response.SetCurrentRequestUrl(
      (redirected && !final_url.empty()) ? ToWebURL(GURL(final_url))
                                         : ToWebURL(url));
  response.SetMimeType(blink::WebString::FromUtf8(mime_str));
  // Expose the server's response headers to JS (fetch Response.headers.get(),
  // XHR getResponseHeader) — set them before Content-Type so the explicit one
  // below wins, and skip headers Blink derives from the delivered bytes.
  if (!resp_headers.empty()) {
    // Split into logical header lines, unfolding obs-fold continuations (a line
    // starting with SP/HTAB continues the previous header's value) so a folded
    // value isn't silently dropped.
    std::vector<std::string> hlines;
    std::string hline;
    std::string hbuf = resp_headers;
    hbuf.push_back('\n');  // flush sentinel
    for (char c : hbuf) {
      if (c == '\n') {
        if (!hline.empty() && (hline.front() == ' ' || hline.front() == '\t') &&
            !hlines.empty())
          hlines.back() += hline;  // continuation -> fold into previous header
        else if (!hline.empty())
          hlines.push_back(hline);
        hline.clear();
      } else if (c != '\r') {
        hline.push_back(c);
      }
    }
    for (const std::string& h : hlines) {
      std::string::size_type colon = h.find(':');
      if (colon != std::string::npos && h.rfind("HTTP/", 0) != 0) {
        std::string name = h.substr(0, colon);
        std::string value = h.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
          value.erase(value.begin());
        const std::string lname = ToLower(name);
        // Strip headers that describe the ON-THE-WIRE body, not the bytes we deliver:
        // curl already decompressed the body (CURLOPT_ACCEPT_ENCODING ""), so a stale
        // content-encoding/content-md5 would describe gzip the JS never sees; length/
        // chunking are likewise meaningless post-decode (Blink derives them).
        if (lname != "content-length" && lname != "transfer-encoding" &&
            lname != "content-encoding" && lname != "content-md5") {
          // AddHttpHeaderField (comma-join), not Set (replace): a response with
          // multiple Set-Cookie/Link/Vary/WWW-Authenticate lines must expose all of
          // them, not just the last. Content-Type is set authoritatively below.
          response.AddHttpHeaderField(blink::WebString::FromUtf8(name),
                                      blink::WebString::FromUtf8(value));
        }
      }
    }
  }
  // Stylesheets validate the Content-Type *header* (CSSStyleSheetResource::CanUseSheet).
  response.SetHttpHeaderField(blink::WebString::FromUtf8("Content-Type"),
                              blink::WebString::FromUtf8(content_type_header));
  // Real HTTP status (so fetch sees 404/500 + response.ok); file/data -> 200.
  response.SetHttpStatusCode(http_status > 0 ? http_status : 200);
  // statusText: the reason phrase from the final "HTTP/x.y NNN reason" status line
  // (the first line of the captured header block; empty for file/data).
  {
    std::string reason;
    const std::string::size_type eol = resp_headers.find_first_of("\r\n");
    const std::string status_line = resp_headers.substr(0, eol);
    if (status_line.rfind("HTTP/", 0) == 0) {
      const std::string::size_type sp1 = status_line.find(' ');
      const std::string::size_type sp2 =
          sp1 == std::string::npos ? sp1 : status_line.find(' ', sp1 + 1);
      if (sp2 != std::string::npos) {
        reason = status_line.substr(sp2 + 1);
        while (!reason.empty() && (reason.back() == ' ' || reason.back() == '\t'))
          reason.pop_back();
      }
    }
    // ALWAYS set a non-null statusText. A null one serializes to a null
    // FetchAPIResponse.status_text and FATAL-crashes blink's mojom validator the
    // moment the page caches the response (cache.put) — which YouTube does. The
    // wire reason is present for real HTTP; file/data/mock have no status line, so
    // fall back to "OK" (they are 200) — a non-null placeholder is what matters.
    response.SetHttpStatusText(
        blink::WebString::FromUtf8(reason.empty() ? "OK" : reason));
  }
  response.SetExpectedContentLength(static_cast<int64_t>(contents.size()));

  // Deliver the body via a real Mojo data pipe (the production path). This is required
  // for the Fetch API (FetchManager streams the body through a BytesConsumer); the
  // DidReceiveDataForTesting shortcut breaks fetch() (place_holder_body_ DCHECK).
  body_ = std::move(contents);
  const int64_t len = static_cast<int64_t>(body_.size());

  mojo::ScopedDataPipeProducerHandle producer;
  mojo::ScopedDataPipeConsumerHandle consumer;
  if (mojo::CreateDataPipe(nullptr, producer, consumer) != MOJO_RESULT_OK) {
    client_->DidFail(
        blink::WebURLError(net::ERR_FAILED, ToWebURL(url)),
        base::TimeTicks::Now(), blink::URLLoaderClient::kUnknownEncodedDataLength,
        0, 0);
    return;
  }
  client_->DidReceiveResponse(response, std::move(consumer), std::nullopt);
  if (!alive)
    return;  // blink canceled + deleted this loader during DidReceiveResponse;
             // do NOT touch data_pipe_producer_/weak_factory_ on freed `this`.

  // DataPipeProducer chunk-writes the whole body asynchronously, then we finish.
  data_pipe_producer_ = std::make_unique<mojo::DataPipeProducer>(std::move(producer));
  data_pipe_producer_->Write(
      std::make_unique<mojo::StringDataSource>(
          base::span<const char>(body_),
          mojo::StringDataSource::AsyncWritingMode::
              STRING_STAYS_VALID_UNTIL_COMPLETION),
      base::BindOnce(&MbURLLoader::OnBodyWritten, weak_factory_.GetWeakPtr(), len));
}

scoped_refptr<base::SingleThreadTaskRunner>
MbURLLoader::GetTaskRunnerForBodyLoader() {
  return base::SingleThreadTaskRunner::GetCurrentDefault();
}

void MbURLLoader::OnBodyWritten(int64_t length, uint32_t /*MojoResult*/ result) {
  data_pipe_producer_.reset();
  if (client_)
    client_->DidFinishLoading(base::TimeTicks::Now(), length, length, length);
}

void MbURLLoader::StartSse(const std::string& fetch_url,
                           const std::string& req_headers,
                           const std::string& first_request_headers,
                           const std::string& pinned_pubkey,
                           const std::string& report_url) {
  if (!client_)
    return;
  // Synthesize the response head: EventSource only requires a 2xx + a
  // text/event-stream Content-Type to begin reading the stream.
  blink::WebURLResponse response;
  response.SetCurrentRequestUrl(ToWebURL(GURL(report_url)));
  response.SetMimeType(blink::WebString::FromUtf8("text/event-stream"));
  response.SetHttpStatusCode(200);
  response.SetHttpStatusText(blink::WebString::FromUtf8("OK"));  // non-null (cache.put safe)
  response.SetHttpHeaderField(blink::WebString::FromUtf8("Content-Type"),
                              blink::WebString::FromUtf8("text/event-stream"));
  response.SetExpectedContentLength(-1);  // unknown / streaming

  mojo::ScopedDataPipeProducerHandle producer;
  mojo::ScopedDataPipeConsumerHandle consumer;
  if (mojo::CreateDataPipe(nullptr, producer, consumer) != MOJO_RESULT_OK) {
    client_->DidFail(
        blink::WebURLError(net::ERR_FAILED, ToWebURL(GURL(report_url))),
        base::TimeTicks::Now(),
        blink::URLLoaderClient::kUnknownEncodedDataLength, 0, 0);
    return;
  }
  const base::WeakPtr<MbURLLoader> alive = weak_factory_.GetWeakPtr();
  client_->DidReceiveResponse(response, std::move(consumer), std::nullopt);
  if (!alive)
    return;
  sse_producer_ = std::move(producer);
  sse_watcher_ = std::make_unique<mojo::SimpleWatcher>(
      FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::MANUAL);
  sse_watcher_->Watch(
      sse_producer_.get(), MOJO_HANDLE_SIGNAL_WRITABLE,
      base::BindRepeating(&MbURLLoader::DrainSse, weak_factory_.GetWeakPtr()));

  auto runner = base::SingleThreadTaskRunner::GetCurrentDefault();
  const void* host_ctx = HostContext();
  scoped_refptr<base::SequencedTaskRunner> hook_runner =
      view_context_ ? view_context_->engine_task_runner() : runner;
  sse_stream_ = std::make_shared<MbSseStream>(
      fetch_url, report_url, SessionKey(), req_headers,
      first_request_headers, pinned_pubkey, user_agent_, host_ctx,
      std::move(hook_runner),
      base::BindPostTask(runner, base::BindRepeating(
                                     &MbURLLoader::OnSseChunk,
                                     weak_factory_.GetWeakPtr())),
      base::BindPostTask(runner, base::BindOnce(&MbURLLoader::OnSseDone,
                                                weak_factory_.GetWeakPtr())));
  sse_stream_->Start();
}

void MbURLLoader::OnSseChunk(std::string bytes) {
  sse_buf_ += bytes;
  DrainSse();
}

void MbURLLoader::OnSseDone() {
  sse_ended_ = true;
  DrainSse();
}

void MbURLLoader::DrainSse(MojoResult /*result*/) {
  if (!sse_producer_.is_valid())
    return;
  while (sse_pos_ < sse_buf_.size()) {
    size_t written = 0;
    base::span<const uint8_t> data =
        base::as_byte_span(sse_buf_).subspan(sse_pos_);
    MojoResult rv =
        sse_producer_->WriteData(data, MOJO_WRITE_DATA_FLAG_NONE, written);
    if (rv == MOJO_RESULT_OK && written > 0) {
      sse_pos_ += written;
      sse_total_ += static_cast<int64_t>(written);
      continue;
    }
    if (rv == MOJO_RESULT_SHOULD_WAIT) {
      sse_watcher_->ArmOrNotify();
      return;
    }
    break;  // pipe error -> tear down
  }
  if (sse_pos_ >= sse_buf_.size()) {  // fully drained -> compact
    sse_buf_.clear();
    sse_pos_ = 0;
  }
  if (sse_ended_ && sse_buf_.empty()) {
    sse_watcher_.reset();
    sse_producer_.reset();  // EOF -> EventSource sees the stream end (reconnects)
    if (client_)
      client_->DidFinishLoading(base::TimeTicks::Now(), sse_total_, sse_total_,
                                sse_total_);
  }
}

}  // namespace mb
