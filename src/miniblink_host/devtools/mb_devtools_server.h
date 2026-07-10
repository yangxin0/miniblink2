// mb_devtools_server — an OPT-IN, loopback-only Chrome DevTools Protocol
// endpoint hosted by the engine (IMPROVEMENT.md item 41). It serves the
// standard /json discovery documents and a WebSocket per live view that
// bridges to that view's in-process CDP session (the same one
// mbDevToolsAttach uses), so an ordinary Chrome frontend can inspect a view
// with no host-side socket plumbing.
//
// This is the ONLY part of the engine that opens a socket, and it does so only
// when the host explicitly starts it. The bridge to blink runs on the engine
// main thread (all mb_devtools_bridge calls are marshaled there); the socket
// accept/read loops run on dedicated threads. macOS/POSIX only.

#ifndef MINIBLINK_HOST_DEVTOOLS_MB_DEVTOOLS_SERVER_H_
#define MINIBLINK_HOST_DEVTOOLS_MB_DEVTOOLS_SERVER_H_

namespace mb {

// Start the endpoint on 127.0.0.1:<port>. Call on the engine main thread AFTER
// MbRuntime::Initialize. Returns true on success (or true if already running on
// the same port). Idempotent stop/restart is supported. Returns false on
// bind/listen failure or on a non-POSIX platform.
bool MbDevToolsServerStart(int port);

// Stop the endpoint: close all client sockets + the listener and join the
// threads. Safe to call when not running.
void MbDevToolsServerStop();

// The listening port, or 0 when not running.
int MbDevToolsServerPort();

}  // namespace mb

#endif  // MINIBLINK_HOST_DEVTOOLS_MB_DEVTOOLS_SERVER_H_
