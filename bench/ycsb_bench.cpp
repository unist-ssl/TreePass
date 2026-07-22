#include <iostream>
#include <vector>
#include <cinttypes>
#include <thread>
#include <cmath>
#include <sstream>  // for ParseWorkloadFracList
#include "key_generator.h"
#include "trace_io.h"
#include <string>   // for std::string in logging
#include <algorithm> // for std::sort, std::min
#include <numeric>   // for std::accumulate
#include <iomanip>   // for std::fixed, std::setprecision

#include "splinterdb_interface.h"

#include <sys/mman.h>       // mlockall
#include "gflags/gflags.h"

static MmapKeyArray trace;
#define SCAN_LENGTH 100


void GetTraceFromFile(int trace_num) {
  std::string target_dataset = FLAGS_trace_name;
  fprintf(stderr, "Target dataset name is %s, trace_num: %d\n", target_dataset.c_str(), trace_num);

  std::string bin_path = build_load_bin_path(target_dataset);

  auto build_from_csv = [&]() {
    FILE* trace_file = fopen(target_dataset.c_str(), "r");
    if (trace_file == NULL || trace_num == 0) {
      fprintf(stderr, "[Error] Failed opening trace file %s\n", FLAGS_trace_name.c_str());
      exit(1);
    }

    size_t bufsize = 100;
    char* buf = new char[100];
    key_vector tmp_trace;
    tmp_trace.reserve(trace_num);
    rewind(trace_file);

    int progress_interval = (trace_num >= 100000000) ? 100000000 : 10000000;
    for (int i = 0; i < trace_num; i++) {
      if (i > 0 && i % progress_interval == 0) {
        fprintf(stderr, "[LOAD-TRACE] Reading CSV: %dM / %dM keys (%.0f%%)\n",
                i / 1000000, trace_num / 1000000,
                100.0 * i / trace_num);
      }
      int status = getline(&buf, &bufsize, trace_file);
      if (status <= 1) {
        // The trace file ran out of keys before we read the number this run
        // needs. Insert-bearing workloads (YCSB-D/E) consume the loaded keys
        // plus one fresh key per insert, so the dataset CSV must hold at least
        // (load keys + total inserts) unique keys.
        fprintf(stderr,
                "[Error] Trace file %s ran out of keys after %d of %d needed. "
                "Insert-bearing workloads (YCSB-D/E) need (load + total insert) "
                "keys in the dataset; supply a larger dataset or lower --num / "
                "--phase_op_num.\n",
                target_dataset.c_str(), i, trace_num);
        exit(1);
      }
      if (status > 0 && buf[status - 1] == '\n') {
        buf[status - 1] = '\0';
      }
      key_type cur_key(buf);
      if (cur_key.empty()) {
        fprintf(stderr, "Zero key detected, please check dataset %s\n", FLAGS_trace_name.c_str());
        exit(1);
      }
      tmp_trace.push_back(std::move(cur_key));
    }
    delete[] buf;
    fclose(trace_file);
    fprintf(stderr, "[LOAD-TRACE] Reading CSV done: %dM keys\n", trace_num / 1000000);

    if (!save_load_trace(bin_path, tmp_trace, FLAGS_key_size)) {
      fprintf(stderr, "[Error] Failed to save load-trace binary: %s\n", bin_path.c_str());
      exit(1);
    }
    fprintf(stderr, "[LOAD-TRACE] Saved %zu keys to binary: %s\n", tmp_trace.size(), bin_path.c_str());
  };

  // If binary cache doesn't exist, build it from CSV first.
  if (!std::ifstream(bin_path).good()) {
    fprintf(stderr, "[LOAD-TRACE] Binary cache not found, building from CSV: %s\n", target_dataset.c_str());
    build_from_csv();
  }

  // mmap the binary file (zero-copy). If the existing cache is too small
  // (e.g. a previous run with a lower trace_num staged the file), drop it
  // and rebuild from the CSV before failing.
  if (!trace.open(bin_path, trace_num)) {
    fprintf(stderr, "[LOAD-TRACE] Cached binary is unusable, rebuilding from CSV...\n");
    std::remove(bin_path.c_str());
    build_from_csv();
    if (!trace.open(bin_path, trace_num)) {
      fprintf(stderr, "[Error] Failed to mmap load trace after rebuild: %s\n", bin_path.c_str());
      exit(1);
    }
  }
  if (trace.key_len() != static_cast<uint32_t>(FLAGS_key_size)) {
    fprintf(stderr, "[LOAD-TRACE] Updating key_size from load-trace header: %d -> %u\n",
            FLAGS_key_size, trace.key_len());
    FLAGS_key_size = static_cast<int>(trace.key_len());
  }
}

template <class DBInterface>
void RunBenchmark(void) {
  std::cerr << "Start to runbenchmark " << FLAGS_db_path << std::endl;
  // -------------------- PARSE WORKLOAD FRACTIONS --------------------
  // workload_frac_list is a single comma-list of six op fractions
  // (insert, update, read, scan, delete, rmw). D/E multi-phase
  // semantics come from num_repeats > 1, not from a multi-phase string.
  std::vector<double> phase_frac;
  {
    std::istringstream ss(FLAGS_workload_frac_list);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
      phase_frac.push_back(std::stod(tok));
    }
  }
  std::cerr << std::endl;
  // --------------------------------------------------------------------
  // Precompute per-phase operation counts and global query key traces
  // --------------------------------------------------------------------
  struct PhasePlan {
    size_t op_num;
    size_t insert_num;
    size_t update_num;
    size_t read_num;
    size_t scan_num;
    size_t delete_num;
    size_t rmw_num;
    size_t i_before;       // number of already-inserted keys at phase start
    size_t update_offset;       // offset into all_update_trace
    size_t read_offset;         // offset into all_read_trace
    size_t scan_offset;         // offset into all_scan_trace
    size_t delete_offset;       // offset into all_delete_trace
    size_t rmw_offset;          // offset into all_rmw_trace
  };
  size_t total_insert_keys = 0;
  size_t total_update_keys = 0;
  size_t total_read_keys   = 0;
  size_t total_scan_keys   = 0;
  size_t total_delete_keys = 0;
  size_t total_rmw_keys = 0;

  const int repeats          = FLAGS_num_repeats;  // always >= 1
  const int num_plan_entries = repeats + 1;        // warmup + real phases
  std::vector<PhasePlan> phase_plans(num_plan_entries);

  // Simulate i across phases to compute per-phase counts and i_before.
  size_t cur_i = FLAGS_num;  // i after load phase
  size_t total_future_inserts = 0;

  const double insert_frac = phase_frac[0];
  const double update_frac = phase_frac[1];
  const double read_frac   = phase_frac[2];
  const double scan_frac   = phase_frac[3];
  const double delete_frac = phase_frac[4];
  const double rmw_frac    = phase_frac[5];

  for (int p = 0; p < num_plan_entries; ++p) {
    const bool is_warmup = (p == 0);

    size_t base_op_num = is_warmup ? FLAGS_warmup_num : FLAGS_phase_op_num;
    base_op_num = FLAGS_workload_name != "E"? base_op_num : base_op_num / 8;
    size_t op_num      = base_op_num;

    if (!is_warmup && repeats > 1) {
      const int repeated_index = p - 1;
      const int repeat_index   = repeated_index % repeats;  // 0 .. repeats-1

      size_t per_repeat_floor = base_op_num / static_cast<size_t>(repeats);
      size_t remainder        = base_op_num % static_cast<size_t>(repeats);

      // First 'remainder' repeats get +1 to make sum == base_op_num.
      if (static_cast<size_t>(repeat_index) < remainder) {
        op_num = per_repeat_floor + 1;
      } else {
        op_num = per_repeat_floor;
      }
    }

    size_t phase_insert_num = static_cast<size_t>(op_num * insert_frac);
    size_t phase_update_num = static_cast<size_t>(op_num * update_frac);
    size_t phase_read_num   = static_cast<size_t>(op_num * read_frac);
    size_t phase_scan_num   = static_cast<size_t>(op_num * scan_frac);
    size_t phase_delete_num = static_cast<size_t>(op_num * delete_frac);
    size_t phase_rmw_num    = static_cast<size_t>(op_num * rmw_frac);

    phase_plans[p].op_num        = op_num;
    phase_plans[p].insert_num    = phase_insert_num;
    phase_plans[p].update_num    = phase_update_num;
    phase_plans[p].read_num      = phase_read_num;
    phase_plans[p].scan_num      = phase_scan_num;
    phase_plans[p].delete_num    = phase_delete_num;
    phase_plans[p].rmw_num       = phase_rmw_num;
    phase_plans[p].i_before      = cur_i;
    phase_plans[p].update_offset      = 0;
    phase_plans[p].read_offset        = 0;
    phase_plans[p].scan_offset        = 0;
    phase_plans[p].delete_offset      = 0;
    phase_plans[p].rmw_offset         = 0;

    total_insert_keys += phase_insert_num;
    total_update_keys += phase_update_num;
    total_read_keys   += phase_read_num;
    total_scan_keys   += phase_scan_num;
    total_delete_keys += phase_delete_num;
    total_rmw_keys    += phase_rmw_num;

    cur_i += phase_insert_num;
  }


  key_vector all_update_trace;
  key_vector all_read_trace;
  key_vector all_scan_trace;
  key_vector all_delete_trace;
  key_vector all_rmw_trace;

  all_update_trace.reserve(total_update_keys);
  all_read_trace.reserve(total_read_keys);
  all_scan_trace.reserve(total_scan_keys);
  all_delete_trace.reserve(total_delete_keys);
  all_rmw_trace.reserve(total_rmw_keys);

  GenerateTwoTermExpKeys gen_exp;


  // YCSB-style fudge factor (2x). You can tune this if needed.
  int num_future_keys_for_zipf = static_cast<int>(total_insert_keys * 2);
  
  DBInterface db_interface;
  GetTraceFromFile(FLAGS_num + total_insert_keys);

  std::cerr << "Start to initialize db" << std::endl;
  db_interface.InitializeDatabase();

  size_t load_num = FLAGS_num * FLAGS_load_frac;
  std::cerr << "Trace size: " << trace.size()
            << ", Load size: " << load_num << std::endl;
  const size_t kNumThreads = FLAGS_thread_num;
  std::cerr << "kNumThreads: " << kNumThreads << std::endl;
  std::cout << "kNumThreads: " << kNumThreads << std::endl;

  // i: number of already inserted keys (used for insert key index base)
  std::atomic<size_t> i{static_cast<size_t>(FLAGS_use_existing_db ? FLAGS_num : 0)};

  // -------------------- LOAD PHASE --------------------
  if (FLAGS_load_frac) {
    if (FLAGS_use_existing_db) {
      fprintf(stderr, "use existing db is true. Check options\n");
      exit(1);
    }

    std::cerr << "Start to " << FLAGS_load_frac * 100 << "\% load trace" << std::endl;
    auto loads_start_time = std::chrono::high_resolution_clock::now();
    std::atomic<bool> success{true}; // Atomic flag to track overall success

    const size_t keys_per_thread = load_num / kNumThreads;
    const size_t remainder = load_num % kNumThreads;

    auto thread_function = [&](size_t threadId) {
      splinterdb_register_thread(db_interface.get_raw_handle());
      const size_t start_idx = threadId * keys_per_thread;
      const size_t end_idx = start_idx + keys_per_thread + (threadId == kNumThreads - 1 ? remainder : 0);
      size_t local_processed = 0;
      RandomGenerator gen;

      for (size_t i_local = start_idx; i_local < end_idx; ++i_local) {
        if (i_local == start_idx || i_local % 10000000 == 0 || i_local == (end_idx -1)) {
          std::cerr << "[Thread " << threadId << "] "
                    << "i_local: " << i_local << ", start_idx: " << start_idx
                    << ", end_idx: " << end_idx << std::endl;
        }
        key_type insert_key = trace[i_local];

        size_t val_sz = static_cast<size_t>(FLAGS_value_size);
        char* value = gen.Generate(val_sz);

        if (!db_interface.Insert(insert_key, value, val_sz)) {
          std::cerr << "[Thread " << threadId << "] Error in Load for key: " << insert_key << std::endl;
          success.store(false);
          exit(1);
        }
        local_processed++;
      }
      i += local_processed;
      splinterdb_deregister_thread(db_interface.get_raw_handle());
    };

    std::vector<std::thread> threads;
    for (size_t threadId = 0; threadId < kNumThreads; ++threadId) {
      threads.emplace_back(thread_function, threadId);
    }
    for (auto& thread : threads) {
      thread.join();
    }

    if (!success.load()) {
      std::cerr << "Error occurred during Load. Exiting." << std::endl;
      exit(1);
    }

    auto loads_end_time = std::chrono::high_resolution_clock::now();
    double batch_load_time = std::chrono::duration_cast<std::chrono::nanoseconds>(loads_end_time - loads_start_time).count();
    double load_throughput = static_cast<double>(load_num) / batch_load_time * 1e9;
    std::cout << "Load throughput: " << load_throughput
              << ", elapsed: " << batch_load_time / 1e9
              << ", load_num: " << load_num << std::endl;
    std::cerr << "-------------------- [Load Done] -----------------------" << std::endl;

    if (FLAGS_use_stats) {
      db_interface.PrintTrunkInsertionStats();
      db_interface.PrintTrunkLookupStats();
      db_interface.ResetStats();
    }
  }

  if (FLAGS_workload_name == "load") {
    std::cerr << "LOAD DONE" << std::endl;
    db_interface.ShutdownDatabase();
    std::cerr << "ShutdownDatabase DONE" << std::endl;
    return;
  }

  std::cerr << "-------------------- [Phase Start] -----------------------" << std::endl;


  std::atomic<size_t> total_cum_insert_num(0);
  std::atomic<size_t> total_cum_update_num(0);
  std::atomic<size_t> total_cum_lookup_num(0);
  std::atomic<size_t> total_cum_scan_num(0);
  std::atomic<size_t> total_cum_delete_num(0);
  std::atomic<size_t> total_cum_rmw_num(0);
  std::atomic<size_t> total_cum_operation_num(0);

  // For prefix
  bool use_prefix_modeling = false;
  Random64 rand_gen(1);
  if (FLAGS_query_dist == "prefix" &&
      (FLAGS_keyrange_dist_a != 0.0 || FLAGS_keyrange_dist_b != 0.0 ||
       FLAGS_keyrange_dist_c != 0.0 || FLAGS_keyrange_dist_d != 0.0)) {
    use_prefix_modeling = true;
  }




  // ---- Query-keys cache: mmap a pre-staged QKTR file if one exists ----
  // When --query_trace_dir points at a directory, look for a per-config
  // .bin holding the read/update/scan/delete/rmw key vectors and mmap
  // them; subsequent runs of the same workload skip the (expensive)
  // generator step. On a miss, the keys are generated below and saved
  // back to the same path so the next run hits.
  bool trace_loaded = false;
  std::string trace_path;
  MmapQueryKeys mmap_query;

  if (!FLAGS_query_trace_dir.empty()) {
    trace_path = build_query_keys_path(FLAGS_query_trace_dir,
                                  FLAGS_workload_name,
                                  FLAGS_query_dist,
                                  FLAGS_zipf_const,
                                  FLAGS_num, FLAGS_warmup_num,
                                  FLAGS_phase_op_num, FLAGS_num_repeats,
                                  FLAGS_keyrange_num);
    trace_loaded = mmap_query.open(trace_path,
                                   FLAGS_query_dist,
                                   FLAGS_num, FLAGS_warmup_num,
                                   FLAGS_phase_op_num, FLAGS_num_repeats);
  }

  if (!trace_loaded) {
  // Initialize prefix distribution once (not per-phase) to keep hot ranges stable.
  if (FLAGS_query_dist == "prefix") {
    gen_exp.InitiateExpDistribution(
        FLAGS_num,
        FLAGS_keyrange_dist_a,
        FLAGS_keyrange_dist_b,
        FLAGS_keyrange_dist_c,
        FLAGS_keyrange_dist_d,
        use_prefix_modeling);
  }

  // Pre-generate query keys for all phases (warmup + real phases).
  for (int p = 0; p < num_plan_entries; ++p) {
    PhasePlan &plan = phase_plans[p];

    // logical_phase: -1 = warmup, 0.. = phase 1..
    int logical_phase = (p == 0) ? -1 : (p - 1);

    // ----------------- Update keys -----------------
    plan.update_offset = all_update_trace.size();
    if (plan.update_num) {
      std::cerr << p << "th [KeyGen] "
                << " | query=update"
                << " | i_before=" << plan.i_before
                << " | num_keys=" << plan.update_num
                << " | global_offset=" << plan.update_offset
                << std::endl;

      key_vector tmp;
      if (FLAGS_query_dist == "unif") {
        tmp = get_search_keys_unif(trace, plan.i_before, plan.update_num);
      } else if (FLAGS_query_dist == "zipf") {
        tmp = get_search_keys_zipf(trace, plan.i_before,
                                   num_future_keys_for_zipf,
                                   plan.update_num);
      } else if (FLAGS_query_dist == "prefix") {
        tmp = get_search_keys_prefix_dist(trace,
                                          plan.i_before,
                                          plan.update_num,
                                          rand_gen,
                                          gen_exp,
                                          FLAGS_key_dist_a,
                                          FLAGS_key_dist_b,
                                          use_prefix_modeling);
      } else {
        std::cerr << "Not support update dist version: " << FLAGS_query_dist << std::endl;
        exit(1);
      }
      all_update_trace.insert(all_update_trace.end(), tmp.begin(), tmp.end());
    }

    // ----------------- Read keys -----------------
    plan.read_offset = all_read_trace.size();
    if (plan.read_num) {
      std::cerr << p << "th [KeyGen] "
                << " | query=read"
                << " | i_before=" << plan.i_before
                << " | num_keys=" << plan.read_num
                << " | global_offset=" << plan.read_offset
                << std::endl;


      key_vector tmp;
      if (FLAGS_consider_latest_inserts) {
        // Use "latest" distribution over keys inserted up to plan.i_before
        tmp = get_search_keys_latest(trace,
                                     plan.i_before,
                                     static_cast<int>(plan.read_num));
      } else if (FLAGS_query_dist == "unif") {
        tmp = get_search_keys_unif(trace,
                                   plan.i_before,
                                   static_cast<int>(plan.read_num));
      } else if (FLAGS_query_dist == "zipf") {
        tmp = get_search_keys_zipf(trace,
                                   plan.i_before,
                                   num_future_keys_for_zipf,
                                   static_cast<int>(plan.read_num));
      } else if (FLAGS_query_dist == "prefix") {
        tmp = get_search_keys_prefix_dist(trace,
                                          plan.i_before,
                                          plan.read_num,
                                          rand_gen,
                                          gen_exp,
                                          FLAGS_key_dist_a,
                                          FLAGS_key_dist_b,
                                          use_prefix_modeling);
      } else {
        std::cerr << "Not support read dist version: " << FLAGS_query_dist << std::endl;
        exit(1);
      }
      all_read_trace.insert(all_read_trace.end(), tmp.begin(), tmp.end());
    }


    // ----------------- Scan keys -----------------
    plan.scan_offset = all_scan_trace.size();
    if (plan.scan_num) {
      std::cerr << p << "th [KeyGen] "
                << " | query=scan"
                << " | i_before=" << plan.i_before
                << " | num_keys=" << plan.scan_num
                << " | global_offset=" << plan.scan_offset
                << std::endl;

      key_vector tmp;
      if (FLAGS_query_dist == "unif") {
        tmp = get_search_keys_unif(trace, plan.i_before, plan.scan_num);
      } else if (FLAGS_query_dist == "zipf") {
        tmp = get_search_keys_zipf(trace, plan.i_before,
                                   num_future_keys_for_zipf,
                                   plan.scan_num);
      } else if (FLAGS_query_dist == "prefix") {
        tmp = get_search_keys_prefix_dist(trace,
                                          plan.i_before,
                                          plan.scan_num,
                                          rand_gen,
                                          gen_exp,
                                          FLAGS_key_dist_a,
                                          FLAGS_key_dist_b,
                                          use_prefix_modeling);
      } else {
        std::cerr << "Not support scan dist version: " << FLAGS_query_dist << std::endl;
        exit(1);
      }
      all_scan_trace.insert(all_scan_trace.end(), tmp.begin(), tmp.end());
    }

    // ----------------- Delete keys -----------------
    plan.delete_offset = all_delete_trace.size();
    if (plan.delete_num) {
      std::cerr << p << "th [KeyGen] "
                << " | query=delete"
                << " | i_before=" << plan.i_before
                << " | num_keys=" << plan.delete_num
                << " | global_offset=" << plan.delete_offset
                << std::endl;

      key_vector tmp;
      if (FLAGS_query_dist == "unif") {
        tmp = get_search_keys_unif(trace, plan.i_before, plan.delete_num);
      } else if (FLAGS_query_dist == "zipf") {
        tmp = get_search_keys_zipf(trace, plan.i_before,
                                   num_future_keys_for_zipf,
                                   plan.delete_num);
      } else if (FLAGS_query_dist == "prefix") {
        tmp = get_search_keys_prefix_dist(trace,
                                          plan.i_before,
                                          plan.delete_num,
                                          rand_gen,
                                          gen_exp,
                                          FLAGS_key_dist_a,
                                          FLAGS_key_dist_b,
                                          use_prefix_modeling);
      } else {
        std::cerr << "Not support delete dist version: " << FLAGS_query_dist << std::endl;
        exit(1);
      }
      all_delete_trace.insert(all_delete_trace.end(), tmp.begin(), tmp.end());
    }

    // ----------------- RMW keys -----------------
    plan.rmw_offset = all_rmw_trace.size();
    if (plan.rmw_num) {
      std::cerr << p << "th [KeyGen] "
                << " | query=rmw"
                << " | i_before=" << plan.i_before
                << " | num_keys=" << plan.rmw_num
                << " | global_offset=" << plan.rmw_offset
                << std::endl;

      key_vector tmp;
      if (FLAGS_query_dist == "unif") {
        tmp = get_search_keys_unif(trace, plan.i_before, plan.rmw_num);
      } else if (FLAGS_query_dist == "zipf") {
        tmp = get_search_keys_zipf(trace, plan.i_before,
                                   num_future_keys_for_zipf,
                                   plan.rmw_num);
      } else if (FLAGS_query_dist == "prefix") {
        tmp = get_search_keys_prefix_dist(trace,
                                          plan.i_before,
                                          plan.rmw_num,
                                          rand_gen,
                                          gen_exp,
                                          FLAGS_key_dist_a,
                                          FLAGS_key_dist_b,
                                          use_prefix_modeling);
      } else {
        std::cerr << "Not support rmw dist version: " << FLAGS_query_dist << std::endl;
        exit(1);
      }
      all_rmw_trace.insert(all_rmw_trace.end(), tmp.begin(), tmp.end());
    }
  }

  // Save generated traces to cache
  if (!FLAGS_query_trace_dir.empty()) {
    save_query_keys(trace_path,
                      FLAGS_query_dist,
                      FLAGS_num, FLAGS_warmup_num,
                      FLAGS_phase_op_num, FLAGS_num_repeats,
                      all_update_trace, all_read_trace,
                      all_scan_trace, all_delete_trace,
                      all_rmw_trace);
    std::cerr << "[TRACE] Saved query traces to: " << trace_path << std::endl;

    // Now mmap the saved file and free the key_vectors
    trace_loaded = mmap_query.open(trace_path,
                                   FLAGS_query_dist,
                                   FLAGS_num, FLAGS_warmup_num,
                                   FLAGS_phase_op_num, FLAGS_num_repeats);
    if (trace_loaded) {
      all_update_trace.clear(); all_update_trace.shrink_to_fit();
      all_read_trace.clear();   all_read_trace.shrink_to_fit();
      all_scan_trace.clear();   all_scan_trace.shrink_to_fit();
      all_delete_trace.clear(); all_delete_trace.shrink_to_fit();
      all_rmw_trace.clear();    all_rmw_trace.shrink_to_fit();
    }
  }
  } // end if (!trace_loaded)

  // Recompute per-phase offsets from cumulative trace sizes.
  {
    size_t u_off = 0, r_off = 0, s_off = 0, d_off = 0, rmw_off = 0;
    for (int p = 0; p < num_plan_entries; ++p) {
      phase_plans[p].update_offset = u_off;
      phase_plans[p].read_offset   = r_off;
      phase_plans[p].scan_offset   = s_off;
      phase_plans[p].delete_offset = d_off;
      phase_plans[p].rmw_offset    = rmw_off;
      u_off   += phase_plans[p].update_num;
      r_off   += phase_plans[p].read_num;
      s_off   += phase_plans[p].scan_num;
      d_off   += phase_plans[p].delete_num;
      rmw_off += phase_plans[p].rmw_num;
    }
  }

  // ---- Set up query trace accessors (mmap or key_vector) ----
  // These lambdas provide unified access regardless of backing store
  auto get_update_key = [&](size_t idx) -> std::string {
    return mmap_query.is_open() ? mmap_query.update_trace()[idx] : all_update_trace[idx];
  };
  auto get_read_key = [&](size_t idx) -> std::string {
    return mmap_query.is_open() ? mmap_query.read_trace()[idx] : all_read_trace[idx];
  };
  auto get_scan_key = [&](size_t idx) -> std::string {
    return mmap_query.is_open() ? mmap_query.scan_trace()[idx] : all_scan_trace[idx];
  };
  auto get_delete_key = [&](size_t idx) -> std::string {
    return mmap_query.is_open() ? mmap_query.delete_trace()[idx] : all_delete_trace[idx];
  };
  auto get_rmw_key = [&](size_t idx) -> std::string {
    return mmap_query.is_open() ? mmap_query.rmw_trace()[idx] : all_rmw_trace[idx];
  };

  // -------------------- PHASE EXECUTION --------------------
  double total_workload_elapsed_time = 0;

  // plan_idx: 0 for warmup, 1..num_phases for repeated phases
  for (int plan_idx = 0; plan_idx < num_plan_entries; ++plan_idx) {
    const bool is_warmup = (plan_idx == 0);
    PhasePlan &plan      = phase_plans[plan_idx];

    int num_actual_inserts = static_cast<int>(plan.insert_num);
    int num_actual_updates = static_cast<int>(plan.update_num);
    int num_actual_lookups = static_cast<int>(plan.read_num);
    int num_actual_scans   = static_cast<int>(plan.scan_num);
    int num_actual_deletes = static_cast<int>(plan.delete_num);
    int num_actual_rmws    = static_cast<int>(plan.rmw_num);

    std::atomic<size_t> cum_insert_num(0);
    std::atomic<size_t> cum_update_num(0);
    std::atomic<size_t> cum_lookup_num(0);
    std::atomic<size_t> cum_scan_num(0);
    std::atomic<size_t> cum_delete_num(0);
    std::atomic<size_t> cum_rmw_num(0);

    const int repeat_index = is_warmup ? 0 : (plan_idx - 1);  // 0..repeats-1
    std::string phase_label = is_warmup
        ? std::string("[Warmup Phase]")
        : "[Phase repeat " + std::to_string(repeat_index + 1) +
          "/" + std::to_string(repeats) + "]";

    size_t op_num           = plan.op_num;
    size_t phase_insert_num = plan.insert_num;
    size_t phase_update_num = plan.update_num;
    size_t phase_read_num   = plan.read_num;
    size_t phase_scan_num   = plan.scan_num;
    size_t phase_delete_num = plan.delete_num;
    size_t phase_rmw_num    = plan.rmw_num;

    std::cerr << (is_warmup ? "[INFO] Warmup Phase"
                            : "[INFO] Phase plan_idx=" + std::to_string(plan_idx))
              << " - Operations count: "
              << "Insert: " << phase_insert_num << ", "
              << "Update: " << phase_update_num << ", "
              << "Read: "   << phase_read_num   << ", "
              << "Scan: "   << phase_scan_num   << ", "
              << "Delete: " << phase_delete_num << ", "
              << "RMW: "    << phase_rmw_num
              << std::endl;

    auto workload_start_time = std::chrono::high_resolution_clock::now();
    std::cout << phase_label << " thread function start" << std::endl;

    auto thread_function2 = [&](int threadId) {
      splinterdb_register_thread(db_interface.get_raw_handle());
      std::random_device rd;
      std::mt19937 random_gen(rd());
      std::uniform_int_distribution<> op_dist_(0, 99);
      RandomGenerator gen;

      int insert_thres = static_cast<int>(std::round(insert_frac * 100));
      int update_thres = insert_thres + static_cast<int>(std::round(update_frac * 100));
      int read_thres   = update_thres + static_cast<int>(std::round(read_frac * 100));
      int scan_thres   = read_thres + static_cast<int>(std::round(scan_frac * 100));
      int delete_thres = scan_thres + static_cast<int>(std::round(delete_frac * 100));
      int rmw_thres = scan_thres + static_cast<int>(std::round(rmw_frac * 100));

      // workload_frac_list is ignored in trace mode (op mix from trace file).
      if (FLAGS_query_dist != "trace" && rmw_thres != 100) {
        std::cerr << "Thresholds do not sum to 100: "
                  << "Insert: " << insert_thres
                  << ", Update: " << update_thres
                  << ", Read: " << read_thres
                  << ", Scan: " << scan_thres
                  << ", Delete: " << delete_thres
                  << ", RMW: " << rmw_thres
                  << std::endl;
        exit(1);
      }

      int i_idx = 0, u_idx = 0, r_idx = 0, s_idx = 0, d_idx = 0, rmw_idx = 0;
      int i_num = 0;

      if (plan.insert_num > 0 && plan.i_before < trace.size()) {
        i_num = num_actual_inserts / static_cast<int>(kNumThreads);
      }
      int u_num = static_cast<int>(plan.update_num / kNumThreads);
      int r_num = static_cast<int>(plan.read_num   / kNumThreads);
      int s_num = static_cast<int>(plan.scan_num   / kNumThreads);
      int d_num = static_cast<int>(plan.delete_num / kNumThreads);
      int rmw_num = static_cast<int>(plan.rmw_num / kNumThreads);

      int i_start_idx = static_cast<int>(plan.i_before) + i_num * threadId;
      int u_start_idx = static_cast<int>(plan.update_offset) + u_num * threadId;
      int r_start_idx = static_cast<int>(plan.read_offset)   + r_num * threadId;
      int s_start_idx = static_cast<int>(plan.scan_offset)   + s_num * threadId;
      int d_start_idx = static_cast<int>(plan.delete_offset) + d_num * threadId;
      int rmw_start_idx = static_cast<int>(plan.rmw_offset) + rmw_num * threadId;

      if (threadId == (kNumThreads - 1)) {
        i_num += num_actual_inserts % static_cast<int>(kNumThreads);
        u_num += static_cast<int>(plan.update_num % kNumThreads);
        r_num += static_cast<int>(plan.read_num   % kNumThreads);
        s_num += static_cast<int>(plan.scan_num   % kNumThreads);
        d_num += static_cast<int>(plan.delete_num % kNumThreads);
        rmw_num += static_cast<int>(plan.rmw_num % kNumThreads);
        std::cerr << "i_num: " << i_num << ", u_num: " << u_num
                  << ", r_num: " << r_num << ", s_num: " << s_num
                  << ", d_num: " << d_num
                  << ", rmw_num: " << rmw_num << std::endl;
      }

      // ---- Phase loop: cumulative-threshold random op selection ----
      // Each iteration draws op_dist_(random_gen) and routes the request to
      // insert/update/read/scan/delete/rmw based on the per-phase
      // workload_frac_list thresholds. The loop ends once every op type
      // has drained its per-thread slice.
      while (i_idx < i_num || u_idx < u_num || r_idx < r_num || s_idx < s_num || d_idx < d_num || rmw_idx < rmw_num) {
        const uint32_t choice = op_dist_(random_gen);

        if (choice < insert_thres && i_idx < i_num) { // Insert operation
          bool success = false;
          key_type insert_key = trace[i_start_idx + i_idx];  // 원본 key
          i_idx++;
          char* value = gen.Generate(FLAGS_value_size);
          success = db_interface.Insert(insert_key, value, FLAGS_value_size);
          if (!success) {
            fprintf(stderr, "Error in Insert\n");
            exit(1);
          }
          cum_insert_num++;
        } else if (choice < update_thres && u_idx < u_num) { // Update operation
          bool success = false;
          key_type update_key = get_update_key(u_start_idx + u_idx);
          u_idx++;
          char* value = gen.Generate(FLAGS_value_size);
          success = db_interface.Update(update_key, value, FLAGS_value_size);
          if (!success) {
            fprintf(stderr, "Error in Update\n");
            exit(1);
          }
          cum_update_num++;
        } else if (choice < read_thres && r_idx < r_num) { // Read operation
          if (threadId == 0 && r_idx % 1000000 == 0)
            std::cerr << "r_idx: " << r_idx << ", ";
          bool success = false;
          key_type read_key = get_read_key(r_start_idx + r_idx);
          r_idx++;
          std::string value_out;
          success = db_interface.Read(read_key, &value_out);
          if (!success) {
            fprintf(stderr, "Error in Read\n");
            exit(1);
          }
          cum_lookup_num++;
        } else if (choice < scan_thres && s_idx < s_num) { // Scan operation
          if (threadId == 0 && s_idx % 100000 == 0)
            std::cerr << "s_idx: " << s_idx << std::endl;
          bool success = false;
          key_type scan_key = get_scan_key(s_start_idx + s_idx);
          s_idx++;
          std::vector<std::pair<std::string, std::string>> scan_out;
          success = db_interface.Scan(scan_key, SCAN_LENGTH, &scan_out);
          if (!success) {
            fprintf(stderr, "Error in Scan\n");
            exit(1);
          }
          cum_scan_num++;
        } else if (d_idx < d_num) { // Delete operation
          bool success = false;
          key_type delete_key = get_delete_key(d_start_idx + d_idx);
          d_idx++;
          success = db_interface.Delete(delete_key);
          if (!success) {
            fprintf(stderr, "Error in Delete\n");
            exit(1);
          }
          cum_delete_num++;
        } else if (rmw_idx < rmw_num) { // RMW operation
          bool success = false;
          key_type rmw_key = get_rmw_key(rmw_start_idx + rmw_idx);
          rmw_idx++;
          char* value = gen.Generate(FLAGS_value_size);
          success = db_interface.RMW(rmw_key, value, FLAGS_value_size);
          if (!success) {
            fprintf(stderr, "Error in RMW\n");
            exit(1);
          }
          cum_rmw_num++;
        }
      }

      splinterdb_deregister_thread(db_interface.get_raw_handle());
    };
    std::vector<std::thread> threads2;
    std::cerr << "kNumThreads: " << kNumThreads << std::endl;
    for (size_t tid = 0; tid < kNumThreads; ++tid) {
      threads2.emplace_back(std::thread(thread_function2, static_cast<int>(tid)));
    }
    for (auto& thread : threads2) {
      thread.join();
    }

    // Advance global insert counter by the number of inserts in this phase
    i += num_actual_inserts;

    if (is_warmup)
      db_interface.ResetStats();
    std::cout << phase_label <<" Finished operation: " << FLAGS_trace_name
              << " of " << FLAGS_db_type << std::endl;
    if (is_warmup) {
      continue;
    } 
    double workload_elapsed_time =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now() - workload_start_time)
      .count();
    double total_throughput = static_cast<double>(cum_insert_num + cum_update_num + cum_lookup_num
        + cum_scan_num + cum_delete_num + cum_rmw_num) / workload_elapsed_time * 1e9;
    std::cout << "workload_frac_list: " << FLAGS_workload_frac_list;
    std::cout << ", total throughput: " << total_throughput
              << ", elapsed: " << workload_elapsed_time / 1e9 << std::endl;
    std::cout << "insert num: " << cum_insert_num
              << ", update num: " << cum_update_num
              << ", read num: " << cum_lookup_num
              << ", scan num: " << cum_scan_num
              << ", delete num: " << cum_delete_num
              << ", rmw num: " << cum_rmw_num
              << std::endl;
    total_cum_insert_num += cum_insert_num;
    total_cum_update_num += cum_update_num;
    total_cum_lookup_num += cum_lookup_num;
    total_cum_scan_num += cum_scan_num;
    total_cum_delete_num += cum_delete_num;
    total_cum_rmw_num += cum_rmw_num;
    total_workload_elapsed_time += workload_elapsed_time;
  }   // for phase
  if (FLAGS_use_stats) {
    if (total_cum_insert_num + total_cum_update_num)
      db_interface.PrintTrunkInsertionStats();
    db_interface.PrintTrunkLookupStats();
  }

  std::cout << "TOTAL " << repeats << " phases (repeats=" << repeats << ")"
            << ", Finished operation: " << FLAGS_trace_name
            << " of " << FLAGS_db_type
            << std::endl;

  double total_throughput = static_cast<double>(total_cum_insert_num + total_cum_update_num
                                                + total_cum_lookup_num + total_cum_scan_num
                                                + total_cum_delete_num + total_cum_rmw_num)
                            / (total_workload_elapsed_time) * 1e9;
  total_cum_operation_num = total_cum_insert_num + total_cum_update_num + total_cum_lookup_num
                            + total_cum_scan_num + total_cum_delete_num + total_cum_rmw_num;
  if (total_cum_insert_num) {
    std::cout << "[TOTAL] all insert_fracs : " << FLAGS_workload_frac_list;
  } else {
    std::cout << "[TOTAL] workload_frac_list : " << FLAGS_workload_frac_list;
  }
  std::cout << ", total throughput: " << total_throughput
            << ", total elapsed: " << total_workload_elapsed_time / 1e9 << std::endl;
  std::cout << "[TOTAL] insert num: " << total_cum_insert_num << ", update num: "
            << total_cum_update_num << ", read num: " << total_cum_lookup_num
            << ", scan num: " << total_cum_scan_num << ", delete num: " << total_cum_delete_num
            << ", rmw num: " << total_cum_rmw_num << std::endl;
  if (total_cum_insert_num)
    std::cout << "[TOTAL] real insert ratio: " << (double)total_cum_insert_num / total_cum_operation_num;
  else
    std::cout << "[TOTAL] real update ratio: " << (double)total_cum_update_num / total_cum_operation_num;
  std::cout << ", real read ratio: " << (double)total_cum_lookup_num / total_cum_operation_num
            << ", real scan ratio: " << (double)total_cum_scan_num / total_cum_operation_num
            << ", real delete ratio: " << (double)total_cum_delete_num / total_cum_operation_num
            << ", real rmw ratio: " << (double)total_cum_rmw_num / total_cum_operation_num
            << std::endl;
  std::cout << "trace info: " << FLAGS_trace_name << ", " << trace.size() << std::endl;


  std::cerr << "BENCHMARK DONE" << std::endl;
  db_interface.ShutdownDatabase();
  std::cerr << "ShutdownDatabase DONE" << std::endl;
}

int main(int argc, char* argv[]) {
  gflags::SetUsageMessage("Run YCSB-style load + query workloads on SplinterDB.");
  gflags::ParseCommandLineFlags(&argc, &argv, /*remove_flags=*/true);
  std::cerr << "Run bench: " << FLAGS_db_path << std::endl;
  std::cout << "====================== TreePass bench start ======================"
            << "\n workload=" << FLAGS_workload_name
            << " query_dist=" << FLAGS_query_dist
            << " threads=" << FLAGS_thread_num
            << " cache=" << FLAGS_block_cache_capacity << " MB"
            << " phase_ops=" << FLAGS_phase_op_num << std::endl;
  RunBenchmark<SplinterDBInterface>();
  std::cout << "====================== TreePass bench done  ======================"
            << std::endl;
  return 0;
}

