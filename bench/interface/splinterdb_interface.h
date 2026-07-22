#pragma once

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "util.h"

class SplinterDBInterface {
 public:
  SplinterDBInterface() : spl_handle(nullptr) {}

   splinterdb* get_raw_handle() const {
    return spl_handle;
  }
    void InitializeDatabase() {
      platform_set_log_streams(stderr, stderr);

      splinterdb_cfg_ = SetSplinterDBConfig(splinter_data_cfg_);
      spl_handle = nullptr; // To a running SplinterDB instance

      int rc;
      if (FLAGS_use_existing_db) {
        
        rc = splinterdb_open(&splinterdb_cfg_, &spl_handle);
      }
      else {
        
        rc = splinterdb_create(&splinterdb_cfg_, &spl_handle);
      }
      
      if (rc != 0) {
          throw std::runtime_error("Failed to initialize SplinterDB");
      }
    }

  void ShutdownDatabase() {
    if (spl_handle != nullptr) {
      splinterdb_close(&spl_handle);
      if (splinterdb_cfg_.filename != nullptr) {
          free((void*)splinterdb_cfg_.filename);
          splinterdb_cfg_.filename = nullptr;
      }
      spl_handle = nullptr;
    }
  }
  // Insert key-value pair
  bool Insert(const std::string& key, const char* value, size_t value_size) {
    //std::cerr << "Insert start " << std::endl;
    if (spl_handle == nullptr) {
        throw std::runtime_error("Database handle is null");
    }
    slice key_slice = slice_create(key.size(), key.data());
    slice value_slice = slice_create(value_size, value);

    int rc = splinterdb_insert(spl_handle, key_slice, value_slice);
    //std::cerr << "splinter_insert done, rc: " << rc << std::endl;
    if (rc != 0) {
        std::cerr << "Error during insert: " << rc << std::endl;
        throw std::runtime_error("Insert failed");
    }
    return rc == 0;
  }

  bool Read(const std::string &key, std::string *value_out) {
    slice key_slice = slice_create(key.size(), key.data());
    splinterdb_lookup_result result;
    splinterdb_lookup_result_init(spl_handle, &result, 0, NULL);

    int rc = splinterdb_lookup(spl_handle, key_slice, &result);

    if (rc == 0 && splinterdb_lookup_found(&result)) {
      slice value_slice;
      splinterdb_lookup_result_value(&result, &value_slice);

      const void *data = slice_data(value_slice);
      // NOTE: make the type explicit to avoid signed→unsigned surprises
      size_t len = (size_t)slice_length(value_slice);

      // --- Runtime guards (diagnostic) ---
      // (1) data/len consistency
      if ((data == nullptr && len != 0) || (data != nullptr && len == 0)) {
        fprintf(stderr, "[READ][BUG] data/len mismatch: data=%p len=%zu\n", data, len);
        abort();
      }

      // (2) absurd length guard. Bench writes are clamped to MAX_INLINE_MESSAGE_SIZE
      // (35*4096/100 = 1433B) and are always > 0 for non-deleted keys, so a
      // returned value outside [1, 1433] indicates either silent truncation on
      // the write side or a value size mismatch in the DB layer.
      constexpr size_t kBenchMaxValueSize = 35 * 4096 / 100;
      if (len == 0 || len > kBenchMaxValueSize) {
        fprintf(stderr,
                "[READ][BUG] value size out of bench range: len=%zu key_size=%zu "
                "(expected 1..%zu)\n",
                len, key.size(), kBenchMaxValueSize);
        abort();
      }

      // (3) try/catch to avoid process abort on bad_alloc
      try {
        value_out->assign(static_cast<const char *>(data), len);
      } catch (const std::bad_alloc &) {
        fprintf(stderr, "[READ][OOM] bad_alloc on assign, len=%zu\n", len);
        splinterdb_lookup_result_deinit(&result);
        value_out->clear();
        return false;
      }

    } else if (rc == 0) {
      // Key not found (e.g., after DELETE) — not an error, just empty
      value_out->clear();
    }

    splinterdb_lookup_result_deinit(&result);
    return rc == 0;
  }

  // Update existing key-value pair
  bool Update(const std::string &key, const char* value, size_t value_size) {
    slice key_slice = slice_create(key.size(), key.data());
    slice value_slice = slice_create(value_size, value);
    int rc = splinterdb_insert(spl_handle, key_slice, value_slice);
    return rc == 0;
  }

  // Scan `amount` records starting at start_key. Returns true on success.
  bool Scan(const std::string& start_key, size_t amount,
            std::vector<std::pair<std::string, std::string>>* scan_out) {
    scan_out->clear();
    scan_out->reserve(amount);

    slice start_slice = slice_create(start_key.size(), start_key.data());
    splinterdb_iterator* it;
    int rc = splinterdb_range_iterator_init(spl_handle, &it, start_slice, amount);
    if (rc != 0) return false;

    slice iter_key, iter_value;
    while (splinterdb_iterator_valid(it) && scan_out->size() < amount) {
      splinterdb_iterator_get_current(it, &iter_key, &iter_value);
      scan_out->emplace_back(
          std::string(reinterpret_cast<const char*>(iter_key.data), iter_key.length),
          std::string(reinterpret_cast<const char*>(iter_value.data), iter_value.length));
      splinterdb_iterator_next(it);
    }
    rc = splinterdb_iterator_status(it);
    splinterdb_iterator_deinit(it);
    return rc == 0;
  }

  // Delete a key.
  bool Delete(const std::string& key) {
    slice key_slice = slice_create(key.size(), key.data());
    return splinterdb_delete(spl_handle, key_slice) == 0;
  }

  // Read-Modify-Write: read the current value then update.
  bool RMW(const std::string& key, const char* value, size_t value_size) {
    std::string old;
    (void)Read(key, &old);  // YCSB ignores the read outcome
    return Update(key, value, value_size);
  }

  void PrintTrunkLookupStats() {
      splinterdb_stats_print_lookup(spl_handle);
  }

  // The historical bench main calls PrintTrunkInsertionStats() at end of
  // load. SplinterDB doesn't expose an insertion-only stats dump, so fall
  // back to the same lookup-stats print here.
  void PrintTrunkInsertionStats() {
      splinterdb_stats_print_lookup(spl_handle);
  }

  void ResetStats() {
    splinterdb_stats_reset(spl_handle);
  }

 private:
  splinterdb *spl_handle;
  splinterdb_config splinterdb_cfg_;
  data_config splinter_data_cfg_;
};

