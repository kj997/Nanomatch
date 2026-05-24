// nanomatch/main_step5.cpp
// STEP 5 — ingestion: mmap + CSV parser + ITCH parser + end-to-end engine drive

#include "nanomatch/mapped_file.hpp"
#include "nanomatch/csv_parser.hpp"
#include "nanomatch/itch_parser.hpp"
#include "nanomatch/inbound_order.hpp"
#include "nanomatch/matching_engine.hpp"
#include "nanomatch/spsc_ring.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <memory>
#include <vector>
#include <thread>
#include <atomic>

using namespace nanomatch;

#define GRN "\033[32m"
#define RED "\033[31m"
#define CYN "\033[36m"
#define YLW "\033[33m"
#define BLD "\033[1m"
#define RST "\033[0m"

static int g_tests = 0, g_passed = 0;
#define CHECK(cond, msg) do { ++g_tests;                                 \
    if (cond) { ++g_passed; std::printf(GRN "  [PASS]" RST " %s\n", msg);} \
    else      { std::printf(RED "  [FAIL]" RST " %s\n", msg); }          \
} while(0)

// ── helpers ──────────────────────────────────────────────────────────────
static const char* tmp_path(const char* name) {
    static char buf[512];
#if defined(_WIN32)
    const char* tmp = std::getenv("TEMP");
    if (!tmp) tmp = ".";
    std::snprintf(buf, sizeof(buf), "%s\\nm_%s", tmp, name);
#else
    std::snprintf(buf, sizeof(buf), "/tmp/nm_%s", name);
#endif
    return buf;
}

static bool write_file(const char* path, const void* data, std::size_t bytes) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::size_t w = std::fwrite(data, 1, bytes, f);
    std::fclose(f);
    return w == bytes;
}

// ── 1. MappedFile basic ──────────────────────────────────────────────────
void test_mapped_file_basic() {
    std::printf(BLD CYN "\n[1] MappedFile open/read/close\n" RST);
    const char* p = tmp_path("mf1.bin");
    const char* payload = "Hello, NanoMatch! This is a test mmap payload.";
    std::size_t n = std::strlen(payload);
    CHECK(write_file(p, payload, n), "Test file written to disk");

    MappedFile mf;
    CHECK(mf.open(p), "MappedFile::open succeeds");
    CHECK(mf.is_open(), "is_open() reports true");
    CHECK(mf.size() == n, "size() matches file size");
    std::printf("  backend: %s\n", mf.backend());

    bool match = std::memcmp(mf.data(), payload, n) == 0;
    CHECK(match, "Mapped bytes match file contents (zero-copy)");

    mf.close();
    CHECK(!mf.is_open(), "is_open() false after close()");

    // Re-open works
    CHECK(mf.open(p), "Re-open after close works");
    std::remove(p);
}

// ── 2. MappedFile bad path ───────────────────────────────────────────────
void test_mapped_file_missing() {
    std::printf(BLD CYN "\n[2] MappedFile on missing file\n" RST);
    MappedFile mf;
    CHECK(!mf.open("/no/such/file/nm_does_not_exist.xyz"),
          "open on missing file returns false");
    CHECK(!mf.is_open(), "is_open false after failed open");
}

// ── 3. CsvParser — simple round-trip ─────────────────────────────────────
void test_csv_simple() {
    std::printf(BLD CYN "\n[3] CsvParser — Add/Cancel/Modify round-trip\n" RST);

    const char* p = tmp_path("rows.csv");
    const char* csv =
        "# comment line — should be skipped\n"
        "\n"
        "A,100,B,2000'5000,500,1700000000,0\n"        // note: ' allowed only as comment? we use plain digits
        "A,101,S,20010000,200,1700000001,0\n"
        "C,100,B,0,0,1700000002,0\n"
        "M,101,S,20010000,150,1700000003,0\n"
        "X,101,B,0,50,1700000004,0\n"
        "N,0,B,0,0,1700000005,0\n";

    // Note: digit-grouping apostrophes are C++ literal syntax only — not valid CSV.
    // Rewrite without:
    const char* clean_csv =
        "# comment line — should be skipped\n"
        "\n"
        "A,100,B,20005000,500,1700000000,0\n"
        "A,101,S,20010000,200,1700000001,0\n"
        "C,100,B,0,0,1700000002,0\n"
        "M,101,S,20010000,150,1700000003,0\n"
        "X,101,B,0,50,1700000004,0\n"
        "N,0,B,0,0,1700000005,0\n";
    (void)csv;
    CHECK(write_file(p, clean_csv, std::strlen(clean_csv)), "CSV file written");

    CsvParser parser;
    CHECK(parser.open(p), "CsvParser::open OK");

    InboundOrder rec{};
    int adds = 0, cancels = 0, mods = 0, execs = 0, noops = 0;
    while (parser.next(rec)) {
        switch (rec.op) {
            case Op::Add:     ++adds;    break;
            case Op::Cancel:  ++cancels; break;
            case Op::Modify:  ++mods;    break;
            case Op::Execute: ++execs;   break;
            case Op::Noop:    ++noops;   break;
        }
    }
    parser.close();
    std::remove(p);

    std::printf("  parsed: %dA %dC %dM %dX %dN\n", adds, cancels, mods, execs, noops);
    CHECK(adds    == 2, "Two Add rows parsed");
    CHECK(cancels == 1, "One Cancel row parsed");
    CHECK(mods    == 1, "One Modify row parsed");
    CHECK(execs   == 1, "One Execute row parsed");
    CHECK(noops   == 1, "One Noop row parsed");
}

// ── 4. CsvParser — field correctness ─────────────────────────────────────
void test_csv_fields() {
    std::printf(BLD CYN "\n[4] CsvParser — field values correct\n" RST);

    const char* p = tmp_path("fields.csv");
    const char* csv =
        "A,12345,S,-20005000,777,1700000123456,5\n";
    CHECK(write_file(p, csv, std::strlen(csv)), "field-test file written");

    CsvParser parser;
    [[maybe_unused]] bool _ok = parser.open(p);
    InboundOrder r{};
    CHECK(parser.next(r), "First record parsed");

    CHECK(r.op    == Op::Add,                "op == Add");
    CHECK(r.id    == 12345,                  "id == 12345");
    CHECK(r.side  == Side::Sell,             "side == Sell");
    CHECK(r.price == -20005000LL,            "price (negative) round-trips");
    CHECK(r.qty   == 777,                    "qty == 777");
    CHECK(r.ts_ns == 1700000123456ull,       "ts_ns large uint64 OK");
    CHECK(r.flags == 5,                      "flags == 5 (IOC|post-only)");

    parser.close();
    std::remove(p);
}

// ── 5. ITCH parser — synthetic 'A' Add Order message ─────────────────────
void test_itch_add() {
    std::printf(BLD CYN "\n[5] ItchParser — 'A' Add Order parse\n" RST);

    // Build one ITCH-A message manually.
    // Frame: [u16 BE length][body 36 bytes]
    std::uint8_t buf[2 + 36] = {};
    std::uint16_t len = 36;
    buf[0] = static_cast<std::uint8_t>(len >> 8);
    buf[1] = static_cast<std::uint8_t>(len & 0xFF);

    std::uint8_t* b = buf + 2;
    b[0] = 'A';
    // stock locate (u16): 1
    b[1] = 0; b[2] = 1;
    // tracking number (u16): 0
    b[3] = 0; b[4] = 0;
    // timestamp (u48) = 0x0000_AAAA_BBBB_CCCCull truncated → 0xAAAABBBBCCCC
    b[5]  = 0xAA; b[6]  = 0xAA; b[7]  = 0xBB;
    b[8]  = 0xBB; b[9]  = 0xCC; b[10] = 0xCC;
    // order ref (u64) = 0x0102030405060708
    for (int i = 0; i < 8; ++i) b[11 + i] = static_cast<std::uint8_t>(i + 1);
    // side = 'B'
    b[19] = 'B';
    // shares (u32) = 500
    b[20] = 0; b[21] = 0; b[22] = 0x01; b[23] = 0xF4;
    // symbol = "AAPL    " (8 chars, space padded)
    const char* sym = "AAPL    ";
    for (int i = 0; i < 8; ++i) b[24 + i] = static_cast<std::uint8_t>(sym[i]);
    // price (u32) = 2000'0000 (= $200.0000) = 0x01312D00
    b[32] = 0x01; b[33] = 0x31; b[34] = 0x2D; b[35] = 0x00;

    const char* p = tmp_path("itch_a.bin");
    CHECK(write_file(p, buf, sizeof(buf)), "ITCH-A binary written");

    ItchParser parser;
    CHECK(parser.open(p), "ItchParser::open OK");

    InboundOrder rec{};
    CHECK(parser.next(rec), "next() returns true");
    CHECK(rec.op    == Op::Add,           "Parsed as Op::Add");
    CHECK(rec.id    == 0x0102030405060708ull, "Order ref u64 BE decoded");
    CHECK(rec.side  == Side::Buy,         "Side == Buy");
    CHECK(rec.qty   == 500,               "Qty u32 BE decoded");
    CHECK(rec.price == 2000'0000LL,       "Price u32 BE decoded ($200.0000)");
    CHECK(rec.ts_ns == 0xAAAABBBBCCCCull, "Timestamp u48 BE decoded");

    CHECK(!parser.next(rec), "EOF on second next()");
    parser.close();
    std::remove(p);
}

// ── 6. ITCH parser — Delete (D) and unknown type → Noop ──────────────────
void test_itch_delete_and_noop() {
    std::printf(BLD CYN "\n[6] ItchParser — Delete + unknown type → Noop\n" RST);

    // Build two messages: 'D' (19 bytes) and 'S' (unknown - system event, 12 bytes)
    std::uint8_t buf[2 + 19 + 2 + 12] = {};
    std::size_t off = 0;

    // 'D' Delete
    buf[off++] = 0; buf[off++] = 19;
    buf[off++] = 'D';
    buf[off++] = 0; buf[off++] = 1;     // stock locate
    buf[off++] = 0; buf[off++] = 0;     // tracking
    // ts u48
    buf[off++] = 0x00; buf[off++] = 0x00; buf[off++] = 0x00;
    buf[off++] = 0x00; buf[off++] = 0x00; buf[off++] = 0x42;
    // order ref u64 = 0x1111111111111111
    for (int i = 0; i < 8; ++i) buf[off++] = 0x11;

    // Unknown type 'S' — 12 bytes
    buf[off++] = 0; buf[off++] = 12;
    buf[off++] = 'S';
    for (int i = 0; i < 11; ++i) buf[off++] = 0xAB;

    const char* p = tmp_path("itch_dn.bin");
    CHECK(write_file(p, buf, off), "ITCH file with D + unknown S written");

    ItchParser parser;
    [[maybe_unused]] bool _ok = parser.open(p);
    InboundOrder r{};

    CHECK(parser.next(r),        "First record parsed");
    CHECK(r.op == Op::Cancel,    "'D' → Op::Cancel");
    CHECK(r.id == 0x1111111111111111ull, "Delete id matches");

    CHECK(parser.next(r),        "Second record parsed");
    CHECK(r.op == Op::Noop,      "Unknown type 'S' → Op::Noop");

    CHECK(!parser.next(r),       "EOF after 2 messages");
    parser.close();
    std::remove(p);
}

// ── 7. End-to-end: CSV file → engine → trades ───────────────────────────
void test_end_to_end_csv_drives_engine() {
    std::printf(BLD CYN "\n[7] End-to-end: CSV → engine emits trades\n" RST);

    const char* p = tmp_path("e2e.csv");
    // Scenario:
    //   Seed asks at 200.05, 200.10
    //   Aggressive buy crosses both
    //   Cancel of stale order (none) → no-op handled
    const char* csv =
        "A,1,S,20005000,100,1,0\n"   // sell 100 @ 200.05
        "A,2,S,20010000,150,2,0\n"   // sell 150 @ 200.10
        "A,3,B,20010000,200,3,0\n"   // aggressive buy 200 @ 200.10 — fills 100+100
        "C,999,B,0,0,4,0\n";         // cancel of unknown id — should not crash
    CHECK(write_file(p, csv, std::strlen(csv)), "e2e CSV written");

    auto book = std::make_unique<OrderBook>();
    book->init(1999'5000LL, 1999'5000LL);

    struct CountingSink {
        int trade_count = 0;
        Quantity total_qty = 0;
        void on_trade(const TradeReport& t) noexcept {
            ++trade_count;
            total_qty += t.quantity;
        }
    };
    CountingSink sink;
    MatchingEngineT<CountingSink> eng(*book, sink);

    CsvParser parser;
    [[maybe_unused]] bool _ok = parser.open(p);

    InboundOrder r{};
    int rows = 0, applied = 0;
    while (parser.next(r)) {
        ++rows;
        switch (r.op) {
            case Op::Add:
                eng.submit_limit_order(r.id, r.side, r.price, r.qty, r.ts_ns, r.flags);
                ++applied;
                break;
            case Op::Cancel:
                eng.cancel_order(r.id);   // returns false for unknown — fine
                ++applied;
                break;
            default:
                break;
        }
    }
    parser.close();
    std::remove(p);

    std::printf("  rows=%d applied=%d trades=%d total_qty=%u\n",
                rows, applied, sink.trade_count, sink.total_qty);

    CHECK(rows    == 4,                   "All 4 CSV rows parsed");
    CHECK(applied == 4,                   "All 4 rows dispatched to engine");
    CHECK(sink.trade_count == 2,          "Aggressive buy generated 2 fills");
    CHECK(sink.total_qty   == 200,        "Total filled qty = 100 + 100");
}

// ── 8. Throughput — pure parse cost across many CSV rows ─────────────────
void bench_csv_throughput() {
    std::printf(BLD CYN "\n[8] CSV parse throughput\n" RST);

    constexpr int N = 100'000;
    const char* p = tmp_path("bench.csv");

    // Build large CSV in memory
    std::string s;
    s.reserve(N * 40);
    char line[64];
    for (int i = 0; i < N; ++i) {
        int len = std::snprintf(line, sizeof(line),
                                "A,%d,%c,%d,100,%d,0\n",
                                i, (i & 1) ? 'B' : 'S',
                                20000000 + (i % 1000),
                                i);
        s.append(line, len);
    }
    CHECK(write_file(p, s.data(), s.size()), "Large CSV written");

    CsvParser parser;
    [[maybe_unused]] bool _ok = parser.open(p);

    auto t0 = std::chrono::high_resolution_clock::now();
    InboundOrder r{};
    int count = 0;
    while (parser.next(r)) ++count;
    auto t1 = std::chrono::high_resolution_clock::now();

    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double ns_per = (ms * 1e6) / count;
    double m_per_s = count / (ms / 1e3) / 1e6;
    std::printf("  %d rows in %.2f ms → %.1f ns/row, %.2f M rows/sec\n",
                count, ms, ns_per, m_per_s);

    CHECK(count == N,        "All N rows parsed");
    CHECK(ns_per < 1000.0,   "< 1000 ns/row (loose budget incl. file IO)");
    parser.close();
    std::remove(p);
}

int main() {
    std::printf(BLD "\n══════════════════════════════════════════\n");
    std::printf(    "   NanoMatch — STEP 5 Ingestion Tests\n");
    std::printf(    "══════════════════════════════════════════\n" RST);

    test_mapped_file_basic();
    test_mapped_file_missing();
    test_csv_simple();
    test_csv_fields();
    test_itch_add();
    test_itch_delete_and_noop();
    test_end_to_end_csv_drives_engine();
    bench_csv_throughput();

    std::printf(BLD "\n══════════════════════════════════════════\n");
    std::printf("  Results: %d / %d tests passed\n", g_passed, g_tests);
    std::printf("══════════════════════════════════════════\n\n" RST);
    return (g_passed == g_tests) ? 0 : 1;
}