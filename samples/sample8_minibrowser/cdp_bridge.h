// cdp_bridge — a loopback CDP endpoint for the minibrowser sample: HTTP
// discovery (/json, /json/list, /json/version) + an RFC-6455 WebSocket that
// bridges one mbDevToolsAttach session per tab to the wire, so ORDINARY
// CHROME attaches as the DevTools frontend (chrome://inspect, or the
// devtools:// URL from /json/list).
//
// This is the inspector plan's Stage B living where it belongs — in the
// embedder (here, the sample), keeping sockets out of the engine. Portable
// C++ (BSD sockets / winsock), a port of the Glyph host's bridge that was
// verified against real Chrome Elements/Console/Sources sessions.
//
// Single client: the newest WebSocket wins. All engine calls hop to the main
// thread via the compat scaffold's MbPostToMain.
#ifndef MB_SAMPLES_MINIBROWSER_CDP_BRIDGE_H_
#define MB_SAMPLES_MINIBROWSER_CDP_BRIDGE_H_

#include <string>

#include "miniblink2/webview.h"

// Start the endpoint on 127.0.0.1:`port`. Returns true (and logs the base
// URL) on success; false if the port can't be bound. Idempotent.
bool MbBridgeStart(int port);

// Make `view` an inspectable target advertised as `title`/`url` in
// /json/list (idempotent; re-registering refreshes the metadata). Attaches
// the engine CDP session immediately so it is warm before a frontend
// connects. Call on the main thread with the engine OFF the stack.
void MbBridgeRegister(mbView* view, const std::string& title,
                      const std::string& url);

// Drop `view` from the target list (call before destroying the view).
void MbBridgeUnregister(mbView* view);

#endif  // MB_SAMPLES_MINIBROWSER_CDP_BRIDGE_H_
