// benchmarks/latency_probe.cpp
// Per-operation latency histogram harness. NOT Google Benchmark — we
// capture every individual op's cycle count, then compute p50/p90/p99/p99.9.
// This is the only way to see tail latency that matters in HFT.

#include "nanomatch/matching_engine.hpp"
#include "nanomatch/spsc_ring.hpp"
#include "nanomatch/inbound_order.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <vector>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  #if defined(_MSC_VER)
    #include <intrin.h>
  #else
    #include <x86intrin.h>
  #endif
  #define NM_HAS_RDTSC 1
#else
  #define NM_HAS_RDTSC 0
#endif

using namespace nanomatch;

// ─── Cycle counter ────────────────────────────────────────────────────────
// rdtscp is preferred over rdtsc — it serialises with prior instructions,
// avoiding speculative reordering of the measurement. On modern x86 rdtsc(p)
// is *not* a cycle counter literally — it's an invariant nanosecond-scale
// counter. We convert cycles→ns via runtime calibration.

static inline std::uint64_t now_cycles() noexcept {
#if NM_HAS_RDTSC
  #if defined(_MSC_VER)
    unsigned int aux;
    return __rdtscp(&aux);
  #else
    unsigned int aux;
    return __rdtscp(&aux);
  #endif
#else
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

// Calibrate cycles per nanosecond by measuring rdtsc over a 50ms wall window.
static double calibrate_cycles_per_ns() {
#if !NM_HAS_RDTSC
    return 1.0;   // already in ns
#else
    auto wall0 = std::chrono::steady_clock::now();
    std::uint64_t c0 = now_cycles();
    while (std::chrono::steady_clock::now() - wall0 < std::chrono::milliseconds(50)) { }
    auto wall1 = std::chrono::steady_clock::now();
    std::uint64_t c1 = now_cycles();
    double ns = std::chrono::duration<double, std::nano>(wall1 - wall0).count();
    return (c1 - c0) / ns;
#endif
}

// ─── Histogram → percentile reporter ──────────────────────────────────────
struct Percentiles {
    double p50, p90, p99, p999, p9999, max_, mean;
    std::size_t n;
};

static Percentiles compute(std::vector<std::uint64_t>& samples, double cy_per_ns) {
    Percentiles r{};
    if (samples.empty()) return r;

    std::sort(samples.begin(), samples.end());
    auto pct = [&](double p) -> double {
        std::size_t idx = std::min(samples.size() - 1,
            static_cast<std::size_t>(samples.size() * p));
        return samples[idx] / cy_per_ns;   // cycles → ns
    };
    r.p50   = pct(0.50);
    r.p90   = pct(0.90);
    r.p99   = pct(0.99);
    r.p999  = pct(0.999);
    r.p9999 = pct(0.9999);
    r.max_  = samples.back() / cy_per_ns;

    std::uint64_t sum = 0;
    for (auto v : samples) sum += v;
    r.mean = (static_cast<double>(sum) / samples.size()) / cy_per_ns;
    r.n    = samples.size();
    return r;
}

static void print_row(const char* name, const Percentiles& p) {
    std::printf("  %-32s  n=%-8zu  mean=%6.1f  p50=%6.1f  p90=%6.1f  p99=%7.1f  p99.9=%7.1f  p99.99=%7.1f  max=%8.1f ns\n",
                name, p.n, p.mean, p.p50, p.p90, p.p99, p.p999, p.p9999, p.max_);
}

// ─── Bench: submit_limit_order (passive rest, no match) ──────────────────
static void bench_submit_passive(double cy_per_ns) {
    constexpr int N = 100'000;
    auto book = std::make_unique<OrderBook>();
    book->init(1999'5000LL, 1999'5000LL);
    NullSink sink;
    MatchingEngine eng(*book, sink);

    std::vector<std::uint64_t> samples;
    samples.reserve(N);

    // Pre-warm — touch all the price-index slots we'll write
    for (int i = 0; i < 100; ++i) {
        [[maybe_unused]] auto x = eng.submit_limit_order(900000 + i, Side::Buy,
            2000'0000LL + (i % 50), 10, 0, 0);
    }

    for (int i = 0; i < N; ++i) {
        OrderId id = static_cast<OrderId>(1'000'000 + i);
        PriceTicks p = 2000'0000LL + (i % 1000);   // span 1000 ticks → mostly new levels
        std::uint64_t t0 = now_cycles();
        [[maybe_unused]] OrderIdx r = eng.submit_limit_order(id, Side::Buy, p, 10, 0, 0);
        std::uint64_t t1 = now_cycles();
        samples.push_back(t1 - t0);
    }
    print_row("submit_limit_order (passive)", compute(samples, cy_per_ns));
}

// ─── Bench: submit_limit_order that fully matches ────────────────────────
static void bench_submit_matching(double cy_per_ns) {
    constexpr int N = 100'000;
    auto book = std::make_unique<OrderBook>();
    book->init(1999'5000LL, 1999'5000LL);
    NullSink sink;
    MatchingEngine eng(*book, sink);

    // Pre-seed asks deep enough to satisfy N takers
    for (int i = 0; i < N; ++i) {
        [[maybe_unused]] auto x = eng.submit_limit_order(static_cast<OrderId>(i + 1),
            Side::Sell, 2000'0000LL, 1, 0, 0);
    }

    std::vector<std::uint64_t> samples;
    samples.reserve(N);

    for (int i = 0; i < N; ++i) {
        OrderId id = static_cast<OrderId>(N + i + 1);
        std::uint64_t t0 = now_cycles();
        [[maybe_unused]] OrderIdx r = eng.submit_limit_order(id, Side::Buy,
            2000'0000LL, 1, 0, FLAG_IOC);
        std::uint64_t t1 = now_cycles();
        samples.push_back(t1 - t0);
    }
    print_row("submit_limit_order (1:1 match)", compute(samples, cy_per_ns));
}

// ─── Bench: cancel_order O(1) ────────────────────────────────────────────
static void bench_cancel(double cy_per_ns) {
    constexpr int N = 100'000;
    auto book = std::make_unique<OrderBook>();
    book->init(1999'5000LL, 1999'5000LL);
    NullSink sink;
    MatchingEngine eng(*book, sink);

    // Pre-populate N resting orders across many prices (to spread across levels)
    for (int i = 0; i < N; ++i) {
        [[maybe_unused]] auto x = eng.submit_limit_order(static_cast<OrderId>(i + 1),
            Side::Buy, 2000'0000LL + (i % 100), 10, 0, 0);
    }

    std::vector<std::uint64_t> samples;
    samples.reserve(N);

    for (int i = 0; i < N; ++i) {
        OrderId id = static_cast<OrderId>(i + 1);
        std::uint64_t t0 = now_cycles();
        [[maybe_unused]] bool ok = eng.cancel_order(id);
        std::uint64_t t1 = now_cycles();
        samples.push_back(t1 - t0);
    }
    print_row("cancel_order", compute(samples, cy_per_ns));
}

// ─── Bench: SPSC try_push (single-thread, ring empty most of the time) ──
static void bench_spsc_push(double cy_per_ns) {
    constexpr int N = 100'000;
    auto ring = std::make_unique<SpscRing<InboundOrder, 1024>>();

    std::vector<std::uint64_t> samples;
    samples.reserve(N);

    InboundOrder msg{};
    msg.op = Op::Add; msg.side = Side::Buy;
    msg.price = 2000'0000LL; msg.qty = 100;

    for (int i = 0; i < N; ++i) {
        msg.id = static_cast<OrderId>(i);
        std::uint64_t t0 = now_cycles();
        bool ok = ring->try_push(msg);
        std::uint64_t t1 = now_cycles();
        samples.push_back(t1 - t0);
        if (!ok) {
            // drain so we don't get stuck full
            InboundOrder out{};
            while (ring->try_pop(out)) { }
        }
    }
    print_row("SpscRing::try_push (1024 cap)", compute(samples, cy_per_ns));
}

// ─── Bench: pool alloc + free ────────────────────────────────────────────
static void bench_pool_alloc_free(double cy_per_ns) {
    constexpr int N = 100'000;
    auto pool = std::make_unique<MemoryPool<Order, 1 << 17>>();

    std::vector<std::uint64_t> samples;
    samples.reserve(N);

    // Drain warm-up via high_water
    std::vector<std::uint32_t> warm;
    warm.reserve(N);
    for (int i = 0; i < N; ++i) warm.push_back(pool->allocate());
    for (auto x : warm) pool->deallocate(x);

    // Steady-state: alloc + dealloc
    for (int i = 0; i < N; ++i) {
        std::uint64_t t0 = now_cycles();
        std::uint32_t idx = pool->allocate();
        pool->deallocate(idx);
        std::uint64_t t1 = now_cycles();
        samples.push_back(t1 - t0);
    }
    print_row("MemoryPool alloc+dealloc", compute(samples, cy_per_ns));
}

// ─── Main ────────────────────────────────────────────────────────────────
int main() {
    std::printf("\n══════════════════════════════════════════════════════════════════════════\n");
    std::printf("   NanoMatch — STEP 6 Latency Probe (per-op rdtsc histogram)\n");
    std::printf("══════════════════════════════════════════════════════════════════════════\n");

    double cy_per_ns = calibrate_cycles_per_ns();
    std::printf("  Calibration: %.3f cycles/ns  (clock %.2f GHz)\n",
                cy_per_ns, cy_per_ns);
    std::printf("  rdtsc available: %s\n", NM_HAS_RDTSC ? "YES" : "NO (using steady_clock)");
    std::printf("\n  Per-operation latency percentiles (ns):\n");
    std::printf("  %s\n", std::string(140, '-').c_str());

    bench_pool_alloc_free(cy_per_ns);
    bench_spsc_push(cy_per_ns);
    bench_submit_passive(cy_per_ns);
    bench_submit_matching(cy_per_ns);
    bench_cancel(cy_per_ns);

    std::printf("  %s\n\n", std::string(140, '-').c_str());

    std::printf("  Targets (from project spec):\n");
    std::printf("    p50 < 1000 ns (1 us)     p99 < 5000 ns (5 us)\n\n");
    std::printf("  Note: Run inside a CPU-pinned, frequency-locked process for production numbers.\n");
    std::printf("        On Linux: `taskset -c 2 chrt -f 99 ./latency_probe`\n");
    std::printf("        See README.md for the full hardware/OS tuning checklist.\n\n");
    return 0;
}