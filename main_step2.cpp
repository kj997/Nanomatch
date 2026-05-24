// nanomatch/main_step2.cpp
// STEP 2 — matching engine scenarios

#include "nanomatch/matching_engine.hpp"
#include "nanomatch/trade.hpp"
#include <cstdio>
#include <vector>
#include <chrono>
#include <memory>

using namespace nanomatch;

// ── Test infra ────────────────────────────────────────────────────────────
#define GRN "\033[32m"
#define RED "\033[31m"
#define CYN "\033[36m"
#define BLD "\033[1m"
#define RST "\033[0m"

static int g_tests = 0, g_passed = 0;
#define CHECK(cond, msg) do { ++g_tests;                                 \
    if (cond) { ++g_passed; std::printf(GRN "  [PASS]" RST " %s\n", msg);} \
    else      { std::printf(RED "  [FAIL]" RST " %s\n", msg); }          \
} while(0)

// Trade sink for tests — captures all trades into a vector.
struct CaptureSink {
    std::vector<TradeReport> trades;
    void on_trade(const TradeReport& t) noexcept { trades.push_back(t); }
};

using TestEngine = MatchingEngineT<CaptureSink>;

// ── Helpers ───────────────────────────────────────────────────────────────
// OrderBook is ~7 MB (id_map + price_index arrays). Must heap-allocate;
// default 8 MB stack would overflow.
static std::unique_ptr<OrderBook> make_book(PriceTicks base = 1999'5000LL) {
    auto b = std::make_unique<OrderBook>();
    b->init(base, base);
    return b;
}

// ── 1. Empty book — limit order rests ─────────────────────────────────────
void test_rest_on_empty() {
    std::printf(BLD CYN "\n[1] Rest on empty book\n" RST);
    auto bookp = make_book(); auto& book = *bookp;
    CaptureSink sink; TestEngine eng(book, sink);

    OrderIdx ridx = eng.submit_limit_order(/*id*/1, Side::Buy, 2000'0000LL,
                                           /*qty*/100, /*ts*/1, 0);

    CHECK(ridx != INVALID_IDX,              "Returns valid OrderIdx for rest");
    CHECK(sink.trades.empty(),               "No trades emitted");
    CHECK(book.bids.active_levels == 1,     "bids has 1 active level");
    CHECK(book.bids.best_level != INVALID_IDX, "bids.best_level set");
    CHECK(book.level_pool[book.bids.best_level].total_quantity == 100,
          "Level total_quantity == 100");
    CHECK(book.id_map.find(1) == ridx,      "id_map maps id 1 → returned idx");
}

// ── 2. Single full match ─────────────────────────────────────────────────
void test_single_full_match() {
    std::printf(BLD CYN "\n[2] Single full match\n" RST);
    auto bookp = make_book(); auto& book = *bookp;
    CaptureSink sink; TestEngine eng(book, sink);

    // Maker: sell 100 @ 2000.00
    eng.submit_limit_order(1, Side::Sell, 2000'0000LL, 100, 1, 0);
    sink.trades.clear();

    // Taker: buy 100 @ 2000.00 — full fill
    OrderIdx rem = eng.submit_limit_order(2, Side::Buy, 2000'0000LL, 100, 2, 0);

    CHECK(rem == INVALID_IDX,               "Taker fully filled, no rest");
    CHECK(sink.trades.size() == 1,           "One trade emitted");
    CHECK(sink.trades[0].aggressor_id == 2,  "aggressor_id == 2 (taker)");
    CHECK(sink.trades[0].resting_id   == 1,  "resting_id   == 1 (maker)");
    CHECK(sink.trades[0].price == 2000'0000LL, "Fill price = maker price");
    CHECK(sink.trades[0].quantity == 100,    "Fill qty == 100");
    CHECK(book.asks.active_levels == 0,     "Maker level removed (empty)");
    CHECK(book.asks.best_level == INVALID_IDX, "asks.best_level reset to INVALID");
    CHECK(book.id_map.find(1) == INVALID_IDX, "Maker id removed from id_map");
    CHECK(book.id_map.find(2) == INVALID_IDX, "Taker id never inserted");
}

// ── 3. Partial fill — taker remainder rests ───────────────────────────────
void test_partial_fill_rests() {
    std::printf(BLD CYN "\n[3] Partial fill — taker rests\n" RST);
    auto bookp = make_book(); auto& book = *bookp;
    CaptureSink sink; TestEngine eng(book, sink);

    eng.submit_limit_order(1, Side::Sell, 2000'0000LL, 100, 1, 0);
    sink.trades.clear();

    // Taker buys 250 @ 2000.00 — fills 100, rests 150 on bids
    OrderIdx rem = eng.submit_limit_order(2, Side::Buy, 2000'0000LL, 250, 2, 0);

    CHECK(rem != INVALID_IDX,               "Taker remainder rests");
    CHECK(sink.trades.size() == 1,           "One trade emitted");
    CHECK(sink.trades[0].quantity == 100,    "Trade qty = available 100");
    CHECK(book.asks.active_levels == 0,     "Ask side empty after fill");
    CHECK(book.bids.active_levels == 1,     "Bid side now has resting taker");
    PriceLevelIdx blvl = book.bids.best_level;
    CHECK(book.level_pool[blvl].total_quantity == 150, "Resting qty = 250 - 100 = 150");
    CHECK(book.id_map.find(2) == rem,       "Taker now in id_map");
}

// ── 4. Partial fill of maker — maker stays at head ────────────────────────
void test_partial_maker_stays() {
    std::printf(BLD CYN "\n[4] Partial maker fill — maker stays\n" RST);
    auto bookp = make_book(); auto& book = *bookp;
    CaptureSink sink; TestEngine eng(book, sink);

    eng.submit_limit_order(1, Side::Sell, 2000'0000LL, 500, 1, 0);
    sink.trades.clear();

    OrderIdx rem = eng.submit_limit_order(2, Side::Buy, 2000'0000LL, 200, 2, 0);

    CHECK(rem == INVALID_IDX,                          "Taker fully filled");
    CHECK(sink.trades.size() == 1,                      "One trade");
    CHECK(sink.trades[0].quantity == 200,               "Filled 200");
    CHECK(book.asks.active_levels == 1,                "Maker level still active");
    OrderIdx maker = book.id_map.find(1);
    CHECK(maker != INVALID_IDX,                        "Maker still in id_map");
    CHECK(book.order_pool[maker].remaining_qty == 300, "Maker remaining = 500-200 = 300");
}

// ── 5. Multi-level walk ───────────────────────────────────────────────────
void test_multi_level_walk() {
    std::printf(BLD CYN "\n[5] Multi-level walk (aggressive buy sweeps asks)\n" RST);
    auto bookp = make_book(); auto& book = *bookp;
    CaptureSink sink; TestEngine eng(book, sink);

    // Asks: 100 @ 2000.00, 50 @ 2000.01, 200 @ 2000.02
    eng.submit_limit_order(1, Side::Sell, 2000'0000LL, 100, 1, 0);
    eng.submit_limit_order(2, Side::Sell, 2000'0100LL,  50, 2, 0);
    eng.submit_limit_order(3, Side::Sell, 2000'0200LL, 200, 3, 0);
    sink.trades.clear();

    // Aggressive buy 200 @ 2000.01 — should fill 100 + 50 = 150, rest 50 on bids
    OrderIdx rem = eng.submit_limit_order(99, Side::Buy, 2000'0100LL, 200, 4, 0);

    CHECK(sink.trades.size() == 2,                          "Two fills");
    CHECK(sink.trades[0].price == 2000'0000LL,              "First fill @ 2000.00 (best ask)");
    CHECK(sink.trades[0].quantity == 100,                   "First fill qty 100");
    CHECK(sink.trades[1].price == 2000'0100LL,              "Second fill @ 2000.01");
    CHECK(sink.trades[1].quantity == 50,                    "Second fill qty 50");
    CHECK(book.asks.active_levels == 1,                    "Only 2000.02 level remains");
    CHECK(book.level_pool[book.asks.best_level].price == 2000'0200LL,
          "New best ask = 2000.02");
    CHECK(rem != INVALID_IDX,                              "Taker remainder rests");
    CHECK(book.level_pool[book.bids.best_level].total_quantity == 50,
          "Bid rest qty = 200 - 150 = 50");
}

// ── 6. Price-time priority — FIFO at same price ───────────────────────────
void test_price_time_priority() {
    std::printf(BLD CYN "\n[6] Price-time priority at same price\n" RST);
    auto bookp = make_book(); auto& book = *bookp;
    CaptureSink sink; TestEngine eng(book, sink);

    // Two sells at same price, different times — id 10 first, then id 20
    eng.submit_limit_order(10, Side::Sell, 2000'0000LL, 50, 1, 0);
    eng.submit_limit_order(20, Side::Sell, 2000'0000LL, 50, 2, 0);
    sink.trades.clear();

    // Aggressive buy 50 — must match id 10 first (oldest at price)
    eng.submit_limit_order(99, Side::Buy, 2000'0000LL, 50, 3, 0);

    CHECK(sink.trades.size() == 1,           "One fill");
    CHECK(sink.trades[0].resting_id == 10,   "Older order (id 10) filled first — FIFO");
    CHECK(book.id_map.find(10) == INVALID_IDX, "id 10 removed (filled)");
    CHECK(book.id_map.find(20) != INVALID_IDX, "id 20 still resting");
}

// ── 7. Cancel order ──────────────────────────────────────────────────────
void test_cancel() {
    std::printf(BLD CYN "\n[7] Cancel resting order\n" RST);
    auto bookp = make_book(); auto& book = *bookp;
    CaptureSink sink; TestEngine eng(book, sink);

    eng.submit_limit_order(1, Side::Buy, 2000'0000LL, 100, 1, 0);
    eng.submit_limit_order(2, Side::Buy, 2000'0000LL, 200, 2, 0);

    bool ok = eng.cancel_order(1);
    CHECK(ok,                                        "Cancel of id 1 succeeds");
    CHECK(book.id_map.find(1) == INVALID_IDX,        "id 1 removed from id_map");
    CHECK(book.id_map.find(2) != INVALID_IDX,        "id 2 still present");
    CHECK(book.level_pool[book.bids.best_level].total_quantity == 200,
          "Level qty = remaining 200 only");
    CHECK(book.level_pool[book.bids.best_level].order_count == 1,
          "Level order_count = 1");

    bool fail = eng.cancel_order(999);
    CHECK(!fail,                                     "Cancel of unknown id returns false");
}

// ── 8. Cancel last order at price level — level removed ──────────────────
void test_cancel_drains_level() {
    std::printf(BLD CYN "\n[8] Cancel drains level\n" RST);
    auto bookp = make_book(); auto& book = *bookp;
    CaptureSink sink; TestEngine eng(book, sink);

    eng.submit_limit_order(1, Side::Buy, 2000'0000LL, 100, 1, 0);
    eng.cancel_order(1);

    CHECK(book.bids.active_levels == 0,                  "No active levels left");
    CHECK(book.bids.best_level == INVALID_IDX,           "best_level reset");
    std::uint32_t off = book.bids.tick_to_offset(2000'0000LL);
    CHECK(book.bids.price_index[off] == INVALID_IDX,     "price_index slot cleared");
}

// ── 9. IOC — no rest of remainder ────────────────────────────────────────
void test_ioc_no_rest() {
    std::printf(BLD CYN "\n[9] IOC — remainder discarded\n" RST);
    auto bookp = make_book(); auto& book = *bookp;
    CaptureSink sink; TestEngine eng(book, sink);

    eng.submit_limit_order(1, Side::Sell, 2000'0000LL, 30, 1, 0);
    sink.trades.clear();

    OrderIdx rem = eng.submit_limit_order(2, Side::Buy, 2000'0000LL, 100, 2, FLAG_IOC);

    CHECK(rem == INVALID_IDX,                  "IOC remainder NOT rested");
    CHECK(sink.trades.size() == 1,              "One fill (30 shares)");
    CHECK(sink.trades[0].quantity == 30,        "Filled what was available");
    CHECK(book.bids.active_levels == 0,        "No resting bid created");
    CHECK(book.id_map.find(2) == INVALID_IDX,  "IOC id 2 not in id_map");
}

// ── 10. No-cross — buy below ask just rests ──────────────────────────────
void test_no_cross_rests() {
    std::printf(BLD CYN "\n[10] No cross — buy below ask rests\n" RST);
    auto bookp = make_book(); auto& book = *bookp;
    CaptureSink sink; TestEngine eng(book, sink);

    eng.submit_limit_order(1, Side::Sell, 2000'0100LL, 100, 1, 0);
    sink.trades.clear();

    OrderIdx rem = eng.submit_limit_order(2, Side::Buy, 2000'0000LL, 50, 2, 0);

    CHECK(sink.trades.empty(),                  "No trade — no cross");
    CHECK(rem != INVALID_IDX,                  "Buy rests on bid side");
    CHECK(book.bids.active_levels == 1,        "Bid side has 1 level");
    CHECK(book.asks.active_levels == 1,        "Ask side untouched");
}

// ── 11. Duplicate id rejected ────────────────────────────────────────────
void test_duplicate_id_rejected() {
    std::printf(BLD CYN "\n[11] Duplicate order id rejected\n" RST);
    auto bookp = make_book(); auto& book = *bookp;
    CaptureSink sink; TestEngine eng(book, sink);

    OrderIdx r1 = eng.submit_limit_order(1, Side::Buy, 2000'0000LL, 100, 1, 0);
    OrderIdx r2 = eng.submit_limit_order(1, Side::Buy, 2000'0100LL, 50, 2, 0);

    CHECK(r1 != INVALID_IDX,    "First submit OK");
    CHECK(r2 == INVALID_IDX,    "Duplicate id rejected");
    CHECK(book.bids.active_levels == 1, "Only one level (from first submit)");
}

// ── 12. Sorted active-level list — best price first ──────────────────────
void test_sorted_levels() {
    std::printf(BLD CYN "\n[12] Sorted active-level list\n" RST);
    auto bookp = make_book(); auto& book = *bookp;
    CaptureSink sink; TestEngine eng(book, sink);

    // Insert bids out of order — best (highest) should end up at best_level
    eng.submit_limit_order(1, Side::Buy, 2000'0050LL, 10, 1, 0);
    eng.submit_limit_order(2, Side::Buy, 2000'0100LL, 10, 2, 0);
    eng.submit_limit_order(3, Side::Buy, 2000'0010LL, 10, 3, 0);

    PriceLevelIdx b = book.bids.best_level;
    CHECK(book.level_pool[b].price == 2000'0100LL,     "Best bid = highest price (2000.01)");
    PriceLevelIdx n = book.level_pool[b].next_level;
    CHECK(book.level_pool[n].price == 2000'0050LL,     "Next bid = 2000.0050");
    PriceLevelIdx nn = book.level_pool[n].next_level;
    CHECK(book.level_pool[nn].price == 2000'0010LL,    "Next-next bid = 2000.0010");

    // Asks: insert out of order, lowest should be best
    eng.submit_limit_order(10, Side::Sell, 2001'0050LL, 10, 1, 0);
    eng.submit_limit_order(11, Side::Sell, 2001'0010LL, 10, 2, 0);
    eng.submit_limit_order(12, Side::Sell, 2001'0100LL, 10, 3, 0);

    PriceLevelIdx a = book.asks.best_level;
    CHECK(book.level_pool[a].price == 2001'0010LL,     "Best ask = lowest price (2001.0010)");
}

// ── 13. Throughput micro-bench ───────────────────────────────────────────
void bench_throughput() {
    std::printf(BLD CYN "\n[13] Throughput: insert + match mix\n" RST);

    constexpr int N = 100'000;
    auto bookp = make_book(); auto& book = *bookp;
    NullSink sink;
    MatchingEngine eng(book, sink);   // NullSink — counts trades only, no capture overhead

    // Pre-populate one side
    for (int i = 0; i < N/2; ++i) {
        PriceTicks p = 2000'0000LL + (i % 100);
        eng.submit_limit_order(i+1, Side::Sell, p, 10, i, 0);
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    // Mix: aggressive buys that fully cross
    for (int i = 0; i < N/2; ++i) {
        PriceTicks p = 2000'0000LL + 99;  // crosses everything <= 2000.0099
        eng.submit_limit_order(1'000'000 + i, Side::Buy, p, 5, i, FLAG_IOC);
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    double ns_total = std::chrono::duration<double, std::nano>(t1 - t0).count();
    double ns_per   = ns_total / (N/2);
    std::printf("  %d aggressive orders in %.0f us → %.1f ns/order (%llu trades total)\n",
                N/2, ns_total/1e3, ns_per,
                (unsigned long long)sink.trade_count);
    CHECK(ns_per < 200.0,                  "< 200 ns/order under load (debug-build budget)");
    CHECK(sink.trade_count > 0,            "Some trades occurred");
}

// ── Entry point ───────────────────────────────────────────────────────────
int main() {
    std::printf(BLD "\n══════════════════════════════════════════\n");
    std::printf(    "   NanoMatch — STEP 2 Match Engine Tests\n");
    std::printf(    "══════════════════════════════════════════\n" RST);

    test_rest_on_empty();
    test_single_full_match();
    test_partial_fill_rests();
    test_partial_maker_stays();
    test_multi_level_walk();
    test_price_time_priority();
    test_cancel();
    test_cancel_drains_level();
    test_ioc_no_rest();
    test_no_cross_rests();
    test_duplicate_id_rejected();
    test_sorted_levels();
    bench_throughput();

    std::printf(BLD "\n══════════════════════════════════════════\n");
    std::printf("  Results: %d / %d tests passed\n", g_passed, g_tests);
    std::printf("══════════════════════════════════════════\n\n" RST);

    return (g_passed == g_tests) ? 0 : 1;
}