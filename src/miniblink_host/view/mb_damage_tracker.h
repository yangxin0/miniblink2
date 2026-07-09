// mb_damage_tracker — dirty-RECT damage tracking for the non-composited
// software-paint path (IMPROVEMENT.md item 2 tail).
//
// Blink's paint lifecycle keeps a persistent PaintArtifact per local root and
// only repaints it when something invalidated. This tracker diffs consecutive
// artifacts with blink's own RasterInvalidator — the same machinery cc uses to
// compute per-layer raster invalidations — treating the whole document as ONE
// layer in Root property-tree space, which is the space the software paint
// replays in (PaintOutsideOfLifecycle / GetPaintRecord with Root state). The
// accumulated rect union is the region of the view that can differ from the
// previously painted frame; everything outside it is bit-identical, so a
// damage-gated host blits only the rect.
//
// Timing contract: OnNonCompositedPaint runs inside the paint cycle (the
// LocalFrameView non-composited paint hook, patch 0041) — when the cycle ends,
// PaintController's destructor frees the PREVIOUS artifact's backing store,
// and the invalidator's item-level diff reads that artifact's display items.

#ifndef MINIBLINK_HOST_VIEW_MB_DAMAGE_TRACKER_H_
#define MINIBLINK_HOST_VIEW_MB_DAMAGE_TRACKER_H_

#include "third_party/blink/renderer/platform/graphics/paint/raster_invalidator.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "ui/gfx/geometry/rect.h"

namespace blink {
class LocalFrameView;
}

namespace mb {

class MbDamageTracker : public blink::RasterInvalidator::Callback {
 public:
  MbDamageTracker();
  ~MbDamageTracker() override;
  MbDamageTracker(const MbDamageTracker&) = delete;
  MbDamageTracker& operator=(const MbDamageTracker&) = delete;

  // Diff the root view's just-updated artifact against the previous one and
  // union the raster invalidations into the pending damage. Must run for EVERY
  // non-composited paint cycle of this view: skipping one would leave the
  // invalidator holding an artifact whose backing store the next cycle frees.
  void OnNonCompositedPaint(blink::LocalFrameView& root_view);

  // Host-forced damage (SetDirty, resize, device-scale change — pixel changes
  // blink's invalidation cannot see).
  void AddDamage(const gfx::Rect& rect);

  // The damage accumulated since the last Take, cleared for the next frame.
  // Logical px, viewport origin, NOT clamped to the view bounds.
  gfx::Rect TakeDamage();

 private:
  // blink::RasterInvalidator::Callback
  void InvalidateRect(const gfx::Rect& rect) override;

  blink::Persistent<blink::RasterInvalidator> invalidator_;
  gfx::Rect pending_;
};

}  // namespace mb

#endif  // MINIBLINK_HOST_VIEW_MB_DAMAGE_TRACKER_H_
