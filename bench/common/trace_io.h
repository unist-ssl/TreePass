// bench/common/trace_io.h
//
// Binary cache formats and mmap readers used by ycsb_bench.
//
//   load trace        (.bin) : keys + per-key value sizes pre-staged from
//                              the load CSV (magic "LOAD").
//   query keys        (.bin) : per-op key vectors (update / read / scan
//                              / delete / rmw) that ycsb_bench would have
//                              generated for a given (workload_name,
//                              query_dist, load_num, warmup_num,
//                              phase_op_num, num_repeats) tuple, pre-staged
//                              so subsequent runs skip the generator
//                              step (notably the per-process std::sort
//                              over the trace for the prefix distribution).
//                              Magic "YCSB"; files live alongside the load
//                              trace. It stores one key vector per op type
//                              and ycsb_bench picks the op order itself at
//                              run time.

#pragma once

#include "key_generator.h"  // key_vector

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>


// ============================================================
// Load trace
//
// Layout:
//   LoadTraceHeader (16 B, magic "LOAD")
//   keys : num_keys × key_len bytes (zero-padded)
// ============================================================

struct LoadTraceHeader {
  char     magic[4];   // "LOAD"
  uint32_t key_len;    // max key length in dataset
  uint64_t num_keys;
};
static_assert(sizeof(LoadTraceHeader) == 16, "LoadTraceHeader must be 16 bytes");

inline constexpr char kLoadTraceMagic[4] = {'L','O','A','D'};


// ----------------------------------------------------------------------
// MmapKeyArray: zero-copy mmap reader for load trace files. Exposes a
// std::string view per key.
// ----------------------------------------------------------------------
class MmapKeyArray {
  void*       mapped_    = nullptr;
  size_t      file_size_ = 0;
  size_t      num_keys_  = 0;
  uint32_t    key_len_   = 0;
  const char* data_      = nullptr;
  int         fd_        = -1;

public:
  MmapKeyArray() = default;
  ~MmapKeyArray() { close(); }

  // Non-copyable, movable
  MmapKeyArray(const MmapKeyArray&) = delete;
  MmapKeyArray& operator=(const MmapKeyArray&) = delete;
  MmapKeyArray(MmapKeyArray&& o) noexcept
    : mapped_(o.mapped_), file_size_(o.file_size_), num_keys_(o.num_keys_),
      key_len_(o.key_len_), data_(o.data_), fd_(o.fd_) {
    o.mapped_ = nullptr; o.fd_ = -1;
  }
  MmapKeyArray& operator=(MmapKeyArray&& o) noexcept {
    if (this != &o) {
      close();
      mapped_   = o.mapped_;   file_size_ = o.file_size_;
      num_keys_ = o.num_keys_; key_len_   = o.key_len_;
      data_     = o.data_;     fd_        = o.fd_;
      o.mapped_ = nullptr;     o.fd_      = -1;
    }
    return *this;
  }

  bool open(const std::string& path, size_t expected_num) {
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) return false;

    struct stat st;
    if (fstat(fd_, &st) != 0) { ::close(fd_); fd_ = -1; return false; }
    file_size_ = st.st_size;

    mapped_ = mmap(nullptr, file_size_, PROT_READ,
                   MAP_PRIVATE | MAP_POPULATE, fd_, 0);
    if (mapped_ == MAP_FAILED) {
      mapped_ = nullptr; ::close(fd_); fd_ = -1; return false;
    }

    auto* hdr = reinterpret_cast<const LoadTraceHeader*>(mapped_);
    if (std::memcmp(hdr->magic, kLoadTraceMagic, 4) != 0) {
      fprintf(stderr, "[load-trace] bad magic in: %s\n", path.c_str());
      close(); return false;
    }
    if (hdr->num_keys < static_cast<uint64_t>(expected_num)) {
      fprintf(stderr, "[load-trace] file has %lu keys, need %zu: %s\n",
              (unsigned long)hdr->num_keys, expected_num, path.c_str());
      close(); return false;
    }

    key_len_  = hdr->key_len;
    num_keys_ = expected_num;
    data_     = reinterpret_cast<const char*>(mapped_) + sizeof(LoadTraceHeader);

    // Pin in memory so cgroup pressure won't evict trace pages.
    if (mlock(mapped_, file_size_) != 0) {
      fprintf(stderr, "[load-trace] mlock failed (%.1f MB): %s (non-fatal)\n",
              file_size_ / (1024.0 * 1024.0), strerror(errno));
    } else {
      fprintf(stderr, "[load-trace] mlock pinned %.1f MB\n",
              file_size_ / (1024.0 * 1024.0));
    }

    fprintf(stderr,
            "[load-trace] mapped %s: %zu keys, key_len=%u, file=%.1f MB\n",
            path.c_str(), num_keys_, key_len_,
            file_size_ / (1024.0 * 1024.0));
    return true;
  }

  void close() {
    if (mapped_) { munmap(mapped_, file_size_); mapped_ = nullptr; }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    num_keys_ = 0; data_ = nullptr;
  }

  // Accessors used by the bench drivers.
  size_t   size()    const { return num_keys_; }
  uint32_t key_len() const { return key_len_; }

  // Materialize the i-th key as a std::string (24-byte copy; SSO handles
  // short keys without a heap allocation).
  std::string operator[](size_t i) const {
    const char* start = data_ + i * key_len_;
    size_t len = strnlen(start, key_len_);
    return std::string(start, len);
  }
};


// ----------------------------------------------------------------------
// Serialize a (keys + per-key value sizes) pair as a LoadTrace .bin file.
// Caller supplies an explicit target_key_size (max length for padding);
// pass 0 to auto-detect from the input.
// ----------------------------------------------------------------------

// Build the on-disk load-trace path from a load CSV path: strip the .csv
// suffix (or fall through if absent) and append .bin. Mirrors what
// LoadTraceFromFile derives implicitly.
inline std::string build_load_bin_path(const std::string& csv_path) {
  std::string out = csv_path;
  auto dot = out.rfind('.');
  if (dot != std::string::npos) out = out.substr(0, dot);
  return out + ".bin";
}

inline bool save_load_trace(const std::string& path, const key_vector& keys,
                            uint32_t target_key_size = 0) {
  if (keys.empty()) {
    fprintf(stderr, "[load-trace] save_load_trace requires non-empty keys\n");
    return false;
  }

  FILE* fp = fopen(path.c_str(), "wb");
  if (!fp) {
    fprintf(stderr, "[load-trace] cannot create %s: %s\n",
            path.c_str(), strerror(errno));
    return false;
  }

  uint32_t key_len = target_key_size;
  if (key_len == 0) {
    for (const auto& k : keys) {
      if (k.size() > key_len) key_len = static_cast<uint32_t>(k.size());
    }
  }

  LoadTraceHeader hdr;
  std::memset(&hdr, 0, sizeof(hdr));
  std::memcpy(hdr.magic, kLoadTraceMagic, 4);
  hdr.num_keys = keys.size();
  hdr.key_len  = key_len;
  if (fwrite(&hdr, sizeof(hdr), 1, fp) != 1) { fclose(fp); return false; }

  // Keys: write in zero-padded chunks of key_len bytes.
  constexpr size_t chunk = 65536;
  std::vector<char> buf(chunk * key_len, 0);
  for (size_t i = 0; i < keys.size(); ) {
    size_t batch = std::min(chunk, keys.size() - i);
    std::memset(buf.data(), 0, batch * key_len);
    for (size_t j = 0; j < batch; ++j) {
      std::memcpy(buf.data() + j * key_len,
                  keys[i + j].data(), keys[i + j].size());
    }
    if (fwrite(buf.data(), key_len, batch, fp) != batch) {
      fclose(fp); return false;
    }
    i += batch;
  }

  fclose(fp);
  return true;
}


// ============================================================
// Query keys
//
// Layout:
//   QueryKeysHeader (64 B, magic "QKTR")
//   5 op vectors, each prefixed with [num_keys u64 | key_len u32 | pad u32]
//   followed by num_keys * key_len bytes (keys are right-padded with NULs to
//   key_len). The op order is: update, read, scan, delete, rmw — insert keys
//   are NOT pre-staged because they are taken sequentially from the load
//   trace via cur_inserted++.
// ============================================================

struct QueryKeysHeader {
  char     magic[4];          // "YCSB"
  uint32_t dist_type;         // 0=unif, 1=zipf, 2=prefix, 3=latest
  uint32_t load_num;          // FLAGS_num
  uint32_t warmup_num;
  uint32_t phase_op_num;
  uint32_t num_repeats;
};
static_assert(sizeof(QueryKeysHeader) == 24,
              "QueryKeysHeader must be 24 bytes");

inline constexpr char kQueryKeysMagic[4] = {'Y','C','S','B'};

inline uint32_t query_dist_to_uint(const std::string& dist) {
  if (dist == "unif")   return 0;
  if (dist == "zipf")   return 1;
  if (dist == "prefix") return 2;
  if (dist == "latest") return 3;
  return 0xFFu;
}

// Build the on-disk cache path. Lives next to the load trace (same directory
// the bench reads --trace_load_file / --trace_name from). Filename pattern:
//   {workload}_{dist}[_{kr<N>}][_{zipf_const}]_{load/1M}M_{warmup/1M}M_{phase/1M}M_r{repeats}.bin
// kr<N> and zipf_const are emitted only when they differ from the defaults
// (kr=30, zipf=0.99) so the common case keeps the shorter filename.
inline std::string build_query_keys_path(const std::string& dir,
                                         const std::string& workload_name,
                                         const std::string& query_dist,
                                         double zipf_const,
                                         int load_num,
                                         int warmup_num,
                                         int phase_op_num,
                                         int num_repeats,
                                         int keyrange_num) {
  constexpr double kDefaultZipfConst = 0.99;
  char buf[1024];
  if (query_dist == "zipf" &&
      std::abs(zipf_const - kDefaultZipfConst) > 1e-9) {
    snprintf(buf, sizeof(buf), "%s/%s_%s_%.2f_%dM_%dM_%dM_r%d.bin",
             dir.c_str(), workload_name.c_str(), query_dist.c_str(),
             zipf_const,
             load_num / 1000000, warmup_num / 1000000, phase_op_num / 1000000,
             num_repeats);
  } else if (query_dist == "prefix" && keyrange_num > 1 && keyrange_num != 30) {
    snprintf(buf, sizeof(buf), "%s/%s_%s_kr%d_%dM_%dM_%dM_r%d.bin",
             dir.c_str(), workload_name.c_str(), query_dist.c_str(),
             keyrange_num,
             load_num / 1000000, warmup_num / 1000000, phase_op_num / 1000000,
             num_repeats);
  } else {
    snprintf(buf, sizeof(buf), "%s/%s_%s_%dM_%dM_%dM_r%d.bin",
             dir.c_str(), workload_name.c_str(), query_dist.c_str(),
             load_num / 1000000, warmup_num / 1000000, phase_op_num / 1000000,
             num_repeats);
  }
  return std::string(buf);
}

// Lightweight view into a sub-vector region of a mapped query-keys file.
class MmapSubArray {
  const char* data_     = nullptr;
  uint32_t    key_len_  = 0;
  size_t      num_keys_ = 0;

public:
  MmapSubArray() = default;
  MmapSubArray(const char* data, uint32_t key_len, size_t num_keys)
    : data_(data), key_len_(key_len), num_keys_(num_keys) {}

  size_t size()  const { return num_keys_; }
  bool   empty() const { return num_keys_ == 0; }

  std::string operator[](size_t i) const {
    const char* start = data_ + i * key_len_;
    size_t len = strnlen(start, key_len_);
    return std::string(start, len);
  }
};

// mmap-based reader for the per-op vectors that ycsb_bench would otherwise
// have to regenerate via the key generator.
class MmapQueryKeys {
public:
  enum Op { OP_UPDATE = 0, OP_READ = 1, OP_SCAN = 2, OP_DELETE = 3, OP_RMW = 4,
            NUM_OPS = 5 };

private:
  void*  mapped_    = nullptr;
  size_t file_size_ = 0;
  int    fd_        = -1;
  MmapSubArray subs_[NUM_OPS];

  bool parse_sub(const char* base, size_t& offset, int idx) {
    if (offset + 16 > file_size_) return false;
    uint64_t num_keys;
    uint32_t key_len, padding;
    std::memcpy(&num_keys, base + offset, 8); offset += 8;
    std::memcpy(&key_len,  base + offset, 4); offset += 4;
    std::memcpy(&padding,  base + offset, 4); offset += 4;
    if (num_keys == 0 || key_len == 0) {
      subs_[idx] = MmapSubArray(nullptr, 0, 0);
      return true;
    }
    size_t data_size = static_cast<size_t>(num_keys) * key_len;
    if (offset + data_size > file_size_) return false;
    subs_[idx] = MmapSubArray(base + offset, key_len, num_keys);
    offset += data_size;
    return true;
  }

public:
  MmapQueryKeys() = default;
  ~MmapQueryKeys() { close(); }
  MmapQueryKeys(const MmapQueryKeys&)            = delete;
  MmapQueryKeys& operator=(const MmapQueryKeys&) = delete;

  bool open(const std::string& path, const std::string& query_dist,
            int load_num, int warmup_num,
            int phase_op_num, int num_repeats) {
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) return false;

    struct stat st;
    if (fstat(fd_, &st) != 0) { ::close(fd_); fd_ = -1; return false; }
    file_size_ = st.st_size;

    mapped_ = mmap(nullptr, file_size_, PROT_READ,
                   MAP_PRIVATE | MAP_POPULATE, fd_, 0);
    if (mapped_ == MAP_FAILED) {
      mapped_ = nullptr; ::close(fd_); fd_ = -1; return false;
    }

    const char* base = reinterpret_cast<const char*>(mapped_);
    auto* hdr = reinterpret_cast<const QueryKeysHeader*>(base);
    if (std::memcmp(hdr->magic, kQueryKeysMagic, 4) != 0) {
      fprintf(stderr, "[query-keys] bad magic in: %s\n", path.c_str());
      close(); return false;
    }

    if (hdr->dist_type    != query_dist_to_uint(query_dist)
        || hdr->load_num     != static_cast<uint32_t>(load_num)
        || hdr->warmup_num   != static_cast<uint32_t>(warmup_num)
        || hdr->phase_op_num != static_cast<uint32_t>(phase_op_num)
        || hdr->num_repeats  != static_cast<uint32_t>(num_repeats)) {
      fprintf(stderr, "[query-keys] parameter mismatch in: %s\n",
              path.c_str());
      close(); return false;
    }

    size_t offset = sizeof(QueryKeysHeader);
    for (int i = 0; i < NUM_OPS; ++i) {
      if (!parse_sub(base, offset, i)) {
        fprintf(stderr, "[query-keys] truncated sub-array %d in: %s\n",
                i, path.c_str());
        close(); return false;
      }
    }

    if (mlock(mapped_, file_size_) != 0) {
      fprintf(stderr,
              "[query-keys] mlock failed (%.1f MB): %s (non-fatal)\n",
              file_size_ / (1024.0 * 1024.0), strerror(errno));
    }
    fprintf(stderr,
            "[query-keys] mapped %s: update=%zu read=%zu scan=%zu del=%zu "
            "rmw=%zu, file=%.1f MB\n",
            path.c_str(), subs_[OP_UPDATE].size(), subs_[OP_READ].size(),
            subs_[OP_SCAN].size(), subs_[OP_DELETE].size(),
            subs_[OP_RMW].size(), file_size_ / (1024.0 * 1024.0));
    return true;
  }

  void close() {
    if (mapped_) { munmap(mapped_, file_size_); mapped_ = nullptr; }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    for (int i = 0; i < NUM_OPS; ++i) subs_[i] = MmapSubArray();
  }

  bool is_open() const { return mapped_ != nullptr; }

  const MmapSubArray& sub(Op op) const { return subs_[op]; }

  // Convenience accessors per op type.
  const MmapSubArray& update_trace() const { return subs_[OP_UPDATE]; }
  const MmapSubArray& read_trace()   const { return subs_[OP_READ]; }
  const MmapSubArray& scan_trace()   const { return subs_[OP_SCAN]; }
  const MmapSubArray& delete_trace() const { return subs_[OP_DELETE]; }
  const MmapSubArray& rmw_trace()    const { return subs_[OP_RMW]; }
};

// Persist a freshly generated set of per-op key vectors so the next bench
// run can mmap them. Returns true on success.
inline bool save_query_keys(const std::string& path,
                            const std::string& query_dist,
                            int load_num, int warmup_num,
                            int phase_op_num, int num_repeats,
                            const key_vector& update_keys,
                            const key_vector& read_keys,
                            const key_vector& scan_keys,
                            const key_vector& delete_keys,
                            const key_vector& rmw_keys) {
  FILE* fp = fopen(path.c_str(), "wb");
  if (!fp) {
    fprintf(stderr, "[query-keys] failed to open %s for writing: %s\n",
            path.c_str(), strerror(errno));
    return false;
  }

  QueryKeysHeader hdr;
  std::memset(&hdr, 0, sizeof(hdr));
  std::memcpy(hdr.magic, kQueryKeysMagic, 4);
  hdr.dist_type    = query_dist_to_uint(query_dist);
  hdr.load_num     = static_cast<uint32_t>(load_num);
  hdr.warmup_num   = static_cast<uint32_t>(warmup_num);
  hdr.phase_op_num = static_cast<uint32_t>(phase_op_num);
  hdr.num_repeats  = static_cast<uint32_t>(num_repeats);

  if (fwrite(&hdr, sizeof(hdr), 1, fp) != 1) { fclose(fp); return false; }

  auto write_one = [&](const key_vector& vec) -> bool {
    uint64_t num_keys = vec.size();
    uint32_t key_len  = 0;
    uint32_t padding  = 0;
    for (const auto& k : vec) {
      if (k.size() > key_len) key_len = static_cast<uint32_t>(k.size());
    }
    if (fwrite(&num_keys, sizeof(num_keys), 1, fp) != 1) return false;
    if (fwrite(&key_len,  sizeof(key_len),  1, fp) != 1) return false;
    if (fwrite(&padding,  sizeof(padding),  1, fp) != 1) return false;
    if (num_keys == 0 || key_len == 0) return true;

    constexpr size_t kChunk = 65536;
    std::vector<char> buf(kChunk * key_len, 0);
    for (size_t i = 0; i < num_keys; ) {
      size_t batch = std::min(kChunk, static_cast<size_t>(num_keys) - i);
      std::memset(buf.data(), 0, batch * key_len);
      for (size_t j = 0; j < batch; ++j) {
        std::memcpy(buf.data() + j * key_len,
                    vec[i + j].data(), vec[i + j].size());
      }
      if (fwrite(buf.data(), key_len, batch, fp) != batch) return false;
      i += batch;
    }
    return true;
  };

  bool ok = write_one(update_keys)
         && write_one(read_keys)
         && write_one(scan_keys)
         && write_one(delete_keys)
         && write_one(rmw_keys);
  fclose(fp);
  if (!ok) {
    fprintf(stderr, "[query-keys] error writing %s\n", path.c_str());
  } else {
    fprintf(stderr, "[query-keys] saved %s\n", path.c_str());
  }
  return ok;
}
