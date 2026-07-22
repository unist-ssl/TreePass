// Copyright 2018-2021 VMware, Inc.
// SPDX-License-Identifier: Apache-2.0

/*
 * atomic_ghost.c --
 *
 *     Lock-free ghost buffer implementation based on CacheLib's
 *     AtomicFIFOHashTable.
 */

#include "atomic_ghost.h"
#include "util.h"

/*
 *-----------------------------------------------------------------------------
 * Internal helper macros and constants
 *-----------------------------------------------------------------------------
 */

// Mask for extracting key (lower 32 bits)
#define KEY_MASK   0x00000000FFFFFFFFULL

// Mask for extracting insertion time (upper 32 bits)
#define TIME_MASK  0xFFFFFFFF00000000ULL

// Bucket index mask (align to 8 for linear probing)
#define BUCKET_IDX_MASK  0xFFFFFFFFFFFFFFF8ULL

/*
 * Hash function for disk address
 * Uses multiplicative hashing with a prime multiplier
 */
static inline uint32
ghost_hash_key(uint64 disk_addr)
{
   // Multiplicative hash with golden ratio prime
   return (uint32)((disk_addr * 11400714819323198485ULL) >> 32);
}

/*
 * Create hash table value from key and time
 * Format: [time (32 bits) | key (32 bits)]
 */
static inline uint64
make_table_entry(uint32 key, uint32 time)
{
   return ((uint64)time << 32) | (uint64)key;
}

/*
 * Extract key from hash table value
 */
static inline uint32
get_entry_key(uint64 entry)
{
   return (uint32)(entry & KEY_MASK);
}

/*
 * Extract insertion time from hash table value
 */
static inline uint32
get_entry_time(uint64 entry)
{
   return (uint32)((entry & TIME_MASK) >> 32);
}

/*
 * Get bucket index for a key
 */
static inline uint64
get_bucket_idx(atomic_ghost *ghost, uint32 key)
{
   uint64 idx = (uint64)key % ghost->num_elem;
   return idx & BUCKET_IDX_MASK;
}

/*
 *-----------------------------------------------------------------------------
 * Public API Implementation
 *-----------------------------------------------------------------------------
 */

platform_status
atomic_ghost_init(atomic_ghost    **ghost_out,
                  uint64            capacity,
                  platform_heap_id  heap_id)
{
   atomic_ghost *ghost;

   ghost = TYPED_ZALLOC(heap_id, ghost);
   if (ghost == NULL) {
      return STATUS_NO_MEMORY;
   }

   ghost->heap_id = heap_id;

   // Round up capacity to multiple of 8 for bucket alignment
   ghost->fifo_size = ((capacity >> 3) + 1) << 3;

   // Allocate 2x slots for 50% load factor (collision handling)
   ghost->num_elem = ghost->fifo_size * ATOMIC_GHOST_LOAD_FACTOR_INV;

   // Allocate hash table
   ghost->table = TYPED_ARRAY_ZALLOC(heap_id, ghost->table, ghost->num_elem);
   if (ghost->table == NULL) {
      platform_free(heap_id, ghost);
      return STATUS_NO_MEMORY;
   }

   ghost->num_inserts = 0;
   ghost->num_evicts = 0;
   ghost->num_hits = 0;
   ghost->num_misses = 0;

   *ghost_out = ghost;
   return STATUS_OK;
}

void
atomic_ghost_deinit(atomic_ghost *ghost)
{
   if (ghost == NULL) {
      return;
   }

   platform_heap_id heap_id = ghost->heap_id;

   if (ghost->table != NULL) {
      platform_free(heap_id, ghost->table);
   }
   platform_free(heap_id, ghost);
}

bool32
atomic_ghost_insert(atomic_ghost *ghost, uint64 disk_addr)
{
   uint32 key = ghost_hash_key(disk_addr);

   // Increment logical clock and get current time
   int64 curr_time = __sync_fetch_and_add(&ghost->num_inserts, 1);

   // Handle overflow (wrap around)
   if (curr_time > UINT32_MAX) {
      __sync_val_compare_and_swap(&ghost->num_inserts, curr_time + 1, 0);
      curr_time = 0;
   }

   uint64 bucket_idx = get_bucket_idx(ghost, key);
   uint64 new_entry = make_table_entry(key, (uint32)curr_time);

   // Try to find an empty slot in the bucket (linear probing)
   for (uint32 i = 0; i < ATOMIC_GHOST_ITEMS_PER_BUCKET; i++) {
      uint64 idx = bucket_idx + i;
      if (idx >= ghost->num_elem) {
         idx = idx % ghost->num_elem;
      }

      uint64 old_entry = __atomic_load_n(&ghost->table[idx], __ATOMIC_RELAXED);

      if (old_entry == 0) {
         // Empty slot found, try CAS
         if (__atomic_compare_exchange_n(&ghost->table[idx],
                                         &old_entry,
                                         new_entry,
                                         TRUE,  // weak
                                         __ATOMIC_RELAXED,
                                         __ATOMIC_RELAXED)) {
            return FALSE;  // Successfully inserted, no forced eviction
         }
         // CAS failed, another thread took this slot, continue probing
      }
   }

   // No empty slot found, overwrite a random slot (forced eviction)
   uint64 overwrite_idx = key % ghost->num_elem;
   __atomic_store_n(&ghost->table[overwrite_idx], new_entry, __ATOMIC_RELAXED);
   return TRUE;  // Forced eviction occurred
}

bool32
atomic_ghost_lookup_and_remove(atomic_ghost *ghost, uint64 disk_addr)
{
   uint32 key = ghost_hash_key(disk_addr);
   uint64 bucket_idx = get_bucket_idx(ghost, key);
   int64 curr_time = __atomic_load_n(&ghost->num_inserts, __ATOMIC_RELAXED);

   for (uint32 i = 0; i < ATOMIC_GHOST_ITEMS_PER_BUCKET; i++) {
      uint64 idx = bucket_idx + i;
      if (idx >= ghost->num_elem) {
         idx = idx % ghost->num_elem;
      }

      uint64 entry = __atomic_load_n(&ghost->table[idx], __ATOMIC_RELAXED);

      if (entry == 0) {
         continue;  // Empty slot
      }

      // Check age (implicit FIFO eviction)
      int64 age = curr_time - (int64)get_entry_time(entry);
      if (age > (int64)ghost->fifo_size) {
         // Entry is stale (logically evicted), try to clear it
         __atomic_compare_exchange_n(&ghost->table[idx],
                                     &entry,
                                     0,
                                     TRUE,
                                     __ATOMIC_RELAXED,
                                     __ATOMIC_RELAXED);
         continue;
      }

      // Check if key matches
      if (get_entry_key(entry) == key) {
         // Ghost hit! Atomically remove the entry
         if (__atomic_compare_exchange_n(&ghost->table[idx],
                                         &entry,
                                         0,
                                         TRUE,
                                         __ATOMIC_RELAXED,
                                         __ATOMIC_RELAXED)) {
            return TRUE;  // Successfully found and removed
         }
         // CAS failed, entry was modified by another thread
         // This is fine, just return TRUE since we found a match
         return TRUE;
      }
   }

   return FALSE;  // Not found
}

bool32
atomic_ghost_contains(atomic_ghost *ghost, uint64 disk_addr)
{
   uint32 key = ghost_hash_key(disk_addr);
   uint64 bucket_idx = get_bucket_idx(ghost, key);
   int64 curr_time = __atomic_load_n(&ghost->num_inserts, __ATOMIC_RELAXED);

   for (uint32 i = 0; i < ATOMIC_GHOST_ITEMS_PER_BUCKET; i++) {
      uint64 idx = bucket_idx + i;
      if (idx >= ghost->num_elem) {
         idx = idx % ghost->num_elem;
      }

      uint64 entry = __atomic_load_n(&ghost->table[idx], __ATOMIC_RELAXED);

      if (entry == 0) {
         continue;
      }

      // Check age
      int64 age = curr_time - (int64)get_entry_time(entry);
      if (age > (int64)ghost->fifo_size) {
         continue;  // Stale entry
      }

      // Check if key matches
      if (get_entry_key(entry) == key) {
         return TRUE;
      }
   }

   return FALSE;
}

void
atomic_ghost_get_stats(atomic_ghost *ghost,
                       uint64       *capacity_out,
                       uint64       *num_inserts_out,
                       uint64       *num_evicts_out,
                       uint64       *num_hits_out,
                       uint64       *num_misses_out)
{
   if (capacity_out != NULL) {
      *capacity_out = ghost->fifo_size;
   }
   if (num_inserts_out != NULL) {
      *num_inserts_out = (uint64)ghost->num_inserts;
   }
   if (num_evicts_out != NULL) {
      *num_evicts_out = (uint64)ghost->num_evicts;
   }
   if (num_hits_out != NULL) {
      *num_hits_out = (uint64)ghost->num_hits;
   }
   if (num_misses_out != NULL) {
      *num_misses_out = (uint64)ghost->num_misses;
   }
}

void
atomic_ghost_reset_stats(atomic_ghost *ghost)
{
   if (ghost == NULL) {
      return;
   }
   ghost->num_inserts = 0;
   ghost->num_evicts = 0;
   ghost->num_hits = 0;
   ghost->num_misses = 0;
}

