// mb_url_loader — a minimal blink::URLLoader backing subresource loads from disk.
//
// Returned by MbFrameClient::CreateURLLoaderForTesting(), which the PRODUCTION loader
// path calls (loader_factory_for_frame.cc:151) — so this handles real external
// subresources (CSS/JS/images referenced by a page), not just the top-level doc.
//
// Body is delivered via SegmentedBuffer (the std::variant alternative to a Mojo data
// pipe in URLLoaderClient::DidReceiveResponse) — no data-pipe plumbing.
//
// Today: reads file:// (and file-relative) URLs from disk. http(s) -> DidFail until the
// libcurl backend lands. Modeled on third_party/blink/renderer/platform/testing/url_loader_mock.cc.

#ifndef MINIBLINK_HOST_LOADER_MB_URL_LOADER_H_
#define MINIBLINK_HOST_LOADER_MB_URL_LOADER_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "base/memory/weak_ptr.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/simple_watcher.h"
#include "third_party/blink/renderer/platform/loader/fetch/url_loader/url_loader.h"

namespace network {
struct ResourceRequest;
}
namespace mojo {
class DataPipeProducer;
}

namespace mb {

// A long-lived streaming HTTP read over libcurl on a worker thread (for Event-
// Source / Server-Sent Events). Defined in the .cc; the loader holds it by
// shared_ptr so it outlives the loader if the worker is mid-call during teardown.
class MbSseStream;

// The default User-Agent — a realistic modern desktop string so UA-sniffing sites
// serve their current desktop experience (the base Platform::UserAgent() is empty).
const std::string& MbDefaultUserAgent();

// Add a cookie (a "name=value[; attrs]" string, as from document.cookie) to the
// shared HTTP cookie jar for `url`'s origin, so JS-set cookies are sent on later
// network requests. No-op for non-http(s) URLs. Bridges document.cookie -> fetches.
void MbAddCookieToJar(const std::string& url, const std::string& cookie);

// Read the shared HTTP jar's non-HttpOnly cookies for `url`'s host as a
// "name=value; name2=value2" string. Empty for non-http(s). For session extraction.
std::string MbGetCookiesForUrl(const std::string& url);

// Erase all cookies from the shared HTTP jar (e.g. to reset a session).
void MbClearCookieJar();

// Save the WHOLE shared cookie jar (every host, session + persistent) to `path`
// as a Netscape cookie file, and load it back. For session persistence across
// process runs — log in once, reuse the jar next run. Save returns false if the
// file can't be written; Load returns false if `path` is missing/unreadable.
// (Netscape format = curl's native --cookie-jar format, so it interoperates.)
bool MbSaveCookies(const std::string& path);
bool MbLoadCookies(const std::string& path);

// Snapshot the WHOLE shared cookie jar (every host, session + persistent) as a
// Netscape cookie file in memory — the same content MbSaveCookies writes, but
// returned as a string for in-memory session export (DB, network) with no temp
// file. Empty if the cookie engine is unavailable.
std::string MbGetAllCookies();

// Process-wide request log: every subresource URL the loader fetches (img, css,
// fetch/XHR, etc.) is appended via MbRecordRequest. MbGetRequestLog returns the
// URLs newline-separated, oldest first; MbClearRequestLog empties it. The log is
// capped (oldest entries dropped past the cap) so a long-lived process can't grow
// it without bound. Single-threaded (main-thread loader), so no locking.
void MbRecordRequest(const std::string& url);
std::string MbGetRequestLog();
void MbClearRequestLog();
// Number of requests recorded so far (monotone until cleared) — the signal a
// network-idle wait polls: when it stops increasing, the page has gone quiet.
size_t MbRequestCount();

// Process-wide request blocking: any fetched URL containing a registered
// substring is failed (ERR_BLOCKED_BY_CLIENT) instead of loaded — block ads /
// trackers / images / analytics for faster, cleaner scrapes. MbBlockUrl adds a
// substring; MbClearUrlBlocks removes all; MbIsUrlBlocked is the loader's check.
void MbBlockUrl(const std::string& substring);
void MbClearUrlBlocks();
bool MbIsUrlBlocked(const std::string& url);

// Block by RESOURCE TYPE (the fetch destination string: "image", "font", "style",
// "script", "media"/"audio"/"video", "iframe", ...). A scrape can skip whole classes of
// heavy resources (e.g. block "image" + "font" + "media") to load text-only pages fast,
// without listing URLs. MbSetResourceTypeBlocked toggles a type; MbIsResourceTypeBlocked
// is the loader's check (against network::RequestDestinationToString of each request).
void MbSetResourceTypeBlocked(const std::string& type, bool blocked);
bool MbIsResourceTypeBlocked(const std::string& type);

// Dynamic per-request hook: a process-wide callback consulted for EVERY request
// (alongside the static block/mock/rewrite tables), so an embedder can inspect the
// request — URL, method, request headers, and POST/PUT body — and decide at runtime
// whether to allow it. Returns nonzero to BLOCK (failed like MbBlockUrl), zero to allow.
// {} clears it. MbRequestHookBlocks invokes it. Runs on the main thread inside the load.
// (headers is the "\n"-joined request header lines; body is the raw upload bytes, empty
// for GET.)
using MbRequestHook =
    std::function<int(const std::string& url, const std::string& method,
                      const std::string& headers, const std::string& body)>;
void MbSetRequestHook(MbRequestHook hook);
bool MbRequestHookBlocks(const std::string& url, const std::string& method,
                         const std::string& headers, const std::string& body);

// Response hook: a process-wide callback invoked after a successful fetch/mock/file/data
// load with the request URL, HTTP status, the raw response HEADER block (final response's
// header lines, empty for non-http), and a MUTABLE body pointer — so an embedder can
// inspect headers (Content-Type, Set-Cookie, rate-limit, custom API headers) and/or
// REPLACE the response bytes before they reach the page. Runs on the main thread inside the
// load; replacing the body updates the delivered Content-Length.
// `status` is MUTABLE (an embedder may rewrite the delivered HTTP status — e.g. force a
// 503 to exercise a page's retry path, or normalize an upstream 500 to 200) alongside the
// body; the loader uses the (possibly modified) status for the response it delivers.
// `headers` is the raw response header block and is MUTABLE too — an embedder may inject or
// override header lines (e.g. add a CORS header, set a custom field). For SUBRESOURCE /
// fetch / XHR loads the modified block is re-parsed onto the delivered response, so the
// page's fetch Response.headers / XHR getResponseHeader see the changes.
using MbResponseHook = std::function<void(const std::string& url, int* status,
                                          std::string* headers,
                                          std::string* body)>;
void MbSetResponseHook(MbResponseHook hook);
void MbInvokeResponseHook(const std::string& url, int* status,
                          std::string* headers, std::string* body);

// Response mocking: serve a canned body for any request whose URL contains
// `substring`, WITHOUT a real fetch (run offline, substitute an API response).
// content_type defaults to text/html, status to 200. MbAddMock registers one
// (last matching entry wins); MbClearMocks removes all; MbFindMock is the loader's
// lookup. Process-wide, like the blocklist.
void MbAddMock(const std::string& substring, const std::string& body,
               const std::string& content_type, int status);
void MbClearMocks();
bool MbFindMock(const std::string& url, std::string* body,
                std::string* content_type, int* status);

// Dynamic request mock: consulted by MbFindMock when no static mock matches. The hook may
// COMPUTE a response for any URL (one the caller can't pre-register) and fill body/
// content_type/status; return true to serve it WITHOUT a real fetch, false to fetch
// normally. {} clears it. Process-wide.
using MbRequestMockHook = std::function<bool(const std::string& url, std::string* body,
                                             std::string* content_type, int* status)>;
void MbSetRequestMockHook(MbRequestMockHook hook);

// Request URL rewriting: before any fetch, replace the first occurrence of `from`
// with `to` in the request URL (host swap, scheme upgrade, CDN -> local mock). The
// rewrite is transparent — the page still sees its original URL as the response
// URL. MbAddUrlRewrite registers one (applied in registration order);
// MbClearUrlRewrites removes all; MbApplyUrlRewrites is the loader's transform.
void MbAddUrlRewrite(const std::string& from, const std::string& to);
void MbClearUrlRewrites();
std::string MbApplyUrlRewrites(const std::string& url);

// Per-URL request header injection: add/override an outgoing http(s) header for any
// request whose URL contains `substring` (e.g. send an Authorization / API-key header
// only to its own host, not every origin; or set a per-domain UA). Conditional on the
// URL, unlike the global extra-headers. MbAddRequestHeader registers one; MbClearRequest-
// Headers removes all; MbApplyRequestHeaders appends matches to the loader's header block.
void MbAddRequestHeader(const std::string& substring, const std::string& name,
                        const std::string& value);
void MbClearRequestHeaders();
void MbApplyRequestHeaders(const std::string& url, std::string* req_headers);

// Process-wide HTTP(S) proxy for all network fetches, as a libcurl proxy string:
// "http://host:port", "socks5://host:port", "host:port" (defaults to http), or
// "" to force a direct connection (overriding any *_proxy env vars). Once set
// (even to ""), the choice is applied to every request; if never set, libcurl's
// default proxy resolution (env vars) is honored. Affects http(s) only — file://
// and data: are served directly.
void MbSetProxy(const std::string& proxy);
// If a proxy was explicitly set, copies it into *out and returns true; otherwise
// returns false (honor libcurl defaults). Used by the fetch path.
bool MbProxyConfigured(std::string* out);

// Disable (true) or enable (false, the default) TLS peer/host certificate
// verification for all network fetches — the equivalent of curl -k. Process-wide.
// For scraping/testing sites with self-signed, expired, or otherwise invalid
// certs. MbIgnoreCertErrors() is read on the fetch path.
void MbSetIgnoreCertErrors(bool ignore);
bool MbIgnoreCertErrors();

// Follow HTTP 3xx redirects (true, the default) or stop at the redirect response
// (false), so the caller can read the 30x status + Location header itself — for
// resolving URL shorteners or inspecting a redirect without following it.
// Process-wide. MbFollowRedirects() is read on the fetch path.
void MbSetFollowRedirects(bool follow);
bool MbFollowRedirects();

// The fetch-retry decision lives in mb_retry_policy.h (a dependency-free header so
// the smoke tests can include it without the loader's heavy base/blink deps).

// Fetch a file:// or http(s):// URL into `body` (+ server Content-Type if any).
// Shared by the subresource loader and the top-level navigation in MbWebView::LoadURL.
// `user_agent` sets the HTTP User-Agent (empty -> MbDefaultUserAgent()).
// `extra_headers` are newline-separated "Name: Value" lines added to every request.
// If `post_body` is non-empty (or `http_method` is a non-GET verb) the request
// carries that body with `post_content_type` (default urlencoded) — used for form
// submission and fetch()/XHR with a body. `http_method` (e.g. "POST", "PUT",
// "DELETE") sets the verb; empty means GET, or POST when a body is present. The
// body/method apply only to http(s); file/data ignore them.
// `out_final_url` (optional) receives the URL after any server redirects curl
// followed (http only) — use it as the committed document's base so location and
// relative subresources reflect where we actually landed.
bool MbFetchUrl(const std::string& url_spec, std::string* body,
                std::string* content_type, const std::string& user_agent = "",
                const std::string& extra_headers = "",
                const std::string& post_body = "",
                const std::string& post_content_type = "",
                const std::string& http_method = "",
                std::string* out_final_url = nullptr,
                int* out_status = nullptr,
                std::string* out_headers = nullptr,
                std::string* out_error = nullptr);

class MbURLLoader : public blink::URLLoader {
 public:
  // `user_agent` is sent on every subresource request (empty -> default);
  // `extra_headers` are newline-separated "Name: Value" lines added to each.
  explicit MbURLLoader(std::string user_agent = "", std::string extra_headers = "");
  ~MbURLLoader() override;

  // blink::URLLoader:
  void LoadAsynchronously(
      std::unique_ptr<network::ResourceRequest> request,
      scoped_refptr<const blink::SecurityOrigin> top_frame_origin,
      bool no_mime_sniffing,
      std::unique_ptr<blink::ResourceLoadInfoNotifierWrapper>
          resource_load_info_notifier_wrapper,
      blink::CodeCacheHost* code_cache_host,
      blink::URLLoaderClient* client) override;
  void Freeze(blink::LoaderFreezeMode) override {}
  void DidChangePriority(blink::WebURLRequest::Priority, int) override {}
  // The body-loader task runner backs DataPipeBytesConsumer's SimpleWatcher; the default
  // URLLoader ctor leaves it null, so provide the current (main-thread) runner.
  scoped_refptr<base::SingleThreadTaskRunner> GetTaskRunnerForBodyLoader() override;

 private:
  // Posted task: read the resource and push response+body+finish to the client.
  void Deliver(std::unique_ptr<network::ResourceRequest> request);
  void OnBodyWritten(int64_t length, uint32_t /*MojoResult*/ result);

  // EventSource / SSE streaming path (Accept: text/event-stream): deliver a
  // text/event-stream response head, then stream the body INCREMENTALLY from a
  // worker thread so the page sees events as they arrive (the buffered path would
  // hang on a never-closing stream). OnSseChunk/OnSseDone hop from the worker.
  void StartSse(const std::string& fetch_url, const std::string& req_headers,
                const std::string& report_url);
  void OnSseChunk(std::string bytes);
  void OnSseDone();
  void DrainSse(MojoResult result = 0);

  blink::URLLoaderClient* client_ = nullptr;  // not owned; valid until done/cancel
  std::string user_agent_;  // sent on each request (empty -> default)
  std::string extra_headers_;  // newline-separated "Name: Value" added per request
  std::string body_;  // owns the bytes while the data pipe drains them
  std::unique_ptr<mojo::DataPipeProducer> data_pipe_producer_;
  // SSE streaming state (main thread): the producer + a watcher draining chunks.
  std::shared_ptr<MbSseStream> sse_stream_;
  mojo::ScopedDataPipeProducerHandle sse_producer_;
  std::unique_ptr<mojo::SimpleWatcher> sse_watcher_;
  std::string sse_buf_;       // incoming chunk bytes awaiting the pipe
  size_t sse_pos_ = 0;        // write offset into sse_buf_
  int64_t sse_total_ = 0;     // total bytes delivered (for DidFinishLoading)
  bool sse_ended_ = false;    // worker reported the stream ended
  base::WeakPtrFactory<MbURLLoader> weak_factory_{this};
};

}  // namespace mb

#endif  // MINIBLINK_HOST_LOADER_MB_URL_LOADER_H_
