#include "miniblink_host/view/mb_damage_tracker.h"

#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/platform/graphics/paint/paint_artifact.h"
#include "third_party/blink/renderer/platform/graphics/paint/paint_chunk_subset.h"
#include "third_party/blink/renderer/platform/graphics/paint/property_tree_state.h"

namespace mb {

MbDamageTracker::MbDamageTracker()
    : invalidator_(
          blink::MakeGarbageCollected<blink::RasterInvalidator>(*this)) {}

MbDamageTracker::~MbDamageTracker() = default;

void MbDamageTracker::OnNonCompositedPaint(blink::LocalFrameView& root_view) {
  // The whole document as ONE layer at the viewport's size: chunk rects map
  // through the property trees into Root space, the same space the software
  // paint replays in, so invalidation rects line up 1:1 with the replayed
  // pixels (logical px, viewport origin).
  //
  // Generate() also advances the invalidator's old-artifact reference, so it
  // runs even when this cycle did not repaint: a scroll mutates transform
  // nodes in place without a repaint, and re-mapping the (unchanged) chunks
  // is exactly what turns that into damage. The first call takes the
  // empty-old-bounds path and reports the full layer — the first paint is
  // full damage by construction.
  invalidator_->Generate(blink::PaintChunkSubset(root_view.GetPaintArtifact()),
                         gfx::Vector2dF(), root_view.Size(),
                         blink::PropertyTreeState::Root());
}

void MbDamageTracker::AddDamage(const gfx::Rect& rect) {
  pending_.Union(rect);
}

void MbDamageTracker::SkipCycle(const gfx::Rect& full_view) {
  invalidator_->ClearOldStates();
  pending_.Union(full_view);
}

gfx::Rect MbDamageTracker::TakeDamage() {
  gfx::Rect damage = pending_;
  pending_ = gfx::Rect();
  return damage;
}

void MbDamageTracker::InvalidateRect(const gfx::Rect& rect) {
  pending_.Union(rect);
}

}  // namespace mb
