// mb_websocket — an in-process WebSocket backend.
//
// blink requests a mojom::WebSocketConnector from the frame's BrowserInterfaceBroker.
// Without one, `new WebSocket(url)` never opens. This connector establishes the connection
// (firing onopen) and runs a LOOPBACK ECHO data plane: a message the page sends is delivered
// straight back to it (onmessage). That exercises the whole WebSocket mojo path — handshake,
// SendMessage framing over the writable pipe, OnDataFrame over the readable pipe — entirely
// in-process and offline. (A real network backend over libcurl's WebSocket support can later
// replace the echo; the plumbing is identical.)

#ifndef MINIBLINK_HOST_FRAME_MB_WEBSOCKET_H_
#define MINIBLINK_HOST_FRAME_MB_WEBSOCKET_H_

#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "third_party/blink/public/mojom/websockets/websocket_connector.mojom-blink.h"

namespace mb {

// Bind a WebSocketConnector receiver to the in-process backend (self-owned). Bound on the
// broker's service thread.
void BindWebSocketConnector(
    mojo::PendingReceiver<blink::mojom::blink::WebSocketConnector> receiver);

}  // namespace mb

#endif  // MINIBLINK_HOST_FRAME_MB_WEBSOCKET_H_
