// mb_url_loader.cc — file-backed blink::URLLoader. Status: Phase 2 (subresources).
#include "miniblink_host/loader/mb_url_loader.h"

#include <curl/curl.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "net/base/data_url.h"
#include "net/base/filename_util.h"
#include "net/base/net_errors.h"
#include "net/http/http_request_headers.h"
#include "services/network/public/mojom/referrer_policy.mojom-shared.h"
#include "services/network/public/cpp/data_element.h"
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
               bool follow_redirects = true) {
  CURL* curl = curl_easy_init();
  if (!curl)
    return false;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
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
  return rc == CURLE_OK && http_code > 0;
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

bool MbFetchUrl(const std::string& url_spec, std::string* body,
                std::string* content_type, const std::string& user_agent,
                const std::string& extra_headers, const std::string& post_body,
                const std::string& post_content_type,
                const std::string& http_method, std::string* out_final_url) {
  GURL url(url_spec);
  if (url.SchemeIsFile()) {
    // Convert via net (percent-decodes the path; "Andale%20Mono.ttf" -> a space)
    // — a raw url.path() leaves it encoded and ReadFileToString fails.
    base::FilePath fp;
    return net::FileURLToFilePath(url, &fp) &&
           base::ReadFileToString(fp, body);
  }
  if (url.SchemeIsHTTPOrHTTPS())
    return FetchHttp(url_spec, body, content_type, user_agent, extra_headers,
                     post_body, post_content_type, http_method,
                     /*out_status=*/nullptr, /*out_headers=*/nullptr,
                     out_final_url);
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

MbURLLoader::MbURLLoader(std::string user_agent, std::string extra_headers)
    : user_agent_(std::move(user_agent)),
      extra_headers_(std::move(extra_headers)) {}
MbURLLoader::~MbURLLoader() = default;

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
  const GURL& url = request->url;

  std::string contents;
  std::string http_content_type;  // from the server, may be "text/html; charset=..."
  int http_status = 0;            // real HTTP status (0 -> treat as 200)
  std::string resp_headers;       // raw final-response header block (http only)
  std::string final_url;          // URL after manual redirect following (http)
  bool ok = false;
  if (url.SchemeIsFile()) {
    base::FilePath fp;  // net path conversion percent-decodes (spaces etc.)
    ok = net::FileURLToFilePath(url, &fp) &&
         base::ReadFileToString(fp, &contents);
  } else if (url.SchemeIsHTTPOrHTTPS()) {
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
    // Follow redirects MANUALLY (curl auto-follow off) so each hop is reported to
    // the client via WillFollowRedirect — that updates Blink's url_list_, making
    // fetch's response.url (the final URL) and response.redirected correct. (You
    // can't just rewrite the final response URL: fetch_manager DCHECKs that it
    // equals url_list_.back(), which only the redirect reports advance.)
    std::string cur = url.spec();
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
  } else if (url.SchemeIs("data")) {
    // Decode the data: URL in-process (libcurl doesn't serve it); the parsed
    // mime flows into the response Content-Type below via http_content_type.
    std::string charset;
    ok = net::DataURL::Parse(url, &http_content_type, &charset, &contents);
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

  // Response mime: from the server's Content-Type (http), else the file extension.
  std::string mime_str;
  std::string content_type_header;
  if (!http_content_type.empty()) {
    content_type_header = http_content_type;
    mime_str = http_content_type.substr(0, http_content_type.find(';'));
    while (!mime_str.empty() && mime_str.back() == ' ')
      mime_str.pop_back();
  } else {
    mime_str = MimeFromPath(std::string(url.path())).Utf8();
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

}  // namespace mb
