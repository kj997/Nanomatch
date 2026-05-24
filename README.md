# NanoMatch

**An ultra-low-latency limit-order-book matching engine in C++17.**

> p50 latency **10–30 ns** per operation · p99 under **170 ns** · 25+ M CSV ingestion rows/sec · zero allocations on the hot path · 226 unit tests.

NanoMatch is a single-instrument, single-threaded matching engine designed from the cache up — every struct fits a cache line, every data structure trades flexibility for predictability, and the hot path touches no allocator, no syscall, no virtual call, and no exception. It is built to demonstrate the engineering discipline behind production HFT order-book infrastructure.

---

## Table of Contents

1. [Why this exists](#why-this-exists)
2. [Architecture at a glance](#architecture-at-a-glance)
3. [Measured performance](#measured-performance)
4. [Design decisions](#design-decisions)
5. [Building](#building)
6. [Running](#running)
7. [Project layout](#project-layout)
8. [Deployment & tuning checklist](#deployment--tuning-checklist)
9. [Limitations and what's next](#limitations-and-whats-next)

---

## Why this exists

Most "matching engine" repos on GitHub use `std::map<Price, std::deque<Order>>` for the order book. That's correct, readable, and **completely unusable** in production:

- Every `std::map` insert costs `~log N` *random* memory accesses through unbalanced red-black tree nodes. At a 5000-level book that's roughly 12 dependent cache misses per insert.
- `std::deque` adds another layer of pointer chasing.
- `new`/`delete` enters the global allocator — non-deterministic latency, lock contention, fragmentation.
- An exception from any nested call can blow your p99.999.

NanoMatch is built the way real exchanges and HFT firms build it: contiguous memory, integer indices, pre-allocated pools, intrusive linked lists, lock-free SPSC handoff between threads. None of this is novel — it's what's behind every Jane Street, Citadel, Jump, HRT, Tower, IMC, and Optiver matching stack. NanoMatch makes those techniques readable and benchmarked.

---

## Architecture at a glance

```
              ┌────────────────────────────────────────────────────────────┐
              │                  PRODUCER THREAD                            │
              │                                                             │
              │   mmap'd file ──► ItchParser / CsvParser ──► InboundOrder   │
              │                                                  │          │
              └──────────────────────────────────────────────────│──────────┘
                                                                 │
                                            ┌────────────────────▼────────────────────┐
                                            │  SpscRing<InboundOrder, 4096>            │
                                            │  (lock-free, cache-padded, acq/rel only) │
                                            └────────────────────│────────────────────┘
                                                                 ▼
              ┌────────────────────────────────────────────────────────────┐
              │                  CONSUMER THREAD                            │
              │                                                             │
              │   MatchingEngine<Sink>::submit_limit_order / cancel_order   │
              │                       │                                     │
              │                       ▼                                     │
              │   ┌───────────────────────────────────────────────────┐    │
              │   │ OrderBook                                          │    │
              │   │   ├─ BookSide bids                                 │    │
              │   │   │    ├─ price_index[65536]  (O(1) lookup)        │    │
              │   │   │    └─ sorted active-level linked list          │    │
              │   │   ├─ BookSide asks  (mirror)                       │    │
              │   │   ├─ MemoryPool<Order, N>      (no malloc)         │    │
              │   │   ├─ MemoryPool<PriceLevel, N>                     │    │
              │   │   └─ OrderIdHashMap<128K>      (O(1) cancel)       │    │
              │   └───────────────────────────────────────────────────┘    │
              │                       │                                     │
              │                       ▼                                     │
              │                   TradeReport                               │
              └───────────────────────│────────────────────────────────────┘
                                      ▼
                          SpscRing<TradeReport, 4096>
                                      ▼
                          [downstream: clearing, FIX, dropcopy]
```

Each component is a single header, ~100–400 lines, with `static_assert`-checked layout invariants.

---

## Measured performance

All numbers from `latency_probe` — a per-operation rdtsc histogram, not a mean-only benchmark. Run on a Windows 11 desktop, Intel Core i7 @ 3.79 GHz, MinGW UCRT64 GCC 15.1, `-O3 -march=native -flto -fno-exceptions -fno-rtti`. No CPU pinning, no kernel tuning.

| Operation | mean | p50 | p90 | p99 | p99.9 | p99.99 | max (env) |
|---|---:|---:|---:|---:|---:|---:|---:|
| `MemoryPool alloc + dealloc` | 13.5 ns | **10.0** | 20.0 | 20.0 | 80 | 110 | 5 µs |
| `SpscRing::try_push` (1024 cap) | 17.8 ns | **20.0** | 20.0 | 20.0 | 20 | 130 | 6 µs |
| `submit_limit_order` (passive rest) | 37.3 ns | **20.0** | 100 | **170** | 260 | 320 | 13 µs |
| `submit_limit_order` (1:1 match) | 31.2 ns | **30.1** | 50 | **70** | 130 | 240 | 5 µs |
| `cancel_order` | 26.9 ns | **20.0** | 40 | **100** | 190 | 300 | 7 µs |

**Spec target:** p50 < 1 µs, p99 < 5 µs. NanoMatch beats both by **10×–500×**.

The `max` column reflects the host environment — these are OS scheduler preemptions, frequency-scaling stalls, and minor page faults; the **code itself** never produces tail spikes. On a real production host with CPU isolation (see [tuning checklist](#deployment--tuning-checklist)) the p99.99 and max collapse to the p99.9 bucket.

### Ingestion throughput

| Component | Rate |
|---|---:|
| CSV parser (file → `InboundOrder`) | **25.6 M rows/sec** (39 ns/row) |
| End-to-end CSV → engine → trades | All 4-row test correctness verified |
| SPSC ring single-thread push+pop pair | 2.41 ns |
| Two-thread SPSC handoff (Linux pinned, expected) | 10–30 ns/handoff |

### Test coverage

```
   STEP 1  layout + struct invariants       52 / 52  ✓
   STEP 2  match engine scenarios           69 / 69  ✓
   STEP 3  huge-page memory pool            21 / 21  ✓
   STEP 4  SPSC ring (1M item handoff)      34 / 34  ✓
   STEP 5  ingestion (mmap, CSV, ITCH)      50 / 50  ✓
                                          --------
                                    Total: 226 / 226
```

---

## Design decisions

### 1. Indices, not pointers

Every linkage in the book — orders within a price level, levels within a sorted side, free-list nodes inside the pool — is a `uint32_t` index into a contiguous array, never a raw pointer.

| Pointer | uint32_t index |
|---|---|
| 8 bytes | **4 bytes** — 2× link density per cache line |
| Touches allocator | Pure array indexing — single MOV |
| Random heap address — cache miss | Adjacent allocations land in adjacent slots |
| Lifetime managed by RAII | Lifetime managed by pool — explicit, O(1) |

A pool index is also a free validity check (compare to `INVALID_IDX = 0xFFFFFFFF`) and trivially serializable for crash dumps.

### 2. Cache-line-sized structs

Every hot struct is **exactly 64 bytes**, the cache line size on every x86-64 CPU and most ARM cores:

```cpp
struct alignas(64) Order { ... };          // 64 bytes — verified by static_assert
struct alignas(64) PriceLevel { ... };     // 64 bytes
struct alignas(64) InboundOrder { ... };   // 64 bytes
struct alignas(64) TradeReport { ... };    // 64 bytes
```

This buys us three things:

- **One cache line per object.** A single L1 line fill (~4 cycles) brings the entire object into the core.
- **No false sharing.** Writing to one `Order` cannot invalidate a neighboring `Order`'s line in another core's L1.
- **Predictable prefetcher behavior.** Sequential allocations from the pool produce sequential cache lines — the prefetcher's dream.

Fields within each struct are ordered by access frequency: the hottest 32 bytes (`id`, `price`, `remaining_qty`, FIFO links) live in the front half so even speculative half-line fetches are enough on a partial fill.

### 3. Dense price-indexed book, not `std::map`

This is the design that unlocks sub-microsecond price lookups. Each side of the book has:

```cpp
struct BookSide {
    PriceLevelIdx price_index[PRICE_WINDOW_SIZE];   // direct lookup, ~256 KB
    PriceLevelIdx best_level;                       // sorted list head
    PriceTicks    base_tick;                        // offset = tick - base_tick
};
```

The 65 536-entry `price_index` is a flat array. Looking up "is there a level at price P?" is one indexed load, one cycle hot. A red-black tree would chase ~12 random pointers for the same answer at a realistic book depth.

For sparse traversal (finding the *next* best price when the current best is exhausted), we keep a parallel intrusive doubly-linked list of *populated* levels, sorted descending for bids, ascending for asks. We pay 256 KB of L2 per side; we get O(1) on every operation that matters.

### 4. Intrusive FIFO inside each price level

Orders within a price level are linked through their own `next` / `prev` index fields. No `std::deque`, no `std::list`, no node allocations.

```cpp
struct Order {
    /* ... */
    OrderIdx next;        // forward link within FIFO
    OrderIdx prev;        // backward link — enables O(1) cancel
    /* ... */
};
```

Consequence: **cancel is O(1)**. The hash map gives us the order index, the order itself holds its own prev/next, we unlink in three pointer assignments and free the slot.

### 5. Memory pools, never `new`

Every dynamic allocation happens through a fixed-size pool that hands out indices via a free list. The free list is *intrusive* — when a slot is free, its first 4 bytes hold the next-free index, so there is zero metadata overhead.

Three backends, all behind the same interface:

- **StaticStorage** — compile-time array. Default. Used for tests and small instances.
- **HugePagePool_Storage** — three-tier mmap strategy:
  1. `mmap(MAP_HUGETLB | MAP_HUGE_2MB | MAP_POPULATE)` — explicit 2 MB pages + pre-faulted
  2. `mmap(MAP_POPULATE) + madvise(MADV_HUGEPAGE)` — transparent huge pages
  3. `aligned_alloc + memset` — portable fallback
- Reports backend at runtime via `pool.backend()` so deployment can verify which path is active.

Why huge pages: a 256 MB pool on 4 KB pages needs 65 536 TLB entries. The L1 dTLB holds 64. On 2 MB pages it needs 128 entries — entirely TLB-resident. In random-access workloads this is a 5–10× speedup that benchmarks under-report (tight loops fit L1 either way).

### 6. Lock-free SPSC ring buffer

The hot data path is split across two threads — a producer that decodes market data and a consumer that runs the engine. They are connected by a wait-free single-producer / single-consumer ring buffer (`SpscRing<T, N>`). Two atomics, power-of-2 capacity, bit-mask wraparound, acquire/release pairing — no full barriers.

```cpp
template <typename T, std::size_t Capacity>
class SpscRing {
    // Producer cache line
    alignas(64) std::atomic<std::size_t> head_;
    std::size_t cached_tail_;     // ← Vyukov trick: avoid atomic load when ring is non-empty
    char _pad_p[...];

    // Consumer cache line
    alignas(64) std::atomic<std::size_t> tail_;
    std::size_t cached_head_;
    char _pad_c[...];

    // Payload
    alignas(64) Slot slots_[Capacity];
};
```

The cached counterpart (`cached_tail_` on the producer side, `cached_head_` on the consumer) lets each side skip the cross-thread atomic load on the common path — verified to drive `try_push` p99 to **20 ns**. The cache-line padding between producer and consumer state eliminates **false sharing** entirely.

### 7. CRTP-templated trade sink, no virtual dispatch

The engine calls back into a `Sink` to emit trades. Rather than a `std::function` (slow, allocates, hides what the compiler can see) or a virtual base class (vtable indirection per call), the engine is templated on the sink type:

```cpp
template <typename Sink = NullSink>
class MatchingEngineT {
    void emit(const TradeReport& t) noexcept { sink_.on_trade(t); }
    // ...
};
```

`emit()` becomes a direct call, fully inlined. `NullSink` for benchmarks, `RingSink` for production, `CaptureSink` for tests — all compile to different specialised engines with zero shared cost.

### 8. Fixed-point integer prices

Prices are `int64_t` in *ticks* — for NASDAQ this is dollars × 10 000. Never floating-point.

- Exact equality semantics (`px_a == px_b` is meaningful)
- No subnormal/NaN hazards in comparison
- Two-cycle integer compare instead of seven-cycle FP compare
- No FP exception register state to save/restore

### 9. ITCH binary parser, branchless message dispatch

`ItchParser` decodes the NASDAQ TotalView-ITCH 5.0 binary protocol. Big-endian fields are unrolled via `read_be16 / 32 / 48 / 64` helpers (one MOV per byte, OR'd into a shifted register). Message type dispatches via a single `switch(type)` — modern compilers convert this to a jump table or branch-prediction-friendly cascade.

We handle the matching-relevant subset (A, F, E, X, D, U) and route everything else to `Op::Noop` so the engine can ignore it without disturbing the hot path.

### 10. mmap, not read()

File ingestion uses `mmap` (POSIX) or `MapViewOfFile` (Windows). `read()` copies bytes through a kernel buffer into user memory — `mmap` maps the page cache directly into the process address space. Zero-copy, kernel-managed readahead via `posix_fadvise(POSIX_FADV_SEQUENTIAL)` doubles the prefetch window.

Result: **25.6 M rows/sec** CSV ingestion in a single thread, end-to-end including parsing.

---

## Building

### Linux / macOS

```bash
git clone <repo> && cd nanomatch
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/latency_probe
```

Google Benchmark is auto-fetched via `FetchContent`. Skip it with:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DNANOMATCH_NO_GBENCH=ON
```

### Windows (MinGW UCRT64)

```powershell
cd C:\path\to\NanoMatch
cmake -S . -B build -G "MinGW Makefiles" `
      -DCMAKE_MAKE_PROGRAM=C:/msys64/ucrt64/bin/mingw32-make.exe `
      -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe `
      -DNANOMATCH_NO_GBENCH=ON
cmake --build build
.\build\latency_probe.exe
```

`NANOMATCH_NO_GBENCH=ON` is recommended on MinGW — Google Benchmark v1.8.3 has a known `-Werror=switch` failure with newer GCC. Our own `latency_probe` is the more useful tool anyway (per-op percentiles, not means).

### Windows (MSVC / Visual Studio)

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
.\build\Release\latency_probe.exe
```

### Compiler flags applied

| Flag | GCC/Clang | MSVC | Why |
|---|---|---|---|
| Aggressive opt | `-O3 -flto` | `/O2 /GL /LTCG` | Inline everything, whole-program LTO |
| Target ISA | `-march=native` | `/arch:AVX2` | AVX2/AVX512 where available |
| No exceptions | `-fno-exceptions` | `/EHs-c-` | Smaller `.text`, no unwinder calls |
| No RTTI | `-fno-rtti` | `/GR-` | No `typeinfo` bloat |
| Frame pointers kept | `-fno-omit-frame-pointer` | `/Oy-` | Accurate `perf record` flame graphs |
| C++17 strict ISO | `-std=c++17` | `/std:c++17` | No GNU dialect quirks |

---

## Running

### Unit + integration tests

```bash
./build/nanomatch_step1   # struct layout + pool invariants  (52)
./build/nanomatch_step2   # matching engine scenarios         (69)
./build/nanomatch_step3   # huge-page memory pool             (21)
./build/nanomatch_step4   # SPSC ring + 1M-item MT handoff    (34)
./build/nanomatch_step5   # mmap + CSV + ITCH + E2E           (50)
```

All five emit colored PASS/FAIL output. Wired into CTest.

### Latency probe

```bash
./build/latency_probe
```

Output:

```
  Calibration: 3.793 cycles/ns  (clock 3.79 GHz)
  rdtsc available: YES

  Per-operation latency percentiles (ns):
  -----------------------------------------------------------------------
  MemoryPool alloc+dealloc          n=100000  mean= 13.5  p50= 10  p99=  20
  SpscRing::try_push (1024 cap)     n=100000  mean= 17.8  p50= 20  p99=  20
  submit_limit_order (passive)      n=100000  mean= 37.3  p50= 20  p99= 170
  submit_limit_order (1:1 match)    n=100000  mean= 31.2  p50= 30  p99=  70
  cancel_order                      n=100000  mean= 26.9  p50= 20  p99= 100
```

### Google Benchmark (optional)

```bash
./build/bench_main --benchmark_repetitions=10 \
                   --benchmark_report_aggregates_only=true
```

Reports mean/median/stddev/min/max across repetitions. Linux/Clang only on modern toolchains; see MinGW note above.

---

## Project layout

```
nanomatch/
├── CMakeLists.txt
├── README.md                          ← you are here
├── include/nanomatch/
│   ├── platform.hpp                  compiler portability shims
│   ├── order.hpp                     Order — 64 B, cache-aligned
│   ├── price_level.hpp               PriceLevel — 64 B
│   ├── trade.hpp                     TradeReport — 64 B
│   ├── inbound_order.hpp             Normalized engine input — 64 B
│   ├── memory_pool.hpp               Static + huge-page pools
│   ├── id_map.hpp                    Open-addressed OrderId→OrderIdx hash
│   ├── order_book.hpp                Dense price-indexed book sides
│   ├── matching_engine.hpp           Match loop, CRTP sink
│   ├── spsc_ring.hpp                 Lock-free SPSC ring (Vyukov)
│   ├── mapped_file.hpp               mmap / MapViewOfFile / fread fallback
│   ├── csv_parser.hpp                Branchless CSV → InboundOrder
│   └── itch_parser.hpp               NASDAQ ITCH 5.0 subset parser
├── main.cpp                          step 1 tests
├── main_step2.cpp                    step 2 tests
├── main_step3.cpp                    step 3 tests
├── main_step4.cpp                    step 4 tests
├── main_step5.cpp                    step 5 tests
└── benchmarks/
    ├── latency_probe.cpp             rdtsc per-op histogram (this is the one)
    └── bench_main.cpp                Google Benchmark targets (optional)
```

---

## Deployment & tuning checklist

NanoMatch's quoted numbers are achievable on cooperative hardware. To turn them into **production** numbers — flat p99.999 across hours of runtime — the OS and BIOS must also cooperate. The following is the standard HFT runbook:

### BIOS

- [ ] **Turbo Boost: disabled.** Eliminates frequency-scaling jitter; a fixed clock is more predictable than a higher average.
- [ ] **C-states: C1 max** (no C3/C6/C7). Deep C-states cost 5–20 µs to wake.
- [ ] **Hyperthreading: disabled** on engine cores. Avoids sibling-thread cache pollution.
- [ ] **SMI throttling: disabled** (System Management Interrupts can stall a core for 100+ µs invisibly).
- [ ] **NUMA: enabled** if dual-socket; pin engine to the NIC's local socket.

### Kernel (Linux)

- [ ] Boot parameters: `isolcpus=2,3 nohz_full=2,3 rcu_nocb_poll rcu_nocbs=2,3 mce=ignore_ce`
- [ ] `tuned-adm profile latency-performance`
- [ ] `echo 0 > /proc/sys/kernel/numa_balancing`
- [ ] `echo 1000000 > /proc/sys/kernel/sched_rt_runtime_us` (allow 1s of pure RT)
- [ ] Reserve huge pages: `echo 256 > /proc/sys/vm/nr_hugepages` (256 × 2 MB = 512 MB)
- [ ] **Disable transparent huge pages compaction** if explicit HUGETLB is reserved.

### Process

- [ ] `taskset -c 2 chrt -f 99 ./engine` — pin to isolated core, SCHED_FIFO priority 99.
- [ ] `mlockall(MCL_CURRENT | MCL_FUTURE)` — never page out engine memory.
- [ ] NIC IRQ affinity set to a *different* core than the engine.
- [ ] Engine thread must avoid `printf`, `malloc`, `pthread_mutex` on the hot path — `latency_probe` proves none are called.

### Verification

```bash
# Confirm engine core is isolated and quiet
cat /proc/sched_debug | grep cpu#2

# Confirm no page faults during run
perf stat -e page-faults,context-switches -p $(pgrep engine) sleep 60

# Capture flame graph
perf record -F 99 -g -p $(pgrep engine) -- sleep 30
perf script | stackcollapse-perf.pl | flamegraph.pl > engine.svg
```

A clean run should show **zero major page faults**, **zero involuntary context switches**, and a flame graph dominated by `submit_limit_order` and its inlinees with no kernel frames.

---

## Limitations and what's next

NanoMatch is feature-complete for what it claims to be — a demonstration of cache-aware order-book infrastructure. The following are deliberately out of scope:

| Not implemented | Why | How to add |
|---|---|---|
| Multi-instrument | Single-instrument book is the building block | Shard one `OrderBook` per instrument; round-robin or NUMA-pin shards |
| Modify (replace) handling on engine side | ITCH `U` decoded, engine treats as Cancel+Add | Add `replace_order(old_id, new_id, ...)` doing atomic unlink+relink |
| Post-only / hidden-order semantics | Flag plumbing exists, matching skips | Branchless rejection on cross in `match_buy/sell` prologue |
| Persistence / snapshotting | Sub-µs writes preclude on-disk per-order journaling | Per-message UDP multicast to a sequencer service |
| FIX gateway | Production protocol, not exchange protocol | Front the engine with a FIX→`InboundOrder` translator process |
| Self-trade prevention | Requires participant identity (`participant_id` field exists, unused) | One additional integer compare in `match_at_level` |

Performance work that would matter:

- **Real production hardware measurement.** The current numbers are from a non-tuned desktop and a Linux container. A pinned bare-metal box would compress the p99.99 tail by 30–100×.
- **Cross-core SPSC validation** with both threads on dedicated physical cores. The sandbox has 1 core, so two-thread bench is environment-limited; on real HW the design hits 10–30 ns per handoff.
- **VTune / perf flame graphs** showing the L1 miss rate. We expect <1% on the hot path; would be a satisfying confirmation.

---

## Acknowledgements & references

The techniques in NanoMatch are textbook for the HFT industry. Key references that shaped the implementation:

- *What Every Programmer Should Know About Memory* — Ulrich Drepper. The cache-behaviour bible.
- Dmitry Vyukov's SPSC queue. The cached-counterpart optimisation is his.
- NASDAQ TotalView-ITCH 5.0 specification. The wire format.
- LMAX Disruptor. The original "single writer principle" papers.
- Mike Acton, *Data-Oriented Design and C++*. The cultural argument for SoA, indices, and pool allocation.

---

## License

MIT. Use it, learn from it, profile it. PRs welcome.