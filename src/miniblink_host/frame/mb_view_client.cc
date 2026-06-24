// mb_view_client.cc — minimal blink::WebViewClient. Status: Phase 1 stub.
#include "miniblink_host/frame/mb_view_client.h"

namespace mb {
MbViewClient::MbViewClient() = default;
MbViewClient::~MbViewClient() = default;
// No CreateView override: in M150 the popup/new-window factory has migrated off
// WebViewClient, and the default already denies popups for this host —
// window.open() returns null and a target=_blank activation is a safe no-op, so
// nothing here can crash the single-process embedder. Verified by mb_smoke's
// "popup safety" case; revisit only if we ever need to surface popups.
}  // namespace mb
