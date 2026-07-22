// Copyright 2018-2021 VMware, Inc.
// SPDX-License-Identifier: Apache-2.0

/*
 *-----------------------------------------------------------------------------
 * treepass.c --
 *
 *     This file contains the implementation for a concurrent clock cache.
 *-----------------------------------------------------------------------------
 */
#include "platform.h"

#include "allocator.h"
#include "treepass.h"
#include "btree.h"
#include "btree_private.h"
#include "io.h"
#include <stddef.h>
#include "util.h"
#include "btree_private.h"  // for btree_hdr (per-height stats)

#include "poison.h"

/*
 * Per-height cache hit/miss counters for branch btree pages.
 * height 0 = leaf, 1..8 = internal levels.
 * cache hit, which measurably increases hit latency.
 */
#define TP_HEIGHT_BINS (BTREE_MAX_HEIGHT + 1)  // 0..8

/*
 *-----------------------------------------------------------------------------
 * Constants and Fixed Parameters
 *-----------------------------------------------------------------------------
 */

/* invalid "pointers" used to indicate that the given page or lookup is
 * unmapped
 */
#define TP_UNMAPPED_ENTRY UINT32_MAX
#define TP_UNMAPPED_ADDR  UINT64_MAX

// Number of entries to clean/evict/get_free in a per-thread batch
#define TP_ENTRIES_PER_BATCH 64

// Number of batches that the cleaner hand is ahead of the evictor hand
#define TP_CLEANER_GAP 512

/* number of events to poll for during treepass_wait */
#define TP_DEFAULT_MAX_IO_EVENTS 1

/*
 * Thread-local timestamp used for warm/cold classification and EI tracking.
 * Declared in treepass.h for cross-file visibility.
 */
_Thread_local timestamp treepass_current_ts = 0;

/*
 * Debug counters for clock analysis
 */

/*
 * Set to 1 to skip clock updates entirely (for overhead testing)
 */
#define SKIP_CLOCK_UPDATE 0

/*
 * Set to 1 to enable pivot tracking debug counters (clock distribution).
 * Adds one __sync_fetch_and_add per alloc_compaction_hot call.
 */

/*
 * Timing breakdown macros (conditional compilation)
 */
#define TIMING_START()
#define TIMING_END_ADD(counter)
#define TIMING_COUNT_INC(counter)

/*
 *-----------------------------------------------------------------------------
 * Clockcache Operations Logging and Address Tracing
 *
 *      treepass_log, etc. are used to write an output of cache operations to
 *      a log file for debugging purposes. If TP_LOG is set, then all output is
 *      written. If ADDR_TRACING is set, then only operations which affect
 *      entries with either entry_number TRACE_ENTRY or address TRACE_ADDR are
 *      written.
 *
 *      treepass_log_stream should be called between platform_open_log_stream
 *      and platform_close_log_stream.
 *
 *      Note: these are debug functions, so calling platform_get_tid()
 *      potentially repeatedly is ok.
 *-----------------------------------------------------------------------------
 */

void
treepass_print(platform_log_handle *log_handle, treepass *cc);

#ifdef ADDR_TRACING
#   define treepass_log(addr, entry, message, ...)                           \
      do {                                                                     \
         if (addr == TRACE_ADDR || entry == TRACE_ENTRY) {                     \
            platform_handle_log(cc->logfile,                                   \
                                "(%lu) " message,                              \
                                platform_get_tid(),                            \
                                ##__VA_ARGS__);                                \
         }                                                                     \
      } while (0)
#   define treepass_log_stream(addr, entry, message, ...)                    \
      do {                                                                     \
         if (addr == TRACE_ADDR || entry == TRACE_ENTRY) {                     \
            platform_log_stream(                                               \
               "(%lu) " message, platform_get_tid(), ##__VA_ARGS__);           \
         }                                                                     \
      } while (0)
#else
#   ifdef TP_LOG
#      define treepass_log(addr, entry, message, ...)                        \
         do {                                                                  \
            (void)(addr);                                                      \
            platform_handle_log(cc->logfile,                                   \
                                "(%lu) " message,                              \
                                platform_get_tid(),                            \
                                ##__VA_ARGS__);                                \
         } while (0)

#      define treepass_log_stream(addr, entry, message, ...)                 \
         platform_log_stream(                                                  \
            "(%lu) " message, platform_get_tid(), ##__VA_ARGS__);
#   else
#      define treepass_log(addr, entry, message, ...)                        \
         do {                                                                  \
            (void)(addr);                                                      \
            (void)(entry);                                                     \
            (void)(message);                                                   \
         } while (0)
#      define treepass_log_stream(addr, entry, message, ...)                 \
         do {                                                                  \
            (void)(addr);                                                      \
            (void)(entry);                                                     \
            (void)(message);                                                   \
         } while (0)
#   endif
#endif

#if defined TP_LOG || defined ADDR_TRACING
#   define treepass_open_log_stream() platform_open_log_stream()
#else
#   define treepass_open_log_stream()
#endif

#if defined TP_LOG || defined ADDR_TRACING
#   define treepass_close_log_stream() platform_close_log_stream(cc->logfile)
#else
#   define treepass_close_log_stream()
#endif

/*
 *-----------------------------------------------------------------------------
 * treepass_entry --
 *
 *     The meta data entry in the cache. Each entry has the underlying
 *     page_handle together with some flags.
 *-----------------------------------------------------------------------------
 */

/*
 *-----------------------------------------------------------------------------
 * Definitions for entry_status (treepass_entry->status)
 *-----------------------------------------------------------------------------
 */
#define TP_FREE        (1u << 0) // entry is free
#define TP_CLEAN       (1u << 1) // page has no new changes
#define TP_WRITEBACK   (1u << 2) // page is actively in writeback
#define TP_LOADING     (1u << 3) // page is actively being read from disk
#define TP_WRITELOCKED (1u << 4) // write lock is held
#define TP_CLAIMED     (1u << 5) // claim is held

/*
 * Binary clock counter in status bits (starting at bit 6)
 * Uses atomic ADD/SUB for lock-free increment/decrement.
 * Overflow is safe because bits 10-31 are unused.
 *
 * TP_CLOCK_BITS determines the range:
 *   1-bit: 0-1 (classic CLOCK)
 *   2-bit: 0-3
 *   3-bit: 0-7
 *   4-bit: 0-15
 */
#define TP_CLOCK_SHIFT    6
#define TP_CLOCK_BITS     3   // 3 bits = values 0-7
#define TP_CLOCK_MASK     (((1u << TP_CLOCK_BITS) - 1) << TP_CLOCK_SHIFT)
#define TP_CLOCK_ONE      (1u << TP_CLOCK_SHIFT)  // increment unit
#define TP_CLOCK_MAX      ((1u << TP_CLOCK_BITS) - 1)  // max value (e.g., 3 for 2-bit)

/* Common status flag combinations */
// free entry
#define TP_FREE_STATUS (0 | TP_FREE)

// evictable unlocked page (clock == 0, clean) - clock checked separately
#define TP_EVICTABLE_STATUS (0 | TP_CLEAN)

// evictable locked page
#define TP_LOCKED_EVICTABLE_STATUS (0 | TP_CLEAN | TP_CLAIMED | TP_WRITELOCKED)

// newly allocated page (dirty, writelocked)
#define TP_ALLOC_STATUS (0 | TP_WRITELOCKED | TP_CLAIMED)

// eligible for writeback: dirty and not locked/loading
#define TP_CLEANABLE_BASE_MASK (TP_FREE | TP_CLEAN | TP_WRITEBACK | TP_LOADING \
                               | TP_WRITELOCKED | TP_CLAIMED)

// actively in writeback
#define TP_WRITEBACK_STATUS (0 | TP_WRITEBACK)

// loading for read: clock=0 (CC) or clock=1 (Baseline, like CLOCK3)
#define TP_READ_LOADING_STATUS    (0 | TP_CLEAN | TP_LOADING)
#define TP_READ_LOADING_STATUS_1  (0 | TP_CLEAN | TP_LOADING | (1 << TP_CLOCK_SHIFT))

/* entry_time tag for prefetched-but-not-yet-consumed pages: protects them
 * from the EE pass (entry_time != 0) until the consumer's first unget
 * resets it to 0 (COLD), making the consumed page reclaimable again.
 * Only leaf-extent/filter/log pages are prefetched, so this sentinel never
 * feeds the parent-side RI computation (which reads internal-node
 * entry_time only). */
#define TP_ENTRY_TIME_PREFETCHED ((timestamp)UINT64_MAX)

/*
 *-----------------------------------------------------------------------------
 * Clock cache Functions
 *-----------------------------------------------------------------------------
 */
/*-----------------------------------------------------------------------------
 * treepass_{set/clear/test}_flag --
 *
 *      Atomically sets, clears or tests the given flag in the entry.
 *-----------------------------------------------------------------------------
 */

/* Validate entry_number, and return addr of treepass_entry slot */
static inline treepass_entry *
treepass_get_entry(treepass *cc, uint32 entry_number)
{
   debug_assert(entry_number < cc->cfg->page_capacity,
                "entry_number=%u is out-of-bounds. Should be < %d.",
                entry_number,
                cc->cfg->page_capacity);
   return (&cc->entry[entry_number]);
}

static inline entry_status
treepass_get_status(treepass *cc, uint32 entry_number)
{
   return treepass_get_entry(cc, entry_number)->status;
}
static inline entry_status
treepass_set_flag(treepass *cc, uint32 entry_number, entry_status flag)
{
   return flag
          & __sync_fetch_and_or(&treepass_get_entry(cc, entry_number)->status,
                                flag);
}

static inline uint32
treepass_clear_flag(treepass *cc, uint32 entry_number, entry_status flag)
{
   treepass_entry *entry = treepass_get_entry(cc, entry_number);
   return flag & __sync_fetch_and_and(&entry->status, ~flag);
}

static inline uint32
treepass_test_flag(treepass *cc, uint32 entry_number, entry_status flag)
{
   return flag & treepass_get_status(cc, entry_number);
}

/*
 *-----------------------------------------------------------------------------
 * treepass clock functions --
 *
 *      Clock counter for CLOCK eviction algorithm.
 *      Stored in separate clock field (not in status) for efficient atomic ops.
 *      No CAS loops needed - uses simple fetch_add/fetch_sub.
 *-----------------------------------------------------------------------------
 */
/*
 * Get clock value from status (binary encoded)
 * Returns clock value (0 to TP_CLOCK_MAX)
 */
static inline uint32
treepass_get_clock(treepass *cc, uint32 entry_number)
{
   uint32 status = treepass_get_status(cc, entry_number);
   return (status >> TP_CLOCK_SHIFT) & TP_CLOCK_MAX;
}

/*
 * Set clock to specific value in status (binary encoded).
 * Uses CAS loop to avoid clobbering concurrent flag changes.
 */
static inline void
treepass_set_clock(treepass *cc, uint32 entry_number, uint32 value)
{
   treepass_entry *entry = treepass_get_entry(cc, entry_number);
   uint32 clamped = (value > TP_CLOCK_MAX) ? TP_CLOCK_MAX : value;
   uint32 new_clock_bits = clamped << TP_CLOCK_SHIFT;
   uint32 old_status = __atomic_load_n(&entry->status, __ATOMIC_RELAXED);
   uint32 new_status;
   do {
      new_status = (old_status & ~TP_CLOCK_MASK) | new_clock_bits;
   } while (!__atomic_compare_exchange_n(&entry->status, &old_status,
                                         new_status, /*weak=*/1,
                                         __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

/*
 * Increment clock on cache hit (hot path).
 * Uses atomic ADD for simple lock-free increment.
 * Overflow beyond TP_CLOCK_MAX is safe (handled in eviction).
 */
static inline void
treepass_set_clock_accessed(treepass *cc, uint32 entry_number)
{
#if SKIP_CLOCK_UPDATE
   (void)cc;
   (void)entry_number;
   return;
#endif

   treepass_entry *entry = treepass_get_entry(cc, entry_number);
   uint32 clock = (entry->status >> TP_CLOCK_SHIFT) & TP_CLOCK_MAX;

   if (clock < TP_CLOCK_MAX) {
      __atomic_fetch_add(&entry->status, TP_CLOCK_ONE, __ATOMIC_RELAXED);
   }
}

/*
 * Decrement clock during eviction (cold path).
 * Uses atomic SUB for simple lock-free decrement.
 * Returns TRUE if clock was > 0 (entry gets second chance).
 * Returns FALSE if clock was 0 (entry can be evicted).
 */
static inline bool32
treepass_try_dec_clock(treepass *cc, uint32 entry_number)
{
   treepass_entry *entry = treepass_get_entry(cc, entry_number);
   uint32 clock = (entry->status >> TP_CLOCK_SHIFT) & TP_CLOCK_MAX;

   if (clock == 0) {
      return FALSE;  // Can be evicted
   }

   __atomic_fetch_sub(&entry->status, TP_CLOCK_ONE, __ATOMIC_RELAXED);

   return TRUE;  // Had clock > 0, gets second chance
}

/*
 * Check if page has non-zero clock
 */
static inline bool32
treepass_has_clock(treepass *cc, uint32 entry_number)
{
   return treepass_get_clock(cc, entry_number) > 0;
}

#ifdef RECORD_ACQUISITION_STACKS
static void
treepass_record_backtrace(treepass *cc, uint32 entry_number)
{
   // clang-format off
   int myhistindex = __sync_fetch_and_add(
                            &treepass_get_entry(cc, entry_number)->next_history_record,
                            1);
   // clang-format on
   myhistindex = myhistindex % NUM_HISTORY_RECORDS;

   // entry_number is now known to be valid; offset into slot directly.
   treepass_entry *myEntry = &cc->entry[entry_number];

   myEntry->history[myhistindex].status   = myEntry->status;
   myEntry->history[myhistindex].refcount = 0;
   for (threadid i = 0; i < MAX_THREADS; i++) {
      myEntry->history[myhistindex].refcount +=
         cc->refcount[i * cc->cfg->page_capacity + entry_number];
   }
   backtrace(myEntry->history[myhistindex].backtrace, NUM_HISTORY_RECORDS);
}
#else
#   define treepass_record_backtrace(a, b)
#endif

/*
 *----------------------------------------------------------------------
 *
 * Utility functions
 *
 *----------------------------------------------------------------------
 */

static inline uint64
treepass_config_page_size(const treepass_config *cfg)
{
   return cfg->io_cfg->page_size;
}

static inline uint64
treepass_config_extent_size(const treepass_config *cfg)
{
   return cfg->io_cfg->extent_size;
}

static inline uint64
treepass_multiply_by_page_size(const treepass *cc, uint64 addr)
{
   return addr << cc->cfg->log_page_size;
}

static inline uint64
treepass_divide_by_page_size(const treepass *cc, uint64 addr)
{
   return addr >> cc->cfg->log_page_size;
}

static inline uint32
treepass_lookup(const treepass *cc, uint64 addr)
{
   uint64 lookup_no    = treepass_divide_by_page_size(cc, addr);
   uint32 entry_number = cc->lookup[lookup_no];

   debug_assert(((entry_number < cc->cfg->page_capacity)
                 || (entry_number == TP_UNMAPPED_ENTRY)),
                "entry_number=%u is out-of-bounds. "
                " Should be either TP_UNMAPPED_ENTRY,"
                " or should be < %d.",
                entry_number,
                cc->cfg->page_capacity);
   return entry_number;
}

static inline treepass_entry *
treepass_lookup_entry(const treepass *cc, uint64 addr)
{
   return &cc->entry[treepass_lookup(cc, addr)];
}

/*
 * Inherit clock value from old page to new page.
 * Used during compaction/flush when a trunk node or filter is
 * re-created at a new physical address.
 */
static void
treepass_inherit_clock(treepass *cc, uint64 old_addr, uint64 new_addr, uint32 divisor)
{
   uint32 clock_val = 1; // default min=1

   uint32 old_entry_no = treepass_lookup(cc, old_addr);
   bool32 old_found = (old_entry_no != TP_UNMAPPED_ENTRY);
   uint32 old_clock = 0;
   if (old_found) {
      old_clock = treepass_get_clock(cc, old_entry_no);
      clock_val = (divisor > 1) ? (old_clock / divisor) : old_clock;
   }
   /* Ensure the new page survives at least one CLOCK sweep: floor clock at 1.
    * Without this, old_clock == 0 (or old_clock < divisor) would make the
    * integer division yield 0, leaving the freshly inherited page
    * immediately evictable — contradicting the "at least 1" survival
    * guarantee for a freshly inherited page. */
   if (clock_val == 0) {
      clock_val = 1;
   }

   uint32 new_entry_no = treepass_lookup(cc, new_addr);
   bool32 new_found = (new_entry_no != TP_UNMAPPED_ENTRY);
   if (new_found) {
      treepass_set_clock(cc, new_entry_no, clock_val);

      // Also inherit entry_time from old page if non-zero (preserves CI baseline
      // across CoW/split so compaction interval reflects the node's full history)
      if (old_found) {
         timestamp old_entry_time = __atomic_load_n(
            &cc->entry[old_entry_no].page.entry_time, __ATOMIC_RELAXED);
         if (old_entry_time != 0) {
            __atomic_store_n(&cc->entry[new_entry_no].page.entry_time,
                             old_entry_time, __ATOMIC_RELAXED);
         }
      }

   }

}

static inline treepass_entry *
treepass_page_to_entry(const treepass *cc, page_handle *page)
{
   return (treepass_entry *)((char *)page - offsetof(treepass_entry, page));
}

static inline uint32
treepass_page_to_entry_number(const treepass *cc, page_handle *page)
{
   return treepass_page_to_entry(cc, page) - cc->entry;
}

static inline uint32
treepass_data_to_entry_number(const treepass *cc, char *data)
{
   return treepass_divide_by_page_size(cc, data - cc->data);
}

debug_only static inline treepass_entry *
treepass_data_to_entry(const treepass *cc, char *data)
{
   return &cc->entry[treepass_data_to_entry_number(cc, data)];
}

static inline uint64
treepass_page_size(const treepass *cc)
{
   return treepass_config_page_size(cc->cfg);
}

static inline uint64
treepass_extent_size(const treepass *cc)
{
   return treepass_config_extent_size(cc->cfg);
}

/*
 *-----------------------------------------------------------------------------
 * treepass_wait --
 *
 *      Does some work while waiting. Currently just polls for async IO
 *      completion.
 *
 *      This function needs to poll for async IO callback completion to avoid
 *      deadlock.
 *-----------------------------------------------------------------------------
 */
void
treepass_wait(treepass *cc)
{
   io_cleanup(cc->io, TP_DEFAULT_MAX_IO_EVENTS);
}

/*
 *-----------------------------------------------------------------------------
 * ref counts
 *
 *      Each entry has a distributed ref count. This ref count is striped
 *      across cache lines, so the ref count for entry 0 tid 0 is on a
 *      different cache line from both the ref count for entry 1 tid 0 and
 *      entry 0 tid 1. This reduces false sharing.
 *
 *      get_ref_internal converts an entry_number and tid to the index in
 *      cc->refcount where the ref count is stored.
 *-----------------------------------------------------------------------------
 */

static inline uint32
treepass_get_ref_internal(treepass *cc, uint32 entry_number)
{
   return entry_number % cc->cfg->cacheline_capacity * PLATFORM_CACHELINE_SIZE
          + entry_number / cc->cfg->cacheline_capacity;
}

static inline uint16
treepass_get_ref(treepass *cc, uint32 entry_number, uint64 counter_no)
{
   counter_no %= TP_RC_WIDTH;
   uint64 rc_number = treepass_get_ref_internal(cc, entry_number);
   debug_assert(rc_number < cc->cfg->page_capacity);
   // Use atomic load to ensure visibility of concurrent increments
   return __atomic_load_n(&cc->refcount[counter_no * cc->cfg->page_capacity + rc_number],
                          __ATOMIC_ACQUIRE);
}

static inline void
treepass_inc_ref(treepass *cc, uint32 entry_number, threadid counter_no)
{
   counter_no %= TP_RC_WIDTH;
   uint64 rc_number = treepass_get_ref_internal(cc, entry_number);
   debug_assert(rc_number < cc->cfg->page_capacity);

   debug_only uint16 refcount = __sync_fetch_and_add(
      &cc->refcount[counter_no * cc->cfg->page_capacity + rc_number], 1);
   debug_assert(refcount != MAX_READ_REFCOUNT);
}

static inline void
treepass_dec_ref(treepass *cc, uint32 entry_number, threadid counter_no)
{
   debug_only threadid input_counter_no = counter_no;

   counter_no %= TP_RC_WIDTH;
   uint64 rc_number = treepass_get_ref_internal(cc, entry_number);
   debug_assert((rc_number < cc->cfg->page_capacity),
                "Entry number, %lu, is out of allocator "
                "page capacity range, %u.\n",
                rc_number,
                cc->cfg->page_capacity);

   debug_only uint16 refcount = __sync_fetch_and_sub(
      &cc->refcount[counter_no * cc->cfg->page_capacity + rc_number], 1);
   debug_assert((refcount != 0),
                "Invalid refcount, %u, after decrement."
                " input counter_no=%lu, rc_number=%lu, counter_no=%lu\n",
                refcount,
                input_counter_no,
                rc_number,
                counter_no);
}

static inline uint8
treepass_get_pin(treepass *cc, uint32 entry_number)
{
   uint64 rc_number = treepass_get_ref_internal(cc, entry_number);
   debug_assert(rc_number < cc->cfg->page_capacity);
   return cc->pincount[rc_number];
}

static inline void
treepass_inc_pin(treepass *cc, uint32 entry_number)
{
   uint64 rc_number = treepass_get_ref_internal(cc, entry_number);
   debug_assert(rc_number < cc->cfg->page_capacity);
   debug_only uint8 refcount =
      __sync_fetch_and_add(&cc->pincount[rc_number], 1);
   debug_assert(refcount != UINT8_MAX);
}

static inline void
treepass_dec_pin(treepass *cc, uint32 entry_number)
{
   uint64 rc_number = treepass_get_ref_internal(cc, entry_number);
   debug_assert(rc_number < cc->cfg->page_capacity);
   debug_only uint8 refcount =
      __sync_fetch_and_sub(&cc->pincount[rc_number], 1);
   debug_assert(refcount != 0);
}

static inline void
treepass_reset_pin(treepass *cc, uint32 entry_number)
{
   uint64 rc_number = treepass_get_ref_internal(cc, entry_number);
   debug_assert(rc_number < cc->cfg->page_capacity);
   if (cc->pincount[rc_number] != 0) {
      __sync_lock_test_and_set(&cc->pincount[rc_number], 0);
   }
}

void
treepass_assert_no_refs(treepass *cc)
{
   threadid        i;
   volatile uint32 j;
   for (i = 0; i < MAX_THREADS; i++) {
      for (j = 0; j < cc->cfg->page_capacity; j++) {
         if (treepass_get_ref(cc, j, i) != 0) {
            treepass_get_ref(cc, j, i);
         }
         platform_assert(treepass_get_ref(cc, j, i) == 0);
      }
   }
}

void
treepass_assert_no_refs_and_pins(treepass *cc)
{
   threadid i;
   uint32   j;
   for (i = 0; i < MAX_THREADS; i++) {
      for (j = 0; j < cc->cfg->page_capacity; j++) {
         platform_assert(treepass_get_ref(cc, j, i) == 0);
      }
   }
}

void
treepass_assert_no_locks_held(treepass *cc)
{
   uint64 i;
   treepass_assert_no_refs_and_pins(cc);
   for (i = 0; i < cc->cfg->page_capacity; i++) {
      debug_assert(!treepass_test_flag(cc, i, TP_WRITELOCKED));
   }
}

bool32
treepass_assert_clean(treepass *cc)
{
   uint64 i;
   for (i = 0; (i < cc->cfg->page_capacity)
               && (treepass_test_flag(cc, i, TP_FREE)
                   || treepass_test_flag(cc, i, TP_CLEAN));
        i++)
   { /* Do nothing */
   }
   return (i == cc->cfg->page_capacity);
}

/*
 *----------------------------------------------------------------------
 *
 * page locking functions
 *
 *----------------------------------------------------------------------
 */

typedef enum {
   GET_RC_SUCCESS = 0,
   GET_RC_CONFLICT,
   GET_RC_EVICTED,
   GET_RC_FLUSHING,
} get_rc;

/*
 *----------------------------------------------------------------------
 * treepass_try_get_read
 *
 *      returns:
 *      - GET_RC_SUCCESS if a read lock was obtained
 *      - GET_RC_EVICTED if the entry was evicted
 *      - GET_RC_CONFLICT if another thread holds a write lock
 *
 *      does not block
 *----------------------------------------------------------------------
 */
static get_rc
treepass_try_get_read(treepass *cc, uint32 entry_number, bool32 set_access)
{
   const threadid tid = platform_get_tid();

   // first check if write lock is held
   uint32 cc_writing = treepass_test_flag(cc, entry_number, TP_WRITELOCKED);
   if (UNLIKELY(cc_writing)) {
      return GET_RC_CONFLICT;
   }

   // then obtain the read lock
   treepass_inc_ref(cc, entry_number, tid);

   // treepass_test_flag returns 32 bits, not 1 (cannot use bool)
   uint32 cc_free = treepass_test_flag(cc, entry_number, TP_FREE);
   cc_writing     = treepass_test_flag(cc, entry_number, TP_WRITELOCKED);
   if (LIKELY(!cc_free && !cc_writing)) {
      // Increment clock on access (single atomic add, no CAS loop!)
      // More accesses = higher clock value = more protection from eviction
      if (set_access) {
         treepass_set_clock_accessed(cc, entry_number);
      }
      treepass_record_backtrace(cc, entry_number);
      return GET_RC_SUCCESS;
   }

   // cannot hold the read lock (either write lock is held or entry has been
   // evicted), dec ref and return
   treepass_dec_ref(cc, entry_number, tid);

   if (cc_free) {
      return GET_RC_EVICTED;
   }

   // must be cc_writing
   debug_assert(cc_writing);
   return GET_RC_CONFLICT;
}

/*
 *----------------------------------------------------------------------
 * treepass_get_read
 *
 *      returns:
 *      - GET_RC_SUCCESS if a read lock was obtained
 *      - GET_RC_EVICTED if the entry was evicted
 *
 *      blocks if another thread holds a write lock
 *----------------------------------------------------------------------
 */
static get_rc
treepass_get_read(treepass *cc, uint32 entry_number)
{
   get_rc rc = treepass_try_get_read(cc, entry_number, TRUE);

   uint64 wait = 1;
   while (rc == GET_RC_CONFLICT) {
      platform_sleep_ns(wait);
      wait = wait > 100000 ? wait : 2 * wait;  // cap at 100μs (nanosleep)
      rc   = treepass_try_get_read(cc, entry_number, TRUE);
   }

   return rc;
}

/*
 *----------------------------------------------------------------------
 * treepass_try_get_claim
 *
 *      Attempts to upgrade a read lock to claim.
 *
 *      NOTE: A caller must release the read lock on GET_RC_CONFLICT before
 *      attempting try_get_claim again to avoid deadlock.
 *
 *      returns:
 *      - GET_RC_SUCCESS if a claim was obtained
 *      - GET_RC_CONFLICT if another thread holds a claim (or write lock)
 *
 *      does not block
 *----------------------------------------------------------------------
 */
static get_rc
treepass_try_get_claim(treepass *cc, uint32 entry_number)
{
   treepass_log(0,
                  entry_number,
                  "try_get_claim: entry_number %u claimed: %u\n",
                  entry_number,
                  treepass_test_flag(cc, entry_number, TP_CLAIMED));

   if (treepass_set_flag(cc, entry_number, TP_CLAIMED)) {
      treepass_log(0, entry_number, "return false\n", NULL);
      return GET_RC_CONFLICT;
   }

   treepass_record_backtrace(cc, entry_number);

   return GET_RC_SUCCESS;
}

/*
 *----------------------------------------------------------------------
 * treepass_get_write
 *
 *      Upgrades a claim to a write lock.
 *
 *      blocks:
 *      - while read locks are released
 *      - while write back completes
 *
 *      cannot fail
 *
 *      Note: does not wait on TP_LOADING. Caller must either ensure that
 *      TP_LOADING is not set prior to calling (e.g. via a prior call to
 *      treepass_get).
 *----------------------------------------------------------------------
 */
static void
treepass_get_write(treepass *cc, uint32 entry_number)
{
   const threadid tid = platform_get_tid();

   debug_assert(treepass_test_flag(cc, entry_number, TP_CLAIMED));
   debug_only uint32 was_writing =
      treepass_set_flag(cc, entry_number, TP_WRITELOCKED);
   debug_assert(!was_writing);
   debug_assert(!treepass_test_flag(cc, entry_number, TP_LOADING));

   /*
    * If the thread that wants a write lock holds > 1 refs, it means
    * it has some async lookups which have yielded after taking refs.
    * This is currently not allowed; because such a thread would
    * easily be able to upgrade to write lock and modify the page
    * under it's own yielded lookup.
    *
    * If threads do async lookups, they must leave the
    * compaction+incorporation (that needs write locking) to
    * background threads.
    */
   debug_assert(treepass_get_ref(cc, entry_number, tid) >= 1);
   // Wait for flushing to finish
   while (treepass_test_flag(cc, entry_number, TP_WRITEBACK)) {
      treepass_wait(cc);
   }

   // Wait for readers to finish
   for (threadid thr_i = 0; thr_i < TP_RC_WIDTH; thr_i++) {
      if (tid % TP_RC_WIDTH != thr_i) {
         while (treepass_get_ref(cc, entry_number, thr_i)) {
            platform_sleep_ns(1);
         }
      } else {
         // we have a single ref, so wait for others to drop
         while (treepass_get_ref(cc, entry_number, thr_i) > 1) {
            platform_sleep_ns(1);
         }
      }
   }

   treepass_record_backtrace(cc, entry_number);
}

/*
 *----------------------------------------------------------------------
 * treepass_try_get_write
 *
 *      Attempts to upgrade a claim to a write lock.
 *
 *      returns:
 *      - GET_RC_SUCCESS if the write lock was obtained
 *      - GET_RC_CONFLICT if another thread holds a read lock
 *
 *      blocks on write back
 *
 *      Note: does not wait on TP_LOADING. Caller must either ensure that
 *      TP_LOADING is not set prior to calling (e.g. via a prior call to
 *      treepass_get).
 *----------------------------------------------------------------------
 */
static get_rc
treepass_try_get_write(treepass *cc, uint32 entry_number)
{
   threadid thr_i;
   threadid tid = platform_get_tid();
   get_rc   rc;

   debug_assert(treepass_test_flag(cc, entry_number, TP_CLAIMED));
   debug_only uint32 was_writing =
      treepass_set_flag(cc, entry_number, TP_WRITELOCKED);
   debug_assert(!was_writing);
   debug_assert(!treepass_test_flag(cc, entry_number, TP_LOADING));

   // if flushing, then bail
   if (treepass_test_flag(cc, entry_number, TP_WRITEBACK)) {
      rc = GET_RC_FLUSHING;
      goto failed;
   }

   // check for readers
   for (thr_i = 0; thr_i < TP_RC_WIDTH; thr_i++) {
      if (tid % TP_RC_WIDTH != thr_i) {
         if (treepass_get_ref(cc, entry_number, thr_i)) {
            // there is a reader, so bail
            rc = GET_RC_CONFLICT;
            goto failed;
         }
      } else {
         // we have a single ref, so if > 1 bail
         if (treepass_get_ref(cc, entry_number, thr_i) > 1) {
            // there is a reader, so bail
            rc = GET_RC_CONFLICT;
            goto failed;
         }
      }
   }

   treepass_record_backtrace(cc, entry_number);

   return GET_RC_SUCCESS;

failed:
   was_writing = treepass_clear_flag(cc, entry_number, TP_WRITELOCKED);
   debug_assert(was_writing);
   return rc;
}

/*
 *----------------------------------------------------------------------
 *
 * writeback functions
 *
 *----------------------------------------------------------------------
 */

// Forward declaration for the EI getter
static uint64 treepass_get_evict_interval(const treepass *hc);

/*
 *----------------------------------------------------------------------
 * treepass_ok_to_writeback
 *
 *      Tests the entry to see if write back is possible. Used for test and
 *      test and set.
 *
 *      A page is cleanable if it is dirty (not TP_CLEAN) and not in any
 *      locked/loading/writeback state.
 *
 *      Progressive clock-aware writeback:
 *        num_passes=0: only clock==0 pages (gentle)
 *        num_passes=N: pages with clock <= N (progressively aggressive)
 *        num_passes>=TP_CLOCK_MAX: all dirty pages (flush-equivalent)
 *----------------------------------------------------------------------
 */
static inline bool32
treepass_ok_to_writeback(treepass *cc,
                           uint32      entry_number,
                           uint32      num_passes)
{
   uint32 status = treepass_get_status(cc, entry_number);

   // Base check: dirty and not locked/loading/in-writeback
   if ((status & TP_CLEANABLE_BASE_MASK) != 0) {
      return FALSE;
   }

   // Progressive clock threshold: skip pages with clock > num_passes
   uint32 clock = (status >> TP_CLOCK_SHIFT) & TP_CLOCK_MAX;
   if (clock > num_passes) {
      return FALSE;
   }

   return TRUE;
}

/*
 *----------------------------------------------------------------------
 * treepass_try_set_writeback
 *
 *      Atomically sets the TP_WRITEBACK flag if the status permits.
 *      Page must be dirty and not locked/loading/in-writeback.
 *      Clock value is preserved during writeback.
 *
 *      Progressive: only pages with clock <= num_passes are eligible.
 *----------------------------------------------------------------------
 */
static inline bool32
treepass_try_set_writeback(treepass *cc,
                             uint32      entry_number,
                             uint32      num_passes)
{
   // Validate first, as we need access to volatile status * below.
   debug_assert(entry_number < cc->cfg->page_capacity,
                "entry_number=%u is out-of-bounds. Should be < %d.",
                entry_number,
                cc->cfg->page_capacity);

   platform_assert(cc->entry[entry_number].waiters.head == NULL);

   volatile uint32 *status_ptr = &cc->entry[entry_number].status;
   uint32 old_status, new_status;

   do {
      old_status = *status_ptr;

      // Check if cleanable (base flags must be clear)
      if ((old_status & TP_CLEANABLE_BASE_MASK) != 0) {
         return FALSE;
      }

      // Progressive clock threshold: skip pages with clock > num_passes
      uint32 clock = (old_status >> TP_CLOCK_SHIFT) & TP_CLOCK_MAX;
      if (clock > num_passes) {
         return FALSE;
      }

      // Set writeback flag, preserve clock
      new_status = old_status | TP_WRITEBACK;
   } while (!__sync_bool_compare_and_swap(status_ptr, old_status, new_status));

   return TRUE;
}

typedef struct async_io_state {
   treepass           *cc;
   uint64               *outstanding_pages;
   uint32               num_passes;
   io_async_state_buffer iostate;
} async_io_state;

static void
treepass_write_callback(void *wbs)
{
   async_io_state *state = (async_io_state *)wbs;
   treepass     *cc    = state->cc;

   if (io_async_run(state->iostate) != ASYNC_STATUS_DONE) {
      return;
   }

   platform_assert_status_ok(io_async_state_get_result(state->iostate));

   const struct iovec *iovec;
   uint64              count;
   iovec = io_async_state_get_iovec(state->iostate, &count);

   platform_assert(count > 0);
   platform_assert(count <= cc->cfg->pages_per_extent);

   uint64            i;
   uint32            entry_number;
   treepass_entry *entry;
   uint64            addr;
   debug_only uint32 debug_status;

   for (i = 0; i < count; i++) {
      entry_number =
         treepass_data_to_entry_number(cc, (char *)iovec[i].iov_base);
      entry = treepass_get_entry(cc, entry_number);
      addr  = entry->page.disk_addr;

      treepass_log(addr,
                     entry_number,
                     "write_callback i %lu entry %u addr %lu\n",
                     i,
                     entry_number,
                     addr);

      debug_status = treepass_set_flag(cc, entry_number, TP_CLEAN);
      debug_assert(!debug_status);
      debug_status = treepass_clear_flag(cc, entry_number, TP_WRITEBACK);
      debug_assert(debug_status);
      // CC feature: under pressure, reset branch clock so the page can be
      // evicted immediately after being cleaned.
      if (state->num_passes > 0
          && state->num_passes < TP_CLOCK_MAX
          && entry->type == PAGE_TYPE_BRANCH)
      {
         treepass_set_clock(cc, entry_number, 0);
      }
   }

   if (state->outstanding_pages) {
      __sync_fetch_and_sub(state->outstanding_pages, count);
   }

   io_async_state_deinit(state->iostate);
   platform_free(cc->heap_id, state);
}

/*
 *----------------------------------------------------------------------
 * treepass_batch_start_writeback --
 *
 *      Iterates through all pages in the batch and issues writeback for any
 *      which are cleanable (dirty and not locked/loading).
 *
 *      Where possible, the write is extended to the extent, including pages
 *      outside the batch.
 *
 *      Progressive writeback controlled by num_passes:
 *        num_passes=0: only clock==0 pages (gentle, first pass)
 *        num_passes=N: pages with clock <= N (progressively aggressive)
 *        num_passes>=TP_CLOCK_MAX: all dirty pages (shutdown flush)
 *----------------------------------------------------------------------
 */
void
treepass_batch_start_writeback(treepass *cc,
                              uint64   batch,
                              uint32   num_passes)
{
   uint32         entry_no, next_entry_no;
   uint64         addr, first_addr, end_addr, i;
   const threadid tid            = platform_get_tid();
   uint64         start_entry_no = batch * TP_ENTRIES_PER_BATCH;
   uint64         end_entry_no   = start_entry_no + TP_ENTRIES_PER_BATCH;
   bool32         is_flush       = (num_passes >= TP_CLOCK_MAX);

   treepass_entry *entry, *next_entry;

   debug_assert((tid < MAX_THREADS), "Invalid tid=%lu\n", tid);
   debug_assert(cc != NULL);
   debug_assert(batch < cc->cfg->page_capacity / TP_ENTRIES_PER_BATCH);

   treepass_open_log_stream();
   treepass_log_stream(0,
                         0,
                         "batch_start_writeback: %lu, entries %lu-%lu\n",
                         batch,
                         start_entry_no,
                         end_entry_no - 1);

   uint64 page_size = treepass_page_size(cc);

   allocator_config *allocator_cfg = allocator_get_config(cc->al);
   // Iterate through the entries in the batch and try to write out the extents.
   for (entry_no = start_entry_no; entry_no < end_entry_no; entry_no++) {
      entry = &cc->entry[entry_no];
      addr  = entry->page.disk_addr;
      // CC feature: under pressure (but not flush), only target branch pages
      // -- trunk/memtable/filter won't be evicted anyway.
      if (num_passes > 0 && !is_flush
          && entry->type != PAGE_TYPE_BRANCH)
      {
         continue;
      }
      if (treepass_ok_to_writeback(cc, entry_no, num_passes)
          && treepass_try_set_writeback(cc, entry_no, num_passes))
      {
         debug_assert(treepass_lookup(cc, addr) == entry_no);
         first_addr = entry->page.disk_addr;
         // walk backwards through extent to find first cleanable entry
         do {
            first_addr -= page_size;
            if (allocator_config_pages_share_extent(
                   allocator_cfg, first_addr, addr))
               next_entry_no = treepass_lookup(cc, first_addr);
            else
               next_entry_no = TP_UNMAPPED_ENTRY;
         } while (
            next_entry_no != TP_UNMAPPED_ENTRY
            && treepass_try_set_writeback(cc, next_entry_no, num_passes));
         first_addr += page_size;
         end_addr = entry->page.disk_addr;
         // walk forwards through extent to find last cleanable entry
         do {
            end_addr += page_size;
            if (allocator_config_pages_share_extent(
                   allocator_cfg, end_addr, addr))
               next_entry_no = treepass_lookup(cc, end_addr);
            else
               next_entry_no = TP_UNMAPPED_ENTRY;
         } while (
            next_entry_no != TP_UNMAPPED_ENTRY
            && treepass_try_set_writeback(cc, next_entry_no, num_passes));

         async_io_state *state;
         while ((state = TYPED_MALLOC(cc->heap_id, state)) == NULL) {
            treepass_wait(cc);
         }

         state->cc                = cc;
         state->outstanding_pages = NULL;
         state->num_passes        = num_passes;
         io_async_state_init(state->iostate,
                             cc->io,
                             io_async_pwritev,
                             first_addr,
                             treepass_write_callback,
                             state);

         uint64 req_count =
            treepass_divide_by_page_size(cc, end_addr - first_addr);

         if (cc->cfg->use_stats) {
            cc->stats[tid].page_writes[entry->type] += req_count;
            cc->stats[tid].writes_issued++;
         }

         for (i = 0; i < req_count; i++) {
            addr       = first_addr + treepass_multiply_by_page_size(cc, i);
            next_entry = treepass_lookup_entry(cc, addr);
            next_entry_no = treepass_lookup(cc, addr);

            treepass_log_stream(addr,
                                  next_entry_no,
                                  "flush: entry %u addr %lu\n",
                                  next_entry_no,
                                  addr);
            io_async_state_append_page(state->iostate, next_entry->page.data);
         }

         io_async_run(state->iostate);
      }
   }
   treepass_close_log_stream();
}

/*
 *----------------------------------------------------------------------
 *
 * eviction functions
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 * treepass_try_evict
 *
 *      Attempts to evict the page if it is evictable
 *----------------------------------------------------------------------
 */
static bool32
treepass_try_evict(treepass *cc, uint32 entry_number, bool32 is_early_eviction,
                  bool32 ee_pass)
{
   treepass_entry *entry = treepass_get_entry(cc, entry_number);
   const threadid    tid   = platform_get_tid();
   bool32 evicted = FALSE;

   if (ee_pass) {
      /* EE pass: do NOT decrement clock; only evict if entry_time==0 (cold).
       * Restrict EE to BRANCH pages; filter/memtable pages never set
       * entry_time so they always look cold. */
      if (entry->type != PAGE_TYPE_BRANCH) {
         goto out;
      }
      if (entry->page.entry_time != 0) {
         goto out;
      }
   } else {
      /*
       * Clock-based eviction with decrement
       * Decrement clock if > 0, skip eviction
       * Only evict when clock reaches 0
       */
      if (treepass_try_dec_clock(cc, entry_number)) {
         // clock was > 0, decremented, entry survives this pass
         goto out;
      }
   }

   uint32 status = entry->status;

   /*
    * perform fast tests and quit if they fail
    * Note: this implicitly tests for:
    * TP_CLAIMED, TP_WRITELOCK, TP_WRITEBACK
    * Note: here is where we check that the evicting thread doesn't hold a read
    * lock itself.
    */
   if (status != TP_EVICTABLE_STATUS
       || treepass_get_ref(cc, entry_number, tid)
       || treepass_get_pin(cc, entry_number))
   {
      goto out;
   }

   /* try to evict:
    * 1. try to read lock
    * 2. try to claim
    * 3. try to write lock
    * 4. verify still evictable
    * 5. clear lookup, disk_addr
    * 6. set status to TP_FREE_STATUS (clears claim and write lock)
    * 7. release read lock */

   /* 1. try to read lock */
   if (treepass_try_get_read(cc, entry_number, FALSE) != GET_RC_SUCCESS) {
      goto out;
   }

   /* 2. try to claim */
   if (treepass_try_get_claim(cc, entry_number) != GET_RC_SUCCESS) {
      goto release_ref;
   }

   /*
    * 3. try to write lock
    *      -- first check if loading
    */
   if (treepass_test_flag(cc, entry_number, TP_LOADING)
       || treepass_try_get_write(cc, entry_number) != GET_RC_SUCCESS)
   {
      goto release_claim;
   }


   /* 4. verify still evictable
    * redo fast tests in case another thread has changed the status before we
    * obtained the lock
    * note: do not re-check the ref count for the active thread, because
    * it acquired a read lock in order to lock the entry.
    */
   status = entry->status;
   if (status != TP_LOCKED_EVICTABLE_STATUS
       || treepass_get_pin(cc, entry_number))
   {
      goto release_write;
   }


   /* 5. clear lookup, disk addr */
   uint64 addr = entry->page.disk_addr;
   if (addr != TP_UNMAPPED_ADDR) {
      uint64 lookup_no      = treepass_divide_by_page_size(cc, addr);
      cc->lookup[lookup_no] = TP_UNMAPPED_ENTRY;

      entry->page.disk_addr = TP_UNMAPPED_ADDR;
      entry->page.entry_time   = 0;
   }
   debug_only uint32 debug_status =
      treepass_test_flag(cc, entry_number, TP_WRITELOCKED | TP_CLAIMED);
   debug_assert(debug_status);


   /* Ensure addr clear is visible before type clear (readers check addr first).
    * Mirrors try_page_discard's ordering; without this, readers in
    * treepass_get_in_cache can observe type=INVALID while still seeing the
    * old addr, triggering the "entry type 0 != type" assertion. */
   __sync_synchronize();

   /* 6. set status to TP_FREE_STATUS (clears claim, write lock, and clock) */
   platform_assert(entry->waiters.head == NULL);
   entry->type   = PAGE_TYPE_INVALID;
   entry->status = TP_FREE_STATUS;  // Also clears clock bits
   treepass_log(
      addr, entry_number, "evict: entry %u addr %lu\n", entry_number, addr);

   evicted = TRUE;

   /* 7. release read lock */
   goto release_ref;

release_write:
   debug_status = treepass_clear_flag(cc, entry_number, TP_WRITELOCKED);
   debug_assert(debug_status);
release_claim:
   debug_status = treepass_clear_flag(cc, entry_number, TP_CLAIMED);
   debug_assert(debug_status);
release_ref:
   treepass_dec_ref(cc, entry_number, tid);
out:
   return evicted;
}

/*
 *----------------------------------------------------------------------
 * treepass_evict_batch_ee --
 *
 *      EE pass: evict only pages with entry_time==0 in this batch, without
 *      decrementing clock bits. Returns count of successful evictions so
 *      the caller can decide whether to fall through to the normal pass.
 *----------------------------------------------------------------------
 */
static uint32
treepass_evict_batch_ee(treepass *cc, uint32 batch)
{
   debug_assert(cc != NULL);
   debug_assert(batch < cc->cfg->page_capacity / TP_ENTRIES_PER_BATCH);

   uint32 start_entry_no = batch * TP_ENTRIES_PER_BATCH;
   uint32 end_entry_no   = start_entry_no + TP_ENTRIES_PER_BATCH;
   uint32 evict_count    = 0;

   for (uint32 entry_no = start_entry_no; entry_no < end_entry_no; entry_no++) {
      if (treepass_try_evict(cc, entry_no, TRUE /*is_early_eviction*/,
                            TRUE /*ee_pass*/)) {
         evict_count++;
      }
   }
   return evict_count;
}

/*
 *----------------------------------------------------------------------
 * treepass_evict_batch --
 *
 *      Evicts all evictable pages in the batch.
 *----------------------------------------------------------------------
 */
void
treepass_evict_batch(treepass *cc, uint32 batch, bool32 first_pass)
{
   debug_assert(cc != NULL);
   debug_assert(batch < cc->cfg->page_capacity / TP_ENTRIES_PER_BATCH);

   uint32 start_entry_no = batch * TP_ENTRIES_PER_BATCH;
   uint32 end_entry_no   = start_entry_no + TP_ENTRIES_PER_BATCH;

   treepass_log(0,
                  0,
                  "evict_batch: %u, entries %u-%u\n",
                  batch,
                  start_entry_no,
                  end_entry_no - 1);

   for (uint32 entry_no = start_entry_no; entry_no < end_entry_no; entry_no++) {
      treepass_try_evict(cc, entry_no, FALSE, FALSE);
   }


   /* EI update only on first-pass batches. Multi-pass (num_passes > 0) means
    * cache is under pressure and the same hand re-sweeps the same entries
    * with progressively higher clock thresholds — wall time per cycle drops
    * (clock bits decremented in earlier passes), which would bias avg_sweep
    * and pull EI down. Skip those batches from EI book-keeping. */
   if (!first_pass) {
      return;
   }

   uint32 batches = __sync_add_and_fetch(&cc->evict_batches, 1u);
   uint32 batch_cap = cc->cfg->batch_capacity;
   uint32 cycle = batches / batch_cap;
   bool is_cycle_boundary = (batches % batch_cap == 0);

   if (!(is_cycle_boundary && cycle > 0)) {
      return;
   }


   timestamp now = platform_get_timestamp();
   uint32 expected = __atomic_load_n(&cc->last_ei_check_cycle, __ATOMIC_ACQUIRE);
   if (expected >= cycle) {
      /* Another boundary-thread already committed this cycle (rare: two
       * cycle boundaries firing concurrently). Drop our update. */
      goto ei_done;
   }

   timestamp last_ts = __atomic_load_n(&cc->last_cycle_ts, __ATOMIC_ACQUIRE);
   if (now <= last_ts) {
      goto ei_done;
   }

   uint64 ei = __atomic_load_n(&cc->evict_interval, __ATOMIC_RELAXED);
   uint64 num_cycles = cycle - expected;
   timestamp elapsed = now - last_ts;
   uint64 avg_sweep_time = elapsed / num_cycles;

   uint64 new_ei;
   bool is_init = (ei == EI_NOT_READY);
   if (is_init) {
      /* Initialize EI proportional to cache size, using configured per-GB seed */
      new_ei = (cc->cfg->capacity >> 30) * cc->cfg->ei_initial_per_gb_ns;
      /* Floor at 1 s/GB (i.e. the seed itself) for sub-1GB caches so the
       * proportional formula doesn't underflow to 0 == EI_NOT_READY, which
       * would silently keep EE disabled. */
      if (new_ei == 0) {
         new_ei = cc->cfg->ei_initial_per_gb_ns;
      }
   } else {
      /* EWMA: new_ei = alpha * measured + (1-alpha) * old_ei */
      double alpha = cc->cfg->ei_ewma_alpha;
      new_ei = (uint64)(avg_sweep_time * alpha + ei * (1.0 - alpha));
   }

   /* CAS on last_ei_check_cycle: ensures exactly one EI update per cycle.
    * If CAS fails, another thread updated for this cycle — drop ours. */
   if (!__atomic_compare_exchange_n(&cc->last_ei_check_cycle, &expected, cycle,
                                    false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
      goto ei_done;
   }

   __atomic_store_n(&cc->evict_interval, new_ei, __ATOMIC_RELEASE);
   __atomic_store_n(&cc->last_cycle_ts, now, __ATOMIC_RELEASE);
   __atomic_store_n(&cc->last_ei_check_ts, now, __ATOMIC_RELAXED);

ei_done:
   return;
}

/*
 *----------------------------------------------------------------------
 * treepass_move_hand --
 *
 *      Moves the clock hand forward, cleaning and evicting a batch.
 *----------------------------------------------------------------------
 */
int64
treepass_move_hand(treepass *cc, int64 num_passes)
{
   const threadid   tid = platform_get_tid();
   volatile bool32 *evict_batch_busy;
   volatile bool32 *clean_batch_busy;
   uint64           cleaner_hand;

   /* move the hand a batch forward */
   uint64            evict_hand = cc->per_thread[tid].free_hand;
   debug_only bool32 was_busy   = TRUE;

   if (evict_hand != TP_UNMAPPED_ENTRY) {
      /* B1: EE the currently-held batch FIRST, before releasing/advancing.
       * If it frees any cold (entry_time==0) page, stay on this batch (the
       * caller re-scans it for the freed slot) and do NOT advance the hand.
       * Only when nothing cold remains here do we release and advance. */
      if (treepass_evict_batch_ee(cc, evict_hand % cc->cfg->batch_capacity) > 0) {
         return num_passes;  /* free_hand unchanged; caller finds freed slots */
      }
      evict_batch_busy = &cc->batch_busy[evict_hand];
      was_busy = __sync_bool_compare_and_swap(evict_batch_busy, TRUE, FALSE);
      debug_assert(was_busy);
   }
   do {
      evict_hand =
         __sync_add_and_fetch(&cc->evict_hand, 1) % cc->cfg->batch_capacity;
      evict_batch_busy = &cc->batch_busy[evict_hand];
      // clean the batch ahead
      cleaner_hand = (evict_hand + cc->cleaner_gap) % cc->cfg->batch_capacity;
      clean_batch_busy = &cc->batch_busy[cleaner_hand];
      if (__sync_bool_compare_and_swap(clean_batch_busy, FALSE, TRUE)) {
         treepass_batch_start_writeback(cc, cleaner_hand, (uint32)num_passes);
         was_busy = __sync_bool_compare_and_swap(clean_batch_busy, TRUE, FALSE);
         debug_assert(was_busy);
      }
   } while (!__sync_bool_compare_and_swap(evict_batch_busy, FALSE, TRUE));

   uint32 batch_idx = evict_hand % cc->cfg->batch_capacity;

   /* B1: the EE pass now runs on the *previously held* batch before advancing
    * (see top of function). The freshly advanced-into batch gets the standard
    * clock-decrementing eviction pass. */
   treepass_evict_batch(cc, batch_idx, num_passes == 0);
   cc->per_thread[tid].free_hand = batch_idx;
   return num_passes;  // return unchanged for regular eviction
}

/*
 *----------------------------------------------------------------------
 * treepass_get_free_page --
 *
 *      returns a free page with given status and ref count.
 *----------------------------------------------------------------------
 */
uint32
treepass_get_free_page(treepass *cc,
                         uint32      status,
                         page_type   type,
                         bool32      refcount,
                         bool32      blocking)
{
   uint32            entry_no;
   // Prescan only when EI is ready (otherwise PT can't classify COLD pages)
   int64             num_passes = 0;
   const threadid    tid        = platform_get_tid();
   uint64            max_hand   = cc->per_thread[tid].free_hand;
   treepass_entry *entry;
   timestamp         wait_start;

   debug_assert((tid < MAX_THREADS), "Invalid tid=%lu\n", tid);
   if (cc->per_thread[tid].free_hand == TP_UNMAPPED_ENTRY) {
      treepass_move_hand(cc, 0);
   }

   /*
    * Debug builds can run on very high latency storage eg. Nimbus. Do
    * not give up after 3 passes on the cache. At least wait for the
    * max latency of an IO and keep making passes.
    */
   while (num_passes < 3
          || (blocking && !io_max_latency_elapsed(cc->io, wait_start)))
   {
      uint64 start_entry = cc->per_thread[tid].free_hand * TP_ENTRIES_PER_BATCH;
      uint64 end_entry   = start_entry + TP_ENTRIES_PER_BATCH;
      for (entry_no = start_entry; entry_no < end_entry; entry_no++) {
         entry = &cc->entry[entry_no];
         if (entry->status == TP_FREE_STATUS
             && __sync_bool_compare_and_swap(
                &entry->status, TP_FREE_STATUS, TP_ALLOC_STATUS))
         {
            if (refcount) {
               treepass_inc_ref(cc, entry_no, tid);
            }
            platform_assert(entry->waiters.head == NULL);
            entry->status = status;  // Clock set via TP_READ_LOADING_STATUS or on access
            entry->type   = type;
            debug_assert(entry->page.disk_addr == TP_UNMAPPED_ADDR);
            treepass_record_backtrace(cc, entry_no);
            return entry_no;
         }
      }

      num_passes = treepass_move_hand(cc, num_passes);

      // Check if we completed a full pass (hand wrapped around)
      if (cc->per_thread[tid].free_hand < max_hand) {
         num_passes++;
         /*
          * The first pass doesn't really have a fair chance at having
          * looked at the entire cache, still it's ok to start
          * reckoning start time for max latency. Since it runs into
          * seconds, we'll make another complete pass in a tiny
          * fraction of the max latency.
          */
         if (num_passes == 1) {
            wait_start = platform_get_timestamp();
         } else {
            platform_yield();
         }
         treepass_wait(cc);
      }
      max_hand = cc->per_thread[tid].free_hand;
   }
   if (blocking) {
      platform_default_log("cache locked (num_passes=%lu time=%lu nsecs)\n",
                           num_passes,
                           platform_timestamp_elapsed(wait_start));
      treepass_print(Platform_default_log_handle, cc);
      platform_assert(0);
   }

   return TP_UNMAPPED_ENTRY;
}
/*
 *-----------------------------------------------------------------------------
 * treepass_flush --
 *
 *      Issues writeback for all page in the cache.
 *
 *      Asserts that there are no pins, read locks, claims or write locks.
 *-----------------------------------------------------------------------------
 */
void
treepass_flush(treepass *cc)
{
   // make sure all aio is complete first
   io_wait_all(cc->io);

   // there can be no references or pins or things won't flush
   // treepass_assert_no_locks_held(cc); // take out for performance

   // clean all the pages
   for (uint32 flush_hand = 0;
        flush_hand < cc->cfg->page_capacity / TP_ENTRIES_PER_BATCH;
        flush_hand++)
   {
      treepass_batch_start_writeback(cc, flush_hand, TP_CLOCK_MAX);
   }

   // make sure all aio is complete again
   io_wait_all(cc->io);

   debug_assert(treepass_assert_clean(cc));
}

/*
 *-----------------------------------------------------------------------------
 * treepass_evict_all --
 *
 *      evicts all the pages.
 *-----------------------------------------------------------------------------
 */
int
treepass_evict_all(treepass *cc, bool32 ignore_pinned_pages)
{
   uint32 evict_hand;
   uint32 i;

   if (!ignore_pinned_pages) {
      // there can be no references or pins or locks or it will block eviction
      treepass_assert_no_locks_held(cc); // take out for performance
   }

   // evict all the pages (need TP_CLOCK_MAX + 1 passes to drain max clock value)
   // Shutdown flush: pass first_pass=FALSE so we don't pollute EI book-keeping
   // with rapid drain sweeps (clock progressively decremented across r passes).
   for (evict_hand = 0; evict_hand < cc->cfg->batch_capacity; evict_hand++) {
      for (int r = 0; r < TP_CLOCK_MAX + 1; r++) {
         treepass_evict_batch(cc, evict_hand, FALSE);
      }
   }

   for (i = 0; i < cc->cfg->page_capacity; i++) {
      debug_only uint32 entry_no =
         treepass_page_to_entry_number(cc, &cc->entry->page);
      // Every page should either be evicted or pinned.
      debug_assert(
         cc->entry[i].status == TP_FREE_STATUS
         || (ignore_pinned_pages && treepass_get_pin(cc, entry_no)));
   }

   return 0;
}

/*
 *----------------------------------------------------------------------
 * treepass_alloc --
 *
 *      Given a disk_addr, allocate entry in the cache and return its page with
 *      a write lock.
 *----------------------------------------------------------------------
 */
page_handle *
treepass_alloc(treepass *cc, uint64 addr, page_type type)
{
   // When IC+CI enabled: dirty pages get clock=1 (higher than read pages at 0)
   // When IC+CI disabled: dirty pages get clock=0 (same as read pages)
   uint32 alloc_clock = TP_CLOCK_ONE;
   uint32            entry_no = treepass_get_free_page(cc,
                                              TP_ALLOC_STATUS | alloc_clock,
                                              type,
                                              TRUE,  // refcount
                                              TRUE); // blocking
   treepass_entry *entry    = &cc->entry[entry_no];
   entry->page.disk_addr      = addr;
   entry->type                = type;
   platform_assert(type != PAGE_TYPE_TRUNK || treepass_current_ts != 0,
                   "treepass_current_ts is 0 when allocating TRUNK page -- "
                   "entry_time will be permanently 0, breaking memtable CI "
                   "calculation");
   entry->page.entry_time        = treepass_current_ts;
   uint64 lookup_no = treepass_divide_by_page_size(cc, entry->page.disk_addr);
   // bool32 rc        = __sync_bool_compare_and_swap(
   //    &cc->lookup[lookup_no], TP_UNMAPPED_ENTRY, entry_no);
   // platform_assert(rc);
   cc->lookup[lookup_no] = entry_no;
   treepass_record_backtrace(cc, entry_no);

   treepass_log(entry->page.disk_addr,
                  entry_no,
                  "alloc: entry %u addr %lu\n",
                  entry_no,
                  entry->page.disk_addr);
   return &entry->page;
}

/*
 *----------------------------------------------------------------------
 * treepass_try_page_discard --
 *
 *      Evicts the page with address addr if it is in cache.
 *----------------------------------------------------------------------
 */
void
treepass_try_page_discard(treepass *cc, uint64 addr)
{
   const threadid tid = platform_get_tid();
   while (TRUE) {
      uint32 entry_number = treepass_lookup(cc, addr);
      if (entry_number == TP_UNMAPPED_ENTRY) {
         treepass_log(addr,
                        entry_number,
                        "try_discard_page (uncached): entry %u addr %lu\n",
                        entry_number,
                        addr);
         return;
      }

      /*
       * in cache, so evict:
       * 1. read lock
       * 2. wait for loading
       * 3. claim
       * 4. write lock
       * 5. clear lookup, disk_addr
       * 6. set status to TP_FREE_STATUS (clears claim and write lock)
       * 7. reset pincount to zero
       * 8. release read lock
       */

      // platform_assert(treepass_get_ref(cc, entry_number, tid) == 0);

      /* 1. read lock */
      if (treepass_get_read(cc, entry_number) == GET_RC_EVICTED) {
         // raced with eviction, try again
         continue;
      }

      /* 2. wait for loading */
      while (treepass_test_flag(cc, entry_number, TP_LOADING)) {
         treepass_wait(cc);
      }

      treepass_entry *entry = treepass_get_entry(cc, entry_number);

      if (entry->page.disk_addr != addr) {
         // raced with eviction, try again
         treepass_dec_ref(cc, entry_number, tid);
         continue;
      }

      /* 3. claim */
      if (treepass_try_get_claim(cc, entry_number) != GET_RC_SUCCESS) {
         // failed to get claim, try again
         treepass_dec_ref(cc, entry_number, tid);
         continue;
      }

      /* log only after steps that can fail */
      treepass_log(addr,
                     entry_number,
                     "try_discard_page (cached): entry %u addr %lu\n",
                     entry_number,
                     addr);

      /* 4. write lock */
      treepass_get_write(cc, entry_number);

      // --- Discard stats ---
      if (cc->cfg->use_stats) {
         threadid stid = platform_get_tid();
         if (entry->status & TP_CLEAN) {
            cc->stats[stid].discard_clean[entry->type]++;
         } else {
            cc->stats[stid].discard_dirty[entry->type]++;
         }
      }

      /* 5. clear lookup and disk addr; set status to TP_FREE_STATUS */
      uint64 lookup_no      = treepass_divide_by_page_size(cc, addr);
      cc->lookup[lookup_no] = TP_UNMAPPED_ENTRY;
      debug_assert(entry->page.disk_addr == addr);
      entry->page.disk_addr = TP_UNMAPPED_ADDR;
      entry->page.entry_time   = 0;

      /* Ensure addr clear is visible before type clear (readers check addr first) */
      __sync_synchronize();

      /* 6. set status to TP_FREE_STATUS (clears claim, write lock, and clock) */
      platform_assert(entry->waiters.head == NULL);
      entry->type   = PAGE_TYPE_INVALID;
      entry->status = TP_FREE_STATUS;  // Also clears clock bits

      /* 7. reset pincount */
      treepass_reset_pin(cc, entry_number);

      /* 8. release read lock */
      treepass_dec_ref(cc, entry_number, tid);
      return;
   }
}

/*
 *----------------------------------------------------------------------
 * treepass_extent_discard --
 *
 *      Attempts to evict all the pages in the extent. Will wait for writeback,
 *      but will evict and discard dirty pages.
 *----------------------------------------------------------------------
 */
void
treepass_extent_discard(treepass *cc, uint64 addr, page_type type)
{
   debug_assert(addr % treepass_extent_size(cc) == 0);
   debug_assert(allocator_get_refcount(cc->al, addr) == AL_NO_REFS);

   treepass_log(addr, 0, "hard evict extent: addr %lu\n", addr);
   for (uint64 i = 0; i < cc->cfg->pages_per_extent; i++) {
      uint64 page_addr = addr + treepass_multiply_by_page_size(cc, i);
      if (cc->cfg->use_stats) {
         uint32 e = treepass_lookup(cc, page_addr);
         if (e == TP_UNMAPPED_ENTRY) {
            threadid stid = platform_get_tid();
            cc->stats[stid].discard_uncached[type]++;
         }
      }
      treepass_try_page_discard(cc, page_addr);
   }
}

/*
 * Get addr if addr is at entry_number.  Returns TRUE if successful.
 */
static bool32
treepass_get_in_cache(treepass   *cc,           // IN
                        uint64        addr,         // IN
                        bool32        blocking,     // IN
                        page_type     type,         // IN
                        bool32        is_warm,      // IN
                        uint32        entry_number, // IN
                        page_handle **page)         // OUT
{
   threadid tid = platform_get_tid();


   if (blocking) {
      if (treepass_get_read(cc, entry_number) != GET_RC_SUCCESS) {
         // this means we raced with eviction, start over
         treepass_log(addr,
                        entry_number,
                        "get (eviction race): entry %u addr %lu\n",
                        entry_number,
                        addr);
         return TRUE;
      }
      if (treepass_get_entry(cc, entry_number)->page.disk_addr != addr) {
         // this also means we raced with eviction and really lost
         treepass_dec_ref(cc, entry_number, tid);
         return TRUE;
      }
   } else {
      switch (treepass_try_get_read(cc, entry_number, TRUE)) {
         case GET_RC_CONFLICT:
            treepass_log(addr,
                           entry_number,
                           "get (locked -- non-blocking): entry %u addr %lu\n",
                           entry_number,
                           addr);
            *page = NULL;
            return FALSE;
         case GET_RC_EVICTED:
            treepass_log(addr,
                           entry_number,
                           "get (eviction race): entry %u addr %lu\n",
                           entry_number,
                           addr);
            return TRUE;
         case GET_RC_SUCCESS:
            if (treepass_get_entry(cc, entry_number)->page.disk_addr != addr)
            {
               // this also means we raced with eviction and really lost
               treepass_dec_ref(cc, entry_number, tid);
               return TRUE;
            }
            break;
         default:
            platform_assert(0);
      }
   }


   while (treepass_test_flag(cc, entry_number, TP_LOADING)) {
      treepass_wait(cc);
   }


   treepass_entry *entry = treepass_get_entry(cc, entry_number);

   /* Re-verify addr: try_page_discard can clear addr under write lock
    * while we hold a read ref. If addr changed, retry. */
   if (entry->page.disk_addr != addr) {
      treepass_dec_ref(cc, entry_number, tid);
      return TRUE;
   }


   __sync_synchronize();


   /* Double-check: if type mismatches, re-read addr to detect race with
    * try_page_discard (which clears addr before type). */
   if (entry->type != type) {
      if (entry->page.disk_addr != addr) {
         treepass_dec_ref(cc, entry_number, tid);
         return TRUE;
      }
      platform_assert(0,
                      "entry %u type %d != %d",
                      entry_number,
                      entry->type,
                      type);
   }


   // COLD → WARM promotion: if is_warm=true and page is currently COLD, promote to WARM
   // COLD pages have entry_time == 0, try to CAS from 0 -> now to promote
   // Only needed when early eviction is enabled (entry_time used for COLD detection)
   if (is_warm && type == PAGE_TYPE_BRANCH) {
      timestamp entry_time = entry->page.entry_time;
      if (entry_time == 0) {
         // Page is currently COLD, try to promote to WARM by CAS'ing entry_time from 0 -> now
         timestamp now = treepass_current_ts;
         timestamp exp0 = 0;
         if (__sync_bool_compare_and_swap(&entry->page.entry_time, exp0, now)) {
            // Successfully promoted: entry_time is now non-zero, page becomes WARM/META
         }
      }
   }


   if (cc->cfg->use_stats) {
      cc->stats[tid].cache_hits[type]++;
   }
   treepass_log(addr,
                  entry_number,
                  "get (cached): entry %u addr %lu rc %u\n",
                  entry_number,
                  addr,
                  treepass_get_ref(cc, entry_number, tid));
   *page = &entry->page;

   return FALSE;
}

static uint64
treepass_acquire_entry_for_load(treepass *cc, // IN
                                  uint64      addr,
                                  page_type   type) // OUT
{
   threadid          tid          = platform_get_tid();
   uint64            lookup_no    = treepass_divide_by_page_size(cc, addr);
   uint32 load_status = TP_READ_LOADING_STATUS;
   uint32            entry_number = treepass_get_free_page(cc,
                                                  load_status,
                                                  type,
                                                  TRUE,  // refcount
                                                  TRUE); // blocking
   treepass_entry *entry        = treepass_get_entry(cc, entry_number);
   /*
    * If someone else is loading the page and has reserved the lookup, let them
    * do it.
    */
   if (!__sync_bool_compare_and_swap(
          &cc->lookup[lookup_no], TP_UNMAPPED_ENTRY, entry_number))
   {
      treepass_dec_ref(cc, entry_number, tid);
      platform_assert(entry->waiters.head == NULL);
      entry->type   = PAGE_TYPE_INVALID;
      entry->status = TP_FREE_STATUS;  // Also clears clock bits
      treepass_log(addr,
                     entry_number,
                     "get abort: entry: %u addr: %lu\n",
                     entry_number,
                     addr);
      return TP_UNMAPPED_ENTRY;
   }

   /* Set up the page */
   entry->page.disk_addr = addr;
   return entry_number;
}

static void
treepass_finish_load(treepass *cc,      // IN
                       uint64      addr,    // IN
                       uint32      entry_number) // OUT
{
   treepass_log(addr,
                  entry_number,
                  "finish_load): entry %u addr %lu\n",
                  entry_number,
                  addr);

   /* Clear the loading flag */
   debug_only uint32 was_loading =
      treepass_clear_flag(cc, entry_number, TP_LOADING);
   debug_assert(was_loading);

   treepass_entry *entry = treepass_get_entry(cc, entry_number);
   async_wait_queue_release_all(&entry->waiters);
}

static bool32
treepass_get_from_disk(treepass   *cc,   // IN
                         uint64        addr, // IN
                         page_type     type, // IN
                         bool32        is_warm, // IN
                         page_handle **page) // OUT
{
   threadid tid       = platform_get_tid();
   uint64   page_size = treepass_page_size(cc);

   uint64 io_elapsed = 0;
#ifdef CACHE_TIMING_BREAKDOWN
   uint64 miss_start = 0, evict_elapsed = 0, finish_elapsed = 0;
   if (cc->cfg->use_stats) {
      miss_start = platform_get_timestamp();
   }
#endif

   uint64 entry_number = treepass_acquire_entry_for_load(cc, addr, type);
   if (entry_number == TP_UNMAPPED_ENTRY) {
      return TRUE;
   }
   treepass_entry *entry = treepass_get_entry(cc, entry_number);

#ifdef CACHE_TIMING_BREAKDOWN
   if (cc->cfg->use_stats) {
      evict_elapsed = platform_timestamp_elapsed(miss_start);
   }
#endif

   uint64 io_start = 0;
   if (cc->cfg->use_stats) {
      io_start = platform_get_timestamp();
   }

   platform_status status = io_read(cc->io, entry->page.data, page_size, addr);
   platform_assert_status_ok(status);

   if (cc->cfg->use_stats) {
      io_elapsed = platform_timestamp_elapsed(io_start);
   }

   // Set entry_time on disk load:
   // - BRANCH (warm): WARM/COLD classification for early eviction
   // - TRUNK: CI baseline for compaction interval calculation
   if ((type == PAGE_TYPE_BRANCH && is_warm) || type == PAGE_TYPE_TRUNK) {
      timestamp now = treepass_current_ts;
      platform_assert(now != 0,
                      "treepass_current_ts is 0 when loading TRUNK/BRANCH page "
                      "from disk -- caller must initialize treepass_current_ts");
      entry->page.entry_time = now;
   }

#ifdef CACHE_TIMING_BREAKDOWN
   uint64 finish_start = 0;
   if (cc->cfg->use_stats) {
      finish_start = platform_get_timestamp();
   }
#endif

   treepass_finish_load(cc, addr, entry_number);

   if (cc->cfg->use_stats) {
      cc->stats[tid].cache_misses[type]++;
      cc->stats[tid].page_reads[type]++;
      cc->stats[tid].cache_miss_time_ns[type] += io_elapsed;
#ifdef CACHE_TIMING_BREAKDOWN
      finish_elapsed = platform_timestamp_elapsed(finish_start);
      uint64 total_elapsed = platform_timestamp_elapsed(miss_start);
      cc->stats[tid].miss_eviction_time_ns[type] += evict_elapsed;
      cc->stats[tid].miss_io_time_ns[type] += io_elapsed;
      cc->stats[tid].miss_finish_time_ns[type] += finish_elapsed;
      cc->stats[tid].miss_total_time_ns[type] += total_elapsed;
#endif
   }

   *page = &entry->page;

   return FALSE;
}

/*
 *----------------------------------------------------------------------
 * treepass_get_internal --
 *
 *      Attempts to get a pointer to the page_handle for the page with
 *      address addr. If successful returns FALSE indicating no retries
 *      are needed, else TRUE indicating the caller needs to retry.
 *      Updates the "page" argument to the page_handle on success.
 *
 *      Will ask the caller to retry if we race with the eviction or if
 *      we have to evict an entry and race with someone else loading the
 *      entry.
 *      Blocks while the page is loaded into cache if necessary.
 *----------------------------------------------------------------------
 */
debug_only static bool32
treepass_get_internal(treepass   *cc,       // IN
                        uint64        addr,     // IN
                        bool32        blocking, // IN
                        page_type     type,     // IN
                        bool32        is_warm,  // IN
                        page_handle **page)     // OUT
{
   debug_only uint64 page_size = treepass_page_size(cc);
   debug_assert(
      ((addr % page_size) == 0), "addr=%lu, page_size=%lu\n", addr, page_size);

#if SPLINTER_DEBUG
   uint64 base_addr =
      allocator_config_extent_base_addr(allocator_get_config(cc->al), addr);
   refcount extent_ref_count = allocator_get_refcount(cc->al, base_addr);

   // Dump allocated extents info for deeper debugging.
   if (extent_ref_count == AL_FREE) {
      allocator_print_allocated(cc->al);
   }
   debug_assert((extent_ref_count != AL_FREE),
                "Attempt to get a buffer for page addr=%lu"
                ", page type=%d ('%s'),"
                " from extent addr=%lu, (extent number=%lu)"
                ", which is an unallocated extent, extent_ref_count=%u.",
                addr,
                type,
                page_type_str[type],
                base_addr,
                (base_addr / treepass_extent_size(cc)),
                extent_ref_count);
#endif // SPLINTER_DEBUG

   // We expect entry_number to be valid, but it's still validated below
   // in case some arithmetic goes wrong.
   uint32 entry_number = treepass_lookup(cc, addr);

   if (entry_number != TP_UNMAPPED_ENTRY) {
#ifdef CACHE_TIMING_BREAKDOWN
      uint64 hit_start = 0;
      if (cc->cfg->use_stats) {
         hit_start = platform_get_timestamp();
      }
#endif
      bool32 rc = treepass_get_in_cache(
         cc, addr, blocking, type, is_warm, entry_number, page);
#ifdef CACHE_TIMING_BREAKDOWN
      if (cc->cfg->use_stats && !rc) {
         threadid tid = platform_get_tid();
         cc->stats[tid].cache_hit_time_ns[type] +=
            platform_timestamp_elapsed(hit_start);
      }
#endif
      return rc;
   } else if (blocking) {
      return treepass_get_from_disk(cc, addr, type, is_warm, page);
   } else {
      return FALSE;
   }
}

/*
 *----------------------------------------------------------------------
 * treepass_get --
 *
 *      Returns a pointer to the page_handle for the page with address addr.
 *      Calls clockcachge_get_int till a retry is needed.
 *
 *      If blocking is set, then it blocks until the page is unlocked as
 *well.
 *
 *      Returns with a read lock held.
 *----------------------------------------------------------------------
 */
page_handle *
treepass_get(treepass *cc, uint64 addr, bool32 blocking, page_type type, bool32 is_warm)
{
   bool32       retry;
   page_handle *handle;
   TIMING_START();


   debug_assert(cc->per_thread[platform_get_tid()].enable_sync_get
                || type == PAGE_TYPE_MEMTABLE);
   while (1) {
      retry = treepass_get_internal(cc, addr, blocking, type, is_warm, &handle);
      if (!retry) {
         return handle;
      }
   }
}

/*
 * Same as treepass_get but reports whether the page was a cache miss.
 */
page_handle *
treepass_get_with_miss(treepass   *cc,
                      uint64     addr,
                      bool32     blocking,
                      page_type  type,
                      bool32     is_warm,
                      bool32    *was_miss)
{
   bool32       retry;
   page_handle *handle;
   TIMING_START();


   debug_assert(cc->per_thread[platform_get_tid()].enable_sync_get
                || type == PAGE_TYPE_MEMTABLE);

   uint32 entry_number = treepass_lookup(cc, addr);
   *was_miss = (entry_number == TP_UNMAPPED_ENTRY);

   while (1) {
      retry = treepass_get_internal(cc, addr, blocking, type, is_warm, &handle);
      if (!retry) {
         return handle;
      }
   }
}

/*
 * Get addr if addr is at entry_number.  Returns TRUE if successful.
 */

// clang-format off
DEFINE_ASYNC_STATE(treepass_get_async_state, 3,
   param, treepass *, cc,
   param, uint64, addr,
   param, page_type, type,
   param, bool32, is_warm,
   param, async_callback_fn, callback,
   param, void *, callback_arg,
   local, page_handle *, __async_result,
   local, bool32, succeeded,
   local, threadid, tid,
   local, uint64, entry_number,
   local, treepass_entry *, entry,
   local, uint64, page_size,
   local, uint64, base_addr,
   local, refcount, extent_ref_count,
   local, platform_status, rc,
   local, io_async_state_buffer, iostate,
   local, async_waiter, wait_node)
// clang-format on

_Static_assert(sizeof(treepass_get_async_state)
                  <= PAGE_GET_ASYNC_STATE_BUFFER_SIZE,
               "treepass_get_async_state is too large");

/*
 * Result is FALSE if we failed to find the page in cache and hence need to
 * retry the get from the beginning, TRUE if we succeeded.
 */
static async_status
treepass_get_in_cache_async(treepass_get_async_state *state, uint64 depth)
{
   async_begin(state, depth);

   state->tid = platform_get_tid();

   // We don't bother yielding for writers because they are expected to be
   // fast.  We do yield (below) if someone else is loading the page.
   if (treepass_get_read(state->cc, state->entry_number) != GET_RC_SUCCESS) {
      // this means we raced with eviction, start over
      treepass_log(state->addr,
                     state->entry_number,
                     "get (eviction race): entry %u addr %lu\n",
                     state->entry_number,
                     state->addr);
      state->succeeded = FALSE;
      async_return(state);
   }

   state->entry = treepass_get_entry(state->cc, state->entry_number);
   if (state->entry->page.disk_addr != state->addr) {
      // this also means we raced with eviction and really lost
      treepass_dec_ref(state->cc, state->entry_number, state->tid);
      state->succeeded = FALSE;
      async_return(state);
   }

   async_wait_on_queue_until(
      !treepass_test_flag(state->cc, state->entry_number, TP_LOADING),
      state,
      &state->entry->waiters,
      &state->wait_node,
      state->callback,
      state->callback_arg);

   /* Re-verify addr after loading wait: try_page_discard can clear addr
    * while we hold a read ref. */
   if (state->entry->page.disk_addr != state->addr) {
      treepass_dec_ref(state->cc, state->entry_number, state->tid);
      state->succeeded = FALSE;
      async_return(state);
   }

   __sync_synchronize();

   if (state->entry->type != state->type) {
      if (state->entry->page.disk_addr != state->addr) {
         treepass_dec_ref(state->cc, state->entry_number, state->tid);
         state->succeeded = FALSE;
         async_return(state);
      }
      platform_assert(0,
                      "entry->type %d != state->type %d\n",
                      state->entry->type,
                      state->type);
   }

   // COLD → WARM promotion (same as sync path)
   if (state->is_warm && state->type == PAGE_TYPE_BRANCH)
   {
      timestamp entry_time = state->entry->page.entry_time;
      if (entry_time == 0) {
         timestamp now  = treepass_current_ts;
         timestamp exp0 = 0;
         __sync_bool_compare_and_swap(&state->entry->page.entry_time, exp0, now);
      }
   }

   if (state->cc->cfg->use_stats) {
      state->cc->stats[state->tid].cache_hits[state->type]++;
   }
   treepass_log(
      state->addr,
      state->entry_number,
      "get (cached): entry %u addr %lu rc %u\n",
      state->entry_number,
      state->addr,
      treepass_get_ref(state->cc, state->entry_number, state->tid));
   state->__async_result = &state->entry->page;
   state->succeeded      = TRUE;
   async_return(state);
}

void
treepass_get_from_disk_async_callback(void *arg)
{
   treepass_get_async_state *state = (treepass_get_async_state *)arg;

   // Set entry_time on disk load (same as sync path).
   // Use platform_get_timestamp() directly instead of treepass_current_ts because
   // this callback may run in the laio_cleaner background thread, which never
   // calls core_lookup_async and therefore has treepass_current_ts = 0.
   if ((state->type == PAGE_TYPE_BRANCH && state->is_warm)
       || state->type == PAGE_TYPE_TRUNK) {
      timestamp now              = platform_get_timestamp();
      state->entry->page.entry_time = now;
   }

   treepass_finish_load(state->cc, state->addr, state->entry_number);

   if (state->cc->cfg->use_stats) {
      state->cc->stats[state->tid].cache_misses[state->type]++;
      state->cc->stats[state->tid].page_reads[state->type]++;
   }

   state->callback(state->callback_arg);
}

static async_status
treepass_get_from_disk_async(treepass_get_async_state *state, uint64 depth)
{
   async_begin(state, depth);

   state->entry_number =
      treepass_acquire_entry_for_load(state->cc, state->addr, state->type);
   if (state->entry_number == TP_UNMAPPED_ENTRY) {
      state->succeeded = FALSE;
      async_return(state);
   }
   state->entry = treepass_get_entry(state->cc, state->entry_number);

   // The normal idiom for async functions is to just pass the callback to the
   // async child, but we pass a wrapper function so that we can always clear
   // the TP_LOADING flag, even if our caller abandoned us.
   state->rc = io_async_state_init(state->iostate,
                                   state->cc->io,
                                   io_async_preadv,
                                   state->addr,
                                   treepass_get_from_disk_async_callback,
                                   state);
   // FIXME: I'm not sure if the cache state machine allows us to bail out once
   // we've acquired an entry, because other threads could now be waiting on the
   // load to finish, and there is no way for them to handle our failure to load
   // the page.
   platform_assert_status_ok(state->rc);

   state->rc =
      io_async_state_append_page(state->iostate, state->entry->page.data);
   platform_assert_status_ok(state->rc);

   while (io_async_run(state->iostate) != ASYNC_STATUS_DONE) {
      async_yield(state);
   }
   platform_assert_status_ok(io_async_state_get_result(state->iostate));
   io_async_state_deinit(state->iostate);

   state->__async_result = &state->entry->page;
   state->succeeded      = TRUE;
   async_return(state);
}

// Result is TRUE if successful, FALSE otherwise
static async_status
treepass_get_internal_async(treepass_get_async_state *state, uint64 depth)
{
   async_begin(state, depth);

   state->tid = platform_get_tid();

   state->page_size = treepass_page_size(state->cc);
   debug_assert(((state->addr % state->page_size) == 0),
                "addr=%lu, page_size=%lu\n",
                state->addr,
                state->page_size);

#if SPLINTER_DEBUG
   state->base_addr = allocator_config_extent_base_addr(
      allocator_get_config(state->cc->al), state->addr);
   state->extent_ref_count =
      allocator_get_refcount(state->cc->al, state->base_addr);

   // Dump allocated extents info for deeper debugging.
   if (state->extent_ref_count == AL_FREE) {
      allocator_print_allocated(state->cc->al);
   }
   debug_assert((state->extent_ref_count != AL_FREE),
                "Attempt to get a buffer for page addr=%lu"
                ", page type=%d ('%s'),"
                " from extent addr=%lu, (extent number=%lu)"
                ", which is an unallocated extent, extent_ref_count=%u.",
                state->addr,
                state->type,
                page_type_str[state->type],
                state->base_addr,
                (state->base_addr / treepass_extent_size(state->cc)),
                state->extent_ref_count);
#endif // SPLINTER_DEBUG

   // We expect entry_number to be valid, but it's still validated below
   // in case some arithmetic goes wrong.
   state->entry_number = treepass_lookup(state->cc, state->addr);

   if (state->entry_number != TP_UNMAPPED_ENTRY) {
      async_await_subroutine(state, treepass_get_in_cache_async);
   } else {
      async_await_subroutine(state, treepass_get_from_disk_async);
   }
   async_return(state);
}

async_status
treepass_get_async(treepass_get_async_state *state)
{
   async_begin(state, 0);

   debug_assert(state->cc->per_thread[platform_get_tid()].enable_sync_get
                || state->type == PAGE_TYPE_MEMTABLE);

   state->succeeded = FALSE;
   while (!state->succeeded) {
      async_await_subroutine(state, treepass_get_internal_async);
   }
   async_return(state);
}

void
treepass_unget(treepass *cc, page_handle *page)
{
   uint32         entry_number = treepass_page_to_entry_number(cc, page);
   const threadid tid          = platform_get_tid();

   treepass_record_backtrace(cc, entry_number);

   treepass_log(page->disk_addr,
                  entry_number,
                  "unget: entry %u addr %lu rc %u\n",
                  entry_number,
                  page->disk_addr,
                  treepass_get_ref(cc, entry_number, tid) - 1);

   /* First release of a prefetched page: consumption is done — clear the
    * prefetch tag back to COLD so the EE pass can reclaim it. */
   __sync_bool_compare_and_swap(&page->entry_time,
                                TP_ENTRY_TIME_PREFETCHED,
                                (timestamp)0);

   treepass_dec_ref(cc, entry_number, tid);
}

/*
 *----------------------------------------------------------------------
 * treepass_try_claim --
 *
 *      Upgrades a read lock to a claim. This function does not block and
 *      returns TRUE if the claim was successfully obtained.
 *
 *      A claimed node has the TP_CLAIMED bit set in its status vector.
 *
 *      NOTE: When a call to claim fails, the caller must drop and reobtain
 *the readlock before trying to claim again to avoid deadlock.
 *----------------------------------------------------------------------
 */
bool32
treepass_try_claim(treepass *cc, page_handle *page)
{
   uint32 entry_number = treepass_page_to_entry_number(cc, page);

   treepass_record_backtrace(cc, entry_number);
   treepass_log(page->disk_addr,
                  entry_number,
                  "claim: entry %u addr %lu\n",
                  entry_number,
                  page->disk_addr);

   return treepass_try_get_claim(cc, entry_number) == GET_RC_SUCCESS;
}

void
treepass_unclaim(treepass *cc, page_handle *page)
{
   uint32 entry_number = treepass_page_to_entry_number(cc, page);

   treepass_record_backtrace(cc, entry_number);
   treepass_log(page->disk_addr,
                  entry_number,
                  "unclaim: entry %u addr %lu\n",
                  entry_number,
                  page->disk_addr);

   debug_only uint32 status =
      treepass_clear_flag(cc, entry_number, TP_CLAIMED);
   debug_assert(status);
}

/*
 *----------------------------------------------------------------------
 * treepass_lock --
 *
 *     Write locks a claimed page and blocks while any read locks are
 *released.
 *
 *     The write lock is indicated by having the TP_WRITELOCKED flag set in
 *     addition to the TP_CLAIMED flag.
 *----------------------------------------------------------------------
 */
void
treepass_lock(treepass *cc, page_handle *page)
{
   uint32 entry_number = treepass_page_to_entry_number(cc, page);

   treepass_record_backtrace(cc, entry_number);
   treepass_log(page->disk_addr,
                  entry_number,
                  "lock: entry %u addr %lu\n",
                  entry_number,
                  page->disk_addr);
   treepass_get_write(cc, entry_number);
}

void
treepass_unlock(treepass *cc, page_handle *page)
{
   uint32 entry_number = treepass_page_to_entry_number(cc, page);

   treepass_record_backtrace(cc, entry_number);
   treepass_log(page->disk_addr,
                  entry_number,
                  "unlock: entry %u addr %lu\n",
                  entry_number,
                  page->disk_addr);
   debug_only uint32 was_writing =
      treepass_clear_flag(cc, entry_number, TP_WRITELOCKED);
   debug_assert(was_writing);
}

/*----------------------------------------------------------------------
 * treepass_mark_dirty --
 *
 *      Marks the entry dirty.
 *----------------------------------------------------------------------
 */
void
treepass_mark_dirty(treepass *cc, page_handle *page)
{
   debug_only treepass_entry *entry = treepass_page_to_entry(cc, page);
   uint32 entry_number = treepass_page_to_entry_number(cc, page);

   treepass_log(entry->page.disk_addr,
                  entry_number,
                  "mark_dirty: entry %u addr %lu\n",
                  entry_number,
                  entry->page.disk_addr);
   treepass_clear_flag(cc, entry_number, TP_CLEAN);
   return;
}

/*
 *----------------------------------------------------------------------
 * treepass_pin --
 *
 *      Functionally equivalent to an anonymous read lock. Implemented using
 *a special ref count.
 *
 *      A write lock must be held while pinning to avoid a race with
 *eviction.
 *----------------------------------------------------------------------
 */
void
treepass_pin(treepass *cc, page_handle *page)
{
   debug_only treepass_entry *entry = treepass_page_to_entry(cc, page);
   uint32 entry_number = treepass_page_to_entry_number(cc, page);
   debug_assert(treepass_test_flag(cc, entry_number, TP_WRITELOCKED));
   treepass_inc_pin(cc, entry_number);

   treepass_log(entry->page.disk_addr,
                  entry_number,
                  "pin: entry %u addr %lu\n",
                  entry_number,
                  entry->page.disk_addr);
}

void
treepass_unpin(treepass *cc, page_handle *page)
{
   debug_only treepass_entry *entry = treepass_page_to_entry(cc, page);
   uint32 entry_number = treepass_page_to_entry_number(cc, page);
   treepass_dec_pin(cc, entry_number);

   treepass_log(entry->page.disk_addr,
                  entry_number,
                  "unpin: entry %u addr %lu\n",
                  entry_number,
                  entry->page.disk_addr);
}

/*
 *-----------------------------------------------------------------------------
 * treepass_page_sync --
 *
 *      Asynchronously syncs the page. Currently there is no way to check
 *when the writeback has completed.
 *-----------------------------------------------------------------------------
 */
void
treepass_page_sync(treepass  *cc,
                     page_handle *page,
                     bool32       is_blocking,
                     page_type    type)
{
   uint32          entry_number = treepass_page_to_entry_number(cc, page);
   async_io_state *state;
   uint64          addr = page->disk_addr;
   const threadid  tid  = platform_get_tid();
   platform_status status;

   if (!treepass_try_set_writeback(cc, entry_number, TP_CLOCK_MAX)) {
      platform_assert(treepass_test_flag(cc, entry_number, TP_CLEAN));
      return;
   }

   if (cc->cfg->use_stats) {
      cc->stats[tid].page_writes[type]++;
      cc->stats[tid].syncs_issued++;
   }

   if (!is_blocking) {
      state = TYPED_MALLOC(cc->heap_id, state);
      platform_assert(state);
      state->cc                = cc;
      state->outstanding_pages = NULL;
      state->num_passes        = 0;
      io_async_state_init(state->iostate,
                          cc->io,
                          io_async_pwritev,
                          addr,
                          treepass_write_callback,
                          state);
      io_async_state_append_page(state->iostate, page->data);
      io_async_run(state->iostate);
   } else {
      status = io_write(cc->io, page->data, treepass_page_size(cc), addr);
      platform_assert_status_ok(status);
      treepass_log(addr,
                     entry_number,
                     "page_sync write entry %u addr %lu\n",
                     entry_number,
                     addr);
      debug_only uint8 rc;
      rc = treepass_set_flag(cc, entry_number, TP_CLEAN);
      debug_assert(!rc);
      rc = treepass_clear_flag(cc, entry_number, TP_WRITEBACK);
      debug_assert(rc);
   }
}

/*
 *-----------------------------------------------------------------------------
 * treepass_extent_sync --
 *
 *      Asynchronously syncs the extent.
 *
 *      Adds the number of pages issued writeback to the counter pointed to
 *      by pages_outstanding. When the writes complete, a callback subtracts
 *      them off, so that the caller may track how many pages are in
 *writeback.
 *
 *      Assumes all pages in the extent are clean or cleanable
 *-----------------------------------------------------------------------------
 */
void
treepass_extent_sync(treepass *cc, uint64 addr, uint64 *pages_outstanding)
{
   async_io_state *state = NULL;
   uint64          i;
   uint32          entry_number;
   uint64          req_count = 0;
   uint64          req_addr;
   uint64          page_addr;

   for (i = 0; i < cc->cfg->pages_per_extent; i++) {
      page_addr    = addr + treepass_multiply_by_page_size(cc, i);
      entry_number = treepass_lookup(cc, page_addr);
      if (entry_number != TP_UNMAPPED_ENTRY
          && treepass_try_set_writeback(cc, entry_number, TP_CLOCK_MAX))
      {
         if (state == NULL) {
            req_addr = page_addr;
            state    = TYPED_MALLOC(cc->heap_id, state);
            platform_assert(state);
            state->cc                = cc;
            state->outstanding_pages = pages_outstanding;
            state->num_passes        = 0;
            io_async_state_init(state->iostate,
                                cc->io,
                                io_async_pwritev,
                                req_addr,
                                treepass_write_callback,
                                state);
         }
         io_async_state_append_page(
            state->iostate, treepass_get_entry(cc, entry_number)->page.data);
         req_count++;
      } else {
         // ALEX: There is maybe a race with eviction with this assertion
         debug_assert(entry_number == TP_UNMAPPED_ENTRY
                      || treepass_test_flag(cc, entry_number, TP_CLEAN));
         if (state != NULL) {
            __sync_fetch_and_add(pages_outstanding, req_count);
            io_async_run(state->iostate);
            state     = NULL;
            req_count = 0;
         }
      }
   }
   if (state != NULL) {
      __sync_fetch_and_add(pages_outstanding, req_count);
      io_async_run(state->iostate);
   }
}

/*
 * Clockcache prefetching
 *
 * The main trickiness here is that we call io_async_read() from the callback we
 * get from io_async_read().  The callback will actually come from io_cleanup,
 * but Sometimes the callback will occur before the first invocation of
 * io_async_read has even finished, so we need to avoid running two instances of
 * io_async_read() at the same time on the same state structure.  We accomplish
 * this by using a lock in the state structure.
 *
 * The other trickiness is that we need to free the state structure in the
 * callback, but only once we are done, and we need to ensure that there is not
 * another callback in progress when we free the state structure.  Because of
 * the lock, we get to execute only once our parent (and hence all ancestors)
 * has finished, so we don't have to worry about our parents.  And we spawn a
 * child callback only if our call to io_async_read() returns that the read is
 * not done, and we only free the state structure if the read is done.
 *
 * Hence we free the state structure only when we are the only callback in
 * progress.
 */

/*
 *----------------------------------------------------------------------
 * treepass_prefetch_callback --
 *
 *      Internal callback function to clean up after prefetching a collection
 *      of pages from the device.
 *----------------------------------------------------------------------
 */
static void
treepass_prefetch_callback(void *pfs)
{
   async_io_state *state = (async_io_state *)pfs;

   // Check whether we are done.  If not, this will enqueue us for a future
   // callback so we can check again.
   if (io_async_run(state->iostate) != ASYNC_STATUS_DONE) {
      return;
   }

   platform_assert_status_ok(io_async_state_get_result(state->iostate));

   const struct iovec *iovec;
   uint64              count;
   iovec = io_async_state_get_iovec(state->iostate, &count);

   treepass          *cc        = state->cc;
   debug_only page_type type      = PAGE_TYPE_INVALID;
   debug_only uint64    last_addr = TP_UNMAPPED_ADDR;

   platform_assert(count > 0);
   platform_assert(count <= cc->cfg->pages_per_extent);

   debug_code(uint64 page_size = treepass_page_size(cc));
   for (uint64 page_off = 0; page_off < count; page_off++) {
      uint32 entry_no =
         treepass_data_to_entry_number(cc, (char *)iovec[page_off].iov_base);
      treepass_entry *entry = &cc->entry[entry_no];
      if (page_off != 0) {
         debug_assert(type == entry->type);
      } else {
         type = entry->type;
      }

      uint64 addr = entry->page.disk_addr;
      debug_assert(addr != TP_UNMAPPED_ADDR);
      debug_assert(last_addr == TP_UNMAPPED_ADDR
                   || addr == last_addr + page_size);
      debug_code(last_addr = addr);
      debug_assert(entry_no == treepass_lookup(cc, addr));

      treepass_finish_load(cc, addr, entry_no);
   }

   io_async_state_deinit(state->iostate);
   platform_free(cc->heap_id, state);
}

/*
 *-----------------------------------------------------------------------------
 * treepass_prefetch --
 *
 *      prefetch asynchronously loads the extent with given base address
 *-----------------------------------------------------------------------------
 */
void
treepass_prefetch(treepass *cc, uint64 base_addr, page_type type)
{
   async_io_state *state            = NULL;
   uint64          pages_per_extent = cc->cfg->pages_per_extent;
   threadid        tid              = platform_get_tid();

   debug_assert(base_addr % treepass_extent_size(cc) == 0);

   for (uint64 page_off = 0; page_off < pages_per_extent; page_off++) {
      uint64 addr = base_addr + treepass_multiply_by_page_size(cc, page_off);
      uint32 entry_no = treepass_lookup(cc, addr);
      get_rc get_read_rc;
      if (entry_no != TP_UNMAPPED_ENTRY) {
         get_read_rc = treepass_try_get_read(cc, entry_no, TRUE);
      } else {
         get_read_rc = GET_RC_EVICTED;
      }

      switch (get_read_rc) {
         case GET_RC_SUCCESS:
            treepass_dec_ref(cc, entry_no, tid);
            // fallthrough
         case GET_RC_CONFLICT:
            // in cache, issue IO req if started
            if (state != NULL) {
               if (cc->cfg->use_stats) {
                  threadid tid = platform_get_tid();
                  uint64   count;
                  io_async_state_get_iovec(state->iostate, &count);
                  cc->stats[tid].page_reads[type] += count;
                  cc->stats[tid].prefetches_issued[type]++;
               }
               io_async_run(state->iostate);
               state = NULL;
            }
            treepass_log(addr,
                           entry_no,
                           "prefetch (cached): entry %u addr %lu\n",
                           entry_no,
                           addr);
            break;
         case GET_RC_EVICTED:
         {
            // need to prefetch
            uint32 pf_load_status = TP_READ_LOADING_STATUS;
            uint32 free_entry_no = treepass_get_free_page(
               cc, pf_load_status, type, FALSE, TRUE);
            treepass_entry *entry = &cc->entry[free_entry_no];
            entry->page.disk_addr   = addr;
            entry->type             = type;
            // Tag prefetched branch pages so the EE pass cannot reclaim them
            // before their consumer reads them; the consumer's unget resets
            // the tag to 0 (COLD) so consumed pages are reclaimable as before.
            // Other page types are not subject to EE, so they stay untagged.
            if (type == PAGE_TYPE_BRANCH) {
               entry->page.entry_time = TP_ENTRY_TIME_PREFETCHED;
            }
            uint64 lookup_no        = treepass_divide_by_page_size(cc, addr);
            if (__sync_bool_compare_and_swap(
                   &cc->lookup[lookup_no], TP_UNMAPPED_ENTRY, free_entry_no))
            {
               if (state == NULL) {
                  // start a new IO req
                  state = TYPED_MALLOC(cc->heap_id, state);
                  platform_assert(state);
                  state->cc = cc;
                  io_async_state_init(state->iostate,
                                      cc->io,
                                      io_async_preadv,
                                      addr,
                                      treepass_prefetch_callback,
                                      state);
               }
               platform_status rc =
                  io_async_state_append_page(state->iostate, entry->page.data);
               platform_assert_status_ok(rc);
               treepass_log(addr,
                              entry_no,
                              "prefetch (load): entry %u addr %lu\n",
                              entry_no,
                              addr);
            } else {
               /*
                * someone else is already loading this page, release the free
                * entry and retry
                */
               entry->page.disk_addr = TP_UNMAPPED_ADDR;
               entry->page.entry_time   = 0;
               entry->type           = PAGE_TYPE_INVALID;
               platform_assert(entry->waiters.head == NULL);
               entry->status = TP_FREE_STATUS;  // Also clears clock bits
               page_off--;
            }
            break;
         }
         default:
            platform_assert(0);
      }
   }
   // issue IO req if started
   if (state != NULL) {
      if (cc->cfg->use_stats) {
         threadid tid = platform_get_tid();
         uint64   count;
         io_async_state_get_iovec(state->iostate, &count);
         cc->stats[tid].page_reads[type] += count;
         cc->stats[tid].prefetches_issued[type]++;
      }
      io_async_run(state->iostate);
      state = NULL;
   }
}

/*
 *----------------------------------------------------------------------
 * treepass_print --
 *
 *      Prints a bitmap representation of the cache.
 *----------------------------------------------------------------------
 */
void
treepass_print(platform_log_handle *log_handle, treepass *cc)
{
   uint64   i;
   uint32   status;
   uint16   refcount;
   threadid thr_i;

   platform_log(log_handle,
                "************************** CACHE CONTENTS "
                "**************************\n");
   for (i = 0; i < cc->cfg->page_capacity; i++) {
      if (i != 0 && i % 16 == 0) {
         platform_log(log_handle, "\n");
      }
      if (i % TP_ENTRIES_PER_BATCH == 0) {
         platform_log(log_handle,
                      "Word %lu entries %lu-%lu\n",
                      (i / TP_ENTRIES_PER_BATCH),
                      i,
                      i + 63);
      }
      status   = cc->entry[i].status;
      refcount = 0;
      for (thr_i = 0; thr_i < TP_RC_WIDTH; thr_i++) {
         refcount += treepass_get_ref(cc, i, thr_i);
      }
      platform_log(log_handle, "0x%02x-%u ", status, refcount);
   }

   platform_log(log_handle, "\n\n");
   return;
}

void
treepass_validate_page(treepass *cc, page_handle *page, uint64 addr)
{
   debug_assert(allocator_page_valid(cc->al, addr));
   debug_assert(page->disk_addr == addr);
   debug_assert(!treepass_test_flag(
      cc, treepass_page_to_entry_number(cc, page), TP_FREE));
}

void
treepass_assert_ungot(treepass *cc, uint64 addr)
{
   uint32 entry_number = treepass_lookup(cc, addr);

   if (entry_number != TP_UNMAPPED_ENTRY) {
      for (threadid tid = 0; tid < TP_RC_WIDTH; tid++) {
         debug_only uint16 ref_count =
            treepass_get_ref(cc, entry_number, tid);
         debug_assert(ref_count == 0,
                      "Entry %u addr %lu has ref count %u for thread %lu",
                      entry_number,
                      addr,
                      ref_count,
                      tid);
      }
   }
}

void
treepass_io_stats(treepass *cc, uint64 *read_bytes, uint64 *write_bytes)
{
   *read_bytes  = 0;
   *write_bytes = 0;

   if (!cc->cfg->use_stats) {
      return;
   }

   uint64 read_pages  = 0;
   uint64 write_pages = 0;
   for (uint64 i = 0; i < MAX_THREADS; i++) {
      for (page_type type = 0; type < NUM_PAGE_TYPES; type++) {
         write_pages += cc->stats[i].page_writes[type];
         read_pages += cc->stats[i].page_reads[type];
      }
   }

   *write_bytes = write_pages * 4 * KiB;
   *read_bytes  = read_pages * 4 * KiB;
}


void
treepass_print_stats(platform_log_handle *log_handle, treepass *cc)
{
   uint64      i;
   page_type   type;
   cache_stats global_stats;

   if (!cc->cfg->use_stats) {
      return;
   }

   uint64 page_writes = 0;
   ZERO_CONTENTS(&global_stats);
   for (i = 0; i < MAX_THREADS; i++) {
      for (type = 0; type < NUM_PAGE_TYPES; type++) {
         global_stats.cache_hits[type] += cc->stats[i].cache_hits[type];
         global_stats.cache_misses[type] += cc->stats[i].cache_misses[type];
         global_stats.cache_miss_time_ns[type] +=
            cc->stats[i].cache_miss_time_ns[type];
         global_stats.page_writes[type] += cc->stats[i].page_writes[type];
         page_writes += cc->stats[i].page_writes[type];
         global_stats.page_reads[type] += cc->stats[i].page_reads[type];
         global_stats.prefetches_issued[type] +=
            cc->stats[i].prefetches_issued[type];
         global_stats.discard_clean[type] += cc->stats[i].discard_clean[type];
         global_stats.discard_dirty[type] += cc->stats[i].discard_dirty[type];
         global_stats.discard_uncached[type] +=
            cc->stats[i].discard_uncached[type];
         global_stats.cache_hit_time_ns[type] +=
            cc->stats[i].cache_hit_time_ns[type];
         global_stats.miss_eviction_time_ns[type] +=
            cc->stats[i].miss_eviction_time_ns[type];
         global_stats.miss_io_time_ns[type] +=
            cc->stats[i].miss_io_time_ns[type];
         global_stats.miss_finish_time_ns[type] +=
            cc->stats[i].miss_finish_time_ns[type];
         global_stats.miss_total_time_ns[type] +=
            cc->stats[i].miss_total_time_ns[type];
      }
      global_stats.writes_issued += cc->stats[i].writes_issued;
      global_stats.syncs_issued += cc->stats[i].syncs_issued;

   }

   fraction miss_time[NUM_PAGE_TYPES];
   fraction avg_prefetch_pages[NUM_PAGE_TYPES];
   fraction avg_write_pages;

   for (type = 0; type < NUM_PAGE_TYPES; type++) {
      miss_time[type] =
         init_fraction(global_stats.cache_miss_time_ns[type], SEC_TO_NSEC(1));
      avg_prefetch_pages[type] = init_fraction(
         global_stats.page_reads[type] - global_stats.cache_misses[type],
         global_stats.prefetches_issued[type]);
   }
   avg_write_pages = init_fraction(page_writes - global_stats.syncs_issued,
                                   global_stats.writes_issued);

   // Compute totals for TOTAL column
   uint64 total_hits = 0, total_misses = 0, total_miss_time_ns = 0;
   uint64 total_page_reads = 0, total_page_writes = 0;
   for (type = 0; type < NUM_PAGE_TYPES; type++) {
      total_hits += global_stats.cache_hits[type];
      total_misses += global_stats.cache_misses[type];
      total_miss_time_ns += global_stats.cache_miss_time_ns[type];
      total_page_reads += global_stats.page_reads[type];
      total_page_writes += global_stats.page_writes[type];
   }
   double hit_ratio = (total_hits + total_misses > 0)
      ? 100.0 * (double)total_hits / (double)(total_hits + total_misses)
      : 0.0;
   fraction total_miss_time = init_fraction(total_miss_time_ns, SEC_TO_NSEC(1));

   // clang-format off
   platform_log(log_handle, "Cache Statistics\n");
   platform_log(log_handle, "----------------|------------|------------|------------|------------|------------|------------|------------|\n");
   platform_log(log_handle, "page type       |      trunk |     branch |   memtable |     filter |        log |       misc |      TOTAL |\n");
   platform_log(log_handle, "----------------|------------|------------|------------|------------|------------|------------|------------|\n");
   platform_log(log_handle, "cache hits      | %10.2E | %10.2E | %10.2E | %10.2E | %10.2E | %10.2E | %10.2E |\n",
         (double)global_stats.cache_hits[PAGE_TYPE_TRUNK],
         (double)global_stats.cache_hits[PAGE_TYPE_BRANCH],
         (double)global_stats.cache_hits[PAGE_TYPE_MEMTABLE],
         (double)global_stats.cache_hits[PAGE_TYPE_FILTER],
         (double)global_stats.cache_hits[PAGE_TYPE_LOG],
         (double)global_stats.cache_hits[PAGE_TYPE_SUPERBLOCK],
         (double)total_hits);
   platform_log(log_handle, "cache misses    | %10.2E | %10.2E | %10.2E | %10.2E | %10.2E | %10.2E | %10.2E |\n",
         (double)global_stats.cache_misses[PAGE_TYPE_TRUNK],
         (double)global_stats.cache_misses[PAGE_TYPE_BRANCH],
         (double)global_stats.cache_misses[PAGE_TYPE_MEMTABLE],
         (double)global_stats.cache_misses[PAGE_TYPE_FILTER],
         (double)global_stats.cache_misses[PAGE_TYPE_LOG],
         (double)global_stats.cache_misses[PAGE_TYPE_SUPERBLOCK],
         (double)total_misses);
   platform_log(log_handle, "cache miss time | " FRACTION_FMT(9, 2)"s | "
                FRACTION_FMT(9, 2)"s | "FRACTION_FMT(9, 2)"s | "
                FRACTION_FMT(9, 2)"s | "FRACTION_FMT(9, 2)"s | "
                FRACTION_FMT(9, 2)"s | "FRACTION_FMT(9, 2)"s |\n",
                FRACTION_ARGS(miss_time[PAGE_TYPE_TRUNK]),
                FRACTION_ARGS(miss_time[PAGE_TYPE_BRANCH]),
                FRACTION_ARGS(miss_time[PAGE_TYPE_MEMTABLE]),
                FRACTION_ARGS(miss_time[PAGE_TYPE_FILTER]),
                FRACTION_ARGS(miss_time[PAGE_TYPE_LOG]),
                FRACTION_ARGS(miss_time[PAGE_TYPE_SUPERBLOCK]),
                FRACTION_ARGS(total_miss_time));
   platform_log(log_handle, "pages written   | %10.2E | %10.2E | %10.2E | %10.2E | %10.2E | %10.2E | %10.2E |\n",
         (double)global_stats.page_writes[PAGE_TYPE_TRUNK],
         (double)global_stats.page_writes[PAGE_TYPE_BRANCH],
         (double)global_stats.page_writes[PAGE_TYPE_MEMTABLE],
         (double)global_stats.page_writes[PAGE_TYPE_FILTER],
         (double)global_stats.page_writes[PAGE_TYPE_LOG],
         (double)global_stats.page_writes[PAGE_TYPE_SUPERBLOCK],
         (double)total_page_writes);
   // Discard stats (clean=wasted writeback, dirty=saved, uncached=already evicted)
   uint64 total_discard_clean = 0, total_discard_dirty = 0, total_discard_uncached = 0;
   for (type = 0; type < NUM_PAGE_TYPES; type++) {
      total_discard_clean += global_stats.discard_clean[type];
      total_discard_dirty += global_stats.discard_dirty[type];
      total_discard_uncached += global_stats.discard_uncached[type];
   }
   platform_log(log_handle, "discard clean   | %10.2E | %10.2E | %10.2E | %10.2E | %10.2E | %10.2E | %10.2E |\n",
         (double)global_stats.discard_clean[PAGE_TYPE_TRUNK],
         (double)global_stats.discard_clean[PAGE_TYPE_BRANCH],
         (double)global_stats.discard_clean[PAGE_TYPE_MEMTABLE],
         (double)global_stats.discard_clean[PAGE_TYPE_FILTER],
         (double)global_stats.discard_clean[PAGE_TYPE_LOG],
         (double)global_stats.discard_clean[PAGE_TYPE_SUPERBLOCK],
         (double)total_discard_clean);
   platform_log(log_handle, "discard dirty   | %10.2E | %10.2E | %10.2E | %10.2E | %10.2E | %10.2E | %10.2E |\n",
         (double)global_stats.discard_dirty[PAGE_TYPE_TRUNK],
         (double)global_stats.discard_dirty[PAGE_TYPE_BRANCH],
         (double)global_stats.discard_dirty[PAGE_TYPE_MEMTABLE],
         (double)global_stats.discard_dirty[PAGE_TYPE_FILTER],
         (double)global_stats.discard_dirty[PAGE_TYPE_LOG],
         (double)global_stats.discard_dirty[PAGE_TYPE_SUPERBLOCK],
         (double)total_discard_dirty);
   platform_log(log_handle, "discard uncache | %10.2E | %10.2E | %10.2E | %10.2E | %10.2E | %10.2E | %10.2E |\n",
         (double)global_stats.discard_uncached[PAGE_TYPE_TRUNK],
         (double)global_stats.discard_uncached[PAGE_TYPE_BRANCH],
         (double)global_stats.discard_uncached[PAGE_TYPE_MEMTABLE],
         (double)global_stats.discard_uncached[PAGE_TYPE_FILTER],
         (double)global_stats.discard_uncached[PAGE_TYPE_LOG],
         (double)global_stats.discard_uncached[PAGE_TYPE_SUPERBLOCK],
         (double)total_discard_uncached);
   platform_log(log_handle, "pages read      | %10.2E | %10.2E | %10.2E | %10.2E | %10.2E | %10.2E | %10.2E |\n",
         (double)global_stats.page_reads[PAGE_TYPE_TRUNK],
         (double)global_stats.page_reads[PAGE_TYPE_BRANCH],
         (double)global_stats.page_reads[PAGE_TYPE_MEMTABLE],
         (double)global_stats.page_reads[PAGE_TYPE_FILTER],
         (double)global_stats.page_reads[PAGE_TYPE_LOG],
         (double)global_stats.page_reads[PAGE_TYPE_SUPERBLOCK],
         (double)total_page_reads);
   platform_log(log_handle, "avg prefetch pg |  " FRACTION_FMT(9, 2)" |  "
                FRACTION_FMT(9, 2)" |  "FRACTION_FMT(9, 2)" |  "
                FRACTION_FMT(9, 2)" |  "FRACTION_FMT(9, 2)" |  "
                FRACTION_FMT(9, 2)" |            |\n",
                FRACTION_ARGS(avg_prefetch_pages[PAGE_TYPE_TRUNK]),
                FRACTION_ARGS(avg_prefetch_pages[PAGE_TYPE_BRANCH]),
                FRACTION_ARGS(avg_prefetch_pages[PAGE_TYPE_MEMTABLE]),
                FRACTION_ARGS(avg_prefetch_pages[PAGE_TYPE_FILTER]),
                FRACTION_ARGS(avg_prefetch_pages[PAGE_TYPE_LOG]),
                FRACTION_ARGS(avg_prefetch_pages[PAGE_TYPE_SUPERBLOCK]));
   platform_log(log_handle, "----------------|------------|------------|------------|------------|------------|------------|------------|\n");
   #define HIT_RATIO(t) ((global_stats.cache_hits[t] + global_stats.cache_misses[t]) > 0 \
      ? 100.0 * (double)global_stats.cache_hits[t] / (double)(global_stats.cache_hits[t] + global_stats.cache_misses[t]) \
      : 0.0)
   platform_log(log_handle, "hit ratio       |    %6.2f%% |    %6.2f%% |    %6.2f%% |    %6.2f%% |    %6.2f%% |    %6.2f%% |    %6.2f%% |\n",
         HIT_RATIO(PAGE_TYPE_TRUNK), HIT_RATIO(PAGE_TYPE_BRANCH),
         HIT_RATIO(PAGE_TYPE_MEMTABLE), HIT_RATIO(PAGE_TYPE_FILTER),
         HIT_RATIO(PAGE_TYPE_LOG), HIT_RATIO(PAGE_TYPE_SUPERBLOCK), hit_ratio);
   #undef HIT_RATIO
   platform_log(log_handle, "----------------|------------|------------|------------|------------|------------|------------|------------|\n");
   platform_log(log_handle, "avg write pgs: "FRACTION_FMT(9,2)"\n",
                FRACTION_ARGS(avg_write_pages));

   // clang-format on


   allocator_print_stats(cc->al);

}

void
treepass_reset_stats(treepass *cc)
{
   uint64 i;

   for (i = 0; i < MAX_THREADS; i++) {
      cache_stats *stats = &cc->stats[i];

      memset(stats->cache_hits, 0, sizeof(stats->cache_hits));
      memset(stats->cache_misses, 0, sizeof(stats->cache_misses));
      memset(stats->cache_miss_time_ns, 0, sizeof(stats->cache_miss_time_ns));
      memset(stats->cache_hit_time_ns, 0, sizeof(stats->cache_hit_time_ns));
      memset(stats->miss_eviction_time_ns, 0, sizeof(stats->miss_eviction_time_ns));
      memset(stats->miss_io_time_ns, 0, sizeof(stats->miss_io_time_ns));
      memset(stats->miss_finish_time_ns, 0, sizeof(stats->miss_finish_time_ns));
      memset(stats->miss_total_time_ns, 0, sizeof(stats->miss_total_time_ns));

      memset(stats->page_reads, 0, sizeof(stats->page_reads));
      memset(stats->page_writes, 0, sizeof(stats->page_writes));
      memset(stats->discard_clean, 0, sizeof(stats->discard_clean));
      memset(stats->discard_dirty, 0, sizeof(stats->discard_dirty));
      memset(stats->discard_uncached, 0, sizeof(stats->discard_uncached));


   }

}

/*
 *----------------------------------------------------------------------
 *
 * verification functions for cache_test
 *
 *----------------------------------------------------------------------
 */

uint32
treepass_count_dirty(treepass *cc)
{
   uint32 entry_no;
   uint32 dirty_count = 0;
   for (entry_no = 0; entry_no < cc->cfg->page_capacity; entry_no++) {
      if (!treepass_test_flag(cc, entry_no, TP_CLEAN)
          && !treepass_test_flag(cc, entry_no, TP_FREE))
      {
         dirty_count++;
      }
   }
   return dirty_count;
}

bool32
treepass_in_use(treepass *cc, uint64 addr)
{
   uint32 entry_no = treepass_lookup(cc, addr);
   if (entry_no == TP_UNMAPPED_ENTRY) {
      return FALSE;
   }
   for (threadid thr_i = 0; thr_i < TP_RC_WIDTH; thr_i++) {
      if (treepass_get_ref(cc, entry_no, thr_i) > 0) {
         return TRUE;
      }
   }
   return treepass_test_flag(cc, entry_no, TP_WRITELOCKED);
}

uint16
treepass_get_read_ref(treepass *cc, page_handle *page)
{
   uint32 entry_no = treepass_page_to_entry_number(cc, page);
   platform_assert(entry_no != TP_UNMAPPED_ENTRY);
   uint16 ref_count = 0;
   for (threadid thr_i = 0; thr_i < TP_RC_WIDTH; thr_i++) {
      ref_count += treepass_get_ref(cc, entry_no, thr_i);
   }
   return ref_count;
}

bool32
treepass_present(treepass *cc, page_handle *page)
{
   return treepass_lookup(cc, page->disk_addr) != TP_UNMAPPED_ENTRY;
}

static void
treepass_enable_sync_get(treepass *cc, bool32 enabled)
{
   cc->per_thread[platform_get_tid()].enable_sync_get = enabled;
}

static allocator *
treepass_get_allocator(const treepass *cc)
{
   return cc->al;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Virtual Functions
 *
 *      Here we define virtual functions for cache_ops
 *
 *      These are just boilerplate polymorph trampolines that cast the
 *      interface type to the concrete (treepass-specific type) and then call
 *      into the treepass_ method, so that the treepass_ method signature
 *      can contain concrete types. These trampolines disappear in link-time
 *      optimization.
 *
 *-----------------------------------------------------------------------------
 */

uint64
treepass_config_page_size_virtual(const cache_config *cfg)
{
   treepass_config *ccfg = (treepass_config *)cfg;
   return treepass_config_page_size(ccfg);
}

uint64
treepass_config_extent_size_virtual(const cache_config *cfg)
{
   treepass_config *ccfg = (treepass_config *)cfg;
   return treepass_config_extent_size(ccfg);
}

cache_config_ops treepass_config_ops = {
   .page_size   = treepass_config_page_size_virtual,
   .extent_size = treepass_config_extent_size_virtual,
};

page_handle *
treepass_alloc_virtual(cache *c, uint64 addr, page_type type)
{
   treepass *cc = (treepass *)c;
   return treepass_alloc(cc, addr, type);
}

void
treepass_extent_discard_virtual(cache *c, uint64 addr, page_type type)
{
   treepass *cc = (treepass *)c;
   return treepass_extent_discard(cc, addr, type);
}

page_handle *
treepass_get_virtual(cache *c, uint64 addr, bool32 blocking, page_type type, bool32 is_warm)
{
   treepass *cc = (treepass *)c;
   // is_warm parameter is for warm/cold classification
   return treepass_get(cc, addr, blocking, type, is_warm);
}

static page_handle *
treepass_get_with_miss_virtual(cache     *c,
                              uint64     addr,
                              bool32     blocking,
                              page_type  type,
                              bool32     is_warm,
                              bool32    *was_miss)
{
   treepass *cc = (treepass *)c;
   return treepass_get_with_miss(cc, addr, blocking, type, is_warm, was_miss);
}

void
treepass_unget_virtual(cache *c, page_handle *page)
{
   treepass *cc = (treepass *)c;
   treepass_unget(cc, page);
}

bool32
treepass_try_claim_virtual(cache *c, page_handle *page)
{
   treepass *cc = (treepass *)c;
   return treepass_try_claim(cc, page);
}

void
treepass_unclaim_virtual(cache *c, page_handle *page)
{
   treepass *cc = (treepass *)c;
   treepass_unclaim(cc, page);
}

void
treepass_lock_virtual(cache *c, page_handle *page)
{
   treepass *cc = (treepass *)c;
   treepass_lock(cc, page);
}

void
treepass_unlock_virtual(cache *c, page_handle *page)
{
   treepass *cc = (treepass *)c;
   treepass_unlock(cc, page);
}

void
treepass_prefetch_virtual(cache *c, uint64 addr, page_type type)
{
   treepass *cc = (treepass *)c;
   treepass_prefetch(cc, addr, type);
}

void
treepass_mark_dirty_virtual(cache *c, page_handle *page)
{
   treepass *cc = (treepass *)c;
   treepass_mark_dirty(cc, page);
}

void
treepass_pin_virtual(cache *c, page_handle *page)
{
   treepass *cc = (treepass *)c;
   treepass_pin(cc, page);
}

void
treepass_unpin_virtual(cache *c, page_handle *page)
{
   treepass *cc = (treepass *)c;
   treepass_unpin(cc, page);
}

static void
treepass_get_async_state_init_virtual(page_get_async_state_buffer buffer,
                                        cache                      *cc,
                                        uint64                      addr,
                                        page_type                   type,
                                        bool32                      is_warm,
                                        async_callback_fn           callback,
                                        void *callback_arg)
{
   treepass_get_async_state_init((treepass_get_async_state *)buffer,
                                   (treepass *)cc,
                                   addr,
                                   type,
                                   is_warm,
                                   callback,
                                   callback_arg);
}

static async_status
treepass_get_async_virtual(page_get_async_state_buffer buffer)
{
   return treepass_get_async((treepass_get_async_state *)buffer);
}

static page_handle *
treepass_get_async_state_result_virtual(page_get_async_state_buffer buffer)
{
   treepass_get_async_state *state = (treepass_get_async_state *)buffer;
   return state->__async_result;
}

void
treepass_page_sync_virtual(cache       *c,
                             page_handle *page,
                             bool32       is_blocking,
                             page_type    type)
{
   treepass *cc = (treepass *)c;
   treepass_page_sync(cc, page, is_blocking, type);
}

void
treepass_extent_sync_virtual(cache *c, uint64 addr, uint64 *pages_outstanding)
{
   treepass *cc = (treepass *)c;
   treepass_extent_sync(cc, addr, pages_outstanding);
}

void
treepass_flush_virtual(cache *c)
{
   treepass *cc = (treepass *)c;
   treepass_flush(cc);
}

int
treepass_evict_all_virtual(cache *c, bool32 ignore_pinned)
{
   treepass *cc = (treepass *)c;
   return treepass_evict_all(cc, ignore_pinned);
}

void
treepass_wait_virtual(cache *c)
{
   treepass *cc = (treepass *)c;
   return treepass_wait(cc);
}

bool32
treepass_in_use_virtual(cache *c, uint64 addr)
{
   treepass *cc = (treepass *)c;
   return treepass_in_use(cc, addr);
}

void
treepass_assert_ungot_virtual(cache *c, uint64 addr)
{
   treepass *cc = (treepass *)c;
   treepass_assert_ungot(cc, addr);
}

void
treepass_assert_no_locks_held_virtual(cache *c)
{
   treepass *cc = (treepass *)c;
   treepass_assert_no_locks_held(cc);
}

void
treepass_print_virtual(platform_log_handle *log_handle, cache *c)
{
   treepass *cc = (treepass *)c;
   treepass_print(log_handle, cc);
}

void
treepass_validate_page_virtual(cache *c, page_handle *page, uint64 addr)
{
   treepass *cc = (treepass *)c;
   treepass_validate_page(cc, page, addr);
}

void
treepass_print_stats_virtual(platform_log_handle *log_handle, cache *c)
{
   treepass *cc = (treepass *)c;
   treepass_print_stats(log_handle, cc);
}

static void
treepass_get_lookup_timing_impl(treepass *cc, cache_lookup_timing *out)
{
   uint64 total_cache_ns = 0, total_miss_ns = 0;
   uint64 total_nc_ns = 0, total_accesses = 0;
   for (uint64 i = 0; i < MAX_THREADS; i++) {
      for (page_type type = 0; type < NUM_PAGE_TYPES; type++) {
         total_miss_ns += cc->stats[i].cache_miss_time_ns[type];
         total_accesses += cc->stats[i].cache_hits[type]
                         + cc->stats[i].cache_misses[type];
      }
   }
   out->total_cache_access_time_ns = total_cache_ns;
   out->total_miss_time_ns = total_miss_ns;
   out->total_non_cache_time_ns = total_nc_ns;
   out->total_accesses = total_accesses;
}

static void
treepass_get_lookup_timing_virtual(cache *c, cache_lookup_timing *out)
{
   treepass_get_lookup_timing_impl((treepass *)c, out);
}

static void
treepass_set_in_lookup_virtual(cache *c, bool32 in_lookup)
{
}

void
treepass_io_stats_virtual(cache *c, uint64 *read_bytes, uint64 *write_bytes)
{
   treepass *cc = (treepass *)c;
   treepass_io_stats(cc, read_bytes, write_bytes);
}

void
treepass_reset_stats_virtual(cache *c)
{
   treepass *cc = (treepass *)c;
   treepass_reset_stats(cc);
}

uint32
treepass_count_dirty_virtual(cache *c)
{
   treepass *cc = (treepass *)c;
   return treepass_count_dirty(cc);
}

uint16
treepass_get_read_ref_virtual(cache *c, page_handle *page)
{
   treepass *cc = (treepass *)c;
   return treepass_get_read_ref(cc, page);
}

bool32
treepass_present_virtual(cache *c, page_handle *page)
{
   treepass *cc = (treepass *)c;
   return treepass_present(cc, page);
}

bool32
treepass_present_by_addr_virtual(cache *c, uint64 addr)
{
   treepass *cc = (treepass *)c;
   return treepass_lookup(cc, addr) != TP_UNMAPPED_ENTRY;
}

void
treepass_enable_sync_get_virtual(cache *c, bool32 enabled)
{
   treepass *cc = (treepass *)c;
   treepass_enable_sync_get(cc, enabled);
}

allocator *
treepass_get_allocator_virtual(const cache *c)
{
   treepass *cc = (treepass *)c;
   return treepass_get_allocator(cc);
}

cache_config *
treepass_get_config_virtual(const cache *c)
{
   treepass *cc = (treepass *)c;
   return &cc->cfg->super;
}

/* Alloc with max clock for compaction-hot pages (CI <= EI) */
page_handle *
treepass_alloc_compaction_hot(treepass *hc, uint64 addr, page_type type)
{
   uint32 alloc_status = TP_ALLOC_STATUS | (TP_CLOCK_MAX << TP_CLOCK_SHIFT);
   uint32 entry_no = treepass_get_free_page(hc, alloc_status, type, TRUE, TRUE);
   treepass_entry *entry = &hc->entry[entry_no];
   entry->page.disk_addr = addr;
   entry->type           = type;
   entry->page.entry_time   = (type == PAGE_TYPE_BRANCH) ? platform_get_timestamp() : 0;

   uint64 lookup_no = treepass_divide_by_page_size(hc, entry->page.disk_addr);
   hc->lookup[lookup_no] = entry_no;

   treepass_record_backtrace(hc, entry_no);
   return &entry->page;
}

page_handle *
treepass_alloc_compaction_hot_virtual(cache *c, uint64 addr, page_type type)
{
   treepass *hc = (treepass *)c;
   return treepass_alloc_compaction_hot(hc, addr, type);
}

/* TreePass helper functions (internal) - must be before virtual wrappers */
static uint64
treepass_get_evict_interval(const treepass *hc)
{
   return (__atomic_load_n(&hc->evict_interval, __ATOMIC_RELAXED));
}

void
treepass_mark_page_hot_virtual(cache *c, page_handle *page)
{
   (void)c; (void)page;
   // Class is derived from type and entry_time; HOT is implicit
}

uint64
treepass_get_evict_interval_virtual(const cache *c)
{
   const treepass *hc = (const treepass *)c;
   return treepass_get_evict_interval(hc);
}

static void
treepass_inherit_clock_virtual(cache *cc, uint64 old_addr, uint64 new_addr, uint32 right_shift)
{
   treepass_inherit_clock((treepass *)cc, old_addr, new_addr, right_shift);
}

static uint32
treepass_get_page_clock_virtual(cache *cc, uint64 addr, timestamp *out_entry_time)
{
   treepass *hc = (treepass *)cc;
   uint32 entry_no = treepass_lookup(hc, addr);
   if (entry_no == TP_UNMAPPED_ENTRY) {
      if (out_entry_time) {
         *out_entry_time = 0;
      }
      return 0;
   }
   if (out_entry_time) {
      *out_entry_time = __atomic_load_n(
         &hc->entry[entry_no].page.entry_time, __ATOMIC_RELAXED);
   }
   return treepass_get_clock(hc, entry_no);
}

static void
treepass_set_page_clock_virtual(cache *cc, page_handle *page, uint32 clock)
{
   treepass *hc = (treepass *)cc;
   uint32 entry_no = treepass_page_to_entry_number(hc, page);
   treepass_set_clock(hc, entry_no, clock);
}

static cache_ops treepass_ops = {
   .page_alloc           = treepass_alloc_virtual,
   .page_alloc_compaction_hot = treepass_alloc_compaction_hot_virtual,
   .extent_discard       = treepass_extent_discard_virtual,
   .page_get            = treepass_get_virtual,
   .page_get_with_miss  = treepass_get_with_miss_virtual,

   .page_get_async_state_init = treepass_get_async_state_init_virtual,
   .page_get_async            = treepass_get_async_virtual,
   .page_get_async_result     = treepass_get_async_state_result_virtual,

   .page_unget           = treepass_unget_virtual,
   .page_mark_hot     = treepass_mark_page_hot_virtual,
   .page_try_claim    = treepass_try_claim_virtual,
   .page_unclaim      = treepass_unclaim_virtual,
   .page_lock         = treepass_lock_virtual,
   .page_unlock       = treepass_unlock_virtual,
   .page_prefetch     = treepass_prefetch_virtual,
   .page_mark_dirty   = treepass_mark_dirty_virtual,
   .page_pin          = treepass_pin_virtual,
   .page_unpin        = treepass_unpin_virtual,
   .page_sync         = treepass_page_sync_virtual,
   .extent_sync       = treepass_extent_sync_virtual,
   .flush             = treepass_flush_virtual,
   .evict             = treepass_evict_all_virtual,
   .cleanup           = treepass_wait_virtual,
   .in_use            = treepass_in_use_virtual,
   .assert_ungot      = treepass_assert_ungot_virtual,
   .assert_free       = treepass_assert_no_locks_held_virtual,
   .print             = treepass_print_virtual,
   .print_stats       = treepass_print_stats_virtual,
   .io_stats          = treepass_io_stats_virtual,
   .reset_stats       = treepass_reset_stats_virtual,
   .validate_page     = treepass_validate_page_virtual,
   .count_dirty       = treepass_count_dirty_virtual,
   .page_get_read_ref = treepass_get_read_ref_virtual,
   .cache_present     = treepass_present_virtual,
   .cache_present_by_addr = treepass_present_by_addr_virtual,
   .enable_sync_get   = treepass_enable_sync_get_virtual,
   .get_allocator     = treepass_get_allocator_virtual,
   .get_config        = treepass_get_config_virtual,
   .get_ei       = treepass_get_evict_interval_virtual,
   .inherit_clock     = treepass_inherit_clock_virtual,
   .get_page_clock    = treepass_get_page_clock_virtual,
   .set_page_clock    = treepass_set_page_clock_virtual,
   .get_lookup_timing = treepass_get_lookup_timing_virtual,
   .set_in_lookup     = treepass_set_in_lookup_virtual,
};

/*
 *-----------------------------------------------------------------------------
 * treepass_config_init --
 *
 *      Initialize treepass config values
 *-----------------------------------------------------------------------------
 */
void
treepass_config_init(treepass_config *cache_cfg,
                       io_config         *io_cfg,
                       uint64             capacity,
                       const char        *cache_logfile,
                       uint64             use_stats,
                       bool32             use_cache_stats)
{
   int rc;
   ZERO_CONTENTS(cache_cfg);

   cache_cfg->super.ops      = &treepass_config_ops;
   cache_cfg->io_cfg         = io_cfg;
   cache_cfg->capacity       = capacity;
   cache_cfg->log_page_size  = 63 - __builtin_clzll(io_cfg->page_size);
   cache_cfg->page_capacity  = capacity / io_cfg->page_size;
   cache_cfg->use_stats      = use_stats;
   cache_cfg->use_cache_stats = use_cache_stats;
   // TreePass extensions
   cache_cfg->ei_ewma_alpha = 0.2;
   cache_cfg->ei_initial_per_gb_ns = SEC_TO_NSEC(1);

   rc = snprintf(cache_cfg->logfile, MAX_STRING_LENGTH, "%s", cache_logfile);
   platform_assert(rc < MAX_STRING_LENGTH);
}

platform_status
treepass_init(treepass        *cc,   // OUT
                treepass_config *cfg,  // IN
                io_handle         *io,   // IN
                allocator         *al,   // IN
                char              *name, // IN
                platform_heap_id   hid,  // IN
                platform_module_id mid)  // IN
{
   int      i;
   threadid thr_i;

   platform_assert(cc != NULL);
   ZERO_CONTENTS(cc);

   cc->cfg       = cfg;
   cc->super.ops = &treepass_ops;

   uint64 allocator_page_capacity =
      treepass_divide_by_page_size(cc, allocator_get_capacity(al));
   uint64 debug_capacity =
      treepass_multiply_by_page_size(cc, cc->cfg->page_capacity);
   cc->cfg->batch_capacity = cc->cfg->page_capacity / TP_ENTRIES_PER_BATCH;
   cc->cfg->cacheline_capacity =
      cc->cfg->page_capacity / PLATFORM_CACHELINE_SIZE;
   cc->cfg->pages_per_extent =
      treepass_divide_by_page_size(cc, treepass_extent_size(cc));

   platform_assert(cc->cfg->page_capacity % PLATFORM_CACHELINE_SIZE == 0);
   platform_assert(cc->cfg->capacity == debug_capacity);
   platform_assert(cc->cfg->page_capacity % TP_ENTRIES_PER_BATCH == 0);

   cc->cleaner_gap = TP_CLEANER_GAP;

#if defined(TP_LOG) || defined(ADDR_TRACING)
   cc->logfile = platform_open_log_file(cfg->logfile, "w");
#else
   cc->logfile = NULL;
#endif
   treepass_log(
      0, 0, "init: capacity %lu name %s\n", cc->cfg->capacity, name);

   cc->al      = al;
   cc->io      = io;
   cc->heap_id = hid;

   /* lookup maps addrs to entries, entry contains the entries themselves */
   cc->lookup =
      TYPED_ARRAY_MALLOC(cc->heap_id, cc->lookup, allocator_page_capacity);
   if (!cc->lookup) {
      goto alloc_error;
   }
   for (i = 0; i < allocator_page_capacity; i++) {
      cc->lookup[i] = TP_UNMAPPED_ENTRY;
   }

   cc->entry =
      TYPED_ARRAY_ZALLOC(cc->heap_id, cc->entry, cc->cfg->page_capacity);
   if (!cc->entry) {
      goto alloc_error;
   }

   platform_status rc = STATUS_NO_MEMORY;

   /* data must be aligned because of O_DIRECT */
   rc = platform_buffer_init(&cc->bh, cc->cfg->capacity);
   if (!SUCCESS(rc)) {
      goto alloc_error;
   }
   cc->data = platform_buffer_getaddr(&cc->bh);

   /* Set up the entries */
   for (i = 0; i < cc->cfg->page_capacity; i++) {
      cc->entry[i].page.data =
         cc->data + treepass_multiply_by_page_size(cc, i);
      cc->entry[i].page.disk_addr = TP_UNMAPPED_ADDR;
      cc->entry[i].page.entry_time   = 0;
      cc->entry[i].status         = TP_FREE_STATUS;  // Clock bits also 0
      cc->entry[i].type           = PAGE_TYPE_INVALID;
      async_wait_queue_init(&cc->entry[i].waiters);
   }

   /* Entry per-thread ref counts */
   size_t refcount_size =
      cc->cfg->page_capacity * TP_RC_WIDTH * sizeof(cc->refcount[0]);

   rc = platform_buffer_init(&cc->rc_bh, refcount_size);
   if (!SUCCESS(rc)) {
      goto alloc_error;
   }
   cc->refcount = platform_buffer_getaddr(&cc->rc_bh);

   /* Separate ref counts for pins */
   cc->pincount =
      TYPED_ARRAY_ZALLOC(cc->heap_id, cc->pincount, cc->cfg->page_capacity);
   if (!cc->pincount) {
      goto alloc_error;
   }

   /* The hands and associated page */
   cc->free_hand  = 0;
   cc->evict_hand = 1;
   for (thr_i = 0; thr_i < MAX_THREADS; thr_i++) {
      cc->per_thread[thr_i].free_hand       = TP_UNMAPPED_ENTRY;
      cc->per_thread[thr_i].enable_sync_get = TRUE;
   }
   cc->batch_busy =
      TYPED_ARRAY_ZALLOC(cc->heap_id,
                         cc->batch_busy,
                         cc->cfg->page_capacity / TP_ENTRIES_PER_BATCH);
   if (!cc->batch_busy) {
      goto alloc_error;
   }

   /* Initialize TreePass extensions: EI tracking and early eviction */
   cc->evict_interval = EI_NOT_READY;
   cc->last_cycle_ts = platform_get_timestamp();
   cc->evict_batches = 0;
   cc->last_boost_ts = 0;
   cc->last_ei_check_ts = 0;
   cc->last_ei_check_cycle = 0;

   /* Cache trace init (only when compiled with -DCACHE_TRACE) */
#ifdef CACHE_TRACE
   if (cc->trace) {
      }
#endif



   return STATUS_OK;

alloc_error:
   treepass_deinit(cc);
   return STATUS_NO_MEMORY;
}

/*
 * De-init the resources allocated to initialize a treepass.
 * This function may be called to deal with error situations, or a failed
 * treepass_init(). So check for non-NULL handles before trying to release
 * resources.
 */
void
treepass_deinit(treepass *cc) // IN/OUT
{
   platform_assert(cc != NULL);

#ifdef CACHE_TRACE
   if (cc->trace) {
      cache_trace_deinit(cc->trace);
      platform_free(cc->heap_id, cc->trace);
   }
#endif


   if (cc->logfile) {
      treepass_log(0, 0, "deinit %s\n", "");
#if defined(TP_LOG) || defined(ADDR_TRACING)
      platform_close_log_file(cc->logfile);
#endif
   }

   if (cc->lookup) {
      platform_free(cc->heap_id, cc->lookup);
   }
   if (cc->entry) {
      for (int i = 0; i < cc->cfg->page_capacity; i++) {
         async_wait_queue_deinit(&cc->entry[i].waiters);
      }
      platform_free(cc->heap_id, cc->entry);
   }

   debug_only platform_status rc = STATUS_TEST_FAILED;
   if (cc->data) {
      rc = platform_buffer_deinit(&cc->bh);

      // We expect above to succeed. Anyway, we are in the process of
      // dismantling the treepass, hence, for now, can't do much by way
      // of reporting errors further upstream.
      debug_assert(SUCCESS(rc), "rc=%s", platform_status_to_string(rc));
      cc->data = NULL;
   }
   if (cc->refcount) {
      rc = platform_buffer_deinit(&cc->rc_bh);
      debug_assert(SUCCESS(rc), "rc=%s", platform_status_to_string(rc));
      cc->refcount = NULL;
   }

   if (cc->pincount) {
      platform_free_volatile(cc->heap_id, cc->pincount);
   }
   if (cc->batch_busy) {
      platform_free_volatile(cc->heap_id, cc->batch_busy);
   }
}

/* ===== TreePass Extensions ===== */
