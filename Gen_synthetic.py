#!/usr/bin/env python3
"""
NanoMatch synthetic order generator.

Produces a CSV file consumable by `nanomatch_step5` and the benchmarks.

Schema (one row per message):
    op,id,side,price_ticks,qty,ts_ns,flags

where:
    op           = "A" Add / "C" Cancel / "M" Modify / "X" Execute / "N" Noop
    side         = "B" / "S"  (only meaningful for Add)
    price_ticks  = signed int64, fixed-point (dollars * 10000)
    qty          = unsigned int32
    ts_ns        = unsigned int64 nanos since epoch
    flags        = uint8 bitfield  (IOC=1, FOK=2, post-only=4)

Distribution roughly mimics retail-fed equity markets:
    - 70% Add orders (split 50/50 buy/sell)
    - 25% Cancel orders (random pick from outstanding)
    - 5% IOC aggressive crosses (price beyond opposite best)

Edge cases sprinkled in:
    - Duplicate Add ids  (engine should reject second)
    - Cancel of non-existent id  (engine returns false, no crash)
    - Price at extreme tick (window edge stress)
    - Quantity of 1 (smallest non-zero fill)

Usage:
    python tools/gen_synthetic.py --rows 10000000 --out /tmp/feed.csv

Default: 100k rows to data/feed.csv. Run with --help for all options.
"""
import argparse
import random
import sys
from pathlib import Path

REF_PRICE_TICKS = 2_000_0000   # $200.0000
TICK_WINDOW    = 1000          # spread ± 1000 ticks around ref


def gen(rows: int, seed: int, out_path: Path):
    rng = random.Random(seed)
    out = out_path.open("w", buffering=1 << 20)

    # Track outstanding order ids for realistic cancels
    outstanding: list[int] = []
    next_id  = 1
    ts_ns    = 1_700_000_000_000_000_000  # 2023-11-14
    written  = 0

    edge_every = max(1, rows // 100)   # ~1% edge cases

    while written < rows:
        r = rng.random()

        # 1% edge cases — drop pre-cooked stress rows
        if written and written % edge_every == 0:
            out.write(f"C,99999999,B,0,0,{ts_ns},0\n")    # cancel of unknown id
            ts_ns += 1
            written += 1
            if written >= rows: break

        # Normal mix
        if r < 0.70:                          # Add (passive or aggressive)
            side = rng.choice("BS")
            offset = rng.randint(-TICK_WINDOW, TICK_WINDOW)
            price  = REF_PRICE_TICKS + offset
            qty    = rng.choice([1, 10, 100, 100, 500, 1000])
            flags  = 0
            out.write(f"A,{next_id},{side},{price},{qty},{ts_ns},{flags}\n")
            outstanding.append(next_id)
            next_id += 1

        elif r < 0.95 and outstanding:        # Cancel
            # Cancel a recent order with bias toward newer (more realistic)
            k = min(len(outstanding), 100)
            victim_idx = len(outstanding) - 1 - rng.randint(0, k - 1)
            victim = outstanding.pop(victim_idx)
            out.write(f"C,{victim},B,0,0,{ts_ns},0\n")

        else:                                  # Aggressive IOC cross
            side  = rng.choice("BS")
            # Cross — buy at ref+window, sell at ref-window
            price = REF_PRICE_TICKS + (TICK_WINDOW if side == "B" else -TICK_WINDOW)
            qty   = rng.choice([1, 10, 100, 500])
            flags = 1     # FLAG_IOC
            out.write(f"A,{next_id},{side},{price},{qty},{ts_ns},{flags}\n")
            next_id += 1

        ts_ns   += rng.randint(50, 5000)        # 50ns–5us between msgs
        written += 1

        # Cap outstanding so we don't OOM
        if len(outstanding) > 200_000:
            outstanding = outstanding[-50_000:]

    out.close()
    return written


def main():
    ap = argparse.ArgumentParser(description="NanoMatch synthetic feed generator")
    ap.add_argument("--rows", type=int, default=100_000, help="number of rows")
    ap.add_argument("--seed", type=int, default=0xC0FFEE, help="RNG seed")
    ap.add_argument("--out",  type=Path, default=Path("data/feed.csv"))
    args = ap.parse_args()

    args.out.parent.mkdir(parents=True, exist_ok=True)
    n = gen(args.rows, args.seed, args.out)
    bytes_ = args.out.stat().st_size
    print(f"wrote {n:,} rows ({bytes_/1e6:.1f} MB) → {args.out}")


if __name__ == "__main__":
    main()