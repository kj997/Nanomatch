// benchmarks/bench_main.cpp
// Google Benchmark targets. Run with:
//   ./bench_main --benchmark_repetitions=10 --benchmark_report_aggregates_only=true
//
// Reports: mean, median, stddev, min, max across N repetitions.
// Each benchmark auto-tunes iteration count for statistical significance.

#include <benchmark/benchmark.h>

#include "nanomatch/matching_engine.hpp"
#include "nanomatch/spsc_ring.hpp"
#include "nanomatch/inbound_order.hpp"
#include "nanomatch/csv_parser.hpp"

#include <memory>
#include <vector>
#include <random>
#include <cstdio>
#include <cstring>

using namespace nanomatch;

// ─── 1. Pool alloc+dealloc tight loop ────────────────────────────────────
static void BM_PoolAllocFree(benchmark::State& state) {
    auto pool = std::make_unique<MemoryPool<Order, 1 << 17>>();

    // Warm
    std::vector<std::uint32_t> warm;
    warm.reserve(1024);
    for (int i = 0; i < 1024; ++i) warm.push_back(pool->allocate());
    for (auto x : warm) pool->deallocate(x);

    for (auto _ : state) {
        std::uint32_t idx = pool->allocate();
        benchmark::DoNotOptimize(idx);
        pool->deallocate(idx);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PoolAllocFree);

// ─── 2. SPSC ring push then pop (single thread, no contention) ──────────
static void BM_SpscPushPop(benchmark::State& state) {
    auto ring = std::make_unique<SpscRing<InboundOrder, 1024>>();
    InboundOrder msg{};
    msg.op = Op::Add; msg.side = Side::Buy;
    msg.price = 2000'0000LL; msg.qty = 100;

    OrderId id = 0;
    for (auto _ : state) {
        msg.id = id++;
        bool p = ring->try_push(msg);
        benchmark::DoNotOptimize(p);
        InboundOrder out{};
        bool q = ring->try_pop(out);
        benchmark::DoNotOptimize(q);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SpscPushPop);

// ─── 3. Passive limit-order submit (new price each time) ────────────────
static void BM_SubmitPassive(benchmark::State& state) {
    auto book = std::make_unique<OrderBook>();
    book->init(1999'5000LL, 1999'5000LL);
    NullSink sink;
    MatchingEngine eng(*book, sink);

    // Warm: populate some levels so the path is realistic
    for (int i = 0; i < 1024; ++i) {
        [[maybe_unused]] auto x = eng.submit_limit_order(900'000 + i, Side::Buy,
            2000'0000LL + (i % 50), 10, 0, 0);
    }

    OrderId id = 1'000'000;
    int span = 0;
    for (auto _ : state) {
        // Use FLAG_IOC to prevent the book from growing unboundedly across iterations.
        // Real-world passive-rest path is similar cost.
        ++span;
        bool match_path = (span & 1) == 0;
        if (match_path) {
            [[maybe_unused]] auto r = eng.submit_limit_order(id++, Side::Buy,
                2000'0000LL, 1, 0, FLAG_IOC);
            benchmark::DoNotOptimize(r);
        } else {
            [[maybe_unused]] auto r = eng.submit_limit_order(id++, Side::Sell,
                2000'0500LL + (id % 100), 1, 0, FLAG_IOC);
            benchmark::DoNotOptimize(r);
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SubmitPassive);

// ─── 4. Matching-side submit — aggressive crosses each iter ─────────────
static void BM_SubmitMatch(benchmark::State& state) {
    auto book = std::make_unique<OrderBook>();
    book->init(1999'5000LL, 1999'5000LL);
    NullSink sink;
    MatchingEngine eng(*book, sink);

    // Refill the book whenever it runs low — keep ~5000 levels deep
    auto refill = [&](OrderId& seed) {
        for (int i = 0; i < 5000; ++i) {
            [[maybe_unused]] auto x = eng.submit_limit_order(seed++, Side::Sell,
                2000'0000LL, 1, 0, 0);
        }
    };
    OrderId maker_seed = 1;
    refill(maker_seed);

    OrderId taker_id = 10'000'000;
    int iter_in_window = 0;
    for (auto _ : state) {
        if (++iter_in_window >= 4000) {
            state.PauseTiming();
            refill(maker_seed);
            iter_in_window = 0;
            state.ResumeTiming();
        }
        [[maybe_unused]] auto r = eng.submit_limit_order(taker_id++, Side::Buy,
            2000'0000LL, 1, 0, FLAG_IOC);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SubmitMatch);

// ─── 5. Cancel — O(1) intrusive unlink ──────────────────────────────────
static void BM_CancelOrder(benchmark::State& state) {
    auto book = std::make_unique<OrderBook>();
    book->init(1999'5000LL, 1999'5000LL);
    NullSink sink;
    MatchingEngine eng(*book, sink);

    // Steady-state: maintain a population of ~50k resting orders.
    // Each iter: cancel one, submit one (so book size stays constant).
    constexpr int POOL = 50'000;
    OrderId seed = 1;
    std::vector<OrderId> live;
    live.reserve(POOL);
    for (int i = 0; i < POOL; ++i) {
        OrderId id = seed++;
        live.push_back(id);
        [[maybe_unused]] auto x = eng.submit_limit_order(id, Side::Buy,
            2000'0000LL + (id % 1000), 10, 0, 0);
    }

    std::size_t idx = 0;
    for (auto _ : state) {
        OrderId victim = live[idx];
        bool ok = eng.cancel_order(victim);
        benchmark::DoNotOptimize(ok);
        // Replace it so the population stays ~POOL
        OrderId fresh = seed++;
        [[maybe_unused]] auto r = eng.submit_limit_order(fresh, Side::Buy,
            2000'0000LL + (fresh % 1000), 10, 0, 0);
        live[idx] = fresh;
        idx = (idx + 1) % POOL;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CancelOrder);

BENCHMARK_MAIN();