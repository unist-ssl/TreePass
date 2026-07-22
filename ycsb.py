#!/usr/bin/env python3
import os
import sys
import subprocess
import shutil
import multiprocessing
import time
import datetime
from contextlib import contextmanager

# -------------------------------
# db_type → backend directory mapping
# TREEPASS is vendored in-tree under splinterdb/. The bench CMake selects the
# backend via -Dbackend=<NAME>.
# -------------------------------
BACKEND_DIR_MAP = {
    "TREEPASS": "splinterdb",                       # in-tree
}

# -------------------------------
# External Configuration
# -------------------------------
EXTERNAL_CONFIG = {
    # Default reproduction config (TreePass-only).
    # ------------------------------------------------------------------
    # Query distribution and cache capacity are the outermost loops
    'query_dists': [
        "prefix",
        "zipf",
        # "unif",
    ],
    # Cache capacity in MB.
    'cache_capacity_in_MBs': [
        4096,              # default
    ],

    'thread_nums': ["100"],              # default
    # Backends to sweep. Only TREEPASS is shipped.
    'db_types': [
        "TREEPASS",
    ],
    # Datasets to run: each entry is a key in the DATASETS dict below.
    'datasets' : [
        "673M",
        # "84M",   # add after 673M sanity completes
    ],

    # Base directory for the SplinterDB data file. Point this at a large, fast
    # (NVMe) disk: the DB is sized by db_size_in_GB (~160 GB for the 673M
    # dataset), so do NOT use /tmp (often tmpfs/RAM or a small root partition).
    # A DATASETS entry may override this per-dataset via its own "db_base_dir".
    'db_base_dir': '/mnt/nvme',

    # Direct I/O setting
    'use_direct_io': True,

    # NUMA interleave: distribute memory pages across all NUMA nodes
    'use_numa_interleave': True,

    # CPU governor: set to 'performance' during benchmarks to reduce run-to-run variation
    'use_performance_governor': True,

    # use_stats: enable SplinterDB internal statistics collection
    'use_stats': True,
}

# -------------------------------
# Internal Benchmark Configuration
# -------------------------------
INTERNAL_CONFIG = {
    'workloads': [
        "load",
        "A",
        # "B",
        "C",
        # "D",
        # "E",
        # "F",
    ],

}

# --------------------------------------
# Workload definitions (YCSB-like mixes)
# --------------------------------------
# Operation order in workload_frac_list:
#   insert, update, read, scan, delete, read-modify-write
WORKLOAD_PROFILES = {
    # Pure load phase: only inserts
    "load": {
        "workload_frac_list": "0,0,0,0,0,0",
        "num_repeats": 1,
        "consider_latest_inserts": False,
    },

    # A: 50% read, 50% update
    "A": {
        # insert, update, read, scan, delete, rmw
        "workload_frac_list": "0,0.5,0.5,0,0,0",
        "num_repeats": 1,
        "consider_latest_inserts": False,
    },

    # B: 95% read, 5% update
    "B": {
        "workload_frac_list": "0,0.05,0.95,0,0,0",
        "num_repeats": 1,
        "consider_latest_inserts": False,
    },

    # C: 100% read
    "C": {
        "workload_frac_list": "0,0,1,0,0,0",
        "num_repeats": 1,
        "consider_latest_inserts": False,
    },

    # D: 95% read, 5% insert (latest)
    "D": {
        "workload_frac_list": "0.05,0,0.95,0,0,0",
        # Multi-phase so that queries can consider keys inserted in previous phases
        "num_repeats": 5,
        "consider_latest_inserts": True,
    },

    # E: 95% scan, 5% insert (NOT latest)
    "E": {
        "workload_frac_list": "0.05,0,0,0.95,0,0",
        "num_repeats": 5,
        "consider_latest_inserts": False,
    },

    # F: 50% read, 50% read-modify-write
    "F": {
        "workload_frac_list": "0,0,0.5,0,0,0.5",
        "num_repeats": 1,
        "consider_latest_inserts": False,
    },
}


# -------------------------------
# Dataset-specific tuning profiles
# -------------------------------
# Map dataset nickname to its CSV path and tuning params
# (key_size, value_size, load_num, warmup_num, phase_op_num, db_size_in_GB).
#
# NOTE on insert-bearing workloads (YCSB-D/E): those workloads insert fresh
# keys drawn sequentially from the dataset *beyond* the loaded prefix, so the
# dataset CSV must contain more than `load_num` keys -- at least
# load_num + (insert fraction x total ops) unique keys. If the CSV runs short,
# ycsb_bench aborts with a "ran out of keys" error. Read-only / update-only
# workloads (A, B, C, F) need only `load_num` keys.
#
# NOTE: the large datasets are NOT shipped. The two entries below are the
# paper's synthetic datasets, included only as templates. To run on your own
# data, add a DATASETS entry keyed by a short nickname (the nickname is used in
# log file names), with the CSV path and the tuning params. You can generate a
# compatible synthetic CSV at any size with
#   examples/gen_smoke_load.py --num N --out <path>
# (24-byte keys, matching key_size=24).
DATASETS = {
    "673M": {
        "path": "/path/to/dry_run_673M.csv",
        "key_size": 24,
        "value_size": 100,
        "load_num": 673_000_000,
        "warmup_num": 80_000_000,
        "phase_op_num": 160_000_000,
        "db_size_in_GB": 160,
    },
    "84M": {
        "path": "/path/to/dry_run_84M.csv",
        "key_size": 24,
        "value_size": 1024,
        "load_num": 84_000_000,
        "warmup_num": 20_000_000,
        "phase_op_num": 40_000_000,
        "db_size_in_GB": 200,
    },
}


ROOT_DIR = os.path.dirname(os.path.abspath(__file__))


@contextmanager
def in_dir(path: str):
    """Context manager to temporarily change directory."""
    old = os.getcwd()
    os.chdir(path)
    try:
        yield
    finally:
        os.chdir(old)


def run_command(cmd, cwd=None, env=None):
    print(" ".join(cmd))
    result = subprocess.run(cmd, cwd=cwd, env=env)
    if result.returncode != 0:
        print(f"Command failed: {' '.join(cmd)}")
        sys.exit(result.returncode)


def get_cache_str(cache_capacity_in_MB):
    """Return human-readable cache size string."""
    if cache_capacity_in_MB >= 1024:
        return f"{cache_capacity_in_MB / 1024:.1f}GB"
    else:
        return f"{cache_capacity_in_MB}MB"


def set_env():
    """Prepare environment for the SplinterDB build."""
    os.environ["COMPILER"] = "gcc"
    os.environ["CC"] = "gcc"
    os.environ["LD"] = "gcc"
    os.environ["CXX"] = "g++"
    os.environ["CFLAGS"] = ""
    os.environ["CXXFLAGS"] = ""

    env = os.environ.copy()
    return env

def build_log_header(key_size, value_size, thread_num, bench, db_type,
                     dataset, use_direct_io, use_numa_interleave,
                     use_performance_governor, db_base_dir, warmup_num, phase_op_num,
                     query_dist, cache_capacity_in_MB, db_size_in_GB,
                     workload_frac_list):
    """Build (and echo) the benchmark log header lines."""
    header_lines = [
        "# External Benchmark Configuration\n",
        f"# Key Sizes: {key_size}\n",
        f"# Value Sizes: {value_size}\n",
        f"# Thread Numbers: {thread_num}\n",
        f"# Benches: {bench}\n",
        f"# DB Types: {db_type}\n",
        f"# Datasets: {dataset}\n",
    ]
    if db_base_dir is not None:
        header_lines.append(f"# DB Base Dir: {db_base_dir}\n")
    header_lines += [
        f"# Direct I/O: {use_direct_io}\n",
        f"# NUMA Interleave: {use_numa_interleave}\n",
        f"# Performance Governor: {use_performance_governor}\n",
        "\n",
        "# Internal Benchmark Configuration\n",
        f"# Warmup Num: {warmup_num}\n",
        f"# Phase Op Num: {phase_op_num}\n",
        f"# Query Distribution: {query_dist}\n",
    ]
    header_lines += [
        f"# Cache Capacity (MB): {cache_capacity_in_MB}\n",
        f"# DB Size (GB): {db_size_in_GB}\n",
        f"# Workload Fraction List: {workload_frac_list}\n",
        "\n",
    ]
    for line in header_lines:
        print(line.strip())
    return header_lines

def build_all(bench, db_type, env):
    """Build backend (per db_type) and the bench binaries. Return build artifact info.

    Layout in this repo:
      splinterdb/                          ← TreePass (in-tree, vendored)
    The backend exports the SplinterDB public API; the bench CMake picks it
    via -Dbackend=<NAME>.
    """
    build_asan, build_ubsan, build_mode, verbose = 0, 0, "release", 0

    cpu_core = multiprocessing.cpu_count()
    make_core = max(cpu_core // 2, 1)

    if db_type not in BACKEND_DIR_MAP:
        print(f"[ERROR] Unknown db_type {db_type!r}; expected one of "
              f"{sorted(BACKEND_DIR_MAP.keys())}", file=sys.stderr)
        sys.exit(1)

    backend_rel = BACKEND_DIR_MAP[db_type]
    backend_dir = os.path.join(ROOT_DIR, backend_rel)
    if not os.path.isdir(backend_dir):
        print(f"[ERROR] Backend dir not found for {db_type}: {backend_dir}",
              file=sys.stderr)
        sys.exit(1)

    # Each backend builds into its own BUILD_ROOT so concurrent backend builds
    # don't fight over a single build/release/ dir inside the backend tree.
    backend_build_root = f"/tmp/splinterdb-{db_type.lower()}-build"
    with in_dir(backend_dir):
        if os.path.isdir(backend_build_root):
            shutil.rmtree(backend_build_root, ignore_errors=True)
        build_env = env.copy()
        build_env["BUILD_ROOT"] = backend_build_root
        run_command([
            "make", f"-j{make_core}",
            f"BUILD_MODE={build_mode}",
            f"BUILD_ASAN={build_asan}",
            f"BUILD_UBSAN={build_ubsan}",
            f"BUILD_VERBOSE={verbose}",
            "LD=gcc",
            "libs",
        ], env=build_env)

    # Build bench binaries (separate build dir per db_type so concurrent backends
    # have independent caches).
    baseline_build_path = os.path.join(ROOT_DIR, "build", db_type.lower())
    os.makedirs(baseline_build_path, exist_ok=True)

    cmake_env = env.copy()
    cmake_env["BUILD_ROOT"] = backend_build_root

    bench_backend = db_type
    with in_dir(baseline_build_path):
        run_command([
            "cmake", os.path.join(ROOT_DIR),
            "-B", baseline_build_path,
            f"-Dbackend={bench_backend}",
            f"-DBUILD_MODE={build_mode}",
            f"-DBUILD_ASAN={build_asan}",
            f"-DBUILD_UBSAN={build_ubsan}",
            "-DBENCH_NAME=ycsb",
        ], env=cmake_env)
        run_command(["make", "-C", baseline_build_path, f"-j{make_core}"], env=cmake_env)
        bench_dir = os.path.join(baseline_build_path, "bench")

    bench_executable = os.path.join(bench_dir, f"{bench}_bench")
    if not (os.path.isfile(bench_executable) and os.access(bench_executable, os.X_OK)):
        print(f"[ERROR] bench executable not found or not executable: {bench_executable}",
              file=sys.stderr)
        sys.exit(1)

    return {"bench_dir": bench_dir}


def setup_db_path(dataset, db_type, db_base_dir):
    dataset_name = os.path.splitext(os.path.basename(dataset))[0]
    base_db = os.path.join(db_base_dir, dataset_name)
    os.makedirs(base_db, exist_ok=True)
    return os.path.join(base_db, f"{db_type}")

def _get_mount_point(path):
    """Get the mount point for the given path."""
    path = os.path.realpath(path)
    while not os.path.ismount(path):
        path = os.path.dirname(path)
    return path


def remove_stale_db(db_path, load):
    """Prepare DB directory for load or reuse."""
    if load:
        if os.path.islink(db_path):
            print(f"[SCRIPT] REMOVE PREVIOUS SYMLINK: {db_path}")
            os.unlink(db_path)
        elif os.path.isdir(db_path):
            print(f"[SCRIPT] REMOVE PREVIOUS DIRECTORY: {db_path}")
            shutil.rmtree(db_path)
        os.makedirs(db_path, exist_ok=True)
        return db_path
    else:
        if not os.path.isdir(db_path):
            print(f"[ERROR] DB directory not found: {db_path}", file=sys.stderr)
            print(f"[ERROR] Run load phase first before running workload phases.", file=sys.stderr)
            sys.exit(1)
        # reuse DB and make a copy for this run
        copy_db_path = f"{db_path}_copy"
        if os.path.isdir(copy_db_path):
            print(f"[SCRIPT] REMOVE PREVIOUS COPY DIRECTORY: {copy_db_path}")
            shutil.rmtree(copy_db_path)
        # TRIM freed blocks so SSD can reclaim them without GC during reads
        mount_point = _get_mount_point(db_path)
        if mount_point:
            print(f"[SCRIPT] Running fstrim on {mount_point}...")
            ret = subprocess.run(["sudo", "fstrim", mount_point], check=False)
            if ret.returncode != 0:
                print(f"[WARN] fstrim {mount_point} failed "
                      f"(returncode={ret.returncode}); "
                      "freed blocks may not be reclaimed before the DB copy, "
                      "which can bias read timings.")
        shutil.copytree(db_path, copy_db_path)
        os.sync()
        print(f"[SCRIPT] DB copy done.")
        return copy_db_path


def setup_log_path(db_type, workload_name, query_dist, cache_capacity_in_MB, thread_num,
                   date_str, dataset_tag, use_stats=True):
    """Return (log_base_dir, log_filename)."""
    log_base_dir = os.path.join(ROOT_DIR, "result")
    os.makedirs(log_base_dir, exist_ok=True)

    # Normalize workload name for logging
    workload_tag = f"YCSB_{workload_name}"

    # Base name: include workload name and query distribution (except for load)
    if workload_name == "load":
        # Load phase doesn't use query distribution
        log_name = (
            f"{db_type}_{workload_tag}_{dataset_tag}"
            f"_cache_{get_cache_str(cache_capacity_in_MB)}"
            f"_thread_{thread_num}"
        )
    else:
        log_name = (
            f"{db_type}_{workload_tag}_{dataset_tag}_{query_dist}"
            f"_cache_{get_cache_str(cache_capacity_in_MB)}"
            f"_thread_{thread_num}"
        )

    if not use_stats:
        log_name += "_nostats"

    log_name += f"_{date_str}.txt"

    return log_base_dir, log_name


def set_cpu_governor(governor):
    """Set CPU frequency governor for all cores. Returns the previous governor."""
    try:
        with open("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor") as f:
            prev_governor = f.read().strip()
    except FileNotFoundError:
        print("[WARN] CPU frequency scaling not available, skipping governor change")
        return None

    if prev_governor == governor:
        print(f"[INFO] CPU governor already set to '{governor}'")
        return prev_governor

    print(f"[INFO] Setting CPU governor: {prev_governor} -> {governor}")
    ret = subprocess.run(
        ["sudo", "cpupower", "frequency-set", "-g", governor],
        check=False,
        stdout=subprocess.DEVNULL,
    )
    if ret.returncode != 0:
        print(f"[WARN] cpupower failed to set governor '{governor}' "
              f"(returncode={ret.returncode}); CPU frequency scaling remains "
              f"'{prev_governor}', throughput comparisons may be biased.")
    return prev_governor


def drop_cache(db_base_dir):
    """Drop OS page cache and trim filesystem."""
    ret = subprocess.run(
        ["sudo", "sh", "-c", "echo 3 > /proc/sys/vm/drop_caches"], check=False)
    if ret.returncode != 0:
        print(f"[WARN] drop_caches failed (returncode={ret.returncode}); "
              "OS page cache not cleared, next-phase reads may hit warm cache "
              "and bias the results.")
    ret = subprocess.run(["sudo", "fstrim", "-v", db_base_dir], check=False)
    if ret.returncode != 0:
        print(f"[WARN] fstrim {db_base_dir} failed (need sudo privileges)")


def setup_cmd(db_type, bench_dir, bench, db_path, query_dist, dataset,
              key_size, value_size, load_num, warmup_num, phase_op_num,
              load, cache_capacity_in_MB, db_size_in_GB, thread_num, use_stats, workload_frac_list,
              use_direct_io, query_trace_dir=""):
    """Build command line for benchmark process."""
    cmd = [
        os.path.join(bench_dir, f"{bench}_bench"),
        f"--db_type={db_type}",
        f"--db_path={db_path}",
        f"--num={load_num}",
        f"--query_dist={query_dist}",
        f"--trace_name={dataset}",
        f"--key_size={key_size}",
        f"--value_size={value_size}",
        f"--warmup_num={warmup_num}",
        f"--phase_op_num={phase_op_num}",
    ]


    cmd.extend([
        f"--load_frac={1 if load else 0}",
        f"--block_cache_capacity={cache_capacity_in_MB}",
        f"--db_size_in_GB={db_size_in_GB}",
        f"--thread_num={thread_num}",
        f"--use_existing_db={'false' if load else 'true'}",
        f"--use_stats={str(use_stats).lower()}",
        f"--workload_frac_list={workload_frac_list}",
        f"--use_direct_io={'true' if use_direct_io else 'false'}",
    ])

    # Reuse the staged query-keys cache if the caller pointed us at one.
    # ycsb_bench writes/reads {workload}_{dist}[_kr<N>]_{load}M_{warmup}M_{phase}M_r{r}.bin
    # in this directory; an empty path disables both load and save.
    if query_trace_dir:
        cmd.append(f"--query_trace_dir={query_trace_dir}")

    print("Executing benchmark command:")
    print(" ".join(cmd))

    return cmd


def start_splinterdb(cmd, result_path, header_lines):
    """Run benchmark and write logs."""
    with open(result_path, "w") as outfile:
        outfile.writelines(header_lines)
        process = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True)
        for line in process.stdout:
            sys.stdout.write(line)
            outfile.write(line)
        process.wait()
        if process.returncode != 0:
            print(
                f"[ERROR] Command failed with return code {process.returncode}",
                file=sys.stderr,
            )
            sys.exit(process.returncode)
    print(f"[INFO] Result log written to: {result_path}")


def run_internal_config_loop(key_size,
                             value_size,
                             load_num,
                             warmup_num,
                             phase_op_num,
                             db_size_in_GB,
                             thread_num,
                             bench,
                             db_type,
                             dataset,
                             query_dist,
                             cache_capacity_in_MB,
                             use_direct_io,
                             use_numa_interleave=False,
                             query_trace_dir="",
                             use_performance_governor=False,
                             use_stats=True,
                             bench_dir=None,
                             workload_name=None,
                             db_base_dir=None,
                             dataset_tag=None):
    """Iterate over internal configs and run benchmarks for a fixed dataset profile."""

    if workload_name not in WORKLOAD_PROFILES:
        raise ValueError(f"Unknown workload: {workload_name}")

    profile = WORKLOAD_PROFILES[workload_name]
    workload_frac_list = profile["workload_frac_list"]
    num_repeats = profile.get("num_repeats", 1)
    consider_latest_inserts = profile.get("consider_latest_inserts", False)

    load = (workload_name == "load")

    header = build_log_header(
        key_size,
        value_size,
        thread_num,
        bench,
        db_type,
        dataset,
        use_direct_io,
        use_numa_interleave,
        use_performance_governor,
        db_base_dir,
        warmup_num,
        phase_op_num,
        query_dist,
        cache_capacity_in_MB,
        db_size_in_GB,
        workload_frac_list,
    )
    # Explicitly log workload / phase structure
    header.append(f"# Workload Name: {workload_name}\n")
    header.append(f"# Num Repeats: {num_repeats}\n")
    header.append(
        f"# Consider Latest Inserts: {consider_latest_inserts}\n"
    )
    header.append(f"# Use Stats: {use_stats}\n")

    date_str = datetime.datetime.now().strftime("%Y%m%d_%H%M")

    db_path = setup_db_path(
        dataset,
        db_type,
        db_base_dir,
    )

    db_path = remove_stale_db(db_path, load)

    log_base_dir, log_name = setup_log_path(
        db_type,
        workload_name,
        query_dist,
        cache_capacity_in_MB,
        thread_num,
        date_str,
        dataset_tag,
        use_stats,
    )
    print(f"[INFO] Log file will be saved to: {os.path.join(log_base_dir, log_name)}")

    cmd = setup_cmd(
        db_type,
        bench_dir,
        bench,
        db_path,
        query_dist,
        dataset,
        key_size,
        value_size,
        load_num,
        warmup_num,
        phase_op_num,
        load,
        cache_capacity_in_MB,
        db_size_in_GB,
        thread_num,
        use_stats,
        workload_frac_list,
        use_direct_io,
        query_trace_dir,
    )

    # Pass workload semantics to the benchmark binary.
    cmd.append(f"--workload_name={workload_name}")
    # YCSB-D / YCSB-E split the measured phase into num_repeats
    # sub-phases (default 5). D additionally reads keys from the
    # "latest" distribution.
    if workload_name in {"D", "E"}:
        cmd.extend([
            f"--num_repeats={num_repeats}",
            f"--consider_latest_inserts="
            f"{'true' if consider_latest_inserts else 'false'}",
        ])

    # Optionally wrap with numactl --interleave=all
    if use_numa_interleave:
        cmd = ["numactl", "--interleave=all"] + cmd

    drop_cache(db_base_dir)

    start_splinterdb(
        cmd, os.path.join(log_base_dir, log_name), header,
    )

    time.sleep(2)

    if not load:
        # Clean up only per-run DB (not the loaded one)
        if os.path.isdir(db_path):
            print(
                f"[SCRIPT] CLEAN UP CURRENT DIRECTORY: {db_path}"
            )
            shutil.rmtree(db_path)


def main():
    ext_config = EXTERNAL_CONFIG
    os.chdir(ROOT_DIR)

    # Set CPU governor to 'performance' for stable benchmarking
    prev_governor = None
    if ext_config.get('use_performance_governor', True):
        prev_governor = set_cpu_governor("performance")

    try:
        _run_all_benchmarks(ext_config)
    finally:
        # Restore original CPU governor
        if prev_governor is not None and prev_governor != "performance":
            set_cpu_governor(prev_governor)


def _run_all_benchmarks(ext_config):
    build_cache = {}
    loaded = set()
    for query_dist in ext_config['query_dists']:

        for cache_capacity_in_MB in ext_config['cache_capacity_in_MBs']:
            for thread_num in ext_config['thread_nums']:
                bench = "ycsb"
                # Build any (db_type, node-config) combos we
                # haven't compiled yet. build_cache is scoped
                # to _run_all_benchmarks so subsequent
                # query_dists reuse the same binaries.
                for db_type in ext_config['db_types']:
                    build_key = (db_type,)
                    if build_key in build_cache:
                        continue
                    env_build = set_env()
                    build_cache[build_key] = build_all(bench, db_type, env_build)

                for workload_name in INTERNAL_CONFIG['workloads']:
                    for db_type in ext_config['db_types']:
                        for dataset_key in ext_config['datasets']:
                            if workload_name == "load":
                                load_key = (db_type, dataset_key)
                                if load_key in loaded:
                                    print(f"[SCRIPT] load already done for "
                                          f"{db_type}/{dataset_key}; skipping "
                                          f"for query_dist={query_dist}")
                                    continue
                                loaded.add(load_key)
                                print(f"[SCRIPT] load for {db_type}/{dataset_key} "
                                      f"— runs once, shared across query_dists")
                            d = DATASETS[dataset_key]
                            dataset = d["path"]
                            dataset_tag = dataset_key
                            dataset_db_base_dir = d.get(
                                "db_base_dir",
                                ext_config.get("db_base_dir", "/mnt/nvme"),
                            )
                            key_size = d["key_size"]
                            value_size = d["value_size"]
                            load_num = d["load_num"]
                            warmup_num = d["warmup_num"]
                            phase_op_num = d["phase_op_num"]
                            db_size_in_GB = d["db_size_in_GB"]

                            resolved_cache_capacity = cache_capacity_in_MB

                            build_key = (db_type,)
                            build_artifact = build_cache[build_key]
                            cached_bench_dir = build_artifact["bench_dir"]

                            use_stats = ext_config.get('use_stats', True)
                            use_direct_io = ext_config.get('use_direct_io', True)
                            use_numa_interleave = ext_config.get('use_numa_interleave', True)
                            use_performance_governor = ext_config.get('use_performance_governor', True)
                            # Stage the generated query keys in the dataset's
                            # directory so later runs reuse them (see
                            # --query_trace_dir in config.h).
                            query_trace_dir = os.path.dirname(dataset)
                            run_internal_config_loop(
                                key_size,
                                value_size,
                                load_num,
                                warmup_num,
                                phase_op_num,
                                db_size_in_GB,
                                thread_num,
                                bench,
                                db_type,
                                dataset,
                                query_dist,
                                resolved_cache_capacity,
                                use_direct_io,
                                use_numa_interleave,
                                query_trace_dir,
                                use_performance_governor,
                                use_stats,
                                bench_dir=cached_bench_dir,
                                workload_name=workload_name,
                                db_base_dir=dataset_db_base_dir,
                                dataset_tag=dataset_tag,
                            )


if __name__ == '__main__':
    main()
