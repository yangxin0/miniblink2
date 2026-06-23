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

  // TODO(mb): DidStopLoading/DidMeaningfulLayout (paint signal), CreateChildFrame.

 private:
  [[maybe_unused]] MbWebView* owner_;  // not owned (used once handshake bodies land)
};

}  // namespace mb

#endif  // MINIBLINK_HOST_FRAME_MB_FRAME_CLIENT_H_
