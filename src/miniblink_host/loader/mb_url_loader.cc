// mb_url_loader.cc — file-backed blink::URLLoader. Status: Phase 2 (subresources).
#include "miniblink_host/loader/mb_url_loader.h"

#include <curl/curl.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
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
#include "base/synchronization/lock.h"
#include "base/task/bind_post_task.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "net/base/data_url.h"
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

// A process-wide in-memory cookie jar shared by every fetch, so Set-Cookie is honored
// across a redirect chain (consent walls, login flows) AND across separate requests
// (the main document and its subresources, or successive navigations) — i.e. the host
// behaves like one browsing session. The share is touched from more than one
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
CURLSH* CookieShare() {
  static CURLSH* share = [] {
    CURLSH* s = curl_share_init();
    if (s) {
      curl_share_setopt(s, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
      curl_share_setopt(s, CURLSHOPT_LOCKFUNC, CookieShareLockCb);
      curl_share_setopt(s, CURLSHOPT_UNLOCKFUNC, CookieShareUnlockCb);
    }
    return s;
  }();
  return share;
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
  std::string line;
  std::string buf = block;
  buf.push_back('\n');  // flush sentinel
  for (char c : buf) {
    if (c == '\n' || c == '\r') {
      std::string::size_type colon = line.find(':');
      if (colon != std::string::npos && ToLower(line.substr(0, colon)) == lname) {
        std::string v = line.substr(colon + 1);
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
          v.erase(v.begin());
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t'))
          v.pop_back();
        return v;
      }
      line.clear();
    } else {
      line.push_back(c);
    }
  }
  return {};
}

bool FetchHttp(const std::string& url, std::string* body, std::string* content_type,
               const std::string& user_agent, const std::string& extra_headers,
               const std::string& post_body = "",
               const std::string& post_content_type = "",
               const std::string& http_method = "", int* out_status = nullptr,
               std::string* out_headers = nullptr,
               std::string* out_final_url = nullptr,
               bool follow_redirects = true,
               std::string* out_error = nullptr) {
  CURL* curl = curl_easy_init();
  if (!curl)
    return false;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
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
  std::string header_block;  // final response's raw header lines (for the caller)
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, CurlHeaderWrite);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header_block);
  // Method + body. A non-GET verb (form submit, fetch/XHR POST/PUT/DELETE…) or a
  // non-empty body makes this a write request. COPYPOSTFIELDS copies the body so
  // it survives retries; CUSTOMREQUEST sets the exact verb (POST is otherwise
  // implied by POSTFIELDS, but set it explicitly so empty-body POST and
  // PUT/PATCH/DELETE are correct too).
  std::string method = http_method;
  if (method.empty() && !post_body.empty())
    method = "POST";
  if (!method.empty() && method != "GET" && method != "HEAD") {
    if (!post_body.empty()) {
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                       static_cast<long>(post_body.size()));
      curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, post_body.c_str());
    }
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
  if (CURLSH* share = CookieShare())
    curl_easy_setopt(curl, CURLOPT_SHARE, share);  // shared jar across all fetches

  // Request headers: the host's extra "Name: Value" lines, plus a default
  // Accept-Language (so sites serve localized content) unless the host set one.
  curl_slist* header_list = nullptr;
  bool has_accept_language = false;
  std::string line;
  std::string lines = extra_headers;
  // Per-URL injected headers (mbSetRequestHeader) — conditional on the URL. Done here
  // so BOTH the top-level navigation (MbFetchUrl) and subresources/fetch (Deliver),
  // which share this function, get them.
  mb::MbApplyRequestHeaders(url, &lines);
  lines.push_back('\n');  // sentinel so the last line flushes
  for (char c : lines) {
    if (c == '\n' || c == '\r') {
      if (!line.empty()) {
        if (ToLower(line).rfind("accept-language:", 0) == 0)
          has_accept_language = true;
        header_list = curl_slist_append(header_list, line.c_str());
        line.clear();
      }
    } else {
      line.push_back(c);
    }
  }
  if (!has_accept_language)
    header_list = curl_slist_append(header_list, "Accept-Language: en-US,en;q=0.9");
  if (!post_body.empty()) {
    // Forms default to urlencoded; honor an explicit type (e.g. text/plain).
    const std::string ct_line =
        "Content-Type: " +
        (post_content_type.empty() ? std::string("application/x-www-form-urlencoded")
                                   : post_content_type);
    header_list = curl_slist_append(header_list, ct_line.c_str());
  }
  if (header_list)
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);

  constexpr int kMaxAttempts = 3;
  CURLcode rc = CURLE_OK;
  long http_code = 0;
  for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
    body->clear();  // CurlWrite appends; start each attempt fresh
    rc = curl_easy_perform(curl);
    http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    const bool success_code =
        rc == CURLE_OK && http_code >= 200 && http_code < 400;
    const bool ok = success_code && !body->empty();
    // An empty body on an otherwise-OK response is anomalous — it's the exact
    // shape a throttled/half-open connection produces, and it's what made bursts
    // of requests render blank. Treat it as transient and retry.
    const bool retryable =
        !ok && attempt < kMaxAttempts &&
        (IsTransientCurlError(rc) ||
         (rc == CURLE_OK && IsTransientHttpCode(http_code)) ||
         (success_code && body->empty()));
    if (!retryable)
      break;

    if (std::getenv("MB_VERBOSE")) {
      std::fprintf(stderr,
                   "[mb_url_loader] transient failure (curl=%d http=%ld) on %s "
                   "— retry %d/%d\n",
                   rc, http_code, url.c_str(), attempt, kMaxAttempts - 1);
    }
    // Linear backoff: 250ms, 500ms. Modest — a render shouldn't stall for long,
    // and curl already bounds each attempt with its own timeouts.
    base::PlatformThread::Sleep(base::Milliseconds(250 * attempt));
  }

  const char* ct = nullptr;
  curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
  if (ct)
    *content_type = ct;
  if (out_status)
    *out_status = static_cast<int>(http_code);
  if (out_headers)
    *out_headers = std::move(header_block);
  if (out_final_url) {
    const char* eu = nullptr;  // URL after curl-followed redirects
    if (curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eu) == CURLE_OK && eu)
      *out_final_url = eu;
  }
  curl_easy_cleanup(curl);
  if (header_list)
    curl_slist_free_all(header_list);
  // Success means we got a complete HTTP response — ANY status, incl. 4xx/5xx.
  // Those are real answers the caller must deliver (fetch needs response.status
  // and the error body; a 404 page should render), NOT network failures. Only a
  // transport error (no response at all) returns false.
  const bool ok = rc == CURLE_OK && http_code > 0;
  if (out_error) {
    if (ok)
      out_error->clear();
    else if (rc != CURLE_OK)
      *out_error = curl_easy_strerror(rc);  // "Couldn't resolve host name", etc.
    else
      *out_error = "no response from server";
  }
  return ok;
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

const std::string& MbDefaultUserAgent() {
  // M150 desktop Chrome on macOS — current enough that UA-sniffing sites serve
  // their modern desktop layout/JS. NoDestructor: no exit-time dtor (-Werror).
  static const base::NoDestructor<std::string> kUa(
      "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/150.0.0.0 Safari/537.36");
  return *kUa;
}

namespace {
// Process-wide proxy. Set on the main thread (host config) before navigating;
// read on the fetch path. A NoDestructor string (no exit-time dtor) plus a flag
// distinguishing "never set" (honor libcurl defaults) from "set to ''" (force
// direct). The single bool is touched only around config + fetch on the same
// thread, so no synchronization is needed beyond the cookie jar's existing model.
std::string& ProxyStorage() {
  static base::NoDestructor<std::string> s;
  return *s;
}
bool g_proxy_set = false;
}  // namespace

void MbSetProxy(const std::string& proxy) {
  ProxyStorage() = proxy;
  g_proxy_set = true;
}

bool MbProxyConfigured(std::string* out) {
  if (g_proxy_set && out)
    *out = ProxyStorage();
  return g_proxy_set;
}

namespace {
// When true, TLS peer/host verification is disabled for network fetches (the
// equivalent of curl -k / --no-check-certificate). Default false — verify, the
// secure default. Set on the main thread before navigating; read on the fetch
// path (same single-thread model as the proxy flag).
bool g_ignore_cert_errors = false;

// When false, a top-level/subresource fetch does NOT follow 3xx redirects, so the
// caller sees the redirect response itself (status 30x + Location header) — for
// resolving URL shorteners / inspecting redirect chains. Default true (follow).
bool g_follow_redirects = true;
}  // namespace

void MbSetIgnoreCertErrors(bool ignore) {
  g_ignore_cert_errors = ignore;
}

bool MbIgnoreCertErrors() {
  return g_ignore_cert_errors;
}

void MbSetFollowRedirects(bool follow) {
  g_follow_redirects = follow;
}

bool MbFollowRedirects() {
  return g_follow_redirects;
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

void MbAddCookieToJar(const std::string& url, const std::string& cookie) {
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
  // Session by default (expiry 0). A deletion (max-age=0 / 1970 expiry) becomes a
  // past expiry so curl drops it from the jar.
  std::string expiry = "0";
  if (lattrs.find("max-age=0") != std::string::npos ||
      lattrs.find("expires=thu, 01 jan 1970") != std::string::npos)
    expiry = "1";
  // Netscape TSV (domain tailmatch path secure expiry name value) — injected via
  // COOKIELIST with an explicit domain, so it needs no active transfer to land in
  // the shared jar (a "Set-Cookie:"-format line without a domain is dropped).
  const std::string ns = domain + "\t" + tailmatch + "\t" + path + "\t" + secure +
                         "\t" + expiry + "\t" + name + "\t" + value;
  CURL* curl = curl_easy_init();
  if (!curl)
    return;
  curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");  // enable the cookie engine
  if (CURLSH* share = CookieShare())
    curl_easy_setopt(curl, CURLOPT_SHARE, share);
  curl_easy_setopt(curl, CURLOPT_COOKIELIST, ns.c_str());
  curl_easy_cleanup(curl);
}

void MbClearCookieJar() {
  CURL* curl = curl_easy_init();
  if (!curl)
    return;
  curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");  // enable the cookie engine
  if (CURLSH* share = CookieShare())
    curl_easy_setopt(curl, CURLOPT_SHARE, share);
  curl_easy_setopt(curl, CURLOPT_COOKIELIST, "ALL");  // erase every cookie
  curl_easy_cleanup(curl);
}

std::string MbGetAllCookies() {
  // Snapshot the WHOLE shared jar (every host, session + persistent) via
  // CURLINFO_COOKIELIST — the same list MbGetCookiesForUrl reads — formatted as a
  // Netscape cookie file in memory. We format it ourselves rather than relying on
  // CURLOPT_COOKIEJAR flushing a shared store, which is unreliable without a
  // transfer.
  CURL* curl = curl_easy_init();
  if (!curl)
    return std::string();
  curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");  // enable the cookie engine
  if (CURLSH* share = CookieShare())
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

bool MbSaveCookies(const std::string& path) {
  if (path.empty())
    return false;
  // Write the whole-jar snapshot (a Netscape cookie file, curl's native format,
  // reloadable by curl itself) to `path`.
  return base::WriteFile(base::FilePath::FromUTF8Unsafe(path),
                         MbGetAllCookies());
}

bool MbLoadCookies(const std::string& path) {
  std::string contents;
  if (path.empty() ||
      !base::ReadFileToString(base::FilePath::FromUTF8Unsafe(path), &contents))
    return false;
  CURL* curl = curl_easy_init();
  if (!curl)
    return false;
  curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");  // enable the cookie engine
  if (CURLSH* share = CookieShare())
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

std::string MbGetCookiesForUrl(const std::string& url) {
  GURL gurl(url);
  if (!gurl.SchemeIsHTTPOrHTTPS())
    return {};
  const std::string host(gurl.host());
  CURL* curl = curl_easy_init();
  if (!curl)
    return {};
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");  // enable engine
  if (CURLSH* share = CookieShare())
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
constexpr size_t kRequestLogCap = 2048;
}  // namespace

void MbRecordRequest(const std::string& url) {
  if (url.empty())
    return;
  auto& log = RequestLog();
  if (log.size() >= kRequestLogCap)
    log.erase(log.begin(), log.begin() + (log.size() - kRequestLogCap + 1));
  log.push_back(url);
}

std::string MbGetRequestLog() {
  std::string out;
  for (const std::string& u : RequestLog()) {
    out += u;
    out += "\n";
  }
  return out;
}

void MbClearRequestLog() {
  RequestLog().clear();
}

size_t MbRequestCount() {
  return RequestLog().size();
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

void MbInvokeResponseHook(const std::string& url, int* status,
                          std::string* headers, std::string* body) {
  if (ResponseHook())
    ResponseHook()(url, status, headers, body);
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

bool MbFindMock(const std::string& url, std::string* body,
                std::string* content_type, int* status) {
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
// Add/override an outgoing request header for any http(s) request whose URL contains a
// registered substring — e.g. send an Authorization / API-key header only to its own
// API host (not leaked to every origin a page touches), or set a per-domain UA. Unlike
// the global extra-headers, this is conditional on the URL.
namespace {
struct HeaderEntry {
  std::string substring;
  std::string name;
  std::string value;
};
std::vector<HeaderEntry>& HeaderList() {
  static std::vector<HeaderEntry>* h = new std::vector<HeaderEntry>();
  return *h;
}
}  // namespace

void MbAddRequestHeader(const std::string& substring, const std::string& name,
                        const std::string& value) {
  if (!substring.empty() && !name.empty())
    HeaderList().push_back({substring, name, value});
}

void MbClearRequestHeaders() {
  HeaderList().clear();
}

void MbApplyRequestHeaders(const std::string& url, std::string* req_headers) {
  for (const HeaderEntry& e : HeaderList()) {
    if (url.find(e.substring) == std::string::npos)
      continue;
    if (!req_headers->empty())
      *req_headers += "\n";
    *req_headers += e.name + ": " + e.value;
  }
}

bool MbFetchUrl(const std::string& url_spec, std::string* body,
                std::string* content_type, const std::string& user_agent,
                const std::string& extra_headers, const std::string& post_body,
                const std::string& post_content_type,
                const std::string& http_method, std::string* out_final_url,
                int* out_status, std::string* out_headers,
                std::string* out_error) {
  if (out_error)
    out_error->clear();
  GURL url(url_spec);
  // Response mocking: a registered URL substring serves its canned body without a real fetch.
  // Checked before any scheme — matching the async loader (Deliver) — so worker scripts,
  // iframes, and top-level navigations (all of which fetch through here) can be mocked too.
  {
    std::string mock_body, mock_ct;
    int mock_status = 0;
    if (MbFindMock(url.spec(), &mock_body, &mock_ct, &mock_status)) {
      *body = std::move(mock_body);
      if (content_type)
        *content_type = mock_ct;
      if (out_status)
        *out_status = mock_status > 0 ? mock_status : 200;
      if (out_final_url)
        *out_final_url = url_spec;
      return true;
    }
  }
  if (url.SchemeIsFile()) {
    // Convert via net (percent-decodes the path; "Andale%20Mono.ttf" -> a space)
    // — a raw url.path() leaves it encoded and ReadFileToString fails.
    base::FilePath fp;
    const bool ok =
        net::FileURLToFilePath(url, &fp) && base::ReadFileToString(fp, body);
    if (!ok && out_error)
      *out_error = "file not found or unreadable";
    return ok;
  }
  if (url.SchemeIsHTTPOrHTTPS())
    return FetchHttp(url_spec, body, content_type, user_agent, extra_headers,
                     post_body, post_content_type, http_method, out_status,
                     out_headers, out_final_url, MbFollowRedirects(), out_error);
  if (url.SchemeIs("data")) {
    std::string mime, charset;
    if (!net::DataURL::Parse(url, &mime, &charset, body))
      return false;
    if (content_type)
      *content_type = mime;
    return true;
  }
  return false;
}

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
              std::string req_headers,
              std::string user_agent,
              ChunkCb chunk,
              DoneCb done)
      : url_(std::move(url)),
        req_headers_(std::move(req_headers)),
        user_agent_(std::move(user_agent)),
        chunk_(std::move(chunk)),
        done_(std::move(done)) {}

  void Start() {
    auto self = shared_from_this();  // keep alive across the worker's life
    std::thread([self] { self->Run(); }).detach();
  }
  void Stop() { stop_.store(true); }

 private:
  static size_t WriteThunk(char* p, size_t s, size_t n, void* ud) {
    auto* self = static_cast<MbSseStream*>(ud);
    if (self->stop_.load())
      return 0;  // returning <bytes aborts curl_easy_perform
    self->chunk_.Run(std::string(p, s * n));
    return s * n;
  }
  static int XferThunk(void* ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    // Called periodically even while the stream is idle -> prompt cancellation.
    return static_cast<MbSseStream*>(ud)->stop_.load() ? 1 : 0;
  }

  void Run() {
    CURL* c = curl_easy_init();
    if (!c) {
      std::move(done_).Run();
      return;
    }
    curl_easy_setopt(c, CURLOPT_URL, url_.c_str());
    std::string proxy;
    if (MbProxyConfigured(&proxy))
      curl_easy_setopt(c, CURLOPT_PROXY, proxy.c_str());
    if (MbIgnoreCertErrors()) {
      curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
      curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    curl_easy_setopt(
        c, CURLOPT_USERAGENT,
        (user_agent_.empty() ? MbDefaultUserAgent() : user_agent_).c_str());
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_COOKIEFILE, "");  // in-memory cookie engine
    if (CURLSH* share = CookieShare())
      curl_easy_setopt(c, CURLOPT_SHARE, share);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    // NO CURLOPT_TIMEOUT — an SSE stream is intentionally long-lived.
    curl_slist* headers = nullptr;
    std::string line, lines = req_headers_;
    lines.push_back('\n');
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
    if (headers)
      curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, &WriteThunk);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, &XferThunk);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, this);
    curl_easy_perform(c);  // streams via WriteThunk until EOF / error / abort
    if (headers)
      curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    std::move(done_).Run();  // -> OnSseDone on the loader thread (if alive)
  }

  std::string url_;
  std::string req_headers_;
  std::string user_agent_;
  ChunkCb chunk_;
  DoneCb done_;
  std::atomic<bool> stop_{false};
};

MbURLLoader::MbURLLoader(std::string user_agent, std::string extra_headers)
    : user_agent_(std::move(user_agent)),
      extra_headers_(std::move(extra_headers)) {}
MbURLLoader::~MbURLLoader() {
  if (sse_stream_)
    sse_stream_->Stop();  // detached worker observes stop_ and exits
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
  // Deliver on the main thread, async (blink expects no reentrancy here).
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&MbURLLoader::Deliver, weak_factory_.GetWeakPtr(),
                                std::move(request)));
}

void MbURLLoader::Deliver(std::unique_ptr<network::ResourceRequest> request) {
  if (!client_)
    return;
  const GURL& url = request->url;     // the page's URL (response.url, log, errors)
  MbRecordRequest(url.spec());        // log the URL the page actually requested

  // URL rewriting: TRANSPARENTLY redirect the request to a different URL — the
  // block/mock/fetch act on `fetch_url`, but the response is still reported as the
  // page's original `url` (so fetch()'s url_list_ DCHECK holds; host swap, scheme
  // upgrade, CDN -> local mock).
  const std::string rewritten = MbApplyUrlRewrites(url.spec());
  const GURL fetch_url = (rewritten == url.spec()) ? url : GURL(rewritten);

  // Request blocking: fail a blocked URL up front (ERR_BLOCKED_BY_CLIENT), before
  // any fetch — the resource simply never loads (ad/tracker/image suppression).
  // Both the static blocklist (matched on the post-rewrite fetch_url) and the dynamic
  // per-request hook can veto. The hook gets the full request (url + method + headers +
  // body) so an embedder can inspect/monitor API calls, not just match URLs.
  std::string hook_headers;
  for (const auto& kv : request->headers.GetHeaderVector()) {
    if (!hook_headers.empty())
      hook_headers += "\n";
    hook_headers += kv.key + ": " + kv.value;
  }
  std::string hook_body;
  if (request->request_body) {
    for (const auto& el : *request->request_body->elements())
      if (el.type() == network::DataElement::Tag::kBytes) {
        std::string_view sv = el.As<network::DataElementBytes>().AsStringView();
        hook_body.append(sv.data(), sv.size());
      }
  }
  if (MbIsUrlBlocked(fetch_url.spec()) ||
      MbIsResourceTypeBlocked(
          network::RequestDestinationToString(request->destination)) ||
      MbRequestHookBlocks(url.spec(), request->method, hook_headers, hook_body)) {
    client_->DidFail(
        blink::WebURLError(net::ERR_BLOCKED_BY_CLIENT, ToWebURL(url)),
        base::TimeTicks::Now(), blink::URLLoaderClient::kUnknownEncodedDataLength,
        0, 0);
    return;
  }

  // EventSource / SSE: a request with `Accept: text/event-stream` to http(s) is a
  // long-lived stream. The buffered path below would block forever (no EOF) and
  // freeze the engine. Stream it on a worker thread instead — UNLESS it's mocked
  // (an offline test serves a complete event-stream body the buffered way).
  if (fetch_url.SchemeIsHTTPOrHTTPS()) {
    std::string accept;
    for (const auto& kv : request->headers.GetHeaderVector())
      if (ToLower(kv.key) == "accept")
        accept = kv.value;
    std::string mock_b, mock_c;
    int mock_s = 0;
    if (accept.find("text/event-stream") != std::string::npos &&
        !MbFindMock(fetch_url.spec(), &mock_b, &mock_c, &mock_s)) {
      std::string req_headers = extra_headers_;
      for (const auto& kv : request->headers.GetHeaderVector()) {
        if (ToLower(kv.key) == "content-type")
          continue;
        if (!req_headers.empty())
          req_headers += "\n";
        req_headers += kv.key + ": " + kv.value;
      }
      mb::MbApplyRequestHeaders(fetch_url.spec(), &req_headers);
      StartSse(fetch_url.spec(), req_headers, url.spec());
      return;
    }
  }

  std::string contents;
  std::string http_content_type;  // from the server, may be "text/html; charset=..."
  int http_status = 0;            // real HTTP status (0 -> treat as 200)
  std::string resp_headers;       // raw final-response header block (http only)
  std::string final_url;          // URL after manual redirect following (http)
  bool ok = false;
  // Response mocking: a registered URL substring serves its canned body without a
  // real fetch (offline tests, API substitution). Checked before any scheme fetch.
  std::string mock_body, mock_ct;
  int mock_status = 0;
  if (MbFindMock(fetch_url.spec(), &mock_body, &mock_ct, &mock_status)) {
    contents = std::move(mock_body);
    http_content_type = mock_ct.empty() ? std::string("text/html") : mock_ct;
    http_status = mock_status > 0 ? mock_status : 200;
    ok = true;
  } else if (fetch_url.SchemeIsFile()) {
    base::FilePath fp;  // net path conversion percent-decodes (spaces etc.)
    ok = net::FileURLToFilePath(fetch_url, &fp) &&
         base::ReadFileToString(fp, &contents);
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
    std::string req_headers = extra_headers_;
    for (const auto& kv : request->headers.GetHeaderVector()) {
      if (ToLower(kv.key) == "content-type") {
        post_ct = kv.value;
        continue;
      }
      if (!req_headers.empty())
        req_headers += "\n";
      req_headers += kv.key + ": " + kv.value;
    }
    // (Per-URL injected headers from mbSetRequestHeader are applied inside FetchHttp,
    // the shared http chokepoint, so the top-level navigation gets them too.)
    // Follow redirects MANUALLY (curl auto-follow off) so each hop is reported to
    // the client via WillFollowRedirect — that updates Blink's url_list_, making
    // fetch's response.url (the final URL) and response.redirected correct. (You
    // can't just rewrite the final response URL: fetch_manager DCHECKs that it
    // equals url_list_.back(), which only the redirect reports advance.)
    std::string cur = fetch_url.spec();
    std::string cur_method = request->method;
    std::string cur_body = post_body;
    for (int hop = 0; hop <= 20; ++hop) {
      contents.clear();
      http_content_type.clear();
      resp_headers.clear();
      http_status = 0;
      ok = FetchHttp(cur, &contents, &http_content_type, user_agent_, req_headers,
                     cur_body, post_ct, cur_method, &http_status, &resp_headers,
                     &final_url, /*follow_redirects=*/false);
      if (!ok || http_status < 300 || http_status >= 400)
        break;  // transport error or a final (non-3xx) response
      const std::string loc = HeaderValueFromBlock(resp_headers, "location");
      if (loc.empty())
        break;  // 3xx without Location -> treat as the final response
      const GURL next = GURL(cur).Resolve(loc);
      if (!next.is_valid() || hop == 20) {
        ok = false;
        break;
      }
      // Method rewrite: 303 (and POST on 301/302) becomes a bodyless GET.
      std::string new_method = cur_method;
      if (http_status == 303 ||
          ((http_status == 301 || http_status == 302) && cur_method == "POST")) {
        new_method = "GET";
        cur_body.clear();
      }
      blink::WebURLResponse redirect_response;
      redirect_response.SetCurrentRequestUrl(ToWebURL(GURL(cur)));
      redirect_response.SetHttpStatusCode(http_status);
      bool report_raw_headers = false;
      std::vector<std::string> removed_headers;
      net::HttpRequestHeaders modified_headers;
      if (!client_->WillFollowRedirect(
              ToWebURL(next), request->site_for_cookies, blink::WebString(),
              network::mojom::ReferrerPolicy::kDefault,
              blink::WebString::FromUtf8(new_method), redirect_response,
              report_raw_headers, &removed_headers, modified_headers,
              /*insecure_scheme_was_upgraded=*/false)) {
        ok = false;
        break;  // client declined the redirect
      }
      cur = next.spec();
      cur_method = new_method;
    }
  } else if (fetch_url.SchemeIs("data")) {
    // Decode the data: URL in-process (libcurl doesn't serve it); the parsed
    // mime flows into the response Content-Type below via http_content_type.
    std::string charset;
    ok = net::DataURL::Parse(fetch_url, &http_content_type, &charset, &contents);
  }
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

  // Response hook: let an embedder inspect the headers / rewrite the body before delivery
  // (given the page's original URL + status + raw response header block). Any replacement
  // flows into the mime/header/content-length build below (SetExpectedContentLength uses
  // contents.size()). resp_headers is the final response's header lines (empty for non-http).
  MbInvokeResponseHook(url.spec(), &http_status, &resp_headers, &contents);

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
  // After manual redirect following, the response URL is the final URL — which
  // now equals Blink's url_list_.back() (advanced by the WillFollowRedirect
  // calls above), so fetch's response.url/redirected are correct and the
  // fetch_manager URL DCHECK holds.
  response.SetCurrentRequestUrl(
      (!final_url.empty() && final_url != url.spec()) ? ToWebURL(GURL(final_url))
                                                      : ToWebURL(url));
  response.SetMimeType(blink::WebString::FromUtf8(mime_str));
  // Expose the server's response headers to JS (fetch Response.headers.get(),
  // XHR getResponseHeader) — set them before Content-Type so the explicit one
  // below wins, and skip headers Blink derives from the delivered bytes.
  if (!resp_headers.empty()) {
    std::string hline;
    std::string hbuf = resp_headers;
    hbuf.push_back('\n');  // flush sentinel
    for (char c : hbuf) {
      if (c == '\n' || c == '\r') {
        std::string::size_type colon = hline.find(':');
        if (colon != std::string::npos && hline.rfind("HTTP/", 0) != 0) {
          std::string name = hline.substr(0, colon);
          std::string value = hline.substr(colon + 1);
          while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.erase(value.begin());
          const std::string lname = ToLower(name);
          if (lname != "content-length" && lname != "transfer-encoding") {
            response.SetHttpHeaderField(blink::WebString::FromUtf8(name),
                                        blink::WebString::FromUtf8(value));
          }
        }
        hline.clear();
      } else {
        hline.push_back(c);
      }
    }
  }
  // Stylesheets validate the Content-Type *header* (CSSStyleSheetResource::CanUseSheet).
  response.SetHttpHeaderField(blink::WebString::FromUtf8("Content-Type"),
                              blink::WebString::FromUtf8(content_type_header));
  // Real HTTP status (so fetch sees 404/500 + response.ok); file/data -> 200.
  response.SetHttpStatusCode(http_status > 0 ? http_status : 200);
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
                           const std::string& report_url) {
  if (!client_)
    return;
  // Synthesize the response head: EventSource only requires a 2xx + a
  // text/event-stream Content-Type to begin reading the stream.
  blink::WebURLResponse response;
  response.SetCurrentRequestUrl(ToWebURL(GURL(report_url)));
  response.SetMimeType(blink::WebString::FromUtf8("text/event-stream"));
  response.SetHttpStatusCode(200);
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
  client_->DidReceiveResponse(response, std::move(consumer), std::nullopt);
  sse_producer_ = std::move(producer);
  sse_watcher_ = std::make_unique<mojo::SimpleWatcher>(
      FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::MANUAL);
  sse_watcher_->Watch(
      sse_producer_.get(), MOJO_HANDLE_SIGNAL_WRITABLE,
      base::BindRepeating(&MbURLLoader::DrainSse, weak_factory_.GetWeakPtr()));

  auto runner = base::SingleThreadTaskRunner::GetCurrentDefault();
  sse_stream_ = std::make_shared<MbSseStream>(
      fetch_url, req_headers, user_agent_,
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
