// mb_broadcast_channel — an in-process BroadcastChannel backend for windows.
//
// A window's BroadcastChannel does NOT use the BrowserInterfaceBroker; it requests an
// ASSOCIATED mojom::BroadcastChannelProvider from the frame's navigation-associated
// interface provider (LocalFrame::GetRemoteNavigationAssociatedInterfaces, backed by the
// browser's RenderFrameHost). We have no browser, so MbFrameClient returns a LOCAL
// AssociatedInterfaceProvider and installs this binder, which serves the provider in
// process: messages posted on one channel are delivered to every other channel registered
// under the same name (a process-wide registry), per the BroadcastChannel spec.
//
// Scope: same-thread (window) channels — the common case. Worker BroadcastChannels (which
// go through the broker on the worker thread) are not yet wired.

#ifndef MINIBLINK_HOST_FRAME_MB_BROADCAST_CHANNEL_H_
#define MINIBLINK_HOST_FRAME_MB_BROADCAST_CHANNEL_H_

#include <cstdint>

#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/scoped_interface_endpoint_handle.h"
#include "third_party/blink/public/mojom/broadcastchannel/broadcast_channel.mojom-blink.h"

namespace mb {

// Bind an in-process BroadcastChannelProvider to an ASSOCIATED-interface endpoint handle.
// Called from the frame's navigation-associated interface provider (window path) when blink
// requests BroadcastChannelProvider::Name_. `frame_key` identifies the binding frame so the
// provider's channels can be scoped to the frame's origin (cross-origin isolation).
void BindBroadcastChannelProvider(mojo::ScopedInterfaceEndpointHandle handle,
                                  uint64_t frame_key);

// Bind one to a regular (non-associated) pipe receiver. Called from the BrowserInterface
// Broker for the WORKER path (a worker's BroadcastChannel asks its broker). Both paths share
// one process-wide registry (both run on the service thread), so window and worker channels
// of the same name interoperate.
void BindBroadcastChannelProviderPipe(
    mojo::PendingReceiver<blink::mojom::blink::BroadcastChannelProvider> receiver,
    uint64_t frame_key);

}  // namespace mb

#endif  // MINIBLINK_HOST_FRAME_MB_BROADCAST_CHANNEL_H_
