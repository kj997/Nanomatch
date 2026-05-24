// benchmarks/run_feed.cpp
// End-to-end: load synthetic CSV via mmap → engine → report perf + stats.
//
// Usage:  ./run_feed [path-to-csv]    (default: data/feed.csv)
//
// Reports:
//   - Total rows processed
//   - Total trades emitted
//   - Wall time, throughput (M rows/sec), avg ns/row
//   - Final book state (active levels, top of book if any)

#include "nanomatch/csv_parser.hpp"
#include "nanomatch/matching_engine.hpp"
#include "nanomatch/inbound_order.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

using namespace nanomatch;

// A counting sink that also tracks total filled qty + first/last trade ts
struct StatsSink {
    std::uint64_t trade_count    = 0;
    std::uint64_t filled_volume  = 0;
    Timestamp     first_trade_ts = 0;
    Timestamp     last_trade_ts  = 0;
    void on_trade(const TradeReport& t) noexcept {
        if (trade_count == 0) first_trade_ts = t.ts_ns;
        last_trade_ts = t.ts_ns;
        ++trade_count;
        filled_volume += t.quantity;
    }
};

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "data/feed.csv";

    std::printf("\n══════════════════════════════════════════════════════════════════\n");
    std::printf("   NanoMatch — feed driver\n");
    std::printf("══════════════════════════════════════════════════════════════════\n");
    std::printf("  Feed file : %s\n", path);

    CsvParser parser;
    if (!parser.open(path)) {
        std::fprintf(stderr, "  ERROR: cannot open %s\n", path);
        std::fprintf(stderr, "  Hint:  generate one with `python tools/gen_synthetic.py`\n");
        return 1;
    }

    auto book = std::make_unique<OrderBook>();
    // Base just below the synthetic ref price (2000.0000) so the 1000-tick
    // window in the generator lands comfortably inside the 65536-tick window.
    book->init(1999'5000LL, 1999'5000LL);

    StatsSink sink;
    MatchingEngineT<StatsSink> eng(*book, sink);

    InboundOrder rec{};
    std::uint64_t rows = 0, adds = 0, cancels = 0, mods = 0, skips = 0;

    auto t0 = std::chrono::steady_clock::now();
    while (parser.next(rec)) {
        ++rows;
        switch (rec.op) {
            case Op::Add: {
                [[maybe_unused]] auto r = eng.submit_limit_order(
                    rec.id, rec.side, rec.price, rec.qty, rec.ts_ns, rec.flags);
                ++adds;
                break;
            }
            case Op::Cancel:
                eng.cancel_order(rec.id);     // false on unknown id is fine
                ++cancels;
                break;
            case Op::Modify:
                // Modify in this engine is "cancel + add" — done at the feed layer.
                // The CSV format doesn't carry both old+new ids, so we just count.
                ++mods;
                break;
            default:
                ++skips;
                break;
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    parser.close();

    double sec     = std::chrono::duration<double>(t1 - t0).count();
    double ns_row  = (sec * 1e9) / std::max<std::uint64_t>(rows, 1);
    double m_rows  = (rows / sec) / 1e6;

    std::printf("\n  Processed: %llu rows in %.3f s   (%.2f M rows/sec  |  %.1f ns/row)\n",
                static_cast<unsigned long long>(rows), sec, m_rows, ns_row);
    std::printf("  Mix      : %llu Add  /  %llu Cancel  /  %llu Modify  /  %llu skipped\n",
                static_cast<unsigned long long>(adds),
                static_cast<unsigned long long>(cancels),
                static_cast<unsigned long long>(mods),
                static_cast<unsigned long long>(skips));

    std::printf("\n  Trades   : %llu trades emitted, %llu shares filled\n",
                static_cast<unsigned long long>(sink.trade_count),
                static_cast<unsigned long long>(sink.filled_volume));

    if (sink.trade_count > 0) {
        double feed_span_us =
            (sink.last_trade_ts - sink.first_trade_ts) / 1e3;
        std::printf("  Feed span: %.1f us (first trade ts → last trade ts)\n",
                    feed_span_us);
    }

    std::printf("\n  Final book:\n");
    std::printf("    bids: %u active levels", book->bids.active_levels);
    if (book->bids.best_level != INVALID_IDX) {
        auto& lvl = book->level_pool[book->bids.best_level];
        std::printf("   best=%lld (qty %u, %u orders)",
                    static_cast<long long>(lvl.price),
                    lvl.total_quantity, lvl.order_count);
    }
    std::printf("\n    asks: %u active levels", book->asks.active_levels);
    if (book->asks.best_level != INVALID_IDX) {
        auto& lvl = book->level_pool[book->asks.best_level];
        std::printf("   best=%lld (qty %u, %u orders)",
                    static_cast<long long>(lvl.price),
                    lvl.total_quantity, lvl.order_count);
    }
    std::printf("\n══════════════════════════════════════════════════════════════════\n\n");
    return 0;
}