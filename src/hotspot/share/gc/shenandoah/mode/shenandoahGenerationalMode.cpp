/*
 * Copyright (c) 2019, 2020, Red Hat, Inc. All rights reserved.
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
#include "gc/shenandoah/shenandoahConcurrentRoots.hpp"
#include "gc/shenandoah/heuristics/shenandoahGenerationalHeuristics.hpp"
#include "gc/shenandoah/mode/shenandoahGenerationalMode.hpp"
#include "logging/log.hpp"
#include "logging/logTag.hpp"

void ShenandoahGenerationalMode::initialize_flags() const {
  if (ShenandoahConcurrentRoots::can_do_concurrent_class_unloading()) {
    FLAG_SET_DEFAULT(ShenandoahSuspendibleWorkers, true);
    FLAG_SET_DEFAULT(VerifyBeforeExit, false);
  }

  SHENANDOAH_ERGO_ENABLE_FLAG(ExplicitGCInvokesConcurrent);
  SHENANDOAH_ERGO_ENABLE_FLAG(ShenandoahImplicitGCInvokesConcurrent);

  // Final configuration checks
  SHENANDOAH_CHECK_FLAG_SET(ShenandoahLoadRefBarrier);
  SHENANDOAH_CHECK_FLAG_UNSET(ShenandoahStoreValEnqueueBarrier);
  SHENANDOAH_CHECK_FLAG_SET(ShenandoahSATBBarrier);
  SHENANDOAH_CHECK_FLAG_SET(ShenandoahCASBarrier);
  SHENANDOAH_CHECK_FLAG_SET(ShenandoahCloneBarrier);
}

ShenandoahHeuristics* ShenandoahGenerationalMode::initialize_heuristics() const {
  // HEY! If we had a better abstraction over the concepts of 'capacity'
  // and 'available' we could leverage it to get some re-use out of the
  // existing heuristics.

  // HEY! Eventually we will want separate heuristics for old and young
  // generations. Or perhaps we'll need to have 'should_start_gc' return the
  // type/generation to run a GC on.
  ShenandoahHeap *heap = ShenandoahHeap::heap();
  ShenandoahGeneration *generation = heap->young_generation();
  assert(generation != nullptr, "Expected generation to be initialized here.");
  return new ShenandoahGenerationalHeuristics(generation);
}
