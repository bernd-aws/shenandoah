/*
 * Copyright (c) 2020, Red Hat, Inc. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "gc/shenandoah/shenandoahConcurrentRoots.hpp"
#include "gc/shenandoah/shenandoahFreeSet.hpp"
#include "gc/shenandoah/shenandoahHeap.hpp"
#include "gc/shenandoah/shenandoahMarkClosures.hpp"
#include "gc/shenandoah/shenandoahOopClosures.hpp"
#include "gc/shenandoah/shenandoahUtils.hpp"
#include "gc/shenandoah/shenandoahVerifier.hpp"
#include "gc/shenandoah/shenandoahYoungGeneration.hpp"
#include "gc/shenandoah/heuristics/shenandoahHeuristics.hpp"

ShenandoahYoungGeneration::ShenandoahYoungGeneration() : ShenandoahGeneration(YOUNG),
  _affiliated_region_count(0),
  _used(0) {
}

void ShenandoahYoungGeneration::increment_affiliated_region_count() {
  _affiliated_region_count++;
}

void ShenandoahYoungGeneration::decrement_affiliated_region_count() {
  _affiliated_region_count--;
}

void ShenandoahYoungGeneration::increase_used(size_t bytes) {
  shenandoah_assert_heaplocked();
  _used += bytes;
}

void ShenandoahYoungGeneration::decrease_used(size_t bytes) {
  shenandoah_assert_heaplocked_or_safepoint();
  assert(used() >= bytes, "cannot reduce bytes used by young generation below zero");
  _used -= bytes;
}

// There are three JVM parameters for setting young gen capacity:
//    NewSize, MaxNewSize, NewRatio.
//
// If only NewSize is set, it assigns a fixed size and the other two parameters are ignored.
// Otherwise NewRatio applies.
//
// If NewSize is set in any combination, it provides a lower bound.
//
// If MaxNewSize is set it provides an upper bound.
// If this bound is smaller than NewSize, it supersedes,
// resulting in a fixed size given by MaxNewSize.
size_t ShenandoahYoungGeneration::configured_capacity(size_t capacity) const {
  if (FLAG_IS_CMDLINE(NewSize) && !FLAG_IS_CMDLINE(MaxNewSize) && !FLAG_IS_CMDLINE(NewRatio)) {
    capacity = MIN2(NewSize, capacity);
  } else {
    capacity /= NewRatio + 1;
    if (FLAG_IS_CMDLINE(NewSize)) {
      capacity = MAX2(NewSize, capacity);
    }
    if (FLAG_IS_CMDLINE(MaxNewSize)) {
      capacity = MIN2(MaxNewSize, capacity);
    }
  }
  return capacity;
}

size_t ShenandoahYoungGeneration::soft_max_capacity() const {
  size_t capacity = ShenandoahHeap::heap()->soft_max_capacity();
  return configured_capacity(capacity);
}

size_t ShenandoahYoungGeneration::max_capacity() const {
  size_t capacity = ShenandoahHeap::heap()->max_capacity();
  return configured_capacity(capacity);
}

size_t ShenandoahYoungGeneration::used_regions_size() const {
  size_t used_regions_size = _affiliated_region_count * ShenandoahHeapRegion::region_size_bytes();

  assert(used() <= used_regions_size, "Must not use more than we have - used: " SIZE_FORMAT ", used_regions_size: " SIZE_FORMAT,
         used(), used_regions_size);

  return used_regions_size;
}

size_t ShenandoahYoungGeneration::available() const {
  size_t in_use = used();
  size_t soft_capacity = soft_max_capacity();
  return in_use > soft_capacity ? 0 : soft_capacity - in_use;
}

// TODO: This is almost the same code as in ShenandoahGlobalGeneration for now, to be further differentiated.
void ShenandoahYoungGeneration::op_final_mark() {
  ShenandoahHeap* heap = ShenandoahHeap::heap();

  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Should be at safepoint");
  assert(!heap->has_forwarded_objects(), "No forwarded objects on this path");

  // It is critical that we
  // evacuate roots right after finishing marking, so that we don't
  // get unmarked objects in the roots.

  if (!heap->cancelled_gc()) {
    concurrent_mark()->finish_mark_from_roots(/* full_gc = */ false);

    // Marking is completed, deactivate SATB barrier
    heap->set_concurrent_mark_in_progress(false);
    heap->mark_complete_marking_context();

    heap->parallel_cleaning(false /* full gc*/);

    if (ShenandoahVerify) {
      heap->verifier()->verify_roots_no_forwarded();
    }

    {
      ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_update_region_states);
      ShenandoahFinalMarkUpdateRegionStateClosure cl;
      heap->parallel_heap_region_iterate(&cl);

      heap->assert_pinned_region_status();
    }

    // Retire the TLABs, which will force threads to reacquire their TLABs after the pause.
    // This is needed for two reasons. Strong one: new allocations would be with new freeset,
    // which would be outside the collection set, so no cset writes would happen there.
    // Weaker one: new allocations would happen past update watermark, and so less work would
    // be needed for reference updates (would update the large filler instead).
    if (UseTLAB) {
      ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_manage_labs);
      heap->tlabs_retire(false);
    }

    {
      ShenandoahGCPhase phase(ShenandoahPhaseTimings::choose_cset);
      ShenandoahHeapLocker locker(heap->lock());
      heap->collection_set()->clear();
      heap->heuristics()->choose_collection_set(heap->collection_set());
    }

    {
      ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_rebuild_freeset);
      ShenandoahHeapLocker locker(heap->lock());
      heap->free_set()->rebuild();
    }

    if (!heap->is_degenerated_gc_in_progress()) {
      heap->prepare_concurrent_roots();
      heap->prepare_concurrent_unloading();
    }

    // If collection set has candidates, start evacuation.
    // Otherwise, bypass the rest of the cycle.
    if (!heap->collection_set()->is_empty()) {
      ShenandoahGCPhase init_evac(ShenandoahPhaseTimings::init_evac);

      if (ShenandoahVerify) {
        heap->verifier()->verify_before_evacuation();
      }

      heap->set_evacuation_in_progress(true);
      // From here on, we need to update references.
      heap->set_has_forwarded_objects(true);

      if (!heap->is_degenerated_gc_in_progress()) {
        if (ShenandoahConcurrentRoots::should_do_concurrent_class_unloading()) {
          ShenandoahCodeRoots::arm_nmethods();
        }
        heap->evacuate_and_update_roots();
      }

      if (ShenandoahPacing) {
        heap->pacer()->setup_for_evac();
      }

      if (ShenandoahVerify) {
        // If OOM while evacuating/updating of roots, there is no guarantee of their consistencies
        if (!heap->cancelled_gc()) {
          ShenandoahRootVerifier::RootTypes types = ShenandoahRootVerifier::None;
          if (ShenandoahConcurrentRoots::should_do_concurrent_roots()) {
            types = ShenandoahRootVerifier::combine(ShenandoahRootVerifier::JNIHandleRoots, ShenandoahRootVerifier::WeakRoots);
            types = ShenandoahRootVerifier::combine(types, ShenandoahRootVerifier::CLDGRoots);
            types = ShenandoahRootVerifier::combine(types, ShenandoahRootVerifier::StringDedupRoots);
          }

          if (ShenandoahConcurrentRoots::should_do_concurrent_class_unloading()) {
            types = ShenandoahRootVerifier::combine(types, ShenandoahRootVerifier::CodeRoots);
          }
          heap->verifier()->verify_roots_no_forwarded_except(types);
        }
        heap->verifier()->verify_during_evacuation();
      }
    } else {
      if (ShenandoahVerify) {
        heap->verifier()->verify_after_concmark();
      }

      if (VerifyAfterGC) {
        Universe::verify();
      }
    }

  } else {
    // If this cycle was updating references, we need to keep the has_forwarded_objects
    // flag on, for subsequent phases to deal with it.
    concurrent_mark()->cancel();
    heap->set_concurrent_mark_in_progress(false);
  }
}

void ShenandoahYoungGeneration::promote_all() {
  _used = 0;

  ShenandoahHeap* heap = ShenandoahHeap::heap();
  for (size_t index = 0; index < heap->num_regions(); index++) {
    ShenandoahHeapRegion* region = heap->get_region(index);
    if (region->is_young()) {
      region->set_affiliation(ShenandoahRegionAffiliation::OLD_GENERATION);
    }
  }

  assert(_affiliated_region_count == 0, "young generation must not have affiliated regions after reset");
}

void ShenandoahYoungGeneration::log_status() const {
  LogTarget(Info, gc, ergo) lt;
  
  if (!lt.is_enabled()) {
    return;
  }

  // Not under a lock here, so just read these once to make sure byte size in
  // proper unit and proper unit for byte size are consistent.
  size_t used = _used;
  size_t soft_max_cap = soft_max_capacity();
  size_t max_cap = max_capacity();
  size_t free = available();
  size_t used_regions = used_regions_size();
  lt.print("Young Generation Used: " SIZE_FORMAT "%s, Used Regions: " SIZE_FORMAT "%s, "
           "Soft Capacity: " SIZE_FORMAT "%s, Max Capacity: " SIZE_FORMAT " %s, Available: " SIZE_FORMAT " %s",
           byte_size_in_proper_unit(used),          proper_unit_for_byte_size(used),
           byte_size_in_proper_unit(used_regions),  proper_unit_for_byte_size(used_regions),
           byte_size_in_proper_unit(soft_max_cap),  proper_unit_for_byte_size(soft_max_cap),
           byte_size_in_proper_unit(max_cap),       proper_unit_for_byte_size(max_cap),
           byte_size_in_proper_unit(free),          proper_unit_for_byte_size(free));
}
