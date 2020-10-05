/*
 * Copyright (c) 2018, 2019, Red Hat, Inc. All rights reserved.
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

#include "precompiled.hpp"

#include "gc/shenandoah/heuristics/shenandoahGenerationalHeuristics.hpp"
#include "gc/shenandoah/shenandoahCollectionSet.hpp"
#include "gc/shenandoah/shenandoahFreeSet.hpp"
#include "gc/shenandoah/shenandoahGeneration.hpp"
#include "logging/log.hpp"
#include "logging/logTag.hpp"
#include "utilities/quickSort.hpp"

ShenandoahGenerationalHeuristics::ShenandoahGenerationalHeuristics(ShenandoahGeneration *generation) :
  ShenandoahHeuristics(), _generation(generation) {}

void ShenandoahGenerationalHeuristics::choose_collection_set_from_regiondata(ShenandoahCollectionSet* cset,
                                                                         RegionData* data, size_t size,
                                                                         size_t actual_free) {
  size_t garbage_threshold = ShenandoahHeapRegion::region_size_bytes() * ShenandoahGarbageThreshold / 100;

  // The logic for cset selection in adaptive is as follows:
  //
  //   1. We cannot get cset larger than available free space. Otherwise we guarantee OOME
  //      during evacuation, and thus guarantee full GC. In practice, we also want to let
  //      application to allocate something. This is why we limit CSet to some fraction of
  //      available space. In non-overloaded heap, max_cset would contain all plausible candidates
  //      over garbage threshold.
  //
  //   2. We should not get cset too low so that free threshold would not be met right
  //      after the cycle. Otherwise we get back-to-back cycles for no reason if heap is
  //      too fragmented. In non-overloaded non-fragmented heap min_garbage would be around zero.
  //
  // Therefore, we start by sorting the regions by garbage. Then we unconditionally add the best candidates
  // before we meet min_garbage. Then we add all candidates that fit with a garbage threshold before
  // we hit max_cset. When max_cset is hit, we terminate the cset selection. Note that in this scheme,
  // ShenandoahGarbageThreshold is the soft threshold which would be ignored until min_garbage is hit.

  size_t capacity    = ShenandoahHeap::heap()->soft_max_capacity();
  size_t max_cset    = (size_t)((1.0 * capacity / 100 * ShenandoahEvacReserve) / ShenandoahEvacWaste);
  size_t free_target = (capacity / 100 * ShenandoahMinFreeThreshold) + max_cset;
  size_t min_garbage = (free_target > actual_free ? (free_target - actual_free) : 0);

  log_info(gc, ergo)("Generational CSet Selection. Target Free: " SIZE_FORMAT "%s, Actual Free: "
                     SIZE_FORMAT "%s, Max CSet: " SIZE_FORMAT "%s, Min Garbage: " SIZE_FORMAT "%s",
                     byte_size_in_proper_unit(free_target), proper_unit_for_byte_size(free_target),
                     byte_size_in_proper_unit(actual_free), proper_unit_for_byte_size(actual_free),
                     byte_size_in_proper_unit(max_cset),    proper_unit_for_byte_size(max_cset),
                     byte_size_in_proper_unit(min_garbage), proper_unit_for_byte_size(min_garbage));

  // Better select garbage-first regions
  QuickSort::sort<RegionData>(data, (int)size, compare_by_garbage, false);

  size_t cur_cset = 0;
  size_t cur_garbage = 0;

  for (size_t idx = 0; idx < size; idx++) {
    ShenandoahHeapRegion* r = data[idx]._region;

    size_t new_cset    = cur_cset + r->get_live_data_bytes();
    size_t new_garbage = cur_garbage + r->garbage();

    if (new_cset > max_cset) {
      break;
    }

    if ((new_garbage < min_garbage) || (r->garbage() > garbage_threshold)) {
      cset->add_region(r);
      cur_cset = new_cset;
      cur_garbage = new_garbage;
    }
  }
}

void ShenandoahGenerationalHeuristics::record_cycle_start() {
  ShenandoahHeuristics::record_cycle_start();
  log_info(gc)("Starting cycle, generation is using: " SIZE_FORMAT " regions.",
               _generation->affiliated_region_count());
}

void ShenandoahGenerationalHeuristics::record_cycle_end() {
  ShenandoahHeuristics::record_cycle_end();
  log_info(gc)("Ending cycle, generation is using: " SIZE_FORMAT " regions.",
               _generation->affiliated_region_count());
}

bool ShenandoahGenerationalHeuristics::should_start_gc() const {
  size_t regions_allowed = _generation->region_quota();
  size_t regions_available = _generation->free_region_quota();

  ShenandoahHeap* heap = ShenandoahHeap::heap();
  size_t capacity = heap->soft_max_capacity();


  // Check if we are falling below the worst limit, time to trigger the GC, regardless of
  // anything else.
  size_t min_threshold = regions_allowed / 100 * ShenandoahMinFreeThreshold;
  if (regions_available < min_threshold) {
    log_info(gc)("Trigger: Free regions (" SIZE_FORMAT ") is below minimum threshold (" SIZE_FORMAT ")",
                 byte_size_in_proper_unit(regions_available),  byte_size_in_proper_unit(min_threshold));
    return true;
  }

  size_t threshold_bytes_allocated = capacity / 100 * ShenandoahAllocationThreshold;
  size_t bytes_allocated = heap->bytes_allocated_since_gc_start();
  if (bytes_allocated > threshold_bytes_allocated) {
    log_info(gc)("Trigger: Allocated since last cycle (" SIZE_FORMAT "%s) is larger than allocation threshold (" SIZE_FORMAT "%s)",
                 byte_size_in_proper_unit(bytes_allocated),           proper_unit_for_byte_size(bytes_allocated),
                 byte_size_in_proper_unit(threshold_bytes_allocated), proper_unit_for_byte_size(threshold_bytes_allocated));
    return true;
  }

  return ShenandoahHeuristics::should_start_gc();
}
