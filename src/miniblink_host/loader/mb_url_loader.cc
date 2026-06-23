// mb_url_loader.cc — file-backed blink::URLLoader. Status: Phase 2 (subresources).
#include "miniblink_host/loader/mb_url_loader.h"

#include <curl/curl.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <variant>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "net/base/data_url.h"
#include "net/base/net_errors.h"
#include "services/network/public/cpp/resource_request.h"
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
// behaves like one browsing session. Single-threaded use, so no share lock callbacks.
CURLSH* CookieShare() {
  static CURLSH* share = [] {
    CURLSH* s = curl_share_init();
    if (s)
      curl_share_setopt(s, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
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

bool FetchHttp(const std::string& url, std::string* body, std::string* content_type,
               const std::string& user_agent, const std::string& extra_headers) {
  CURL* curl = curl_easy_init();
  if (!curl)
    return false;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWrite);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
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
  if (header_list)
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);

  constexpr int kMaxAttempts = 3;
  bool ok = false;
  for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
    body->clear();  // CurlWrite appends; start each attempt fresh
    const CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    const bool success_code =
        rc == CURLE_OK && http_code >= 200 && http_code < 400;
    ok = success_code && !body->empty();
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
  curl_easy_cleanup(curl);
  if (header_list)
    curl_slist_free_all(header_list);
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

void MbAddCookieToJar(const std::string& url, const std::string& cookie) {
  GURL gurl(url);
  if (!gurl.SchemeIsHTTPOrHTTPS())
    return;  // cookies only make sense for network origins
  CURL* curl = curl_easy_init();
  if (!curl)
    return;
  // Associate the cookie with this URL's domain/path, share into the global jar.
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");  // enable the cookie engine
  if (CURLSH* share = CookieShare())
    curl_easy_setopt(curl, CURLOPT_SHARE, share);
  const std::string line = "Set-Cookie: " + cookie;
  curl_easy_setopt(curl, CURLOPT_COOKIELIST, line.c_str());
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
                const std::string& extra_headers) {
  GURL url(url_spec);
  if (url.SchemeIsFile())
    return base::ReadFileToString(base::FilePath(std::string(url.path())), body);
  if (url.SchemeIsHTTPOrHTTPS())
    return FetchHttp(url_spec, body, content_type, user_agent, extra_headers);
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
  bool ok = false;
  if (url.SchemeIsFile()) {
    ok = base::ReadFileToString(base::FilePath(std::string(url.path())), &contents);
  } else if (url.SchemeIsHTTPOrHTTPS()) {
    ok = FetchHttp(url.spec(), &contents, &http_content_type, user_agent_,
                   extra_headers_);
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
  response.SetCurrentRequestUrl(ToWebURL(url));
  response.SetMimeType(blink::WebString::FromUtf8(mime_str));
  // Stylesheets validate the Content-Type *header* (CSSStyleSheetResource::CanUseSheet).
  response.SetHttpHeaderField(blink::WebString::FromUtf8("Content-Type"),
                              blink::WebString::FromUtf8(content_type_header));
  response.SetHttpStatusCode(200);
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
