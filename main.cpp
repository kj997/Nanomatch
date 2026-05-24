// nanomatch/main.cpp
// Validates struct layouts, pool behaviour, and book-side indexing.
// This is the STEP 1 smoke-test — not the matching engine (STEP 2).

#include "nanomatch/order.hpp"
#include "nanomatch/price_level.hpp"
#include "nanomatch/memory_pool.hpp"
#include "nanomatch/order_book.hpp"

#include <cstdio>
#include <cstring>
#include <chrono>
#include <cassert>

using namespace nanomatch;

// ── ANSI colours for terminal output ──────────────────────────────────────
#define GRN "\033[32m"
#define RED "\033[31m"
#define YLW "\033[33m"
#define CYN "\033[36m"
#define RST "\033[0m"
#define BLD "\033[1m"

static int  g_tests  = 0;
static int  g_passed = 0;

#define CHECK(cond, msg) do {                               \
    ++g_tests;                                              \
    if (cond) { ++g_passed;                                 \
        std::printf(GRN "  [PASS]" RST " %s\n", msg);      \
    } else {                                                \
        std::printf(RED "  [FAIL]" RST " %s\n", msg);      \
    }                                                       \
} while(0)

// ── 1. Struct layout verification ─────────────────────────────────────────
void test_struct_layouts() {
    std::printf(BLD CYN "\n[1] Struct layout & alignment\n" RST);

    CHECK(sizeof(Order)      == 64, "sizeof(Order) == 64 bytes");
    CHECK(alignof(Order)     == 64, "alignof(Order) == 64 (cache-line aligned)");
    CHECK(sizeof(PriceLevel) == 64, "sizeof(PriceLevel) == 64 bytes");
    CHECK(alignof(PriceLevel)== 64, "alignof(PriceLevel) == 64 (cache-line aligned)");

    CHECK(offsetof(Order, id)           ==  0, "Order::id           @ byte  0");
    CHECK(offsetof(Order, price)        ==  8, "Order::price        @ byte  8");
    CHECK(offsetof(Order, remaining_qty)== 16, "Order::remaining_qty @ byte 16");
    CHECK(offsetof(Order, original_qty) == 20, "Order::original_qty @ byte 20");
    CHECK(offsetof(Order, next)         == 24, "Order::next         @ byte 24");
    CHECK(offsetof(Order, prev)         == 28, "Order::prev         @ byte 28");
    CHECK(offsetof(Order, level_idx)    == 32, "Order::level_idx    @ byte 32");
    CHECK(offsetof(Order, ts_ns)        == 40, "Order::ts_ns        @ byte 40 (4B align-pad after level_idx)");
    CHECK(offsetof(Order, side)         == 48, "Order::side         @ byte 48");
    CHECK(offsetof(Order, flags)        == 49, "Order::flags        @ byte 49");
    CHECK(offsetof(Order, participant_id)==50, "Order::participant_id @ byte 50");

    CHECK(offsetof(PriceLevel, price)         == 0,  "PriceLevel::price        @ byte  0");
    CHECK(offsetof(PriceLevel, total_quantity) == 8,  "PriceLevel::total_qty    @ byte  8");
    CHECK(offsetof(PriceLevel, order_count)   == 12, "PriceLevel::order_count  @ byte 12");
    CHECK(offsetof(PriceLevel, head)          == 16, "PriceLevel::head         @ byte 16");
    CHECK(offsetof(PriceLevel, tail)          == 20, "PriceLevel::tail         @ byte 20");
    CHECK(offsetof(PriceLevel, next_level)    == 24, "PriceLevel::next_level   @ byte 24");
    CHECK(offsetof(PriceLevel, prev_level)    == 28, "PriceLevel::prev_level   @ byte 28");
}

// ── 2. Order fields sanity ─────────────────────────────────────────────────
void test_order_fields() {
    std::printf(BLD CYN "\n[2] Order field read/write\n" RST);

    alignas(64) Order o{};
    o.id             = 0xDEADBEEFCAFEBABEull;
    o.price          = 2000'0000LL;   // $200.0000 in fixed-point ticks
    o.remaining_qty  = 500;
    o.original_qty   = 500;
    o.next           = INVALID_IDX;
    o.prev           = INVALID_IDX;
    o.level_idx      = 7;
    o.ts_ns          = 1'700'000'000'000'000'000ull;
    o.side           = Side::Buy;
    o.flags          = 0x01;          // e.g. IOC flag
    o.participant_id = 42;

    CHECK(o.id             == 0xDEADBEEFCAFEBABEull,   "OrderId round-trips correctly");
    CHECK(o.price          == 2000'0000LL,              "Price (fixed-point) round-trips");
    CHECK(o.remaining_qty  == 500,                      "remaining_qty == 500");
    CHECK(o.next           == INVALID_IDX,              "next sentinel == INVALID_IDX");
    CHECK(o.side           == Side::Buy,                "Side::Buy stored correctly");
    CHECK(o.flags          == 0x01,                     "flags byte round-trips");

    // Simulate a partial fill: subtract 200 shares
    o.remaining_qty -= 200;
    CHECK(o.remaining_qty == 300, "Partial fill: remaining_qty 500→300");
}

// ── 3. MemoryPool: alloc / free / reuse ───────────────────────────────────
void test_memory_pool() {
    std::printf(BLD CYN "\n[3] MemoryPool<Order, 8>\n" RST);

    MemoryPool<Order, 8> pool;

    // Allocate all 8 slots
    std::uint32_t idxs[8];
    bool all_valid = true;
    for (int i = 0; i < 8; ++i) {
        idxs[i] = pool.allocate();
        if (idxs[i] == INVALID_IDX) all_valid = false;
    }
    CHECK(all_valid, "Allocated 8 slots without hitting INVALID_IDX");

    // 9th allocation must fail
    std::uint32_t overflow = pool.allocate();
    CHECK(overflow == INVALID_IDX, "9th allocation returns INVALID_IDX (pool exhausted)");

    // Initialize both slots explicitly (pool storage is intentionally uninit for perf)
    pool[idxs[0]].id = 111;
    pool[idxs[1]].id = 222;
    // Write to slot 0 again — must not corrupt slot 1
    pool[idxs[0]].remaining_qty = 999;
    CHECK(pool[idxs[0]].id == 111 && pool[idxs[1]].id == 222,
          "Adjacent slots are independent (no overlap / false sharing)");

    // Free slot 3, reallocate — must get 3 back
    pool.deallocate(idxs[3]);
    std::uint32_t recycled = pool.allocate();
    CHECK(recycled == idxs[3], "Freed slot 3 is recycled on next allocate()");

    // Free two slots, reallocate two — LIFO order (free-list)
    pool.deallocate(idxs[5]);
    pool.deallocate(idxs[6]);
    std::uint32_t r1 = pool.allocate();
    std::uint32_t r2 = pool.allocate();
    CHECK(r1 == idxs[6] && r2 == idxs[5],
          "Free list is LIFO: last-freed slot is first recycled");
}

// ── 4. BookSide: tick_to_offset mapping ───────────────────────────────────
void test_book_side_indexing() {
    std::printf(BLD CYN "\n[4] BookSide price-index mapping\n" RST);

    BookSide side{};
    PriceTicks base = 1990'0000LL;  // $199.0000 base
    side.init(base);

    // Price at base → offset 0
    CHECK(side.tick_to_offset(1990'0000LL) == 0,
          "base_tick maps to offset 0");

    // Price at base + 500 → offset 500
    CHECK(side.tick_to_offset(1990'0500LL) == 500,
          "base + 500 ticks maps to offset 500");

    // Price below base → INVALID_IDX
    CHECK(side.tick_to_offset(1989'9999LL) == INVALID_IDX,
          "Tick below base returns INVALID_IDX");

    // Price at exactly the window ceiling → INVALID_IDX
    CHECK(side.tick_to_offset(base + PRICE_WINDOW_SIZE) == INVALID_IDX,
          "Tick at window ceiling + 1 returns INVALID_IDX");

    // price_index for a new price starts as INVALID_IDX
    std::uint32_t off = side.tick_to_offset(1990'0100LL);
    CHECK(side.price_index[off] == INVALID_IDX,
          "Uninitialised price slot contains INVALID_IDX");

    // Simulate inserting a level
    side.price_index[off]  = 7;   // arbitrary level index
    side.active_levels     = 1;
    CHECK(side.price_index[off] == 7,
          "After insert, price_index[offset] holds correct level index");
}

// ── 5. OrderBook: init + pool sanity ──────────────────────────────────────
void test_order_book() {
    std::printf(BLD CYN "\n[5] OrderBook init & pool allocation\n" RST);

    OrderBook book;
    book.init(1990'0000LL, 1990'0100LL);   // bids base, asks base

    CHECK(book.bids.base_tick == 1990'0000LL, "bids.base_tick set correctly");
    CHECK(book.asks.base_tick == 1990'0100LL, "asks.base_tick set correctly");
    CHECK(book.bids.best_level == INVALID_IDX, "bids.best_level starts INVALID");
    CHECK(book.asks.best_level == INVALID_IDX, "asks.best_level starts INVALID");

    // Allocate an order from the pool
    std::uint32_t oidx = book.order_pool.allocate();
    CHECK(oidx != INVALID_IDX, "First order allocation succeeds");

    Order& o = book.order_pool[oidx];
    o.id            = 42;
    o.price         = 1990'0050LL;
    o.remaining_qty = 100;
    o.side          = Side::Buy;
    o.next          = INVALID_IDX;
    o.prev          = INVALID_IDX;

    CHECK(book.order_pool[oidx].id            == 42,           "Order stored in pool: id == 42");
    CHECK(book.order_pool[oidx].remaining_qty == 100,          "Order stored in pool: qty == 100");
    CHECK(book.order_pool[oidx].side          == Side::Buy,    "Order stored in pool: side == Buy");

    // Allocate a level
    std::uint32_t lidx = book.level_pool.allocate();
    CHECK(lidx != INVALID_IDX, "First level allocation succeeds");

    PriceLevel& lvl = book.level_pool[lidx];
    lvl.price          = 1990'0050LL;
    lvl.total_quantity = 100;
    lvl.order_count    = 1;
    lvl.head           = oidx;
    lvl.tail           = oidx;
    lvl.next_level     = INVALID_IDX;
    lvl.prev_level     = INVALID_IDX;

    // Link level into the book side
    std::uint32_t off = book.bids.tick_to_offset(lvl.price);
    book.bids.price_index[off] = lidx;
    book.bids.best_level       = lidx;
    book.bids.active_levels    = 1;

    CHECK(book.bids.best_level == lidx,    "bids.best_level points to new level");
    CHECK(book.level_pool[book.bids.best_level].head == oidx,
          "Level head points to allocated order");
}

// ── 6. Micro-benchmark: pool alloc throughput ────────────────────────────
void bench_pool_alloc() {
    std::printf(BLD CYN "\n[6] Pool allocation throughput (warm loop)\n" RST);

    constexpr int N = 1 << 16;   // 64K iterations
    MemoryPool<Order, N> pool;

    // Warm up
    for (int i = 0; i < N; ++i) { [[maybe_unused]] auto x = pool.allocate(); }
    for (int i = 0; i < N; ++i) pool.deallocate(N - 1 - i);

    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < N; ++i) { [[maybe_unused]] auto x = pool.allocate(); }
    auto t1 = std::chrono::high_resolution_clock::now();

    double ns_total = std::chrono::duration<double, std::nano>(t1 - t0).count();
    double ns_per   = ns_total / N;

    std::printf("  Allocated %d orders in %.0f ns  →  %.2f ns/alloc\n",
                N, ns_total, ns_per);
    CHECK(ns_per < 10.0, "Pool alloc < 10 ns/op (well under cache-miss budget)");
}

// ── Entry point ───────────────────────────────────────────────────────────
int main() {
    std::printf(BLD "\n══════════════════════════════════════════\n");
    std::printf(    "   NanoMatch — STEP 1 Validation Suite\n");
    std::printf(    "══════════════════════════════════════════\n" RST);

    test_struct_layouts();
    test_order_fields();
    test_memory_pool();
    test_book_side_indexing();
    test_order_book();
    bench_pool_alloc();

    std::printf(BLD "\n══════════════════════════════════════════\n");
    std::printf("  Results: %d / %d tests passed\n", g_passed, g_tests);
    std::printf("══════════════════════════════════════════\n\n" RST);

    return (g_passed == g_tests) ? 0 : 1;
}