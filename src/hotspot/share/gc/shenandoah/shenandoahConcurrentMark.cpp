/*
 * Copyright (c) 2013, 2020, Red Hat, Inc. All rights reserved.
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

#include "classfile/symbolTable.hpp"
#include "classfile/systemDictionary.hpp"
#include "code/codeCache.hpp"

#include "gc/shared/weakProcessor.inline.hpp"
#include "gc/shared/gcTimer.hpp"
#include "gc/shared/gcTrace.hpp"
#include "gc/shared/referenceProcessor.hpp"
#include "gc/shared/referenceProcessorPhaseTimes.hpp"
#include "gc/shared/strongRootsScope.hpp"

#include "gc/shenandoah/shenandoahBarrierSet.inline.hpp"
#include "gc/shenandoah/shenandoahClosures.inline.hpp"
#include "gc/shenandoah/shenandoahConcurrentMark.inline.hpp"
#include "gc/shenandoah/shenandoahMarkCompact.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahRootProcessor.inline.hpp"
#include "gc/shenandoah/shenandoahOopClosures.inline.hpp"
#include "gc/shenandoah/shenandoahPhaseTimings.hpp"
#include "gc/shenandoah/shenandoahTaskqueue.inline.hpp"
#include "gc/shenandoah/shenandoahUtils.hpp"
#include "gc/shenandoah/shenandoahScanRemembered.inline.hpp"

#include "memory/iterator.inline.hpp"
#include "memory/metaspace.hpp"
#include "memory/resourceArea.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/handles.inline.hpp"

#undef DEBUG_TRACE

template<GenerationMode GENERATION, UpdateRefsMode UPDATE_REFS>
class ShenandoahInitMarkRootsClosure : public OopClosure {
private:
  ShenandoahObjToScanQueue* _queue;
  ShenandoahHeap* _heap;
  ShenandoahMarkingContext* const _mark_context;

  template <class T>
  inline void do_oop_work(T* p) {
    ShenandoahConcurrentMark::mark_through_ref<T, GENERATION, UPDATE_REFS, NO_DEDUP>(p, _heap, _queue, _mark_context);
  }

public:
  ShenandoahInitMarkRootsClosure(ShenandoahObjToScanQueue* q) :
    _queue(q),
    _heap(ShenandoahHeap::heap()),
    _mark_context(_heap->marking_context()) {};

  void do_oop(narrowOop* p) { do_oop_work(p); }
  void do_oop(oop* p)       { do_oop_work(p); }
};

ShenandoahMarkRefsSuperClosure::ShenandoahMarkRefsSuperClosure(ShenandoahObjToScanQueue* q, ReferenceProcessor* rp) :
  MetadataVisitingOopIterateClosure(rp),
  _queue(q),
  _heap(ShenandoahHeap::heap()),
  _mark_context(_heap->marking_context())
{ }

// TODO: The C++ compiler does not accept a ShenandoahInitMarkRootsClosure in lieu of this class here,
// no matter how we shape the template parameters and iterator arguments.
// But this almost exact copy works.
template<UpdateRefsMode UPDATE_REFS>
class ShenandoahInitMarkOopClosure : public BasicOopIterateClosure {
private:
  ShenandoahObjToScanQueue* _queue;
  ShenandoahHeap* _heap;
  ShenandoahMarkingContext* const _mark_context;

  template <class T>
  inline void do_oop_work(T* p) {
    ShenandoahConcurrentMark::mark_through_ref<T, YOUNG, UPDATE_REFS, NO_DEDUP>(p, _heap, _queue, _mark_context);
  }

public:
  ShenandoahInitMarkOopClosure(ShenandoahObjToScanQueue* q) :
    _queue(q),
    _heap(ShenandoahHeap::heap()),
    _mark_context(_heap->marking_context()) {};

  void do_oop(narrowOop* p) { do_oop_work(p); }
  void do_oop(oop* p)       { do_oop_work(p); }
};

template<UpdateRefsMode UPDATE_REFS>
class ShenandoahInitMarkRootsTask : public AbstractGangTask {
private:
  ShenandoahConcurrentMark* const _scm;
  ShenandoahRootScanner* const _rp;
  uint const _nworkers;
  ShenandoahRegionIterator* _regions;
public:
  ShenandoahInitMarkRootsTask(ShenandoahConcurrentMark* scm, ShenandoahRootScanner* rp, uint nworkers, ShenandoahRegionIterator* regions) :
    AbstractGangTask("Shenandoah Init Mark Roots"),
    _scm(scm),
    _rp(rp),
    _nworkers(nworkers),
    _regions(regions) {
  }

  void work(uint worker_id) {
    assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");
    ShenandoahParallelWorkerSession worker_session(worker_id);

    ShenandoahHeap* heap = ShenandoahHeap::heap();
    ShenandoahObjToScanQueueSet* queues = _scm->task_queues();
    assert(queues->get_reserved() > worker_id, "Queue has not been reserved for worker id: %d", worker_id);

    ShenandoahObjToScanQueue* q = queues->queue(worker_id);

    switch (_scm->generation_mode()) {
      case YOUNG: {
        ShenandoahInitMarkRootsClosure<YOUNG, UPDATE_REFS> mark_cl(q);

        if (!ShenandoahUseSimpleCardScanning) {
          // Do the remembered set scanning before the root scanning as the current implementation of remembered set scanning
          // does not do workload balancing.  If certain worker threads end up with disproportionate amounts of remembered set
          // scanning effort, the subsequent root scanning effort will balance workload to even effort between threads.
          uint32_t r;
          RememberedScanner *rs = heap->card_scan();
          ReferenceProcessor* rp = heap->ref_processor();
          unsigned int total_regions = heap->num_regions();

          for (r = worker_id % _nworkers; r < total_regions; r += _nworkers) {
            ShenandoahHeapRegion *region = heap->get_region(r);
            if (region->affiliation() == OLD_GENERATION) {
              HeapWord *start_of_range = region->bottom();
              uint32_t start_cluster_no = rs->cluster_for_addr(start_of_range);

	      // region->end() represents the end of memory spanned by this region, but not all of this
	      //   memory is eligible to be scanned because some of this memory has not yet been allocated.
	      //
	      // region->top() represents the end of allocated memory within this region.  Any addresses
	      //   beyond region->top() should not be scanned as that memory does not hold valid objects.
	      HeapWord *end_of_range = region->top();

              // end_of_range may point to the middle of a cluster because region->top() may be different than region->end.
              // We want to assure that our process_clusters() request spans all relevant clusters.  Note that each cluster
              // processed will avoid processing beyond end_of_range.

              size_t num_heapwords = end_of_range - start_of_range;
              uint32_t num_clusters = (uint32_t)
                  ((num_heapwords - 1 + CardTable::card_size_in_words * ShenandoahCardCluster<ShenandoahDirectCardMarkRememberedSet>::CardsPerCluster)
                   / (CardTable::card_size_in_words * ShenandoahCardCluster<ShenandoahDirectCardMarkRememberedSet>::CardsPerCluster));

#ifdef DEBUG_TRACE
	      printf("preparing to issue invocation of process_clusters for worker: %u, start_cluster_no: %u, scanning %u clusters\n",
		     worker_id, start_cluster_no, num_clusters);
              printf("  for range: %llx to %llx\n", (unsigned long long) start_of_range, (unsigned long long) end_of_range);
              fflush(stdout);
#endif
              rs->process_clusters<ShenandoahInitMarkRootsClosure<YOUNG, UPDATE_REFS>>(worker_id, rp, _scm, start_cluster_no,
										       num_clusters, end_of_range, &mark_cl);
            }
          }
        }

        do_work(heap, &mark_cl, worker_id);

        if (ShenandoahUseSimpleCardScanning) {
          ShenandoahInitMarkOopClosure<UPDATE_REFS> mark_oop_cl(q);
          ShenandoahHeapRegion* r = _regions->next();
          while (r != NULL) {
            if (r->affiliation() == OLD_GENERATION && ShenandoahBarrierSet::barrier_set()->card_table()->is_dirty(MemRegion(r->bottom(), r->top()))) {
              r->oop_iterate(&mark_oop_cl);
            }
            r = _regions->next();
          }
        }
        break;
      }
      case GLOBAL: {
        ShenandoahInitMarkRootsClosure<GLOBAL, UPDATE_REFS> mark_cl(q);
        do_work(heap, &mark_cl, worker_id);
        break;
      }
      default: {
        ShouldNotReachHere();
        break;
      }
    }
  }

private:
  void do_work(ShenandoahHeap* heap, OopClosure* oops, uint worker_id) {
    _rp->roots_do(worker_id, oops);
  }
};

class ShenandoahUpdateRootsTask : public AbstractGangTask {
private:
  ShenandoahRootUpdater*  _root_updater;
  bool                    _check_alive;
public:
  ShenandoahUpdateRootsTask(ShenandoahRootUpdater* root_updater, bool check_alive) :
    AbstractGangTask("Shenandoah Update Roots"),
    _root_updater(root_updater),
    _check_alive(check_alive){
  }

  void work(uint worker_id) {
    assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");
    ShenandoahParallelWorkerSession worker_session(worker_id);

    ShenandoahHeap* heap = ShenandoahHeap::heap();
    ShenandoahUpdateRefsClosure cl;
    if (_check_alive) {
      ShenandoahForwardedIsAliveClosure is_alive;
      _root_updater->roots_do<ShenandoahForwardedIsAliveClosure, ShenandoahUpdateRefsClosure>(worker_id, &is_alive, &cl);
    } else {
      AlwaysTrueClosure always_true;;
      _root_updater->roots_do<AlwaysTrueClosure, ShenandoahUpdateRefsClosure>(worker_id, &always_true, &cl);
    }
  }
};

class ShenandoahConcurrentMarkingTask : public AbstractGangTask {
private:
  ShenandoahConcurrentMark* _cm;
  TaskTerminator* _terminator;

public:
  ShenandoahConcurrentMarkingTask(ShenandoahConcurrentMark* cm, TaskTerminator* terminator) :
    AbstractGangTask("Shenandoah Concurrent Mark"), _cm(cm), _terminator(terminator) {
  }

  void work(uint worker_id) {
    ShenandoahHeap* heap = ShenandoahHeap::heap();
    ShenandoahConcurrentWorkerSession worker_session(worker_id);
    ShenandoahSuspendibleThreadSetJoiner stsj(ShenandoahSuspendibleWorkers);
    ShenandoahObjToScanQueue* q = _cm->get_queue(worker_id);
    ReferenceProcessor* rp;
    if (heap->process_references()) {
      rp = heap->ref_processor();
      shenandoah_assert_rp_isalive_installed();
    } else {
      rp = NULL;
    }

    _cm->mark_loop(worker_id, _terminator, rp,
                   true, // cancellable
                   ShenandoahStringDedup::is_enabled()); // perform string dedup
  }
};

template <GenerationMode GENERATION>
class ShenandoahSATBAndRemarkCodeRootsThreadsClosure : public ThreadClosure {
private:
  ShenandoahSATBBufferClosure<GENERATION>* _satb_cl;
  OopClosure*            const _cl;
  MarkingCodeBlobClosure*      _code_cl;
  uintx _claim_token;

public:
  ShenandoahSATBAndRemarkCodeRootsThreadsClosure(ShenandoahSATBBufferClosure<GENERATION>* satb_cl, OopClosure* cl, MarkingCodeBlobClosure* code_cl) :
    _satb_cl(satb_cl), _cl(cl), _code_cl(code_cl),
    _claim_token(Threads::thread_claim_token()) {}

  void do_thread(Thread* thread) {
    if (thread->claim_threads_do(true, _claim_token)) {
      ShenandoahThreadLocalData::satb_mark_queue(thread).apply_closure_and_empty(_satb_cl);
      if (thread->is_Java_thread()) {
        if (_cl != NULL) {
          ResourceMark rm;
          thread->oops_do(_cl, _code_cl);
        } else if (_code_cl != NULL) {
          // In theory it should not be neccessary to explicitly walk the nmethods to find roots for concurrent marking
          // however the liveness of oops reachable from nmethods have very complex lifecycles:
          // * Alive if on the stack of an executing method
          // * Weakly reachable otherwise
          // Some objects reachable from nmethods, such as the class loader (or klass_holder) of the receiver should be
          // live by the SATB invariant but other oops recorded in nmethods may behave differently.
          thread->as_Java_thread()->nmethods_do(_code_cl);
        }
      }
    }
  }
};

// Process concurrent roots at safepoints
template <typename T>
class ShenandoahProcessConcurrentRootsTask : public AbstractGangTask {
private:
  ShenandoahConcurrentRootScanner<false /* concurrent */> _rs;
  ShenandoahConcurrentMark* const _cm;
  ReferenceProcessor*             _rp;
public:

  ShenandoahProcessConcurrentRootsTask(ShenandoahConcurrentMark* cm,
                                       ShenandoahPhaseTimings::Phase phase,
                                       uint nworkers);
  void work(uint worker_id);
};

template <typename T>
ShenandoahProcessConcurrentRootsTask<T>::ShenandoahProcessConcurrentRootsTask(ShenandoahConcurrentMark* cm,
                                                                              ShenandoahPhaseTimings::Phase phase,
                                                                              uint nworkers) :
  AbstractGangTask("Shenandoah Process Concurrent Roots"),
  _rs(nworkers, phase),
  _cm(cm),
  _rp(NULL) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  if (heap->process_references()) {
    _rp = heap->ref_processor();
    shenandoah_assert_rp_isalive_installed();
  }
}

template <typename T>
void ShenandoahProcessConcurrentRootsTask<T>::work(uint worker_id) {
  ShenandoahParallelWorkerSession worker_session(worker_id);
  ShenandoahObjToScanQueue* q = _cm->task_queues()->queue(worker_id);
  T cl(q, _rp);
  _rs.oops_do(&cl, worker_id);
}

template <GenerationMode GENERATION>
class ShenandoahFinalMarkingTask : public AbstractGangTask {
private:
  ShenandoahConcurrentMark* _cm;
  TaskTerminator*           _terminator;
  bool                      _dedup_string;

public:
  ShenandoahFinalMarkingTask(ShenandoahConcurrentMark* cm, TaskTerminator* terminator, bool dedup_string) :
    AbstractGangTask("Shenandoah Final Mark"), _cm(cm), _terminator(terminator), _dedup_string(dedup_string) {
  }

  void work(uint worker_id) {
    ShenandoahHeap* heap = ShenandoahHeap::heap();

    ShenandoahParallelWorkerSession worker_session(worker_id);
    ReferenceProcessor* rp;
    if (heap->process_references()) {
      rp = heap->ref_processor();
      shenandoah_assert_rp_isalive_installed();
    } else {
      rp = NULL;
    }

    // First drain remaining SATB buffers.
    // Notice that this is not strictly necessary for mark-compact. But since
    // it requires a StrongRootsScope around the task, we need to claim the
    // threads, and performance-wise it doesn't really matter. Adds about 1ms to
    // full-gc.
    {
      ShenandoahObjToScanQueue* q = _cm->get_queue(worker_id);

      ShenandoahSATBBufferClosure<GENERATION> cl(q);
      SATBMarkQueueSet& satb_mq_set = ShenandoahBarrierSet::satb_mark_queue_set();
      while (satb_mq_set.apply_closure_to_completed_buffer(&cl));
      bool do_nmethods = heap->unload_classes() && !ShenandoahConcurrentRoots::can_do_concurrent_class_unloading();
      if (heap->has_forwarded_objects()) {
        ShenandoahMarkResolveRefsClosure<GENERATION> resolve_mark_cl(q, rp);
        MarkingCodeBlobClosure blobsCl(&resolve_mark_cl, !CodeBlobToOopClosure::FixRelocations);
        ShenandoahSATBAndRemarkCodeRootsThreadsClosure<GENERATION> tc(&cl,
                                                                      ShenandoahStoreValEnqueueBarrier ? &resolve_mark_cl : NULL,
                                                                      do_nmethods ? &blobsCl : NULL);
        Threads::threads_do(&tc);
      } else {
        ShenandoahMarkRefsClosure<GENERATION> mark_cl(q, rp);
        MarkingCodeBlobClosure blobsCl(&mark_cl, !CodeBlobToOopClosure::FixRelocations);
        ShenandoahSATBAndRemarkCodeRootsThreadsClosure<GENERATION> tc(&cl,
                                                                      ShenandoahStoreValEnqueueBarrier ? &mark_cl : NULL,
                                                                      do_nmethods ? &blobsCl : NULL);
        Threads::threads_do(&tc);
      }
    }

    _cm->mark_loop(worker_id, _terminator, rp,
                   false, // not cancellable
                   _dedup_string);

    assert(_cm->task_queues()->is_empty(), "Should be empty");
  }
};

void ShenandoahConcurrentMark::mark_roots(GenerationMode generation, ShenandoahPhaseTimings::Phase root_phase) {
  assert(Thread::current()->is_VM_thread(), "can only do this in VMThread");
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");

  ShenandoahHeap* heap = ShenandoahHeap::heap();

  ShenandoahGCPhase phase(root_phase);

  WorkGang* workers = heap->workers();
  uint nworkers = workers->active_workers();

  assert(nworkers <= task_queues()->size(), "Just check");

  ShenandoahRootScanner root_proc(nworkers, root_phase);
  TASKQUEUE_STATS_ONLY(task_queues()->reset_taskqueue_stats());
  task_queues()->reserve(nworkers);

  if (heap->has_forwarded_objects()) {
    ShenandoahRegionIterator regions;
    ShenandoahInitMarkRootsTask<RESOLVE> mark_roots(this, &root_proc, nworkers, &regions);
    workers->run_task(&mark_roots);
  } else {
    // No need to update references, which means the heap is stable.
    // Can save time not walking through forwarding pointers.
    ShenandoahRegionIterator regions;
    ShenandoahInitMarkRootsTask<NONE> mark_roots(this, &root_proc, nworkers, &regions);
    workers->run_task(&mark_roots);
  }
}

void ShenandoahConcurrentMark::update_roots(ShenandoahPhaseTimings::Phase root_phase) {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");
  assert(root_phase == ShenandoahPhaseTimings::full_gc_update_roots ||
         root_phase == ShenandoahPhaseTimings::degen_gc_update_roots,
         "Only for these phases");

  ShenandoahGCPhase phase(root_phase);

  bool check_alive = root_phase == ShenandoahPhaseTimings::degen_gc_update_roots;

#if COMPILER2_OR_JVMCI
  DerivedPointerTable::clear();
#endif

  uint nworkers = _heap->workers()->active_workers();

  ShenandoahRootUpdater root_updater(nworkers, root_phase);
  ShenandoahUpdateRootsTask update_roots(&root_updater, check_alive);
  _heap->workers()->run_task(&update_roots);

#if COMPILER2_OR_JVMCI
  DerivedPointerTable::update_pointers();
#endif
}

class ShenandoahUpdateThreadRootsTask : public AbstractGangTask {
private:
  ShenandoahThreadRoots           _thread_roots;
  ShenandoahPhaseTimings::Phase   _phase;
  ShenandoahGCWorkerPhase         _worker_phase;
public:
  ShenandoahUpdateThreadRootsTask(bool is_par, ShenandoahPhaseTimings::Phase phase) :
    AbstractGangTask("Shenandoah Update Thread Roots"),
    _thread_roots(phase, is_par),
    _phase(phase),
    _worker_phase(phase) {}

  void work(uint worker_id) {
    ShenandoahParallelWorkerSession worker_session(worker_id);
    ShenandoahUpdateRefsClosure cl;
    _thread_roots.oops_do(&cl, NULL, worker_id);
  }
};

void ShenandoahConcurrentMark::update_thread_roots(ShenandoahPhaseTimings::Phase root_phase) {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");

  ShenandoahGCPhase phase(root_phase);

#if COMPILER2_OR_JVMCI
  DerivedPointerTable::clear();
#endif

  WorkGang* workers = _heap->workers();
  bool is_par = workers->active_workers() > 1;

  ShenandoahUpdateThreadRootsTask task(is_par, root_phase);
  workers->run_task(&task);

#if COMPILER2_OR_JVMCI
  DerivedPointerTable::update_pointers();
#endif
}

void ShenandoahConcurrentMark::initialize(uint workers) {
  _heap = ShenandoahHeap::heap();

  uint num_queues = MAX2(workers, 1U);

  _task_queues = new ShenandoahObjToScanQueueSet((int) num_queues);

  for (uint i = 0; i < num_queues; ++i) {
    ShenandoahObjToScanQueue* task_queue = new ShenandoahObjToScanQueue();
    task_queue->initialize();
    _task_queues->register_queue(i, task_queue);
  }
}

// Mark concurrent roots during concurrent phases
class ShenandoahMarkConcurrentRootsTask : public AbstractGangTask {
private:
  ShenandoahConcurrentMark*          _scm;
  SuspendibleThreadSetJoiner         _sts_joiner;
  ShenandoahConcurrentRootScanner<true /* concurrent */> _rs;
  ShenandoahObjToScanQueueSet* const _queue_set;
  ReferenceProcessor* const          _rp;

public:
  ShenandoahMarkConcurrentRootsTask(ShenandoahConcurrentMark* scm,
                                    ShenandoahObjToScanQueueSet* qs,
                                    ReferenceProcessor* rp,
                                    ShenandoahPhaseTimings::Phase phase,
                                    uint nworkers);
  void work(uint worker_id);
};

ShenandoahMarkConcurrentRootsTask::ShenandoahMarkConcurrentRootsTask(ShenandoahConcurrentMark* scm,
                                                                     ShenandoahObjToScanQueueSet* qs,
                                                                     ReferenceProcessor* rp,
                                                                     ShenandoahPhaseTimings::Phase phase,
                                                                     uint nworkers) :
  AbstractGangTask("Shenandoah Concurrent Mark Roots"),
  _scm(scm),
  _rs(nworkers, phase),
  _queue_set(qs),
  _rp(rp) {
  assert(!ShenandoahHeap::heap()->has_forwarded_objects(), "Not expected");
}

void ShenandoahMarkConcurrentRootsTask::work(uint worker_id) {
  ShenandoahConcurrentWorkerSession worker_session(worker_id);
  ShenandoahObjToScanQueue* q = _queue_set->queue(worker_id);
  switch (_scm->generation_mode()) {
    case YOUNG: {
      ShenandoahMarkResolveRefsClosure<YOUNG> cl(q, _rp);
      _rs.oops_do(&cl, worker_id);
      break;
    }
    case GLOBAL: {
      ShenandoahMarkResolveRefsClosure<GLOBAL> cl(q, _rp);
      _rs.oops_do(&cl, worker_id);
      break;
    }
    default: {
      ShouldNotReachHere();
      break;
    }
  }
}

void ShenandoahConcurrentMark::mark_from_roots() {
  WorkGang* workers = _heap->workers();
  uint nworkers = workers->active_workers();

  ReferenceProcessor* rp = NULL;
  if (_heap->process_references()) {
    rp = _heap->ref_processor();
    rp->set_active_mt_degree(nworkers);

    // enable ("weak") refs discovery
    rp->enable_discovery(true /*verify_no_refs*/);
    rp->setup_policy(_heap->soft_ref_policy()->should_clear_all_soft_refs());
  }

  shenandoah_assert_rp_isalive_not_installed();
  ShenandoahIsAliveSelector is_alive;
  ReferenceProcessorIsAliveMutator fix_isalive(_heap->ref_processor(), is_alive.is_alive_closure());

  task_queues()->reserve(nworkers);

  {
    ShenandoahGCPhase phase(ShenandoahPhaseTimings::conc_mark_roots);
    // Use separate task to mark concurrent roots, since it may hold ClassLoaderData_lock and CodeCache_lock
    ShenandoahMarkConcurrentRootsTask task(this, task_queues(), rp, ShenandoahPhaseTimings::conc_mark_roots, nworkers);
    workers->run_task(&task);
  }

  {
    TaskTerminator terminator(nworkers, task_queues());
    ShenandoahConcurrentMarkingTask task(this, &terminator);
    workers->run_task(&task);
  }

  assert(task_queues()->is_empty() || _heap->cancelled_gc(), "Should be empty when not cancelled");
}

void ShenandoahConcurrentMark::finish_mark_from_roots(bool full_gc) {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");

  uint nworkers = _heap->workers()->active_workers();

  {
    shenandoah_assert_rp_isalive_not_installed();
    ShenandoahIsAliveSelector is_alive;
    ReferenceProcessorIsAliveMutator fix_isalive(_heap->ref_processor(), is_alive.is_alive_closure());

    // Full GC does not execute concurrent cycle. Degenerated cycle may bypass concurrent cycle.
    // In those cases, concurrent roots might not be scanned, scan them here. Ideally, this
    // should piggyback to ShenandoahFinalMarkingTask, but it makes time tracking very hard.
    // Given full GC and degenerated GC are rare, use a separate task.
    if (_heap->is_degenerated_gc_in_progress() || _heap->is_full_gc_in_progress()) {
      ShenandoahPhaseTimings::Phase phase = _heap->is_full_gc_in_progress() ?
                                            ShenandoahPhaseTimings::full_gc_scan_conc_roots :
                                            ShenandoahPhaseTimings::degen_gc_scan_conc_roots;
      ShenandoahGCPhase gc_phase(phase);
      switch (generation_mode()) {
         case YOUNG: {
           if (_heap->has_forwarded_objects()) {
             ShenandoahProcessConcurrentRootsTask<ShenandoahMarkResolveRefsClosure<YOUNG>> task(this, phase, nworkers);
             _heap->workers()->run_task(&task);
           } else {
             ShenandoahProcessConcurrentRootsTask<ShenandoahMarkRefsClosure<YOUNG>> task(this, phase, nworkers);
             _heap->workers()->run_task(&task);
           }
           break;
         }
         case GLOBAL: {
           if (_heap->has_forwarded_objects()) {
             ShenandoahProcessConcurrentRootsTask<ShenandoahMarkResolveRefsClosure<GLOBAL>> task(this, phase, nworkers);
             _heap->workers()->run_task(&task);
           } else {
             ShenandoahProcessConcurrentRootsTask<ShenandoahMarkRefsClosure<GLOBAL>> task(this, phase, nworkers);
             _heap->workers()->run_task(&task);
           }
           break;
         }
         default: {
           ShouldNotReachHere();
           break;
         }
      }
    }

    // Finally mark everything else we've got in our queues during the previous steps.
    // It does two different things for concurrent vs. mark-compact GC:
    // - For concurrent GC, it starts with empty task queues, drains the remaining
    //   SATB buffers, and then completes the marking closure.
    // - For mark-compact GC, it starts out with the task queues seeded by initial
    //   root scan, and completes the closure, thus marking through all live objects
    // The implementation is the same, so it's shared here.
    {
      ShenandoahGCPhase phase(full_gc ?
                              ShenandoahPhaseTimings::full_gc_mark_finish_queues :
                              ShenandoahPhaseTimings::finish_queues);
      task_queues()->reserve(nworkers);

      StrongRootsScope scope(nworkers);
      TaskTerminator terminator(nworkers, task_queues());
      switch (generation_mode()) {
        case YOUNG: {
          ShenandoahFinalMarkingTask<YOUNG> task(this, &terminator, ShenandoahStringDedup::is_enabled());
          _heap->workers()->run_task(&task);
          break;
        }
        case GLOBAL: {
          ShenandoahFinalMarkingTask<GLOBAL> task(this, &terminator, ShenandoahStringDedup::is_enabled());
          _heap->workers()->run_task(&task);
          break;
        }
        default: {
          ShouldNotReachHere();
          break;
        }
      }
    }

    assert(task_queues()->is_empty(), "Should be empty");
  }

  // When we're done marking everything, we process weak references.
  if (_heap->process_references()) {
    weak_refs_work(full_gc);
  }

  assert(task_queues()->is_empty(), "Should be empty");
  TASKQUEUE_STATS_ONLY(task_queues()->print_taskqueue_stats());
  TASKQUEUE_STATS_ONLY(task_queues()->reset_taskqueue_stats());
}

// Weak Reference Closures
class ShenandoahCMDrainMarkingStackClosure: public VoidClosure {
  ShenandoahConcurrentMark* _scm;
  uint _worker_id;
  TaskTerminator* _terminator;
  bool _reset_terminator;

public:
  ShenandoahCMDrainMarkingStackClosure(ShenandoahConcurrentMark* scm, uint worker_id, TaskTerminator* t, bool reset_terminator = false) :
    _scm(scm),
    _worker_id(worker_id),
    _terminator(t),
    _reset_terminator(reset_terminator) {
  }

  void do_void() {
    assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");

    ShenandoahHeap* sh = ShenandoahHeap::heap();
    assert(sh->process_references(), "why else would we be here?");
    ReferenceProcessor* rp = sh->ref_processor();

    shenandoah_assert_rp_isalive_installed();

    _scm->mark_loop(_worker_id, _terminator, rp,
                    false,   // not cancellable
                    false);  // do not do strdedup

    if (_reset_terminator) {
      _terminator->reset_for_reuse();
    }
  }
};

template <GenerationMode GENERATION>
class ShenandoahCMKeepAliveClosure : public OopClosure {
private:
  ShenandoahObjToScanQueue* _queue;
  ShenandoahHeap* _heap;
  ShenandoahMarkingContext* const _mark_context;

  template <class T>
  inline void do_oop_work(T* p) {
    ShenandoahConcurrentMark::mark_through_ref<T, GENERATION, NONE, NO_DEDUP>(p, _heap, _queue, _mark_context);
  }

public:
  ShenandoahCMKeepAliveClosure(ShenandoahObjToScanQueue* q) :
    _queue(q),
    _heap(ShenandoahHeap::heap()),
    _mark_context(_heap->marking_context()) {}

  void do_oop(narrowOop* p) { do_oop_work(p); }
  void do_oop(oop* p)       { do_oop_work(p); }
};

template <GenerationMode GENERATION>
class ShenandoahCMKeepAliveUpdateClosure : public OopClosure {
private:
  ShenandoahObjToScanQueue* _queue;
  ShenandoahHeap* _heap;
  ShenandoahMarkingContext* const _mark_context;

  template <class T>
  inline void do_oop_work(T* p) {
    ShenandoahConcurrentMark::mark_through_ref<T, GENERATION, SIMPLE, NO_DEDUP>(p, _heap, _queue, _mark_context);
  }

public:
  ShenandoahCMKeepAliveUpdateClosure(ShenandoahObjToScanQueue* q) :
    _queue(q),
    _heap(ShenandoahHeap::heap()),
    _mark_context(_heap->marking_context()) {}

  void do_oop(narrowOop* p) { do_oop_work(p); }
  void do_oop(oop* p)       { do_oop_work(p); }
};

class ShenandoahWeakUpdateClosure : public OopClosure {
private:
  ShenandoahHeap* const _heap;

  template <class T>
  inline void do_oop_work(T* p) {
    oop o = _heap->maybe_update_with_forwarded(p);
    shenandoah_assert_marked_except(p, o, o == NULL);
  }

public:
  ShenandoahWeakUpdateClosure() : _heap(ShenandoahHeap::heap()) {}

  void do_oop(narrowOop* p) { do_oop_work(p); }
  void do_oop(oop* p)       { do_oop_work(p); }
};

class ShenandoahRefProcTaskProxy : public AbstractGangTask {
private:
  ShenandoahConcurrentMark* _scm;
  AbstractRefProcTaskExecutor::ProcessTask& _proc_task;
  TaskTerminator* _terminator;

public:
  ShenandoahRefProcTaskProxy(ShenandoahConcurrentMark* scm,
                             AbstractRefProcTaskExecutor::ProcessTask& proc_task,
                             TaskTerminator* t) :
    AbstractGangTask("Shenandoah Process Weak References"),
    _scm(scm),
    _proc_task(proc_task),
    _terminator(t) {
  }

  void work(uint worker_id) {
    Thread* current_thread = Thread::current();
    ResourceMark rm(current_thread);
    HandleMark hm(current_thread);
    assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");
    ShenandoahHeap* heap = ShenandoahHeap::heap();
    ShenandoahParallelWorkerSession worker_session(worker_id);
    ShenandoahCMDrainMarkingStackClosure complete_gc(_scm, worker_id, _terminator);
    if (heap->has_forwarded_objects()) {
      ShenandoahForwardedIsAliveClosure is_alive;
      ShenandoahCMKeepAliveUpdateClosure<GLOBAL> keep_alive(_scm->get_queue(worker_id));
      _proc_task.work(worker_id, is_alive, keep_alive, complete_gc);
    } else {
      ShenandoahIsAliveClosure is_alive;
      ShenandoahCMKeepAliveClosure<GLOBAL> keep_alive(_scm->get_queue(worker_id));
      _proc_task.work(worker_id, is_alive, keep_alive, complete_gc);
    }
  }
};

class ShenandoahRefProcTaskExecutor : public AbstractRefProcTaskExecutor {
private:
  ShenandoahConcurrentMark* _scm;
  WorkGang* _workers;

public:
  ShenandoahRefProcTaskExecutor(ShenandoahConcurrentMark* scm, WorkGang* workers) :
    _scm(scm),
    _workers(workers) {
  }

  // Executes a task using worker threads.
  void execute(ProcessTask& task, uint ergo_workers) {
    assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");

    ShenandoahPushWorkerQueuesScope scope(_workers,
                                          _scm->task_queues(),
                                          ergo_workers,
                                          /* do_check = */ false);
    uint nworkers = _workers->active_workers();
    _scm->task_queues()->reserve(nworkers);
    TaskTerminator terminator(nworkers, _scm->task_queues());
    ShenandoahRefProcTaskProxy proc_task_proxy(_scm, task, &terminator);
    _workers->run_task(&proc_task_proxy);
  }
};

void ShenandoahConcurrentMark::weak_refs_work(bool full_gc) {
  assert(_heap->process_references(), "sanity");

  ShenandoahPhaseTimings::Phase phase_root =
          full_gc ?
          ShenandoahPhaseTimings::full_gc_weakrefs :
          ShenandoahPhaseTimings::weakrefs;

  ShenandoahGCPhase phase(phase_root);

  ReferenceProcessor* rp = _heap->ref_processor();

  // NOTE: We cannot shortcut on has_discovered_references() here, because
  // we will miss marking JNI Weak refs then, see implementation in
  // ReferenceProcessor::process_discovered_references.
  weak_refs_work_doit(full_gc);

  rp->verify_no_references_recorded();
  assert(!rp->discovery_enabled(), "Post condition");
}

void ShenandoahConcurrentMark::weak_refs_work_doit(bool full_gc) {
  ReferenceProcessor* rp = _heap->ref_processor();

  ShenandoahPhaseTimings::Phase phase_process =
          full_gc ?
          ShenandoahPhaseTimings::full_gc_weakrefs_process :
          ShenandoahPhaseTimings::weakrefs_process;

  shenandoah_assert_rp_isalive_not_installed();
  ShenandoahIsAliveSelector is_alive;
  ReferenceProcessorIsAliveMutator fix_isalive(rp, is_alive.is_alive_closure());

  WorkGang* workers = _heap->workers();
  uint nworkers = workers->active_workers();

  rp->setup_policy(_heap->soft_ref_policy()->should_clear_all_soft_refs());
  rp->set_active_mt_degree(nworkers);

  assert(task_queues()->is_empty(), "Should be empty");

  // complete_gc and keep_alive closures instantiated here are only needed for
  // single-threaded path in RP. They share the queue 0 for tracking work, which
  // simplifies implementation. Since RP may decide to call complete_gc several
  // times, we need to be able to reuse the terminator.
  uint serial_worker_id = 0;
  TaskTerminator terminator(1, task_queues());
  ShenandoahCMDrainMarkingStackClosure complete_gc(this, serial_worker_id, &terminator, /* reset_terminator = */ true);
  ShenandoahRefProcTaskExecutor executor(this, workers);

  ReferenceProcessorPhaseTimes pt(_heap->gc_timer(), rp->num_queues());

  {
    // Note: Don't emit JFR event for this phase, to avoid overflow nesting phase level.
    // Reference Processor emits 2 levels JFR event, that can get us over the JFR
    // event nesting level limits, in case of degenerated GC gets upgraded to
    // full GC.
    ShenandoahTimingsTracker phase_timing(phase_process);

    if (_heap->has_forwarded_objects()) {
      ShenandoahCMKeepAliveUpdateClosure<GLOBAL> keep_alive(get_queue(serial_worker_id));
      const ReferenceProcessorStats& stats =
        rp->process_discovered_references(is_alive.is_alive_closure(), &keep_alive,
                                          &complete_gc, &executor,
                                          &pt);
       _heap->tracer()->report_gc_reference_stats(stats);
    } else {
      ShenandoahCMKeepAliveClosure<GLOBAL> keep_alive(get_queue(serial_worker_id));
      const ReferenceProcessorStats& stats =
        rp->process_discovered_references(is_alive.is_alive_closure(), &keep_alive,
                                          &complete_gc, &executor,
                                          &pt);
      _heap->tracer()->report_gc_reference_stats(stats);
    }

    pt.print_all_references();

    assert(task_queues()->is_empty(), "Should be empty");
  }
}

class ShenandoahCancelledGCYieldClosure : public YieldClosure {
private:
  ShenandoahHeap* const _heap;
public:
  ShenandoahCancelledGCYieldClosure() : _heap(ShenandoahHeap::heap()) {};
  virtual bool should_return() { return _heap->cancelled_gc(); }
};

class ShenandoahPrecleanCompleteGCClosure : public VoidClosure {
private:
  ShenandoahConcurrentMark* _scm;
public:
  ShenandoahPrecleanCompleteGCClosure(ShenandoahConcurrentMark* scm) : _scm(scm) { }

  void do_void() {
    ShenandoahHeap* sh = ShenandoahHeap::heap();
    assert(sh->process_references(), "why else would we be here?");
    TaskTerminator terminator(1, _scm->task_queues());

    ReferenceProcessor* rp = sh->ref_processor();
    shenandoah_assert_rp_isalive_installed();

    _scm->mark_loop(0, &terminator, rp,
                    false, // not cancellable
                    false); // do not do strdedup
  }
};

class ShenandoahPrecleanTask : public AbstractGangTask {
private:
  ShenandoahConcurrentMark* _scm;
  ReferenceProcessor* _rp;

public:
  ShenandoahPrecleanTask(ShenandoahConcurrentMark* scm, ReferenceProcessor* rp) :
          AbstractGangTask("Shenandoah Precleaning"),
          _scm(scm),
          _rp(rp) {}

  void work(uint worker_id) {
    assert(worker_id == 0, "The code below is single-threaded, only one worker is expected");
    ShenandoahParallelWorkerSession worker_session(worker_id);

    assert(!ShenandoahHeap::heap()->has_forwarded_objects(), "No forwarded objects expected here");

    ShenandoahObjToScanQueue* q = _scm->get_queue(worker_id);

    ShenandoahCancelledGCYieldClosure yield;
    ShenandoahPrecleanCompleteGCClosure complete_gc(_scm);

    ShenandoahIsAliveClosure is_alive;
    ResourceMark rm;
    switch (_scm->generation_mode()) {
      case YOUNG: {
        ShenandoahCMKeepAliveClosure<YOUNG> keep_alive(q);
        _rp->preclean_discovered_references(&is_alive, &keep_alive,
                                            &complete_gc, &yield,
                                            NULL);
        break;
      }
      case GLOBAL: {
        ShenandoahCMKeepAliveClosure<GLOBAL> keep_alive(q);
        ResourceMark rm;
        _rp->preclean_discovered_references(&is_alive, &keep_alive,
                                            &complete_gc, &yield,
                                            NULL);
        break;
      }
      default: {
        ShouldNotReachHere();
        break;
      }
    }
  }
};

void ShenandoahConcurrentMark::preclean_weak_refs() {
  // Pre-cleaning weak references before diving into STW makes sense at the
  // end of concurrent mark. This will filter out the references which referents
  // are alive. Note that ReferenceProcessor already filters out these on reference
  // discovery, and the bulk of work is done here. This phase processes leftovers
  // that missed the initial filtering, i.e. when referent was marked alive after
  // reference was discovered by RP.

  assert(_heap->process_references(), "sanity");

  // Shortcut if no references were discovered to avoid winding up threads.
  ReferenceProcessor* rp = _heap->ref_processor();
  if (!rp->has_discovered_references()) {
    return;
  }

  assert(task_queues()->is_empty(), "Should be empty");

  ReferenceProcessorMTDiscoveryMutator fix_mt_discovery(rp, false);

  shenandoah_assert_rp_isalive_not_installed();
  ShenandoahIsAliveSelector is_alive;
  ReferenceProcessorIsAliveMutator fix_isalive(rp, is_alive.is_alive_closure());

  // Execute precleaning in the worker thread: it will give us GCLABs, String dedup
  // queues and other goodies. When upstream ReferenceProcessor starts supporting
  // parallel precleans, we can extend this to more threads.
  WorkGang* workers = _heap->workers();
  uint nworkers = workers->active_workers();
  assert(nworkers == 1, "This code uses only a single worker");
  task_queues()->reserve(nworkers);

  ShenandoahPrecleanTask task(this, rp);
  workers->run_task(&task);

  assert(task_queues()->is_empty(), "Should be empty");
}

void ShenandoahConcurrentMark::cancel() {
  // Clean up marking stacks.
  ShenandoahObjToScanQueueSet* queues = task_queues();
  queues->clear();

  // Cancel SATB buffers.
  ShenandoahBarrierSet::satb_mark_queue_set().abandon_partial_marking();
}

ShenandoahObjToScanQueue* ShenandoahConcurrentMark::get_queue(uint worker_id) {
  assert(task_queues()->get_reserved() > worker_id, "No reserved queue for worker id: %d", worker_id);
  return _task_queues->queue(worker_id);
}

void ShenandoahConcurrentMark::mark_loop(uint worker_id, TaskTerminator* terminator, ReferenceProcessor *rp, bool cancellable, bool strdedup) {
  switch (generation_mode()) {
    case YOUNG: {
      if (cancellable) {
        mark_loop_prework<YOUNG, true>(worker_id, terminator, rp, strdedup);
      } else {
        mark_loop_prework<YOUNG, false>(worker_id, terminator, rp, strdedup);
      }
      break;
    }
    case GLOBAL: {
      if (cancellable) {
        mark_loop_prework<GLOBAL, true>(worker_id, terminator, rp, strdedup);
      } else {
        mark_loop_prework<GLOBAL, false>(worker_id, terminator, rp, strdedup);
      }
      break;
    }
    default: {
      ShouldNotReachHere();
      break;
    }
  }
}
template <GenerationMode GENERATION, bool CANCELLABLE>
void ShenandoahConcurrentMark::mark_loop_prework(uint w, TaskTerminator *t, ReferenceProcessor *rp,
                                                 bool strdedup) {
  ShenandoahObjToScanQueue* q = get_queue(w);

  ShenandoahLiveData* ld = _heap->get_liveness_cache(w);

  // TODO: We can clean up this if we figure out how to do templated oop closures that
  // play nice with specialized_oop_iterators.
  if (_heap->unload_classes()) {
    if (_heap->has_forwarded_objects()) {
      if (strdedup) {
        ShenandoahMarkUpdateRefsMetadataDedupClosure<GENERATION> cl(q, rp);
        mark_loop_work<ShenandoahMarkUpdateRefsMetadataDedupClosure<GENERATION>, GENERATION, CANCELLABLE>(&cl, ld, w, t);
      } else {
        ShenandoahMarkUpdateRefsMetadataClosure<GENERATION> cl(q, rp);
        mark_loop_work<ShenandoahMarkUpdateRefsMetadataClosure<GENERATION>, GENERATION, CANCELLABLE>(&cl, ld, w, t);
      }
    } else {
      if (strdedup) {
        ShenandoahMarkRefsMetadataDedupClosure<GENERATION> cl(q, rp);
        mark_loop_work<ShenandoahMarkRefsMetadataDedupClosure<GENERATION>, GENERATION, CANCELLABLE>(&cl, ld, w, t);
      } else {
        ShenandoahMarkRefsMetadataClosure<GENERATION> cl(q, rp);
        mark_loop_work<ShenandoahMarkRefsMetadataClosure<GENERATION>, GENERATION, CANCELLABLE>(&cl, ld, w, t);
      }
    }
  } else {
    if (_heap->has_forwarded_objects()) {
      if (strdedup) {
        ShenandoahMarkUpdateRefsDedupClosure<GENERATION> cl(q, rp);
        mark_loop_work<ShenandoahMarkUpdateRefsDedupClosure<GENERATION>, GENERATION, CANCELLABLE>(&cl, ld, w, t);
      } else {
        ShenandoahMarkUpdateRefsClosure<GENERATION> cl(q, rp);
        mark_loop_work<ShenandoahMarkUpdateRefsClosure<GENERATION>, GENERATION, CANCELLABLE>(&cl, ld, w, t);
      }
    } else {
      if (strdedup) {
        ShenandoahMarkRefsDedupClosure<GENERATION> cl(q, rp);
        mark_loop_work<ShenandoahMarkRefsDedupClosure<GENERATION>, GENERATION, CANCELLABLE>(&cl, ld, w, t);
      } else {
        ShenandoahMarkRefsClosure<GENERATION> cl(q, rp);
        mark_loop_work<ShenandoahMarkRefsClosure<GENERATION>, GENERATION, CANCELLABLE>(&cl, ld, w, t);
      }
    }
  }

  _heap->flush_liveness_cache(w);
}

template <class T, GenerationMode GENERATION, bool CANCELLABLE>
void ShenandoahConcurrentMark::mark_loop_work(T* cl, ShenandoahLiveData* live_data, uint worker_id, TaskTerminator *terminator) {
  uintx stride = ShenandoahMarkLoopStride;

  ShenandoahHeap* heap = ShenandoahHeap::heap();
  ShenandoahObjToScanQueueSet* queues = task_queues();
  ShenandoahObjToScanQueue* q;
  ShenandoahMarkTask t;

  /*
   * Process outstanding queues, if any.
   *
   * There can be more queues than workers. To deal with the imbalance, we claim
   * extra queues first. Since marking can push new tasks into the queue associated
   * with this worker id, we come back to process this queue in the normal loop.
   */
  assert(queues->get_reserved() == heap->workers()->active_workers(),
         "Need to reserve proper number of queues: reserved: %u, active: %u", queues->get_reserved(), heap->workers()->active_workers());

  q = queues->claim_next();
  while (q != NULL) {
    if (CANCELLABLE && heap->check_cancelled_gc_and_yield()) {
      return;
    }

    for (uint i = 0; i < stride; i++) {
      if (q->pop(t)) {
        do_task<T>(q, cl, live_data, &t);
      } else {
        assert(q->is_empty(), "Must be empty");
        q = queues->claim_next();
        break;
      }
    }
  }
  q = get_queue(worker_id);

  ShenandoahSATBBufferClosure<GENERATION> drain_satb(q);
  SATBMarkQueueSet& satb_mq_set = ShenandoahBarrierSet::satb_mark_queue_set();

  /*
   * Normal marking loop:
   */
  while (true) {
    if (CANCELLABLE && heap->check_cancelled_gc_and_yield()) {
      return;
    }

    while (satb_mq_set.completed_buffers_num() > 0) {
      satb_mq_set.apply_closure_to_completed_buffer(&drain_satb);
    }

    uint work = 0;
    for (uint i = 0; i < stride; i++) {
      if (q->pop(t) ||
          queues->steal(worker_id, t)) {
        do_task<T>(q, cl, live_data, &t);
        work++;
      } else {
        break;
      }
    }

    if (work == 0) {
      // No work encountered in current stride, try to terminate.
      // Need to leave the STS here otherwise it might block safepoints.
      ShenandoahSuspendibleThreadSetLeaver stsl(CANCELLABLE && ShenandoahSuspendibleWorkers);
      ShenandoahTerminatorTerminator tt(heap);
      if (terminator->offer_termination(&tt)) return;
    }
  }
}
