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

namespace mb {

// Record / forget a frame's current origin (a SecurityOrigin::ToString(), e.g.
// "https://example.com"; opaque origins serialize to "null").
void MbSetFrameOrigin(uint64_t frame_key, const std::string& origin);
void MbClearFrameOrigin(uint64_t frame_key);

// The frame's last-recorded origin, or "" if unknown (no commit yet, or a frame
// that doesn't publish its origin — e.g. a worker bound without a frame_key).
std::string MbGetFrameOrigin(uint64_t frame_key);

}  // namespace mb

#endif  // MINIBLINK_HOST_FRAME_MB_FRAME_ORIGIN_H_
