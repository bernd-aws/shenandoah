/*
 * Copyright (c) Amazon.com, Inc. or its affiliates.  All rights reserved.
 *
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

#include "gc/shenandoah/shenandoahHeapRegion.hpp"
#include "gc/shenandoah/shenandoahScanRemembered.hpp"
#include "gc/shenandoah/shenandoahHeap.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.inline.hpp"

ShenandoahDirectCardMarkRememberedSet::ShenandoahDirectCardMarkRememberedSet(CardTable *card_table, size_t total_card_count) {
  _heap = ShenandoahHeap::heap();
  _card_table = card_table;
  _total_card_count = total_card_count;
  _cluster_count = (total_card_count / ShenandoahCardCluster<ShenandoahDirectCardMarkRememberedSet>::CardsPerCluster);
  _card_shift = CardTable::card_shift;

  printf("Instantiating ShenandoahDirectCardMarkRememberedSet(%s = 0x%lx)\n",
	 "total_card_count", total_card_count);

  _byte_map = _card_table->byte_for_index(0);

  _whole_heap_base = _card_table->addr_for(_byte_map);
  _whole_heap_end = _whole_heap_base + total_card_count * CardTable::card_size;

  printf("  whole heap spans 0x%lx to 0x%lx\n",
	 (uint64_t) _whole_heap_base, (uint64_t) _whole_heap_end);

  _byte_map_base = _byte_map - (uintptr_t(_whole_heap_base) >> _card_shift);


  _overreach_map = (uint8_t *) malloc(total_card_count);
  _overreach_map_base = (_overreach_map -
			 (uintptr_t(_whole_heap_base) >> _card_shift));
      
  assert(total_card_count % ShenandoahCardCluster<ShenandoahDirectCardMarkRememberedSet>::CardsPerCluster == 0, "Invalid card count.");
  assert(total_card_count > 0, "Card count cannot be zero.");
  // assert(_overreach_cards != NULL);
}

ShenandoahDirectCardMarkRememberedSet::~ShenandoahDirectCardMarkRememberedSet() {
  free(_overreach_map);
}

void ShenandoahDirectCardMarkRememberedSet::initializeOverreach(uint32_t first_cluster, uint32_t count) {

  // We can make this run faster in the future by explicitly
  // unrolling the loop and doing wide writes if the compiler
  // doesn't do this for us.
  uint32_t first_card_no = first_cluster * ShenandoahCardCluster<ShenandoahDirectCardMarkRememberedSet>::CardsPerCluster;
  uint8_t *omp = &_overreach_map[first_card_no];
  uint8_t *endp = omp + count * ShenandoahCardCluster<ShenandoahDirectCardMarkRememberedSet>::CardsPerCluster;
  while (omp < endp)
    *omp++ = CardTable::clean_card_val();
}

void ShenandoahDirectCardMarkRememberedSet::mergeOverreach(uint32_t first_cluster, uint32_t count) {

  // We can make this run faster in the future by explicitly unrolling the loop and doing wide writes if the compiler
  // doesn't do this for us.
  uint32_t first_card_no = first_cluster * ShenandoahCardCluster<ShenandoahDirectCardMarkRememberedSet>::CardsPerCluster;
  uint8_t *bmp = &_byte_map[first_card_no];
  uint8_t *endp = bmp + count * ShenandoahCardCluster<ShenandoahDirectCardMarkRememberedSet>::CardsPerCluster;
  uint8_t *omp = &_overreach_map[first_card_no];

  // dirty_card is 0, clean card is 0xff; if either *bmp or *omp is dirty, we need to mark it as dirty
  while (bmp < endp)
    *bmp++ &= *omp++;
}


// Implementation is not correct.  ShenandoahBufferWithSATBRememberedSet is a placeholder for future planned improvements.
ShenandoahBufferWithSATBRememberedSet::ShenandoahBufferWithSATBRememberedSet(size_t card_count)
{
  _heap = ShenandoahHeap::heap();

  _card_count = card_count;
  _cluster_count = _card_count /
      ShenandoahCardCluster<ShenandoahBufferWithSATBRememberedSet>::CardsPerCluster;
  _card_shift = CardTable::card_shift;

  _whole_heap_base = _heap->base();
  _whole_heap_end = _whole_heap_base + _card_count * 
      ShenandoahCardCluster<ShenandoahBufferWithSATBRememberedSet>::CardsPerCluster;
}

// Implementation is not correct.  ShenandoahBufferWithSATBRememberedSet is a placeholder for future planned improvements.
ShenandoahBufferWithSATBRememberedSet::~ShenandoahBufferWithSATBRememberedSet()
{
}

// Implementation is not correct.  ShenandoahBufferWithSATBRememberedSet is a placeholder for future planned improvements.
void ShenandoahBufferWithSATBRememberedSet::initializeOverreach(
    uint32_t first_cluster, uint32_t count) {
}

// Implementation is not correct.  ShenandoahBufferWithSATBRememberedSet is a placeholder for future planned improvements.
void ShenandoahBufferWithSATBRememberedSet::mergeOverreach(
    uint32_t first_cluster, uint32_t count) {
}

#ifdef IMPLEMENT_THIS_OPTIMIZATION_LATER

template <class RememberedSet>
bool ShenandoahCardCluster<RememberedSet>::hasObject(uint32_t card_no) {
  return (object_starts[card_no] & ObjectStartsInCardRegion)? true: false;
}

template <class RememberedSet>
uint32_t ShenandoahCardCluster<RememberedSet>::getFirstStart(uint32_t card_no)
{
  assert(object_starts[card_no] & ObjectStartsInCardRegion);
  return (((object_starts[card_no] & FirstStartBits) >> FirstStartShift) *
	  CardWordOffsetMultiplier);
}

template <class RememberedSet>
uint32_t ShenandoahCardCluster<RememberedSet>::getLastStart(uint32_t card_no) {
  assert(object_starts[card_no] & ObjectStartsInCardRegion);
  return (((object_starts[card_no] & LastStartBits) >> LastStartShift) *
	  CardWordOffsetMultiplier);
}

template <class RememberedSet>
uint8_t ShenandoahCardCluster<RememberedSet>::getCrossingObjectStart(uint32_t card_no) {
  assert((object_starts[card_no] & ObjectStartsInCardRegion) == 0);
  return object_starts[card_no] * CardWordOffsetMultiplier;
}

#endif

