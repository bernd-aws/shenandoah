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
  // Aggressively clean all young regions.
  for (size_t idx = 0; idx < size; idx++) {
    ShenandoahHeapRegion* r = data[idx]._region;
    if (r->is_young() && r->garbage() > 0) {
      cset->add_region(r);
    }
  }
}

void ShenandoahGenerationalHeuristics::record_cycle_start() {
  ShenandoahHeuristics::record_cycle_start();
  log_info(gc)("Starting cycle, generation is using: " SIZE_FORMAT " of "  SIZE_FORMAT " regions.",
               _generation->affiliated_region_count(), _generation->region_quota());
}

void ShenandoahGenerationalHeuristics::record_cycle_end() {
  ShenandoahHeuristics::record_cycle_end();
  log_info(gc)("Ending cycle, generation is using: " SIZE_FORMAT " of " SIZE_FORMAT " regions.",
               _generation->affiliated_region_count(), _generation->region_quota());
}

bool ShenandoahGenerationalHeuristics::should_start_gc() const {
  size_t regions_allowed = _generation->region_quota();
  size_t regions_available = _generation->free_region_quota();


  // Check if the regions available to us is beneath a minimum threshold of
  // the total regions allowed to us.
  size_t min_threshold = regions_allowed / 100 * ShenandoahMinFreeThreshold;
  if (regions_available <= min_threshold) {
    log_info(gc)("Trigger: Free regions (" SIZE_FORMAT ") is below minimum threshold (" SIZE_FORMAT ")",
                 byte_size_in_proper_unit(regions_available),  byte_size_in_proper_unit(min_threshold));
    return true;
  }

  return ShenandoahHeuristics::should_start_gc();
}
