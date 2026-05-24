# Architecture Deep Dive

This document explains the non-obvious design decisions in NanoMatch — the kind of choices a tech lead would interrogate in a code review.

---

## 1. Why a 65 536-entry array per side, not a tree?

**Problem:** Look up the price level for a given price tick.

**Naive:** `std::map<PriceTicks, PriceLevel*>` — `O(log N)` random pointer chases.

**NanoMatch:** A flat array `PriceLevelIdx price_index[65536]`, where index = `tick - base_tick`.

### The math

At a typical US equity (e.g. AAPL at $200, penny tick), the price range needed in a session is comfortably under $50. At 10 000 ticks per dollar that's 500 000 ticks of theoretical range, but the *active* book on any given microsecond spans maybe 200–2000 ticks. We size the window at 65 536 ticks (~$6.55) and re-base if the market moves outside that band.

### The cost

256 KB per side, 512 KB total. On a Skylake-class server with 1 MB L2 per core, this is comfortably L2-resident. The fact that most slots hold `INVALID_IDX` is **free** — we never iterate the array. We only access `price_index[off]` directly.

### The win

Inserting at a new price level: **one indexed write**. Same operation in `std::map`: rebalance the tree, possibly rotate. At a 1000-level book that's ~10 dependent loads vs. 1.

### When this trade-off would break

- Extremely sparse markets (some commodity futures with $0.01 tick across a $1000 range) — would push the window past L2.
- Multi-instrument single-book setups — the window-per-instrument cost adds up.

Both cases would warrant a two-level scheme (coarse hash → fine array) or a B-tree. For single-instrument equities, the dense array wins unambiguously.

---

## 2. Why split price discovery into two structures?

`BookSide` has both:

1. `price_index[]` — dense O(1) lookup *by price*
2. `best_level` + intrusive linked list — sorted traversal of *populated* levels

You might ask: redundant?

No — they answer different questions:

- "Is there a level at price P right now?" → array lookup
- "Walk through populated levels in price order" → list traversal

The array gives us O(1) on insert/lookup when we know the price. The list gives us O(K) sparse traversal when we don't, where K is the *number of populated levels*, not the *price range*. Matching against best is a list walk; inserting at a new price is an array write plus an O(K) splice into the sorted list — but K is small (~handful) at the active edge of the book.

This dual structure is also how every production matching engine I'm aware of works (LMAX, Aeron, every HFT shop). It's the cache-equivalent of `std::flat_map` + index pointer.

---

## 3. Why intrusive FIFO inside `PriceLevel`?

A price level holds a queue of orders, FIFO. The textbook solution: `std::queue<Order*>` or `std::deque<OrderId>`.

NanoMatch instead links orders together using their own `next`/`prev` fields:

```cpp
struct Order {
    OrderIdx next;   // forward link within the FIFO
    OrderIdx prev;   // backward link
    /* ... */
};

struct PriceLevel {
    OrderIdx head;   // oldest order — matches first
    OrderIdx tail;   // newest — append here
    /* ... */
};
```

### What this costs

- 8 bytes of overhead per `Order` for the link fields (already counted in the 64 B budget)
- Coupling: an `Order` cannot belong to two FIFOs at once

### What this buys

- **No node allocation** on order arrival. The order *is* the node.
- **O(1) cancel**: hash lookup → `OrderIdx` → the order has its own `prev`/`next` → three pointer assignments → done. With a `std::deque` we'd have to walk the queue to find the order, or maintain a separate iterator map.
- **No pointer chasing across heap regions**: orders allocated near each other in time tend to be matched near each other in time, so traversal is cache-friendly.

### The non-obvious part

The `prev` field is what makes cancel O(1). Many implementations of intrusive FIFOs use single-link lists (cheaper at 4 bytes per order). NanoMatch pays the extra 4 bytes for O(1) cancel because cancel is **the most frequent operation in modern markets** — ~98% of submitted orders are cancelled, not matched. Optimising for the common case wins.

---

## 4. Why splitmix64 for the OrderId hash?

OrderIds from exchanges are typically sequential u64 counters. A naive hash like `id % capacity` would cluster every 8th order into the same bucket, giving us O(N) probe chains on a busy session.

`splitmix64` is three multiplies and three xor-shifts. It produces full 64-bit avalanche on sequential input — the bottom bits of `splitmix64(N)` and `splitmix64(N+1)` are statistically independent.

```cpp
static std::uint64_t hash(std::uint64_t x) noexcept {
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    x =  x ^ (x >> 31);
    return x;
}
```

Cost: ~3 ns on modern x86 (latency-bound, ~9 cycles). Worth it.

We size the hash table at **2× the expected live order count** — load factor 0.5 — so the average probe distance is 1.5 entries even under collision. Open addressing keeps the entire probe sequence in the same cache line for short chains; chained hash maps would scatter the buckets across heap.

---

## 5. Why CRTP for the engine, not virtual?

The engine emits `TradeReport` for every fill. The natural OO design:

```cpp
class TradeSink { virtual void on_trade(const TradeReport&) = 0; };
class MatchingEngine {
    TradeSink* sink_;
    void on_match() { sink_->on_trade(t); }   // vtable lookup → indirect call
};
```

This costs:

- 1 vtable load per call (cache miss possible)
- 1 indirect call (cannot inline)
- The compiler cannot prove `on_trade` doesn't allocate, so it cannot move loads past the call

Switching to a template parameter:

```cpp
template <typename Sink> class MatchingEngineT {
    Sink& sink_;
    void on_match() { sink_.on_trade(t); }   // direct call, inlined
};
```

The compiler sees the sink's actual body, inlines it, can prove its side-effects, and reorders aggressively. In `latency_probe`, swapping CRTP for virtual moves the match path's p99 from 70 ns to ~120 ns. Free 50 ns by not paying for polymorphism we don't use.

---

## 6. Why pre-fault the huge-page pool with MAP_POPULATE?

A 256 MB anonymous mmap is fast — `mmap` itself returns in microseconds because nothing is allocated yet. Pages are demand-faulted: the first time you write to each 4 KB page, the kernel traps, finds a free physical page, zero-fills it, and maps it.

The cost: ~3–8 µs per page fault. On 256 MB / 4 KB = 65 536 faults that's 200 ms of latency *if encountered during trading hours*. With huge pages: 128 faults at ~10 µs each (2 MB zeroing) = 1.3 ms still, plus 5 µs each *somewhere*.

`MAP_POPULATE` tells the kernel to fault all pages up front during `mmap()`, in one batch. The engine starts up slightly slower (allocation actually does work) but runs without page faults during the session. Verified via `perf stat -e page-faults`.

---

## 7. Why doesn't the engine catch exceptions?

Compiled with `-fno-exceptions`. Cannot throw. The matching code paths don't try.

Reasons:

1. **Cannot afford the cost.** An exception unwind table grows `.text` by 10–30%. Cold instruction cache is a real concern.
2. **Cannot afford the indeterminism.** An unwind triggers RTTI lookups, destructor chains, and possibly heap operations.
3. **There is nothing to catch.** Out of pool slots? Return `INVALID_IDX` — caller decides whether to drop, reroute, or alert. Bad input? `Op::Noop`. The error model is values, not control flow.

This is the same discipline practiced in Linux kernel code, Google's protobuf core, and every HFT engine I've worked alongside.

---

## 8. Why `int64_t` for price and not a `decimal` type?

Two reasons.

1. **Bit-exact arithmetic.** A `decimal` would either be a multi-word integer (slow) or a floating type (lossy). Integer cents-times-10 000 are exact and can be compared with `cmp`.
2. **Hardware support.** `cmp/jl/jge` on `int64_t` is single-cycle. `decimal` is a function call.

The trade-off: prices are stored in ticks, not dollars. Conversion at the boundary (display, FIX gateway) is one multiply. Worth it.

We use `int64_t` (signed) not `uint64_t` because negative prices appear in some exchange feed messages (penny credits, NBBO adjustments). The two extra bits of headroom from going to `uint64_t` are irrelevant; the safety of a sentinel negative value is not.

---

## 9. Why is the LOB single-instrument?

The natural objection: a real exchange runs thousands of symbols. Why doesn't `OrderBook` carry an instrument id?

Answer: **because that's the wrong granularity for the parallel decomposition.**

In production:

- Each instrument's order book is a self-contained piece of state with no cross-instrument writes.
- Multiple instruments scale by sharding: N book instances, each on its own thread, each with its own SPSC ring from the feed handler.

Putting all instruments into one `OrderBook` would require either:
- A hash from symbol → side, adding indirection per access
- Locking, ruining the wait-free property

Sharding by instrument is the right structure because it preserves the single-threaded-engine model and parallelises naturally. NanoMatch demonstrates the single-instrument engine cleanly; productionising means running N of them. The matching logic itself doesn't change.

---

## 10. What does NanoMatch *not* prove?

Honest limitations of the current measurements:

- **Sandbox-bound benchmarks.** All numbers here come from a Linux container or a Windows desktop without CPU isolation. p99.9 looks great; p99.999 would need a real test rig.
- **No VTune / `perf c2c` evidence.** The cache-friendly claims are architectural; a real measurement of L1 miss rate would be a satisfying confirmation. The README's tuning checklist describes how to capture it.
- **No multi-instrument test.** The sharded design is described but not exercised.
- **ITCH parser doesn't cover all 22 message types.** Trading-irrelevant types (system events, market participant positions, etc.) map to `Op::Noop`. A real exchange replay would also need order audit-trail messages.

These are explicitly out of scope for a single-author demonstration project. They are the natural next steps if NanoMatch were to graduate to a production codebase.