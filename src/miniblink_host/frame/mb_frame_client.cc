// mb_frame_client.cc — blink::WebLocalFrameClient. Status: Phase 2.
#include "miniblink_host/frame/mb_frame_client.h"

#include <memory>

#include "miniblink_host/loader/mb_url_loader.h"
#include "miniblink_host/view/mb_webview.h"

namespace mb {
MbFrameClient::MbFrameClient(MbWebView* owner) : owner_(owner) {}
MbFrameClient::~MbFrameClient() = default;

void MbFrameClient::RunScriptsAtDocumentElementAvailable() {
  if (owner_)
    owner_->RunDocumentStartScript();
}

std::unique_ptr<blink::URLLoader> MbFrameClient::CreateURLLoaderForTesting() {
  // Subresources use the same UA + extra headers the top-level fetch does, so the
  // network identity is consistent across the document and its subresources.
  return std::make_unique<MbURLLoader>(
      user_agent_.empty() ? MbDefaultUserAgent() : user_agent_, extra_headers_);
}

blink::WebString MbFrameClient::UserAgentOverride() {
  return blink::WebString::FromUtf8(
      user_agent_.empty() ? MbDefaultUserAgent() : user_agent_);
}

void MbFrameClient::DidAddMessageToConsole(const blink::WebConsoleMessage& msg,
                                           const blink::WebString& /*source*/,
                                           unsigned /*line*/,
                                           const blink::WebString& /*stack*/) {
  const char* level = "log";
  switch (msg.level) {
    case blink::mojom::ConsoleMessageLevel::kVerbose: level = "verbose"; break;
    case blink::mojom::ConsoleMessageLevel::kInfo: level = "log"; break;
    case blink::mojom::ConsoleMessageLevel::kWarning: level = "warn"; break;
    case blink::mojom::ConsoleMessageLevel::kError: level = "error"; break;
  }
  console_.push_back(std::string(level) + ": " + msg.text.Utf8());
}

std::string MbFrameClient::DrainConsole() {
  std::string out;
  for (const auto& line : console_) {
    out += line;
    out += '\n';
  }
  console_.clear();
  return out;
}
// TODO(mb): DidStopLoading/DidMeaningfulLayout -> notify owner to paint;
// CreateChildFrame -> nullptr.
}  // namespace mb
