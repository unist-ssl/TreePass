# Smoke test

`run_smoke.py` is a self-contained quick check: it builds TreePass + the
bench, generates a synthetic 1 M-key dataset, loads it once, then runs
YCSB-A/B/C/F across the `unif`, `zipf`, and `prefix` distributions
(12 timed configs). It needs no external dataset and no root, and runs on
modest machines (~2 GB free RAM, a few hundred MB disk).

Run from the repository root:

```bash
python3 examples/run_smoke.py
```

Per-config logs land under `result/smoke_<timestamp>/`. Flags (see
`python3 examples/run_smoke.py --help`): `--threads` (default 2),
`--cache-mb` (128), `--load-cache-mb` (512), `--phase-ops` (8M),
`--db-path` (defaults to the repo root), `--build-jobs` (nproc),
`--build-root` (defaults to `<repo>/build/lib`; override to move the
TreePass library build onto a larger volume).

> **Cache sizing.** Give the block cache generous headroom: too small a
> cache can exhaust free pages mid-run and abort — more likely on slow
> disks, where eviction write-back lags. Raise `--cache-mb` /
> `--load-cache-mb`, or lower `--threads`, if you hit it.

Each config is bracketed by a banner; a successful config ends with
`TreePass bench done`:

```
====================== TreePass bench start ======================
 workload=C query_dist=zipf threads=2 cache=128 MB phase_ops=8000000
...
hit ratio  |  100.00% |  98.49% |   ...  |  99.34% |
[TOTAL] workload_frac_list : 0,0,1,0,0,0, total throughput: <ops/s>, total elapsed: <s>
BENCHMARK DONE
====================== TreePass bench done  ======================
```

A query configuration finishes cleanly if its log ends with the
`TreePass bench done` banner and contains a summary line reporting
the total throughput, which appears a few lines before the banner.
Both are present regardless of the settings. (The load log ends with
the same banner but reports its throughput as a `Load throughput:`
line earlier in the log.) The smoke enables `--use_stats`, so each
log also carries the cache-statistics block shown above (the
`hit ratio` row); with stats off those lines are omitted.

## Files

- `run_smoke.py` — the smoke runner.
- `gen_smoke_load.py` — generates the synthetic load CSV (invoked
  automatically by `run_smoke.py`).
