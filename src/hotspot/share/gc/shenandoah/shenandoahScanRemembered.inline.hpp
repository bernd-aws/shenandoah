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

inline uint32_t
ShenandoahDirectCardMarkRememberedSet::cardNoForAddr(HeapWord *p) {
  return (uint32_t) (uintptr_t(p) >> _card_shift);
}

inline HeapWord *
ShenandoahDirectCardMarkRememberedSet::addrForCardNo(uint32_t card_no) {
  return _whole_heap_base + CardTable::card_size_in_words * card_no;
}

inline bool 
ShenandoahDirectCardMarkRememberedSet::isCardDirty(uint32_t card_no) {
  uint8_t *bp = &_byte_map_base[card_no];
  return (bp[0] == CardTable::dirty_card_val());
}

inline void 
ShenandoahDirectCardMarkRememberedSet::markCardAsDirty(uint32_t card_no) {
  uint8_t *bp = &_byte_map_base[card_no];
  bp[0] = CardTable::dirty_card_val();
}

inline void 
ShenandoahDirectCardMarkRememberedSet::markCardAsClean(uint32_t card_no) {
  uint8_t *bp = &_byte_map_base[card_no];
  bp[0] = CardTable::clean_card_val();
}

inline void
ShenandoahDirectCardMarkRememberedSet::markOverreachCardAsDirty(
    uint32_t card_no) {
  uint8_t *bp = &_overreach_map_base[card_no];
  bp[0] = CardTable::dirty_card_val();
}

inline bool 
ShenandoahDirectCardMarkRememberedSet::isCardDirty(HeapWord *p) {
  uint8_t *bp = &_byte_map_base[uintptr_t(p) >> _card_shift];
  return (bp[0] == CardTable::dirty_card_val());
}

inline void
ShenandoahDirectCardMarkRememberedSet::markCardAsDirty(HeapWord *p) {
  uint8_t *bp = &_byte_map_base[uintptr_t(p) >> _card_shift];
  bp[0] = CardTable::dirty_card_val();
}

inline void
ShenandoahDirectCardMarkRememberedSet::markCardAsClean(HeapWord *p) {
  uint8_t *bp = &_byte_map_base[uintptr_t(p) >> _card_shift];
  bp[0] = CardTable::clean_card_val();
}

inline void 
ShenandoahDirectCardMarkRememberedSet::markOverreachCardAsDirty(void *p) {
  uint8_t *bp = &_overreach_map_base[uintptr_t(p) >> _card_shift];
  bp[0] = CardTable::dirty_card_val();
}

inline uint32_t
ShenandoahDirectCardMarkRememberedSet::clusterCount() {
  return _cluster_count;
}

// ShenandoahBufferWithSATBRemberedSet is not currently implemented

inline uint32_t
ShenandoahBufferWithSATBRememberedSet::cardNoForAddr(HeapWord *p) {
  return 0;
}

inline HeapWord *
ShenandoahBufferWithSATBRememberedSet::addrForCardNo(uint32_t card_no) {
  return NULL;
}

inline bool
ShenandoahBufferWithSATBRememberedSet::isCardDirty(uint32_t card_no) {
  return false;
}

inline void
ShenandoahBufferWithSATBRememberedSet::markCardAsDirty(uint32_t card_no) {
}

inline void
ShenandoahBufferWithSATBRememberedSet::markCardAsClean(uint32_t card_no) {
}

inline void
ShenandoahBufferWithSATBRememberedSet::markOverreachCardAsDirty(uint32_t card_no) {
}

inline bool
ShenandoahBufferWithSATBRememberedSet::isCardDirty(HeapWord *p) {
  return false;
}


inline void
ShenandoahBufferWithSATBRememberedSet::markCardAsDirty(HeapWord *p) {
}

inline void
ShenandoahBufferWithSATBRememberedSet::markCardAsClean(HeapWord *p) {
}

inline void
ShenandoahBufferWithSATBRememberedSet::markOverreachCardAsDirty(void *p) {
}

inline uint32_t
ShenandoahBufferWithSATBRememberedSet::clusterCount() {
  return 0;
}

template<typename RememberedSet>
inline uint32_t
ShenandoahCardCluster<RememberedSet>::cardNoForAddr(HeapWord *p) {
  return _rs->cardNoForAddr(p);
}

template<typename RememberedSet>
inline bool
ShenandoahCardCluster<RememberedSet>::hasObject(uint32_t card_no) {
  HeapWord *addr = _rs->addrForCardNo(card_no);
  ShenandoahHeap *heap = ShenandoahHeap::heap();
  ShenandoahHeapRegion *region = heap->heap_region_containing(addr);
  HeapWord *obj = region->block_start(addr);
  
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
ShenandoahCardCluster<RememberedSet>::getFirstStart(uint32_t card_no) {
  HeapWord *addr = _rs->addrForCardNo(card_no);
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
ShenandoahCardCluster<RememberedSet>::getLastStart(uint32_t card_no) {
  HeapWord *addr = _rs->addrForCardNo(card_no);
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
ShenandoahCardCluster<RememberedSet>::getCrossingObjectStart(uint32_t card_no) {
  HeapWord *addr = _rs->addrForCardNo(card_no);
  uint32_t cluster_no =
      card_no / ShenandoahCardCluster<RememberedSet>::CardsPerCluster;
  HeapWord *cluster_addr = _rs->addrForCardNo(cluster_no * CardsPerCluster);

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
ShenandoahCardCluster<RememberedSet>::addrForCardNo(uint32_t card_no) {
  return _rs->addrForCardNo(card_no);
}

template<typename RememberedSet>
inline bool
ShenandoahCardCluster<RememberedSet>::isCardDirty(uint32_t card_no) {
  return _rs->isCardDirty(card_no);
}

template<typename RememberedSet>
inline void
ShenandoahCardCluster<RememberedSet>::markCardAsDirty(uint32_t card_no) {
  return _rs->markCardAsDirty(card_no);
}

template<typename RememberedSet>
inline void
ShenandoahCardCluster<RememberedSet>::markCardAsClean(uint32_t card_no) {
  return _rs->markCardAsClean(card_no);
}

template<typename RememberedSet>
inline void
ShenandoahCardCluster<RememberedSet>::markOverreachCardAsDirty(uint32_t card_no) {
  return _rs->markOverreachCardAsDirty(card_no);
}

template<typename RememberedSet>
inline bool
ShenandoahCardCluster<RememberedSet>::isCardDirty(HeapWord *p) {
  return _rs->isCardDirty(p);
}

template<typename RememberedSet>
inline void
ShenandoahCardCluster<RememberedSet>::markCardAsDirty(HeapWord *p) {
  return _rs->markCardAsDirty(p);
}

template<typename RememberedSet>
inline void
ShenandoahCardCluster<RememberedSet>::markCardAsClean(HeapWord *p) {
  return _rs->markCardAsClean(p);
}

template<typename RememberedSet>
inline void
ShenandoahCardCluster<RememberedSet>::markOverreachCardAsDirty(void *p) {
  return _rs->markOverreachCardAsDirty(p);
}

template<typename RememberedSet>
inline uint32_t
ShenandoahCardCluster<RememberedSet>::clusterCount() {
  return _rs->clusterCount();
}

template<typename RememberedSet>
inline void
ShenandoahCardCluster<RememberedSet>::initializeOverreach(uint32_t first_cluster, uint32_t count) {
  return _rs->initializeOverreach(first_cluster, count);
}

template<typename RememberedSet>
inline void
ShenandoahCardCluster<RememberedSet>::mergeOverreach(uint32_t first_cluster, uint32_t count) {
  return _rs->mergeOverreach(first_cluster, count);
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
							  uint32_t first_cluster, uint32_t count, ClosureType *oops) {

  // Unlike traditional Shenandoah marking, the old-gen resident objects that are examined as part of the remembered set are not
  // themselves marked.  Each such object will be scanned only once.  Any young-gen objects referenced from the remembered set will
  // be marked and then subsequently scanned.

  while (count-- > 0) {
    uint32_t card_no = first_cluster *
	ShenandoahCardCluster<RememberedSet>::CardsPerCluster;
    uint32_t end_card_no = card_no +
	ShenandoahCardCluster<RememberedSet>::CardsPerCluster;

    while (card_no < end_card_no) {
     if (_scc->isCardDirty(card_no)) {
       if (_scc->hasObject(card_no)) {
         // Scan all objects that start within this card region.
         uint32_t start_offset = _scc->getFirstStart(card_no);
	 HeapWord *p = _scc->addrForCardNo(card_no);
	 HeapWord *endp = p + CardTable::card_size_in_words;
	 p += start_offset;

	 while (p < endp) {
           oop obj = oop(p);

           // Future TODO:
	   // For improved efficiency, we might want to give special handling of obj->is_objArray().  In
	   // particular, in that case, we might want to divide the effort for scanning of a very long object array
	   // between multiple threads.
	   if (obj->is_objArray()) {
             ShenandoahObjToScanQueue* q = cm->get_queue(worker_id); // kelvin to confirm: get_queue wants worker_id
             ShenandoahMarkRefsClosure<YOUNG> cl(q, rp);
             objArrayOop array = objArrayOop(obj);
	     int len = array->length();
	     array->oop_iterate_range(&cl, 0, len);
	   } else
             oops->do_oop(&obj);
	   p += obj->size();
	 }
	 // p either points to start of next card region, or to the next object that needs to be scanned, which may
	 // reside in some successor card region.
	 card_no = _scc->cardNoForAddr(p);
       } else {
         // otherwise, this card will have been scanned during scan of a previous cluster.
	 card_no++;
       }
     } else if (_scc->hasObject(card_no)) {
       // Scan the last object that starts within this card memory if it spans at least one dirty card within this cluster
       // or if it reaches into the next cluster. 
       uint32_t start_offset = _scc->getFirstStart(card_no);
       HeapWord *p = _scc->addrForCardNo(card_no) + start_offset;
       oop obj = oop(p);
       HeapWord *nextp = p + obj->size();
       uint32_t last_card = _scc->cardNoForAddr(nextp);

       bool reaches_next_cluster = (last_card > end_card_no);
       bool spans_dirty_within_this_cluster = false;
       if (!reaches_next_cluster) {
          uint32_t span_card;
	  for (span_card = card_no+1; span_card < end_card_no; span_card++)
            if (_scc->isCardDirty(span_card)) {
              spans_dirty_within_this_cluster = true;
	      break;
	    }
       }
       if (reaches_next_cluster || spans_dirty_within_this_cluster) {
         if (obj->is_objArray()) {
	   ShenandoahObjToScanQueue* q = cm->get_queue(worker_id); // kelvin to confirm: get_queue wants worker_id
           ShenandoahMarkRefsClosure<YOUNG> cl(q, rp);
           objArrayOop array = objArrayOop(obj);
	   int len = array->length();
	   array->oop_iterate_range(&cl, 0, len);
	 } else
	   oops->do_oop(&obj);
       }
       // Increment card_no to account for the spanning object, even if we didn't scan it.
       card_no = last_card;
     } else
       card_no++;
    }
  }
}

template<typename RememberedSet>
inline uint32_t
ShenandoahScanRemembered<RememberedSet>::cluster_for_addr(HeapWordImpl **addr) {
  uint32_t card_no = _scc->cardNoForAddr(addr);
  return card_no / ShenandoahCardCluster<RememberedSet>::CardsPerCluster;
}

#endif   // SHARE_GC_SHENANDOAH_SHENANDOAHSCANREMEMBEREDINLINE_HPP
