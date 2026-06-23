// mb_url_loader.cc — file-backed blink::URLLoader. Status: Phase 2 (subresources).
#include "miniblink_host/loader/mb_url_loader.h"

#include <curl/curl.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <variant>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
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

// Synchronous HTTP(S) fetch via the system libcurl (HTTPS via SecureTransport on macOS).
// Blocks the calling task; fine for the headless/synchronous render model. Retries
// transient failures with linear backoff so a single network hiccup (which produced
// indistinguishable blank renders before) doesn't doom the whole page.
bool FetchHttp(const std::string& url, std::string* body, std::string* content_type) {
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
                   "Mozilla/5.0 (Macintosh; ARM Mac OS X) miniblink-modern/1.0");
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");  // allow gzip, auto-decode

  constexpr int kMaxAttempts = 3;
  bool ok = false;
  for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
    body->clear();  // CurlWrite appends; start each attempt fresh
    const CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    ok = rc == CURLE_OK && http_code >= 200 && http_code < 400 && !body->empty();
    const bool retryable =
        !ok && attempt < kMaxAttempts &&
        (IsTransientCurlError(rc) ||
         (rc == CURLE_OK && IsTransientHttpCode(http_code)));
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

bool MbFetchUrl(const std::string& url_spec, std::string* body,
                std::string* content_type) {
  GURL url(url_spec);
  if (url.SchemeIsFile())
    return base::ReadFileToString(base::FilePath(std::string(url.path())), body);
  if (url.SchemeIsHTTPOrHTTPS())
    return FetchHttp(url_spec, body, content_type);
  return false;
}

MbURLLoader::MbURLLoader() = default;
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
    ok = FetchHttp(url.spec(), &contents, &http_content_type);
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
