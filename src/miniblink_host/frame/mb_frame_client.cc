// mb_frame_client.cc — blink::WebLocalFrameClient. Status: Phase 2.
#include "miniblink_host/frame/mb_frame_client.h"

#include <memory>

#include "miniblink_host/loader/mb_url_loader.h"

namespace mb {
MbFrameClient::MbFrameClient(MbWebView* owner) : owner_(owner) {}
MbFrameClient::~MbFrameClient() = default;

std::unique_ptr<blink::URLLoader> MbFrameClient::CreateURLLoaderForTesting() {
  // Subresources use the same UA the frame reports, so the network and DOM agree.
  return std::make_unique<MbURLLoader>(
      user_agent_.empty() ? MbDefaultUserAgent() : user_agent_);
}

blink::WebString MbFrameClient::UserAgentOverride() {
  return blink::WebString::FromUtf8(
      user_agent_.empty() ? MbDefaultUserAgent() : user_agent_);
}
// TODO(mb): DidStopLoading/DidMeaningfulLayout -> notify owner to paint;
// CreateChildFrame -> nullptr.
}  // namespace mb
