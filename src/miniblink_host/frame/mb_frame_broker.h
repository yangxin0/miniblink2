// mb_frame_broker — the in-process BrowserInterfaceBroker handed to the main frame.
//
// The frame resolves a few browser-process Mojo services through its
// BrowserInterfaceBroker (distinct from Platform's broker). We only need one:
// RestrictedCookieManager, so window.document.cookie reads/writes work. Everything
// else is dropped (no browser process).
//
// The returned remote's receiver is self-owned (kept alive by the live pipe), so the
// frame can hold the remote for its lifetime.
#ifndef MINIBLINK_HOST_FRAME_MB_FRAME_BROKER_H_
#define MINIBLINK_HOST_FRAME_MB_FRAME_BROKER_H_

#include "mojo/public/cpp/bindings/pending_remote.h"
#include "third_party/blink/public/mojom/browser_interface_broker.mojom-blink.h"

namespace mb {

mojo::PendingRemote<blink::mojom::blink::BrowserInterfaceBroker>
MakeFrameInterfaceBroker();

}  // namespace mb

#endif  // MINIBLINK_HOST_FRAME_MB_FRAME_BROKER_H_
