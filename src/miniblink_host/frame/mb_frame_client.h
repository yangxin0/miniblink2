// mb_frame_client — real blink::WebLocalFrameClient for miniblink-modern.
//
// Modeled on TestWebFrameClient (vendor/reference/frame_test_helpers.h:516). The
// minimal override set to load + render a main frame, with real behavior:
//   - CreateURLLoader        -> hand back an MbURLLoader (libcurl). THE network hook.
//   - BeginNavigation        -> drive the load (P1: commit synchronously/simply).
//   - DidStartLoading/DidStopLoading, DidMeaningfulLayout -> know when to paint.
//   - CreateChildFrame       -> subframes (can return null in P1).
//   - FrameDetached, SwapIn  -> lifecycle.
//
// Status: Phase 1 scaffold. Exact signatures pinned during .cc compile vs M150
// third_party/blink/public/web/web_local_frame_client.h.

#ifndef MINIBLINK_HOST_FRAME_MB_FRAME_CLIENT_H_
#define MINIBLINK_HOST_FRAME_MB_FRAME_CLIENT_H_

#include <memory>
#include <string>
#include <vector>

#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/web/web_console_message.h"
#include "third_party/blink/public/web/web_local_frame_client.h"

namespace mb {

class MbWebView;  // owner / callback sink

class MbFrameClient : public blink::WebLocalFrameClient {
 public:
  explicit MbFrameClient(MbWebView* owner);
  ~MbFrameClient() override;

  // The production loader path calls this (loader_factory_for_frame.cc:151); returning
  // a non-null loader makes Blink use it for subresources. -> our file-backed loader.
  std::unique_ptr<blink::URLLoader> CreateURLLoaderForTesting() override;

  // navigator.userAgent + the value sent on every request. Blink calls this when
  // non-empty (else Platform::UserAgent(), which is empty here), so we always
  // return a real UA. Set before navigating to take effect for that load.
  blink::WebString UserAgentOverride() override;

  void SetUserAgent(const std::string& ua) { user_agent_ = ua; }
  const std::string& user_agent() const { return user_agent_; }

  void SetExtraHeaders(const std::string& h) { extra_headers_ = h; }
  const std::string& extra_headers() const { return extra_headers_; }

  // Capture page console output (console.log/warn/error) so a host or automation
  // script can read it back. Each entry is "level: text".
  void DidAddMessageToConsole(const blink::WebConsoleMessage&,
                              const blink::WebString& source_name,
                              unsigned source_line,
                              const blink::WebString& stack_trace) override;
  // Return all captured console lines joined by '\n' and clear the buffer.
  std::string DrainConsole();

  // TODO(mb): DidStopLoading/DidMeaningfulLayout (paint signal), CreateChildFrame.

 private:
  [[maybe_unused]] MbWebView* owner_;  // not owned (used once handshake bodies land)
  std::string user_agent_;  // empty -> MbDefaultUserAgent() (resolved at use)
  std::string extra_headers_;  // newline-separated "Name: Value" request headers
  std::vector<std::string> console_;  // captured console messages
};

}  // namespace mb

#endif  // MINIBLINK_HOST_FRAME_MB_FRAME_CLIENT_H_
