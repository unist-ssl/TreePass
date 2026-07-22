#!/usr/bin/env python3
"""TreePass smoke runner.

Builds the TreePass library and the ycsb_bench binary, runs the
synthetic load once, then exercises the non-insert YCSB workloads
(A/B/C/F) across unif / zipf / prefix.

This is the quick verification flow; the larger paper-reproduction
sweep lives in run_paper.py (TBD).
"""
from __future__ import annotations

import argparse
import datetime
import os
import shutil
import subprocess
import sys
from pathlib import Path

# run_smoke.py lives in examples/, so the repo root is one level up.
ROOT = Path(__file__).resolve().parent.parent
TREEPASS_DIR = ROOT / "splinterdb"
BENCH_BUILD = ROOT / "build" / "bench"
DEFAULT_TREEPASS_LIB_BUILD = ROOT / "build" / "lib"
LOAD_CSV = ROOT / "examples" / "smoke_load.csv"
RESULT_DIR = ROOT / "result"

# Default db_path is the repo root so the smoke runs on a fresh clone with
# no extra setup. Pass --db-path /mnt/nvme (or any NVMe mount) for paper-
# grade timing.
DEFAULT_DB_PATH = str(ROOT)

KEY_SIZE = 24
VALUE_SIZE = 100
LOAD_NUM = 1_000_000

# YCSB workloads. Frac slots: insert, update, read, scan, delete, rmw.
# Smoke covers the non-insert workloads. YCSB-D/E are omitted: they insert
# fresh keys and their canonical "read-latest" / scan semantics require the
# multi-phase (num_repeats > 1) driver in ycsb.py, which this quick smoke
# does not set up.
WORKLOADS = [
    ("A", "0,0.5,0.5,0,0,0"),     # 50 % update + 50 % read
    ("B", "0,0.05,0.95,0,0,0"),   # 5 % update + 95 % read
    ("C", "0,0,1,0,0,0"),         # 100 % read
    ("F", "0,0,0.5,0,0,0.5"),     # 50 % read + 50 % read-modify-write
]

DISTRIBUTIONS = ["unif", "zipf", "prefix"]


def sh(cmd, *, env=None, cwd=None):
    print(f"$ {' '.join(map(str, cmd))}", flush=True)
    subprocess.check_call([str(c) for c in cmd], env=env, cwd=cwd)


def ensure_load_csv() -> None:
    if LOAD_CSV.is_file():
        return
    print(f"[smoke] {LOAD_CSV} missing; generating...")
    sh([sys.executable, ROOT / "examples" / "gen_smoke_load.py"])


def build_treepass(jobs: int, lib_build: Path) -> None:
    env = os.environ.copy()
    env.setdefault("CC", "gcc")
    if lib_build.exists():
        shutil.rmtree(lib_build, ignore_errors=True)
    env["BUILD_ROOT"] = str(lib_build)
    sh(["make", "-j", str(jobs), "libs"], cwd=TREEPASS_DIR, env=env)


def build_bench(jobs: int, lib_build: Path) -> dict:
    BENCH_BUILD.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.setdefault("CC", "gcc")
    env["BUILD_ROOT"] = str(lib_build)
    sh(
        [
            "cmake", str(ROOT), f"-B{BENCH_BUILD}",
            "-Dbackend=TREEPASS",
            "-DBUILD_MODE=release",
            "-DBUILD_ASAN=0",
            "-DBUILD_UBSAN=0",
        ],
        env=env,
    )
    sh(["make", "-C", BENCH_BUILD, "-j", str(jobs)], env=env)

    binaries = {
        "ycsb":  BENCH_BUILD / "bench" / "ycsb_bench",
    }
    for name, p in binaries.items():
        if not p.is_file():
            sys.exit(f"[smoke] {name} binary missing at {p}")
    return binaries


def _base_cmd(*, db_path: str, cache_mb: int, threads: int, phase_ops: int,
              query_dist: str = "unif") -> list:
    return [
        "--db_type=TREEPASS",
        f"--db_path={db_path}",
        f"--num={LOAD_NUM}",
        f"--key_size={KEY_SIZE}",
        f"--value_size={VALUE_SIZE}",
        f"--query_dist={query_dist}",
        f"--trace_name={LOAD_CSV}",
        f"--block_cache_capacity={cache_mb}",
        f"--db_size_in_GB={2}",
        f"--thread_num={threads}",
        f"--phase_op_num={phase_ops}",
        "--use_stats=true",
        "--use_direct_io=true",
    ]


def run_load(binary: Path, *, db_path: str, cache_mb: int, threads: int,
             log_path: Path) -> None:
    cmd = [binary] + _base_cmd(db_path=db_path, cache_mb=cache_mb,
                               threads=threads, phase_ops=0) + [
        "--workload_name=load",
        "--workload_frac_list=0,0,0,0,0,0",
        "--load_frac=1",
        "--use_existing_db=false",
    ]
    log_path.parent.mkdir(parents=True, exist_ok=True)
    print(f"[smoke] -> {log_path.name}")
    with log_path.open("w") as f:
        f.write(f"# cmd: {' '.join(map(str, cmd))}\n")
        f.flush()
        subprocess.check_call([str(c) for c in cmd], stdout=f,
                              stderr=subprocess.STDOUT)


def run_ycsb(binary: Path, *, db_path: str, cache_mb: int, threads: int,
             phase_ops: int, workload_name: str, workload_frac_list: str,
             query_dist: str, log_path: Path) -> None:
    warmup = phase_ops // 2
    cmd = [binary] + _base_cmd(db_path=db_path, cache_mb=cache_mb,
                               threads=threads, phase_ops=phase_ops,
                               query_dist=query_dist) + [
        f"--warmup_num={warmup}",
        f"--workload_name={workload_name}",
        f"--workload_frac_list={workload_frac_list}",
        "--load_frac=0",
        "--use_existing_db=true",
    ]
    log_path.parent.mkdir(parents=True, exist_ok=True)
    print(f"[smoke] -> {log_path.name}")
    with log_path.open("w") as f:
        f.write(f"# cmd: {' '.join(map(str, cmd))}\n")
        f.flush()
        subprocess.check_call([str(c) for c in cmd], stdout=f,
                              stderr=subprocess.STDOUT)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--threads", type=int, default=2,
                   help="Bench thread count (default: 2).")
    p.add_argument("--cache-mb", type=int, default=128,
                   help="Cache size in MB for the query phase (default: 128). "
                        "Give it generous headroom: too small a cache can "
                        "run out of free pages mid-run and abort, especially "
                        "on slow disks where eviction write-back lags.")
    p.add_argument("--load-cache-mb", type=int, default=512,
                   help="Cache size in MB for the load phase (default: 512). "
                        "Raise it if the load aborts from cache exhaustion.")
    p.add_argument("--phase-ops", type=int, default=8_000_000,
                   help="Timed ops per query phase (default: 8M).")
    p.add_argument("--db-path", default=DEFAULT_DB_PATH,
                   help="Directory holding the SplinterDB data file. "
                        "Defaults to the repo root.")
    p.add_argument("--build-jobs", type=int, default=max(1, os.cpu_count() or 4),
                   help="Parallel make jobs (default: nproc).")
    p.add_argument("--build-root", default=str(DEFAULT_TREEPASS_LIB_BUILD),
                   help="Directory for the TreePass library build "
                        "(default: <repo>/build/lib).")
    return p.parse_args()


def main() -> None:
    args = parse_args()
    ensure_load_csv()
    # Create the DB directory if the user pointed --db-path at a fresh
    # location; SplinterDB opens <db_path>/dbfile and will not create the
    # parent directory itself.
    Path(args.db_path).mkdir(parents=True, exist_ok=True)
    lib_build = Path(args.build_root)
    build_treepass(args.build_jobs, lib_build)
    bins = build_bench(args.build_jobs, lib_build)

    ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    out = RESULT_DIR / f"smoke_{ts}"
    out.mkdir(parents=True, exist_ok=True)
    print(f"[smoke] writing logs under {out}")

    run_load(
        bins["ycsb"],
        db_path=args.db_path,
        cache_mb=args.load_cache_mb,
        threads=args.threads,
        log_path=out / "TREEPASS_load.txt",
    )

    for dist in DISTRIBUTIONS:
        for name, fracs in WORKLOADS:
            run_ycsb(
                bins["ycsb"],
                db_path=args.db_path,
                cache_mb=args.cache_mb,
                threads=args.threads,
                phase_ops=args.phase_ops,
                workload_name=name,
                workload_frac_list=fracs,
                query_dist=dist,
                log_path=out / f"TREEPASS_YCSB_{name}_{dist}.txt",
            )

    print(f"[smoke] done — see {out}")


if __name__ == "__main__":
    main()
