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
#include <memory>
#include <string>

#include "base/memory/weak_ptr.h"
#include "third_party/blink/renderer/platform/loader/fetch/url_loader/url_loader.h"

namespace network {
struct ResourceRequest;
}
namespace mojo {
class DataPipeProducer;
}

namespace mb {

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
                std::string* out_headers = nullptr);

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

  blink::URLLoaderClient* client_ = nullptr;  // not owned; valid until done/cancel
  std::string user_agent_;  // sent on each request (empty -> default)
  std::string extra_headers_;  // newline-separated "Name: Value" added per request
  std::string body_;  // owns the bytes while the data pipe drains them
  std::unique_ptr<mojo::DataPipeProducer> data_pipe_producer_;
  base::WeakPtrFactory<MbURLLoader> weak_factory_{this};
};

}  // namespace mb

#endif  // MINIBLINK_HOST_LOADER_MB_URL_LOADER_H_
