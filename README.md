<h1>
  <img align="right" width="80" src=".github/badges/usenixbadges-functional.png" alt="USENIX Artifact Evaluated: Functional"/><img align="right" width="80" src=".github/badges/usenixbadges-available.png" alt="USENIX Artifact Evaluated: Available"/>
  TreePass: Lightweight Tree-Guided Cache Management for B<sup>+</sup>-tree Key-Value Stores
</h1>

TreePass is a lightweight caching system for B<sup>+</sup>-tree key-value stores,
built on SplinterDB. It augments CLOCK eviction with per-pivot counters embedded
in the B<sup>+</sup>-tree — address-independent signals of subtree hotness and
impending page invalidation — to perform early eviction of cold-subtree pages and
differentiated reference-counter initialization for newly created pages,
suppressing unnecessary writebacks without additional metadata.

For more details about TreePass, please refer to our FAST '27 paper.

## Layout

```
.
├── splinterdb/       TreePass-extended SplinterDB fork
├── bench/            YCSB-style bench (ycsb_bench)
├── examples/         Self-contained smoke test (run_smoke.py — see examples/README.md)
├── ycsb.py           Full YCSB experiment driver (paper-scale sweeps)
├── CMakeLists.txt    Top-level CMake entry
└── README.md
```

`bench/` is TreePass's own bench rather than stock YCSB-C, because the
evaluation needs a `prefix` query distribution, staged / reused query keys,
and a warmup / measured phase split, among others.

TreePass lives in `splinterdb/src/treepass.{c,h}` (derived from upstream
`clockcache.{c,h}`). `btree.c`, `trunk.c`, `core.c`, `routing_filter.c`,
`splinterdb.c`, `cache.h`, and `include/splinterdb/splinterdb.h` are
modified to embed Tree Counters in pivot entries and route them through
eviction. Everything else is stock SplinterDB.

## Prerequisites

- Ubuntu 22.04, `build-essential` (gcc/g++/make), cmake 3.17+, python3 ≥ 3.7
- `libaio-dev libxxhash-dev libgflags-dev`
- `numactl` and `cpupower` (both only required for `ycsb.py`: it wraps
  the bench with `numactl --interleave=all` and calls `cpupower
  frequency-set` to pin the CPU governor to `performance`). `cpupower`
  ships with the `linux-tools-*` packages.

```bash
sudo apt-get install -y build-essential cmake git python3 \
    libaio-dev libxxhash-dev libgflags-dev numactl \
    linux-tools-common "linux-tools-$(uname -r)"
```

Tested on Ubuntu 22.04.2 LTS (kernel 5.15, glibc 2.35). Older glibc
(e.g. Ubuntu 18.04, glibc 2.27) lacks the `gettid()` wrapper and is not
expected to build.

## Quick start

From a clone of this repository, run the self-contained smoke test — it
builds everything and runs the YCSB matrix on a synthetic dataset, with no
external data or root:

```bash
python3 examples/run_smoke.py
```

See [`examples/README.md`](examples/README.md) for options, expected
output, and cache-sizing tips.

## Full YCSB experiments

The paper-scale sweeps are driven by `ycsb.py`. Unlike the smoke it is
**not** self-contained: supply your own dataset CSV(s) and set the dataset
paths, DB size, and warmup / measured op counts in the configuration block
near the top of `ycsb.py`, then run `python3 ycsb.py`.

`ycsb.py` wraps the bench with `numactl --interleave=all` by default (via
`use_numa_interleave`) and invokes several `sudo` commands between phases —
`drop_caches`, `fstrim`, and `cpupower frequency-set` — to give each phase a
clean start. We ran it as root; without root, each failed `sudo` prints a
`[WARN]` line and the run continues, but the results may be biased.

Peak disk footprint is roughly **2× the DB size**: before each query
configuration `ycsb.py` copies the loaded DB (via `shutil.copytree`) so
consecutive workloads start from the same warm-DB state. Ensure the DB
volume has at least `2 × db_size_in_GB` free.

> **Query-key reuse.** The per-workload query keys are generated on the first
> run of a given configuration — for the `prefix` distribution this generation
> can dominate that run's time. They are then written next to the dataset and
> reused by later runs of the same experiment, so repeats are much faster.

Paper measurements were taken on 2× Intel Xeon Gold 6526Y (32 cores /
64 threads), 314 GB DDR5 RAM, and a Samsung PM9A3 1.92 TB NVMe SSD (PCIe 4.0 ×4),
Ubuntu 22.04.2 LTS.

## Datasets

A dataset is just the set of keys to load — one fixed-width key per line
(24 bytes; the YCSB `user` + zero-padded key format, e.g.
`user12161962213042174405`). The CSV holds only load-time keys; the query
streams under the uniform, Zipfian, and prefix distributions are generated
by the bench at run time, so one CSV serves every workload / distribution
combination.

The paper's `dry_run_673M` / `dry_run_84M` datasets were generated with our
YCSB-C fork (https://github.com/sheepjin11/YCSB) — a fork of
[basicthinker/YCSB-C](https://github.com/basicthinker/YCSB-C) with a
SplinterDB binding. Its workload specs match these datasets:

| spec | recordcount | key / value | dataset |
|---|---|---|---|
| `workloads/K24BV100B/load.spec`  | 685,000,000 | 24 B / 100 B  | `dry_run_673M` |
| `workloads/K24BV1024B/load.spec` | 84,750,000  | 24 B / 1024 B | `dry_run_84M`  |

The dataset directory names (`dry_run_673M`, `dry_run_84M`) reflect the
load size — the number of keys actually inserted into the DB and used
by workloads A/B/C/F. Each spec's `recordcount` is set higher so that
YCSB-D and YCSB-E have extra rows (~12M and ~750K, respectively) to
consume as fresh inserts during their run.

In a clone of the YCSB fork, build the `datasets` target — it only
compiles the `basic` backend, so no RocksDB / Redis / SplinterDB / TBB
libraries need to be installed — then dump the load keys and keep the
key column:

```bash
make datasets
./ycsbc -db basic -p basicdb.verbose 1 -L workloads/K24BV100B/load.spec \
    | awk '/^INSERT usertable/ {print $3}' > dry_run_673M.csv
```

Point a `DATASETS` entry's `path` in `ycsb.py` at the resulting CSV. Any set
of unique 24-byte keys works; `examples/gen_smoke_load.py --num N --out <path>`
produces a small synthetic set in the same format for quick tests.

## License

Apache-2.0, inherited from SplinterDB. See `splinterdb/LICENSE`.
