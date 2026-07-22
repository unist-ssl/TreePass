#!/usr/bin/env python3
"""Generate a small load CSV for the smoke benchmark.

Output format: one fixed-width key per line, padded with the line number
in decimal so every key is unique. Each key is 24 bytes (the key size
the smoke run in run_smoke.py declares).

Default size (--num) is 1 000 000 keys (~25 MB CSV, ~100 MB working
set after load). At that scale, a 64 MB cache produces meaningful
miss pressure so backend comparisons are not dominated by full-cache
hits.
"""
import argparse
import os

KEY_SIZE_BYTES = 24


def write_load_csv(out_path: str, num: int) -> None:
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w") as f:
        for i in range(num):
            key = f"smoke_key_{i:014d}"
            assert len(key) == KEY_SIZE_BYTES, (len(key), key)
            f.write(key)
            f.write("\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--out",
        default=os.path.join(os.path.dirname(__file__), "smoke_load.csv"),
        help="Output CSV path (default: examples/smoke_load.csv)",
    )
    parser.add_argument(
        "--num",
        type=int,
        default=1_000_000,
        help="Number of keys to generate (default: 1000000)",
    )
    args = parser.parse_args()
    write_load_csv(args.out, args.num)
    size_mb = os.path.getsize(args.out) / (1 << 20)
    print(f"wrote {args.num} keys to {args.out} ({size_mb:.2f} MB)")


if __name__ == "__main__":
    main()
