// mb_view_client — minimal blink::WebViewClient for miniblink-modern.
//
// WebViewClient is a small interface (most of the old surface migrated to other
// clients/Mojo). For an offscreen single-view host the needed overrides are few:
// new-window/popup creation (P1: return nullptr / deny), and a handful of
// notifications. Pinned against public/web/web_view_client.h during .cc compile.
//
// Status: Phase 1 scaffold.

#ifndef MINIBLINK_HOST_FRAME_MB_VIEW_CLIENT_H_
#define MINIBLINK_HOST_FRAME_MB_VIEW_CLIENT_H_

#include "third_party/blink/public/web/web_view_client.h"

namespace mb {

class MbViewClient : public blink::WebViewClient {
 public:
  MbViewClient();
  ~MbViewClient() override;

  // No popup/new-window override needed: in M150 the CreateView factory has
  // migrated off WebViewClient and the default denies popups (window.open() ->
  // null), so an untrusted page can't spawn an unmanaged view. Other overrides
  // are added only when Blink actually calls them. (See the .cc + mb_smoke's
  // "popup safety" case.)
};

}  // namespace mb

#endif  // MINIBLINK_HOST_FRAME_MB_VIEW_CLIENT_H_
