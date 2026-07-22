#pragma once

#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <vector>

extern "C" {
#include "splinterdb/default_data_config.h"
#include "splinterdb/splinterdb.h"
}

#include "gflags/gflags.h"

#include "util.h"  // Random64

// ------------------------------------------------------------
// gflag definitions consumed by ycsb_bench and by this header's
// SetSplinterDBConfig / GenerateTwoTermExpKeys helpers.
// ------------------------------------------------------------
DEFINE_string(db_type, "TREEPASS", "Backend identifier; only TREEPASS is supported by this artifact.");
DEFINE_string(db_path, "/tmp/splinterdb", "Directory holding the SplinterDB data file.");

DEFINE_string(query_dist, "zipf", "Query distribution: unif | zipf | prefix");
DEFINE_double(zipf_const, 0.99, "Zipfian skew (used only when query_dist=zipf).");

DEFINE_string(trace_name, "", "Load CSV path (one key per line, optional ,value_size).");

DEFINE_int32(num, 0,   "Number of keys to load.");
DEFINE_int32(key_size, 20, "Fixed key size (overridden by the CSV's max key length if larger).");
DEFINE_int32(value_size, 100, "Fixed value size in bytes.");

DEFINE_int32(warmup_num,   0, "Untimed warmup ops; bench computes phase_op_num/2 if zero.");
DEFINE_int32(phase_op_num, 0, "Timed ops in the measured phase.");
DEFINE_int32(thread_num,   1, "Bench thread count.");

DEFINE_string(workload_name, "C", "YCSB-style workload name: load, A..F.");
DEFINE_string(workload_frac_list, "0,0,1,0,0,0",
              "Per-op fractions: insert,update,read,scan,delete,rmw.");
DEFINE_double(load_frac, 0, "If > 0 and workload_name=load, fraction of FLAGS_num to insert.");
// YCSB-D / YCSB-E multi-phase semantics:
//   num_repeats:              split the measured phase into this many sub-phases.
//   consider_latest_inserts:  when true, read keys are drawn from the "latest"
//                             distribution (skewed toward most recently inserted),
//                             matching YCSB-D's "read latest insert" behavior.
DEFINE_int32(num_repeats, 1,
             "Repeat the measured phase this many times (used by YCSB-D / E).");
DEFINE_bool(consider_latest_inserts, false,
            "If true, read keys use the 'latest' distribution rather than --query_dist.");
DEFINE_bool(use_existing_db, false, "Open existing DB instead of creating fresh.");
DEFINE_bool(use_stats, false, "Enable SplinterDB cache hit/miss stats output at shutdown.");

DEFINE_string(query_trace_dir, "",
              "Directory for the staged query-keys binary. Each workload's query "
              "keys are generated once and reused by later runs with the same "
              "parameters instead of being regenerated. Empty = alongside "
              "--trace_name.");

// Prefix-dist (MixGraph) parameters -- the ZippyDB-style "prefix_dist" workload
// from the FAST'20 MixGraph paper. keyrange_dist_* model between-range
// (cross-prefix) hotness; key_dist_* model within-range hotness. The defaults
// below are the paper's ZippyDB values, and ycsb.py relies on them (it no
// longer passes these flags), so change the distribution here, not in ycsb.py.
DEFINE_int64(keyrange_num, 30, "Number of key ranges for prefix-dist hotness.");
DEFINE_double(key_dist_a, 0.002312, "Within-range key distribution parameter 'a'.");
DEFINE_double(key_dist_b, 0.3467,   "Within-range key distribution parameter 'b'.");
DEFINE_double(keyrange_dist_a, 14.18,    "Cross-range prefix hotness 'a'.");
DEFINE_double(keyrange_dist_b, -2.917,   "Cross-range prefix hotness 'b'.");
DEFINE_double(keyrange_dist_c, 0.0164,   "Cross-range prefix hotness 'c'.");
DEFINE_double(keyrange_dist_d, -0.08082, "Cross-range prefix hotness 'd'.");

DEFINE_uint64(block_cache_capacity, 64, "SplinterDB block cache size in MB.");
DEFINE_uint64(db_size_in_GB, 128, "SplinterDB logical device size in GB.");
DEFINE_bool(use_direct_io, true, "Use O_DIRECT when opening the SplinterDB device.");

// ------------------------------------------------------------
// Prefix-distribution key generation helpers.
// (Adapted from RocksDB MixGraph — keeps the two-term exponential
// distribution used by the smoke matrix.)
// ------------------------------------------------------------
struct KeyrangeUnit {
  int64_t keyrange_start;
  int64_t keyrange_access;
  int64_t keyrange_keys;
};

class GenerateTwoTermExpKeys {
 public:
  int64_t keyrange_rand_max_ = 0;
  int64_t keyrange_size_     = 0;
  int64_t keyrange_num_      = 0;
  std::vector<KeyrangeUnit> keyrange_set_;

  bool InitiateExpDistribution(int64_t total_keys,
                               double  prefix_a, double prefix_b,
                               double  prefix_c, double prefix_d,
                               bool    use_prefix_modeling) {
    keyrange_num_ = (FLAGS_keyrange_num <= 0 || prefix_a == 0.0 ||
                     prefix_b == 0.0 || prefix_c == 0.0 || prefix_d == 0.0)
                        ? 1
                        : FLAGS_keyrange_num;
    keyrange_size_ = total_keys / keyrange_num_;
    if (!use_prefix_modeling) return true;

    int64_t amplify = 0;
    int64_t keyrange_start = 0;
    for (int64_t pfx = keyrange_num_; pfx >= 1; --pfx) {
      double p = prefix_a * std::exp(prefix_b * pfx) +
                 prefix_c * std::exp(prefix_d * pfx);
      if (p < 1e-16) p = 0.0;
      if (amplify == 0 && p > 0) {
        amplify = static_cast<int64_t>(std::floor(1 / p)) + 1;
      }
      KeyrangeUnit u;
      u.keyrange_start  = keyrange_start;
      u.keyrange_access = (p <= 0) ? 0 : static_cast<int64_t>(std::floor(amplify * p));
      u.keyrange_keys   = keyrange_size_;
      keyrange_set_.push_back(u);
      keyrange_start += u.keyrange_access;
    }
    keyrange_rand_max_ = keyrange_start;

    // Shuffle so that hot ranges are spread across the key space rather than
    // bunched at one end.
    static Random64 rng(keyrange_rand_max_);
    for (int64_t i = 0; i < keyrange_num_; ++i) {
      int64_t pos = rng.Next() % keyrange_num_;
      std::swap(keyrange_set_[i], keyrange_set_[pos]);
    }

    // Recompute start offsets after the shuffle.
    int64_t offset = 0;
    for (auto& u : keyrange_set_) {
      u.keyrange_start = offset;
      offset += u.keyrange_access;
    }
    return true;
  }

  int64_t DistGetKeyID(int64_t ini_rand,
                       double  key_dist_a, double key_dist_b,
                       bool    use_prefix_modeling) {
    int64_t keyrange_id;
    if (use_prefix_modeling) {
      int64_t rk = ini_rand % keyrange_rand_max_;
      int64_t lo = 0, hi = static_cast<int64_t>(keyrange_set_.size());
      while (lo + 1 < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        if (rk < keyrange_set_[mid].keyrange_start) {
          hi = mid;
        } else {
          lo = mid;
        }
      }
      keyrange_id = lo;
    } else {
      keyrange_id = ini_rand % keyrange_num_;
    }

    int64_t key_offset;
    if (key_dist_a == 0.0 || key_dist_b == 0.0) {
      key_offset = ini_rand % keyrange_size_;
    } else {
      double  u    = static_cast<double>(ini_rand % keyrange_size_) / keyrange_size_;
      int64_t seed = static_cast<int64_t>(
          std::ceil(std::pow((u / key_dist_a), (1 / key_dist_b))));
      Random64 r(seed);
      key_offset = r.Next() % keyrange_size_;
    }
    return keyrange_size_ * keyrange_id + key_offset;
  }
};

// Wrapper kept for compatibility with get_search_keys_prefix_dist() in
// bench/common/key_generator.h.
inline int64_t GetRandomKey(Random64* rand) {
  return rand->Next() % FLAGS_num;
}

#define MY_DB_FILE_NAME "dbfile"

inline splinterdb_config SetSplinterDBConfig(data_config& splinter_data_cfg) {
  splinterdb_config cfg;
  default_data_config_init(FLAGS_key_size, &splinter_data_cfg);
  std::memset(&cfg, 0, sizeof(cfg));

  std::string full_path = FLAGS_db_path + "/" + MY_DB_FILE_NAME;
  cfg.filename          = strdup(full_path.c_str());
  cfg.disk_size         = FLAGS_db_size_in_GB * 1024ULL * 1024 * 1024;
  cfg.cache_size        = FLAGS_block_cache_capacity * 1024 * 1024;
  cfg.data_cfg          = &splinter_data_cfg;
  cfg.io_flags          = FLAGS_use_direct_io ? (O_RDWR | O_CREAT | O_DIRECT)
                                              : (O_RDWR | O_CREAT);
  cfg.memtable_capacity = 24 * 1024 * 1024;
  cfg.use_stats         = FLAGS_use_stats;
  return cfg;
}
