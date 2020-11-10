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

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHSCANREMEMBEREDINLINE_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHSCANREMEMBEREDINLINE_HPP

#include "memory/iterator.hpp"
#include "oops/oop.hpp"
#include "oops/objArrayOop.hpp"
#include "gc/shenandoah/shenandoahCardTable.hpp"
#include "gc/shenandoah/shenandoahHeap.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.hpp"
#include "gc/shenandoah/shenandoahScanRemembered.hpp"

#include "gc/shenandoah/shenandoahBufferWithSATBRememberedSet.inline.hpp"

inline uint32_t
ShenandoahDirectCardMarkRememberedSet::card_index_for_addr(HeapWord *p) {
  return (uint32_t) _card_table->index_for(p);
}

inline HeapWord *
ShenandoahDirectCardMarkRememberedSet::addr_for_card_index(uint32_t card_index) {
  return _whole_heap_base + CardTable::card_size_in_words * card_index;
}

inline bool 
ShenandoahDirectCardMarkRememberedSet::is_card_dirty(uint32_t card_index) {
  uint8_t *bp = &_byte_map_base[card_index];
  return (bp[0] == CardTable::dirty_card_val());
}

inline void 
ShenandoahDirectCardMarkRememberedSet::mark_card_as_dirty(uint32_t card_index) {
  uint8_t *bp = &_byte_map_base[card_index];
  bp[0] = CardTable::dirty_card_val();
}

inline void 
ShenandoahDirectCardMarkRememberedSet::mark_card_as_clean(uint32_t card_index) {
  uint8_t *bp = &_byte_map_base[card_index];
  bp[0] = CardTable::clean_card_val();
}

inline void
ShenandoahDirectCardMarkRememberedSet::mark_overreach_card_as_dirty(
    uint32_t card_index) {
  uint8_t *bp = &_overreach_map_base[card_index];
  bp[0] = CardTable::dirty_card_val();
}

inline bool 
ShenandoahDirectCardMarkRememberedSet::is_card_dirty(HeapWord *p) {
  uint8_t *bp = &_byte_map_base[uintptr_t(p) >> _card_shift];
  return (bp[0] == CardTable::dirty_card_val());
}

inline void
ShenandoahDirectCardMarkRememberedSet::mark_card_as_dirty(HeapWord *p) {
  uint8_t *bp = &_byte_map_base[uintptr_t(p) >> _card_shift];
  bp[0] = CardTable::dirty_card_val();
}

inline void
ShenandoahDirectCardMarkRememberedSet::mark_card_as_clean(HeapWord *p) {
  uint8_t *bp = &_byte_map_base[uintptr_t(p) >> _card_shift];
  bp[0] = CardTable::clean_card_val();
}

inline void 
ShenandoahDirectCardMarkRememberedSet::mark_overreach_card_as_dirty(void *p) {
  uint8_t *bp = &_overreach_map_base[uintptr_t(p) >> _card_shift];
  bp[0] = CardTable::dirty_card_val();
}

inline uint32_t
ShenandoahDirectCardMarkRememberedSet::cluster_count() {
  return _cluster_count;
}


template<typename RememberedSet>
inline uint32_t
ShenandoahCardCluster<RememberedSet>::card_index_for_addr(HeapWord *p) {
  return _rs->card_index_for_addr(p);
}

template<typename RememberedSet>
inline bool
ShenandoahCardCluster<RememberedSet>::has_object(uint32_t card_index) {
  HeapWord *addr = _rs->addr_for_card_index(card_index);

  printf("has_object(%u), range starts at %llx\n", card_index, (unsigned long long) addr);

  ShenandoahHeap *heap = ShenandoahHeap::heap();
  ShenandoahHeapRegion *region = heap->heap_region_containing(addr);

  HeapWord *obj = region->block_start(addr);

  printf(" back from region->block_start(addr), obj is %llx\n", (unsigned long long) obj);


  // addr is the first address of the card region.
  // obj is the object that spans addr (or starts at addr).
  assert(obj != NULL, "Object cannot be null");
  if (obj >= addr)
    return true;
  else {
    HeapWord *end_addr = addr + CardTable::card_size_in_words;
    obj += oop(obj)->size();
    if (obj < end_addr)
      return true;
    else
      return false;
  }
}

template<typename RememberedSet>
inline uint32_t
ShenandoahCardCluster<RememberedSet>::get_first_start(uint32_t card_index) {
  HeapWord *addr = _rs->addr_for_card_index(card_index);
  ShenandoahHeap *heap = ShenandoahHeap::heap();
  ShenandoahHeapRegion *region = heap->heap_region_containing(addr);
  HeapWord *obj = region->block_start(addr);

  assert(obj != NULL, "Object cannot be null.");
  if (obj >= addr)
    return obj - addr;
  else {
    HeapWord *end_addr = addr + CardTable::card_size_in_words;
    obj += oop(obj)->size();

    // If obj > end_addr, offset will reach beyond end of this card
    // region.  But clients should not invoke this service unless
    // they first confirm that this card has an object.
    assert(obj < end_addr, "Object out of range");
    return obj - addr;
  }
}

template<typename RememberedSet>
inline uint32_t
ShenandoahCardCluster<RememberedSet>::get_last_start(uint32_t card_index) {
  HeapWord *addr = _rs->addr_for_card_index(card_index);
  HeapWord *end_addr = addr + CardTable::card_size_in_words;
  ShenandoahHeap *heap = ShenandoahHeap::heap();
  ShenandoahHeapRegion *region = heap->heap_region_containing(addr);
  HeapWord *obj = region->block_start(addr);
  
  assert(obj != NULL, "Object cannot be null.");

  HeapWord *end_obj = obj + oop(obj)->size();
  while (end_obj < end_addr) {
    obj = end_obj;
    end_obj = obj + oop(obj)->size();
  }
  assert(obj >= addr, "Object out of range.");
  return obj - addr;
}

template<typename RememberedSet>
inline uint32_t
ShenandoahCardCluster<RememberedSet>::get_crossing_object_start(uint32_t card_index) {
  HeapWord *addr = _rs->addr_for_card_index(card_index);
  uint32_t cluster_no =
      card_index / ShenandoahCardCluster<RememberedSet>::CardsPerCluster;
  HeapWord *cluster_addr = _rs->addr_for_card_index(cluster_no * CardsPerCluster);

  HeapWord *end_addr = addr + CardTable::card_size_in_words;
  ShenandoahHeap *heap = ShenandoahHeap::heap();
  ShenandoahHeapRegion *region = heap->heap_region_containing(addr);
  HeapWord *obj = region->block_start(addr);
    
  if (obj > cluster_addr)
    return obj - cluster_addr;
  else
    return 0x7fff;
}

template<typename RememberedSet>
inline HeapWord *
ShenandoahCardCluster<RememberedSet>::addr_for_card_index(uint32_t card_index) {
  return _rs->addr_for_card_index(card_index);
}

template<typename RememberedSet>
inline bool
ShenandoahCardCluster<RememberedSet>::is_card_dirty(uint32_t card_index) {
  return _rs->is_card_dirty(card_index);
}

template<typename RememberedSet>
inline void
ShenandoahCardCluster<RememberedSet>::mark_card_as_dirty(uint32_t card_index) {
  return _rs->mark_card_as_dirty(card_index);
}

template<typename RememberedSet>
inline void
ShenandoahCardCluster<RememberedSet>::mark_card_as_clean(uint32_t card_index) {
  return _rs->mark_card_as_clean(card_index);
}

template<typename RememberedSet>
inline void
ShenandoahCardCluster<RememberedSet>::mark_overreach_card_as_dirty(uint32_t card_index) {
  return _rs->mark_overreach_card_as_dirty(card_index);
}

template<typename RememberedSet>
inline bool
ShenandoahCardCluster<RememberedSet>::is_card_dirty(HeapWord *p) {
  return _rs->is_card_dirty(p);
}

template<typename RememberedSet>
inline void
ShenandoahCardCluster<RememberedSet>::mark_card_as_dirty(HeapWord *p) {
  return _rs->mark_card_as_dirty(p);
}

template<typename RememberedSet>
inline void
ShenandoahCardCluster<RememberedSet>::mark_card_as_clean(HeapWord *p) {
  return _rs->mark_card_as_clean(p);
}

template<typename RememberedSet>
inline void
ShenandoahCardCluster<RememberedSet>::mark_overreach_card_as_dirty(void *p) {
  return _rs->mark_overreach_card_as_dirty(p);
}

template<typename RememberedSet>
inline uint32_t
ShenandoahCardCluster<RememberedSet>::cluster_count() {
  return _rs->cluster_count();
}

template<typename RememberedSet>
inline void
ShenandoahCardCluster<RememberedSet>::initialize_overreach(uint32_t first_cluster, uint32_t count) {
  return _rs->initialize_overreach(first_cluster, count);
}

template<typename RememberedSet>
inline void
ShenandoahCardCluster<RememberedSet>::merge_overreach(uint32_t first_cluster, uint32_t count) {
  return _rs->merge_overreach(first_cluster, count);
}

template<typename RememberedSet>
ShenandoahScanRemembered<RememberedSet>::ShenandoahScanRemembered(RememberedSet *rs) {
  _scc = new ShenandoahCardCluster<RememberedSet>(rs);
}
  
template<typename RememberedSet>
ShenandoahScanRemembered<RememberedSet>::~ShenandoahScanRemembered() {
  delete _scc;
}

template<typename RememberedSet>
template <typename ClosureType>
inline void 
ShenandoahScanRemembered<RememberedSet>::process_clusters(uint worker_id, ReferenceProcessor* rp, ShenandoahConcurrentMark* cm,
							  uint32_t first_cluster, uint32_t count, HeapWord *end_of_range,
							  ClosureType *oops) {

  // Unlike traditional Shenandoah marking, the old-gen resident objects that are examined as part of the remembered set are not
  // themselves marked.  Each such object will be scanned only once.  Any young-gen objects referenced from the remembered set will
  // be marked and then subsequently scanned.

  printf("%u: process_clusters first_cluster: %u, count: %u, end_of_range: %llx\n", worker_id, first_cluster, count,
	 (unsigned long long) end_of_range);

  while (count-- > 0) {
    uint32_t card_index = first_cluster * ShenandoahCardCluster<RememberedSet>::CardsPerCluster;
    uint32_t end_card_index = card_index + ShenandoahCardCluster<RememberedSet>::CardsPerCluster;

    printf("%u: scanning first_cluster: %d, card_index: %u to end_card_index: %u\n",
	   worker_id, first_cluster, card_index, end_card_index);


    first_cluster++;
    int next_card_index = 0;
    while (card_index < end_card_index) {
      printf("%u: card_index %d, end_card_index: %d\n", worker_id, card_index, end_card_index);

      if (_scc->is_card_dirty(card_index)) {

        printf("%u: card %u is dirty!\n", worker_id, card_index);

	if (_scc->has_object(card_index)) {

	  printf("%u: card has object\n", worker_id);

	  // Scan all objects that start within this card region.
	  uint32_t start_offset = _scc->get_first_start(card_index);
	  HeapWord *p = _scc->addr_for_card_index(card_index);
	  HeapWord *endp = p + CardTable::card_size_in_words;
	  if (endp > end_of_range) {
            endp = end_of_range;
	    next_card_index = end_card_index;
	  } else {
  	    // endp either points to start of next card region, or to the next object that needs to be scanned, which may
	    // reside in some successor card region.
            next_card_index = _scc->card_index_for_addr(endp);
	  }

	  p += start_offset;
	  
	  printf("%u: scanning objects for memory region %llx to %llx, with first object at offset %u\n", 
		 worker_id, (unsigned long long) p, (unsigned long long) endp, start_offset);

	  while (p < endp) {
	    oop obj = oop(p);

	    printf("%u: about to test for is_objArray() on object @%llx of size %u\n",
		   worker_id, (unsigned long long) p, (unsigned) obj->size());

	    // Future TODO:
	    // For improved efficiency, we might want to give special handling of obj->is_objArray().  In
	    // particular, in that case, we might want to divide the effort for scanning of a very long object array
	    // between multiple threads.
	    if (obj->is_objArray()) {
	      printf("%u: it is an object array, so we'll iterate through its ", worker_id);
		
	      ShenandoahObjToScanQueue* q = cm->get_queue(worker_id);
	      ShenandoahMarkRefsClosure<YOUNG> cl(q, rp);
	      objArrayOop array = objArrayOop(obj);
	      int len = array->length();
	      
	      printf("%d members\n", len);
	      
	      array->oop_iterate_range(&cl, 0, len);
	    } else {
	      printf("%u: it is not an object array, so we'll do_oop() on its reference\n", worker_id);

	      oops->do_oop(&obj);
	    }
	    p += obj->size();
	  }
	  card_index = next_card_index;
	} else {
	  printf("%u: card does not have object\n", worker_id);
	  // otherwise, this card will have been scanned during scan of a previous cluster.
	  card_index++;
	}
      } else if (_scc->has_object(card_index)) {

        printf("%u: card is not dirty, but does have object\n", worker_id);

	// Scan the last object that starts within this card memory if it spans at least one dirty card within this cluster
	// or if it reaches into the next cluster. 
	uint32_t start_offset = _scc->get_last_start(card_index);
	HeapWord *p = _scc->addr_for_card_index(card_index) + start_offset;
	oop obj = oop(p);
	HeapWord *nextp = p + obj->size();
	uint32_t last_card = _scc->card_index_for_addr(nextp);
	
	bool reaches_next_cluster = (last_card > end_card_index);
	bool spans_dirty_within_this_cluster = false;

	if (!reaches_next_cluster) {
	  uint32_t span_card;
	  for (span_card = card_index+1; span_card < end_card_index; span_card++)
	    if (_scc->is_card_dirty(span_card)) {
	      spans_dirty_within_this_cluster = true;
	      break;
	    }
	}
	
	printf("%u: last_start: %u, size: %d, p: %llx, nextp: %llx, reaches_next_cluster: %d, spans_dirty_within_this_cluster: %d\n",
	       worker_id, start_offset, (unsigned) obj->size(), (unsigned long long) p, (unsigned long long) nextp,
	       reaches_next_cluster, spans_dirty_within_this_cluster);
	
	if (reaches_next_cluster || spans_dirty_within_this_cluster) {
	  if (obj->is_objArray()) {

	    printf("%u: it is an object array, so we'll iterate through its ", worker_id);

	    ShenandoahObjToScanQueue* q = cm->get_queue(worker_id); // kelvin to confirm: get_queue wants worker_id
	    ShenandoahMarkRefsClosure<YOUNG> cl(q, rp);
	    objArrayOop array = objArrayOop(obj);
	    int len = array->length();
	    
	    printf("%d members\n", len);

	    array->oop_iterate_range(&cl, 0, len);
	  } else {
	    printf("%u: is is not an object array, so we'll do_oop() on its reference\n", worker_id);
	    oops->do_oop(&obj);
	  }
	} else {
          printf("%u: does not rach next cluster and does not span dirty within this cluster\n", worker_id);
	}
	// Increment card_index to account for the spanning object, even if we didn't scan it.
	card_index = last_card;
      } else {
	printf("%u: card is not dirty, and card does not have object\n", worker_id);
	card_index++;
      }
    }
  }
  printf("Returning from process_clusters\n");
}

template<typename RememberedSet>
inline uint32_t
ShenandoahScanRemembered<RememberedSet>::cluster_for_addr(HeapWordImpl **addr) {
  uint32_t card_index = _scc->card_index_for_addr(addr);
  uint32_t result = card_index / ShenandoahCardCluster<RememberedSet>::CardsPerCluster;
  return result;
}

#endif   // SHARE_GC_SHENANDOAH_SHENANDOAHSCANREMEMBEREDINLINE_HPP
