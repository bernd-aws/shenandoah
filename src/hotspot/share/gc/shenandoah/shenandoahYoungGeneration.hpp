/*
 * Copyright (c) 2020, 2020, Amazon.com, Inc. or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHYOUNGGENERATION_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHYOUNGGENERATION_HPP

#include "gc/shenandoah/shenandoahGeneration.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.hpp"

class ShenandoahYoungGeneration : public ShenandoahGeneration {
private:
  size_t _affiliated_region_count;
  size_t _used;

public:
  ShenandoahYoungGeneration();

  void increment_affiliated_region_count();
  void decrement_affiliated_region_count();

  void increase_used(size_t bytes);
  void decrease_used(size_t bytes);

private:
  size_t configured_capacity(size_t capacity) const;

public:

  void log_status() const;

  virtual size_t max_capacity() const;
  virtual size_t soft_max_capacity() const;
  virtual size_t used_regions_size() const;
  virtual size_t used() const { return _used; }
  virtual size_t available() const;

  virtual void op_final_mark();

  void promote_tenured_regions();
  void promote_all_regions();
};

#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHYOUNGGENERATION_HPP
