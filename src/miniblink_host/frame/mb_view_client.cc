// mb_view_client.cc — minimal blink::WebViewClient. Status: Phase 1 stub.
#include "miniblink_host/frame/mb_view_client.h"

namespace mb {
MbViewClient::MbViewClient() = default;
MbViewClient::~MbViewClient() = default;
// TODO(mb): override CreateView -> nullptr (deny popups) once signature pinned.
}  // namespace mb
