// mb_frame_client.cc — blink::WebLocalFrameClient. Status: Phase 2.
#include "miniblink_host/frame/mb_frame_client.h"

#include <memory>

#include "miniblink_host/loader/mb_url_loader.h"

namespace mb {
MbFrameClient::MbFrameClient(MbWebView* owner) : owner_(owner) {}
MbFrameClient::~MbFrameClient() = default;

std::unique_ptr<blink::URLLoader> MbFrameClient::CreateURLLoaderForTesting() {
  return std::make_unique<MbURLLoader>();
}
// TODO(mb): DidStopLoading/DidMeaningfulLayout -> notify owner to paint;
// CreateChildFrame -> nullptr.
}  // namespace mb
