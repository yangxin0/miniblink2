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

#include "mojo/public/cpp/bindings/scoped_interface_endpoint_handle.h"

namespace mb {

// Bind an in-process BroadcastChannelProvider to an associated-interface endpoint handle.
// Called from the frame's navigation-associated interface provider (on the service thread)
// when blink requests BroadcastChannelProvider::Name_.
void BindBroadcastChannelProvider(mojo::ScopedInterfaceEndpointHandle handle);

}  // namespace mb

#endif  // MINIBLINK_HOST_FRAME_MB_BROADCAST_CHANNEL_H_
