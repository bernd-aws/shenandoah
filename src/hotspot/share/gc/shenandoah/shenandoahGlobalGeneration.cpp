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

#include "gc/shenandoah/shenandoahConcurrentMark.hpp"
#include "gc/shenandoah/shenandoahConcurrentRoots.hpp"
#include "gc/shenandoah/shenandoahFreeSet.hpp"
#include "gc/shenandoah/shenandoahHeap.hpp"
#include "gc/shenandoah/shenandoahMarkClosures.hpp"
#include "gc/shenandoah/shenandoahOopClosures.hpp"
#include "gc/shenandoah/shenandoahUtils.hpp"
#include "gc/shenandoah/shenandoahVerifier.hpp"
#include "gc/shenandoah/shenandoahGlobalGeneration.hpp"
#include "gc/shenandoah/heuristics/shenandoahHeuristics.hpp"

size_t ShenandoahGlobalGeneration::capacity() const {
  return ShenandoahHeap::heap()->soft_max_capacity();
}

size_t ShenandoahGlobalGeneration::used() const {
  return ShenandoahHeap::heap()->free_set()->used();
}

size_t ShenandoahGlobalGeneration::available() const {
  return ShenandoahHeap::heap()->free_set()->available();
}

void ShenandoahGlobalGeneration::op_final_mark() {
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

    if (heap->process_references()) {
      // Abandon reference processing right away: pre-cleaning must have failed.
      ReferenceProcessor *rp = heap->ref_processor();
      rp->disable_discovery();
      rp->abandon_partial_discovery();
      rp->verify_no_references_recorded();
    }
  }
}
