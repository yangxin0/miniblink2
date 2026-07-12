// mb_frame_origin — a tiny process-wide map from a per-frame id (the frame_key
// minted by MbFrameClient) to that frame's CURRENT document origin, updated on
// each commit. It lets origin-agnostic, frame-keyed services (e.g. the in-process
// BroadcastChannel) recover a frame's origin to scope cross-origin isolation —
// the document origin isn't carried in the relevant mojom and the broker is bound
// origin-agnostically. Lock-protected: written on the main thread (commit), read
// on the service thread (service fan-out).

#ifndef MINIBLINK_HOST_FRAME_MB_FRAME_ORIGIN_H_
#define MINIBLINK_HOST_FRAME_MB_FRAME_ORIGIN_H_

#include <cstdint>
#include <string>

#include "third_party/blink/public/common/tokens/tokens.h"

namespace mb {

// Record / forget a frame's current origin (a SecurityOrigin::ToString(), e.g.
// "https://example.com"; opaque origins serialize to "null").
void MbSetFrameOrigin(uint64_t frame_key, const std::string& origin);
void MbClearFrameOrigin(uint64_t frame_key);

// Associate blink's LocalFrameToken (carried by DOM Storage requests) with our
// frame_key. The token is stable for the frame lifetime; MbClearFrameOrigin
// removes this association along with the committed scope.
void MbSetFrameToken(uint64_t frame_key,
                     const blink::LocalFrameToken& frame_token);

// Resolve the session-prefixed committed scope for a DOM Storage caller. Empty
// means the token is unknown or the frame has not committed yet.
std::string MbGetFrameScopeForToken(
    const blink::LocalFrameToken& frame_token);

// The frame's last-recorded origin, or "" if unknown (no commit yet, or a frame
// that doesn't publish its origin — e.g. a worker bound without a frame_key).
std::string MbGetFrameOrigin(uint64_t frame_key);

// Allocate a synthetic frame_key for a WORKER, in a high range disjoint from the
// small sequential keys MbFrameClient mints for windows — so a worker can publish
// its origin (MbSetFrameOrigin) and have origin-scoped services (IndexedDB) treat
// it like a frame, WITHOUT colliding with any window's key. Each worker gets its
// own key (so different-origin workers don't clobber each other's origin entry).
uint64_t MbAllocWorkerFrameKey();

}  // namespace mb

#endif  // MINIBLINK_HOST_FRAME_MB_FRAME_ORIGIN_H_
