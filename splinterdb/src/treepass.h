// Copyright 2018-2021 VMware, Inc.
// SPDX-License-Identifier: Apache-2.0

/*
 * treepass.h --
 *
 *     This file contains interface for the concurrent TreePass cache.
 */

#pragma once

#include "allocator.h"
#include "cache.h"
#include "io.h"

/*
 * Thread-local timestamp for current operation.
 * Set once per top-level operation (foreground query or background task)
 * to avoid repeated platform_get_timestamp() calls during cache accesses.
 */
extern _Thread_local timestamp treepass_current_ts;

// #define ADDR_TRACING
#define TRACE_ADDR  (UINT64_MAX - 1)
#define TRACE_ENTRY (UINT32_MAX - 1)

// #define RECORD_ACQUISITION_STACKS

/* how distributed the rw locks are */
#define TP_RC_WIDTH 4

/*
 * Configuration struct to setup the TreePass cache sub-system.
 */
typedef struct treepass_config {
   cache_config super;
   io_config   *io_cfg;
   uint64       capacity;
   bool32       use_stats;
   bool32       use_cache_stats; // heavy cache debug stats (eviction analysis by init_clock)
   char         logfile[MAX_STRING_LENGTH];

   // computed
   uint64 log_page_size;
   uint64 extent_mask;
   uint32 page_capacity;
   uint64 batch_capacity;
   uint64 cacheline_capacity;
   uint64 pages_per_extent;

   // For TreePass
   // EI update (EWMA) parameters
   double ei_ewma_alpha;          // 0 < a <= 1; new_ei = a*measured + (1-a)*old_ei
   uint64 ei_initial_per_gb_ns;   // initial EI seed per GB cache (ns)
} treepass_config;

typedef struct treepass       treepass;
typedef struct treepass_entry treepass_entry;

#ifdef RECORD_ACQUISITION_STACKS

// Provide a small number of backtrace history records.
#   define NUM_HISTORY_RECORDS 32

typedef struct history_record {
   uint32 status;
   int    refcount;
   void  *backtrace[NUM_HISTORY_RECORDS];
} history_record;
#endif // RECORD_ACQUISITION_STACKS


typedef uint32 entry_status; // Saved in treepass_entry->status

/*
 *-----------------------------------------------------------------------------
 * treepass_entry --
 *
 *     The meta data entry in the cache. Each entry has the underlying
 *     page_handle together with some flags.
 *-----------------------------------------------------------------------------
 */
struct treepass_entry {
   page_handle           page;
   volatile entry_status status;
   page_type             type;
   // Node class is derived from type and entry_time:
   // - BRANCH with entry_time=0 -> COLD, entry_time!=0 -> WARM
   // - FILTER -> META
   // - Others (TRUNK, MEMTABLE, etc.) -> HOT
#if TP_EVICT_STATS_DEBUG
   uint8                 initial_clock;   // clock value at load/alloc (stats)
   volatile uint32       access_count;    // cache hit count since load (stats)
#endif
   async_wait_queue      waiters;
#ifdef RECORD_ACQUISITION_STACKS
   int            next_history_record;
   history_record history[NUM_HISTORY_RECORDS];
#endif
};

/*
 *----------------------------------------------------------------------
 * treepass -- A multi-threaded cache using a hi algorithm for eviction
 *
 *      Pages are indexed by a direct mapping, cc->lookup, which is an array.
 *      For a given address, cc->lookup[addr / page_size] returns an
 *      entry_number which can be used to access the metadata and data of the
 *      page.
 *
 *      Each page in the cache has an entry cc->entry[entry_number] with:
 *         --status: flags, e.g. free, write locked, flushing, etc.
 *         --page: disk address and pointer to the page data
 *         --type: used for stats
 *
 *      Each page has a distributed ref count, accessed by
 *      treepass_[get,inc,dec]_ref(cc, entry_number, tid) and stored in
 *      cc->refcount (it is striped to avoid false sharing in certain
 *      workloads)
 *         -- a read lock is obtained by speculatively incrementing the ref
 *         count before checking the write lock bit, so the ref count should
 *         generally be treated as a lower bound.
 *
 *      Each thread has a batch of pages indicated by cc->thread_free_hand[tid]
 *      from which it draws free pages. When a thread doesn't find a free page
 *      in its batch, it obtains a pair of new batches: one to evict
 *      (from cc->free_hand) and one to clean. The batch to clean is
 *      cc->cleaner_gap batches ahead of the current evictor head, so that
 *      cleaned pages have time to flush before eviction. Both cleaning and
 *      eviction use cc->batch_busy to avoid conflicts and contention.
 *----------------------------------------------------------------------
 */
struct treepass {
   cache              super;
   treepass_config *cfg;
   allocator         *al;
   io_handle         *io;

   uint32              *lookup;
   treepass_entry    *entry;
   buffer_handle        bh;   // actual memory for pages
   char                *data; // convenience pointer for bh
   platform_log_handle *logfile;
   platform_heap_id     heap_id;

   // Distributed locks (the write bit is in the status uint32 of the entry)
   buffer_handle    rc_bh;
   volatile uint16 *refcount;
   volatile uint8  *pincount;

   // hi hands and related metadata
   volatile uint32  evict_hand;
   volatile uint32  free_hand;
   volatile bool32 *batch_busy;
   uint64           cleaner_gap;
   
   volatile uint64  evict_interval;
   volatile timestamp  last_cycle_ts;
   volatile uint32  evict_batches;
   volatile timestamp  last_boost_ts;

   // For EI update logic: track last check point
   volatile timestamp  last_ei_check_ts;
   volatile uint32     last_ei_check_cycle;

   volatile struct {
      volatile uint32 free_hand;
      bool32          enable_sync_get;
   } PLATFORM_CACHELINE_ALIGNED per_thread[MAX_THREADS];

   // Cache trace
   // Stats
   cache_stats stats[MAX_THREADS];
};

_Static_assert(MAX_READ_REFCOUNT
                  < 1ULL << (8 * sizeof(((treepass *)NULL)->refcount[0])),
               "MAX_READ_REFCOUNT too large");

/*
 *-----------------------------------------------------------------------------
 * Function declarations
 *-----------------------------------------------------------------------------
 */

void
treepass_config_init(treepass_config *cache_config,
                       io_config         *io_cfg,
                       uint64             capacity,
                       const char        *cache_logfile,
                       uint64             use_stats,
                       bool32             use_cache_stats);

platform_status
treepass_init(treepass        *cc,   // OUT
                treepass_config *cfg,  // IN
                io_handle         *io,   // IN
                allocator         *al,   // IN
                char              *name, // IN
                platform_heap_id   hid,  // IN
                platform_module_id mid); // IN

void
treepass_deinit(treepass *cc); // IN
