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
  const blink::PaintArtifact& artifact = root_view.GetPaintArtifact();
  // Information short-circuit: damage only ever GROWS toward the full view,
  // and Generate()'s only observable output is damage. Once the pending
  // damage already covers the viewport — a loading document (whose first
  // cycle reports full), wholesale DOM churn, a host SetDirty — running the
  // diff cannot change anything the host will see, so skip the pass,
  // including the per-chunk ChunkToLayerMapper work that dominates Generate
  // when chunks don't match (churn is O(n) with a heavy constant; matched
  // unchanged chunks reuse cached info and are cheap).
  //
  // Two cases keep the baseline sound across a skip:
  //  - The artifact is the SAME one the baseline anchors to (a paint's second
  //    lifecycle pass when nothing re-dirtied): keep the baseline and do
  //    nothing — diffing an artifact against itself is a no-op. Clearing here
  //    would livelock: the re-anchor's own full-damage output would trigger
  //    the skip that destroys the fresh baseline, every frame, forever. The
  //    identity compare is safe: the invalidator's traced old-artifact
  //    reference pins the baseline artifact, so the pointer cannot alias.
  //  - The artifact CHANGED: this cycle's PaintController will free the
  //    baseline artifact's backing store when the cycle ends, so the baseline
  //    must be dropped anyway (ClearOldStates). The first cycle after the
  //    host consumes the damage re-anchors through Generate's cheap
  //    empty-bounds path, reporting full once.
  // Net effect: a loading or churning document pays ONE exact diff when its
  // damage is first discovered, then O(1) per lifecycle until the host
  // consumes; interactive steady state keeps exact per-element rects.
  if (pending_.Contains(gfx::Rect(root_view.Size()))) {
    if (&artifact == last_artifact_)
      return;  // baseline already current for this artifact — keep it
    invalidator_->ClearOldStates();
    last_artifact_ = nullptr;
    return;
  }
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
  invalidator_->Generate(blink::PaintChunkSubset(artifact), gfx::Vector2dF(),
                         root_view.Size(), blink::PropertyTreeState::Root());
  last_artifact_ = &artifact;
}

void MbDamageTracker::AddDamage(const gfx::Rect& rect) {
  pending_.Union(rect);
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
