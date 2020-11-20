/*
 * Copyright (c) 2001, 2018, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHGENERATION_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHGENERATION_HPP

#include "memory/allocation.hpp"
#include "gc/shenandoah/shenandoahLock.hpp"
#include "gc/shenandoah/shenandoahMarkingContext.hpp"
#include "gc/shenandoah/shenandoahConcurrentMark.hpp"
#include "gc/shenandoah/shenandoahOopClosures.hpp"

class ShenandoahGeneration : public CHeapObj<mtGC> {
private:
  GenerationMode const _generation_mode;
  ShenandoahConcurrentMark* const _scm;
  ShenandoahHeuristics* _heuristics;
public:
  ShenandoahGeneration(GenerationMode generation_mode) :
    _generation_mode(generation_mode),
    _scm(new ShenandoahConcurrentMark(generation_mode)) {
  }

  inline GenerationMode generation_mode() const { return _generation_mode; }

  inline ShenandoahConcurrentMark* concurrent_mark() const { return _scm; }

  inline ShenandoahHeuristics* heuristics() const { return _heuristics; }

  virtual const char* name() const = 0;

  void initialize_heuristics(ShenandoahMode* gc_mode);

  virtual size_t soft_max_capacity() const = 0;
  virtual size_t max_capacity() const = 0;
  virtual size_t used_regions_size() const = 0;
  virtual size_t used() const = 0;
  virtual size_t available() const = 0;

  virtual size_t bytes_allocated_since_gc_start();

  void log_status() const;

  // Entry methods to normally STW GC operations. These set up logging, monitoring
  // and workers for net VM operation
  void entry_init_mark();
  void entry_mark();
  void entry_final_mark();

  void op_init_mark();
  virtual void op_final_mark() = 0;

private:
  void op_mark();
};

#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHGENERATION_HPP
