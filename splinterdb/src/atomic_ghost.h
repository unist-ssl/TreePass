// Copyright 2018-2021 VMware, Inc.
// SPDX-License-Identifier: Apache-2.0

/*
 * atomic_ghost.h --
 *
 *     Lock-free ghost buffer implementation based on CacheLib's
 *     AtomicFIFOHashTable. Uses logical timestamps for implicit FIFO eviction.
 *
 *     Reference: CacheLib cachelib/allocator/datastruct/AtomicFIFOHashTable.h
 */

#pragma once

#include "platform.h"

/*
 *-----------------------------------------------------------------------------
 * Constants
 *-----------------------------------------------------------------------------
 */
#define ATOMIC_GHOST_LOAD_FACTOR_INV   2    // 50% load factor
#define ATOMIC_GHOST_ITEMS_PER_BUCKET  8    // Linear probing bucket size

/*
 *-----------------------------------------------------------------------------
 * Atomic Ghost Buffer
 *
 *     A lock-free hash table that implements implicit FIFO eviction using
 *     logical timestamps. Each entry stores both a key (disk_addr hash) and
 *     an insertion time in a single 64-bit value.
 *
 *     Entry format: [insertion_time (32 bits) | key_hash (32 bits)]
 *
 *     Eviction is implicit: entries older than fifo_size inserts are
 *     considered stale and ignored during lookup.
 *-----------------------------------------------------------------------------
 */
typedef struct atomic_ghost {
   uint64            *table;         // Hash table: [time(32) | key(32)]
   uint64             num_elem;      // Number of slots in table
   uint64             fifo_size;     // Logical FIFO capacity (valid entries)
   volatile int64     num_inserts;   // Logical clock (insert counter)
   volatile int64     num_evicts;    // Stats: number of forced overwrites
   volatile int64     num_hits;      // Stats: ghost hits (admission control)
   volatile int64     num_misses;    // Stats: ghost misses (admission control)
   platform_heap_id   heap_id;
} atomic_ghost;

/*
 *-----------------------------------------------------------------------------
 * Function Declarations
 *-----------------------------------------------------------------------------
 */

/*
 * Initialize atomic ghost buffer
 *
 * @param ghost_out     Output pointer to created ghost buffer
 * @param capacity      Logical FIFO capacity (number of entries to track)
 * @param heap_id       Heap for memory allocation
 * @return              STATUS_OK on success
 */
platform_status
atomic_ghost_init(atomic_ghost    **ghost_out,
                  uint64            capacity,
                  platform_heap_id  heap_id);

/*
 * Destroy atomic ghost buffer and free resources
 */
void
atomic_ghost_deinit(atomic_ghost *ghost);

/*
 * Insert a disk address into the ghost buffer
 * Called when a page is evicted from cache
 *
 * @param ghost         Ghost buffer
 * @param disk_addr     Disk address of evicted page
 */
bool32
atomic_ghost_insert(atomic_ghost *ghost, uint64 disk_addr);

/*
 * Check if disk address is in ghost buffer (ghost hit)
 * If found, the entry is atomically removed (consumed)
 *
 * @param ghost         Ghost buffer
 * @param disk_addr     Disk address to lookup
 * @return              TRUE if found (ghost hit), FALSE otherwise
 */
bool32
atomic_ghost_lookup_and_remove(atomic_ghost *ghost, uint64 disk_addr);

/*
 * Check if disk address is in ghost buffer without removing
 * Use this for read-only queries
 *
 * @param ghost         Ghost buffer
 * @param disk_addr     Disk address to lookup
 * @return              TRUE if found, FALSE otherwise
 */
bool32
atomic_ghost_contains(atomic_ghost *ghost, uint64 disk_addr);

/*
 * Get ghost buffer statistics
 */
void
atomic_ghost_get_stats(atomic_ghost *ghost,
                       uint64       *capacity_out,
                       uint64       *num_inserts_out,
                       uint64       *num_evicts_out,
                       uint64       *num_hits_out,
                       uint64       *num_misses_out);

/*
 * Reset ghost buffer statistics (for stats reset)
 */
void
atomic_ghost_reset_stats(atomic_ghost *ghost);

