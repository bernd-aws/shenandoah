#include "gc/shenandoah/shenandoahMarkClosures.hpp"

void ShenandoahFinalMarkUpdateRegionStateClosure::heap_region_do(ShenandoahHeapRegion* r) {
  if (r->is_active()) {
    // All allocations past TAMS are implicitly live, adjust the region data.
    // Bitmaps/TAMS are swapped at this point, so we need to poll complete bitmap.
    HeapWord *tams = _ctx->top_at_mark_start(r);
    HeapWord *top = r->top();
    if (top > tams) {
      r->increase_live_data_alloc_words(pointer_delta(top, tams));
    }

    // We are about to select the collection set, make sure it knows about
    // current pinning status. Also, this allows trashing more regions that
    // now have their pinning status dropped.
    if (r->is_pinned()) {
      if (r->pin_count() == 0) {
        ShenandoahHeapLocker locker(_lock);
        r->make_unpinned();
      }
    } else {
      if (r->pin_count() > 0) {
        ShenandoahHeapLocker locker(_lock);
        r->make_pinned();
      }
    }

    // Remember limit for updating refs. It's guaranteed that we get no
    // from-space-refs written from here on.
    r->set_update_watermark_at_safepoint(r->top());
  } else {
    assert(!r->has_live(), "Region " SIZE_FORMAT " should have no live data", r->index());
    assert(_ctx->top_at_mark_start(r) == r->top(),
           "Region " SIZE_FORMAT " should have correct TAMS", r->index());

    // We only age retired regions,
    // so that objects do not assimilate the extra age of a region they get evacuated into.
    // Here we err on the side of tenuring objects too late, to avoid promoting too early.
    r->increment_age();
  }
}
