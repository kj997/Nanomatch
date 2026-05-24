// nanomatch/main_step3.cpp
// STEP 3 — huge-page memory pool: correctness + backend report + perf compare

#include "nanomatch/memory_pool.hpp"
#include "nanomatch/order.hpp"
#include "nanomatch/matching_engine.hpp"
#include <cstdio>
#include <chrono>
#include <memory>
#include <vector>

using namespace nanomatch;

// ── infra ────────────────────────────────────────────────────────────────
#define GRN "\033[32m"
#define RED "\033[31m"
#define YLW "\033[33m"
#define CYN "\033[36m"
#define BLD "\033[1m"
#define RST "\033[0m"

static int g_tests = 0, g_passed = 0;
#define CHECK(cond, msg) do { ++g_tests;                                 \
    if (cond) { ++g_passed; std::printf(GRN "  [PASS]" RST " %s\n", msg);} \
    else      { std::printf(RED "  [FAIL]" RST " %s\n", msg); }          \
} while(0)

template <class P>
double ns_per_alloc(P& pool, int iters) {
    // Drain pool first to set known state
    std::vector<std::uint32_t> idxs; idxs.reserve(iters);
    for (int i = 0; i < iters; ++i) idxs.push_back(pool.allocate());
    for (int i = iters - 1; i >= 0; --i) pool.deallocate(idxs[i]);

    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iters; ++i) { [[maybe_unused]] auto x = pool.allocate(); }
    auto t1 = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / iters;
}

// ── 1. Static pool: behavior unchanged ───────────────────────────────────
void test_static_pool() {
    std::printf(BLD CYN "\n[1] StaticStorage pool (default)\n" RST);
    MemoryPool<Order, 1024> pool;
    std::printf("  backend: %s\n", pool.backend());
    CHECK(std::strcmp(pool.backend(), "static") == 0, "Backend is 'static'");

    std::uint32_t a = pool.allocate();
    std::uint32_t b = pool.allocate();
    CHECK(a != INVALID_IDX && b != INVALID_IDX, "Two allocs succeed");
    CHECK(a != b,                                "Distinct indices");

    pool[a].id = 0xAA;
    pool[b].id = 0xBB;
    CHECK(pool[a].id == 0xAA && pool[b].id == 0xBB, "Slots independent");

    pool.deallocate(a);
    std::uint32_t r = pool.allocate();
    CHECK(r == a, "Freed slot recycled LIFO");
}

// ── 2. Huge-page pool: report backend + same semantics ───────────────────
void test_huge_pool_correctness() {
    std::printf(BLD CYN "\n[2] HugePagePool correctness\n" RST);

    // Heap-allocate — pool storage may be huge
    auto pool = std::make_unique<MemoryPoolHuge<Order, 1 << 20>>();  // 1M × 64B = 64 MB
    std::printf("  backend: %s\n", pool->backend());

    bool ok_backend = std::strstr(pool->backend(), "mmap") != nullptr
                   || std::strstr(pool->backend(), "aligned_alloc") != nullptr;
    CHECK(ok_backend, "Backend reports a known strategy");

    std::uint32_t a = pool->allocate();
    std::uint32_t b = pool->allocate();
    CHECK(a != INVALID_IDX && b != INVALID_IDX, "Two allocs from huge pool");
    CHECK(a != b,                                "Distinct indices");

    (*pool)[a].id            = 12345;
    (*pool)[a].remaining_qty = 999;
    (*pool)[b].id            = 67890;
    (*pool)[b].remaining_qty = 111;
    CHECK((*pool)[a].id == 12345 && (*pool)[b].id == 67890,
          "Independent slot writes after huge-page mmap");
    CHECK((*pool)[a].remaining_qty == 999 && (*pool)[b].remaining_qty == 111,
          "Field updates persist");

    pool->deallocate(a);
    std::uint32_t r = pool->allocate();
    CHECK(r == a, "Free-list LIFO works on huge-page storage");
}

// ── 3. Stress: alloc many — verify monotone indices when no frees ───────
void test_huge_pool_stress() {
    std::printf(BLD CYN "\n[3] HugePagePool stress: 100k allocs\n" RST);

    constexpr int N = 100'000;
    auto pool = std::make_unique<MemoryPoolHuge<Order, 1 << 17>>();  // 128k cap
    bool monotone = true;
    std::uint32_t prev = INVALID_IDX;
    int failed = -1;
    for (int i = 0; i < N; ++i) {
        std::uint32_t x = pool->allocate();
        if (x == INVALID_IDX) { failed = i; break; }
        if (prev != INVALID_IDX && x != prev + 1) monotone = false;
        prev = x;
    }
    CHECK(failed == -1, "All 100k allocations succeed");
    CHECK(monotone,     "Indices strictly monotonic (high-water path)");
    CHECK(pool->high_water() == N, "high_water == 100k");
}

// ── 4. Huge-page pool inside an OrderBook (engine integration) ──────────
// OrderBook still uses the static pool by default. Demo: a "production"
// book variant with huge-page pools.
struct ProductionOrderBook {
    BookSide bids;
    BookSide asks;
    MemoryPoolHuge<Order,      1 << 20> order_pool;   // 64 MB
    MemoryPoolHuge<PriceLevel, 1 << 15> level_pool;   // 2 MB
    OrderIdHashMap<1 << 21>             id_map;       // 24 MB
    void init(PriceTicks bb, PriceTicks ab) noexcept {
        bids.init(bb); asks.init(ab);
    }
};

void test_engine_with_huge_pool() {
    std::printf(BLD CYN "\n[4] Matching engine on huge-page-backed book\n" RST);

    // OrderBook itself is fine — only the pool memory is huge.
    // But ProductionOrderBook is ~90 MB total → must heap alloc.
    auto pb = std::make_unique<ProductionOrderBook>();
    pb->init(1999'5000LL, 1999'5000LL);

    std::printf("  order_pool backend: %s\n", pb->order_pool.backend());
    std::printf("  level_pool backend: %s\n", pb->level_pool.backend());

    // We can't reuse MatchingEngine<NullSink> directly — it's templated on
    // OrderBook&, not ProductionOrderBook&. For STEP 3 we just demonstrate
    // pool integrity through manual ops. STEP 5+ unifies the book template.
    std::uint32_t o1 = pb->order_pool.allocate();
    std::uint32_t o2 = pb->order_pool.allocate();
    pb->order_pool[o1].id = 1;
    pb->order_pool[o2].id = 2;
    bool ins1 = pb->id_map.insert(1, o1);
    bool ins2 = pb->id_map.insert(2, o2);
    CHECK(ins1 && ins2,                            "id_map inserts succeed on huge book");
    CHECK(pb->id_map.find(1) == o1,                "Lookup id 1 → slot o1");
    CHECK(pb->id_map.find(2) == o2,                "Lookup id 2 → slot o2");
    CHECK(pb->order_pool[o1].id == 1,              "Field read OK from huge pool");
}

// ── 5. Perf: static vs huge — alloc throughput ──────────────────────────
void bench_alloc_compare() {
    std::printf(BLD CYN "\n[5] Alloc throughput: static vs huge\n" RST);
    constexpr int N = 50'000;

    auto sp = std::make_unique<MemoryPool<Order,     1 << 17>>();
    auto hp = std::make_unique<MemoryPoolHuge<Order, 1 << 17>>();

    double s_ns = ns_per_alloc(*sp, N);
    double h_ns = ns_per_alloc(*hp, N);

    std::printf("  static : %.2f ns/alloc\n", s_ns);
    std::printf("  huge   : %.2f ns/alloc   (backend: %s)\n", h_ns, hp->backend());

    // Both should be similar — alloc itself doesn't care about page size,
    // benefit is in *random-access* over hot data (TLB).
    CHECK(s_ns < 20.0 && h_ns < 20.0, "Both pools < 20 ns/alloc");
}

// ── 6. Perf: random access over large pool — TLB win shows here ─────────
// Simulates the real LOB workload: random order access via id_map → pool.
void bench_random_access_compare() {
    std::printf(BLD CYN "\n[6] Random-access read: static vs huge (TLB-sensitive)\n" RST);
    constexpr int N        = 1 << 17;  // 128k orders × 64B = 8 MB working set
    constexpr int LOOKUPS  = 1'000'000;

    auto sp = std::make_unique<MemoryPool<Order,     N>>();
    auto hp = std::make_unique<MemoryPoolHuge<Order, N>>();

    // Fill both pools, write tag in each slot
    for (int i = 0; i < N; ++i) {
        std::uint32_t a = sp->allocate();
        std::uint32_t b = hp->allocate();
        (*sp)[a].id = a;
        (*hp)[b].id = b;
    }

    // Random index sequence (xorshift32 — deterministic)
    std::vector<std::uint32_t> seq(LOOKUPS);
    std::uint32_t s = 0xC0FFEE;
    for (auto& v : seq) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        v = s & (N - 1);
    }

    // Bench static
    std::uint64_t acc1 = 0;
    auto t0 = std::chrono::high_resolution_clock::now();
    for (auto i : seq) acc1 += (*sp)[i].id;
    auto t1 = std::chrono::high_resolution_clock::now();
    double s_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / LOOKUPS;

    // Bench huge
    std::uint64_t acc2 = 0;
    t0 = std::chrono::high_resolution_clock::now();
    for (auto i : seq) acc2 += (*hp)[i].id;
    t1 = std::chrono::high_resolution_clock::now();
    double h_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / LOOKUPS;

    std::printf("  static : %.2f ns/lookup  (sum=%llu)\n", s_ns, (unsigned long long)acc1);
    std::printf("  huge   : %.2f ns/lookup  (sum=%llu)\n", h_ns, (unsigned long long)acc2);
    std::printf("  speedup: %.2fx %s\n",
                s_ns / h_ns,
                (h_ns < s_ns) ? "(huge wins)" : "(static wins — TLB not the bottleneck here)");

    CHECK(acc1 == acc2, "Sums match — same data read from both pools");
    CHECK(s_ns < 100.0 && h_ns < 100.0, "Both pools < 100 ns/random-lookup");
}

int main() {
    std::printf(BLD "\n══════════════════════════════════════════\n");
    std::printf(    "   NanoMatch — STEP 3 Huge-Page Pool Tests\n");
    std::printf(    "══════════════════════════════════════════\n" RST);

    test_static_pool();
    test_huge_pool_correctness();
    test_huge_pool_stress();
    test_engine_with_huge_pool();
    bench_alloc_compare();
    bench_random_access_compare();

    std::printf(BLD "\n══════════════════════════════════════════\n");
    std::printf("  Results: %d / %d tests passed\n", g_passed, g_tests);
    std::printf("══════════════════════════════════════════\n\n" RST);
    return (g_passed == g_tests) ? 0 : 1;
}