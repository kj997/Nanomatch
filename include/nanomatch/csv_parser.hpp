#pragma once
#include "platform.hpp"
#include "inbound_order.hpp"
#include "mapped_file.hpp"
#include <cstdint>
#include <cstddef>

namespace nanomatch {

// ─────────────────────────────────────────────────────────────────────────
// CsvParser — pull one InboundOrder per line.
//
// Schema (one row per message, no header expected; #-prefixed lines skipped):
//   op,id,side,price_ticks,qty,ts_ns,flags
//
// where:
//   op           = "A" Add / "C" Cancel / "M" Modify / "X" Execute / "N" Noop
//   side         = "B" / "S" (only meaningful for Add)
//   price_ticks  = signed int64, fixed-point (e.g. 2000'0000 = $200.0000)
//   qty          = unsigned int32
//   ts_ns        = unsigned int64 nanos since epoch
//   flags        = uint8 bitfield (IOC=1, FOK=2, post-only=4)
//
// All parsing is branchless-ish integer scanning. No std::stoi (slow,
// throws). No allocation per row.
// ─────────────────────────────────────────────────────────────────────────

class CsvParser {
public:
    CsvParser() noexcept = default;

    [[nodiscard]] bool open(const char* path) noexcept {
        if (!file_.open(path)) return false;
        pos_ = file_.data();
        end_ = file_.data() + file_.size();
        return true;
    }

    void close() noexcept { file_.close(); pos_ = end_ = nullptr; }

    // Pull the next record into `out`. Returns false when EOF or on parse error.
    // Sets `out.op = Op::Noop` for skipped lines (comments / blanks).
    [[nodiscard]] bool next(InboundOrder& out) noexcept {
        while (pos_ < end_) {
            // Skip blank lines and # comments
            if (*pos_ == std::byte{'\n'} || *pos_ == std::byte{'\r'}) { ++pos_; continue; }
            if (*pos_ == std::byte{'#'}) { skip_line(); continue; }

            if (parse_row(out)) return true;
            // parse_row consumed the bad line; loop and try the next
        }
        return false;
    }

    [[nodiscard]] bool is_open() const noexcept { return file_.is_open(); }

private:
    NM_ALWAYS_INLINE
    char ch() const noexcept { return static_cast<char>(*pos_); }

    void skip_line() noexcept {
        while (pos_ < end_ && *pos_ != std::byte{'\n'}) ++pos_;
        if (pos_ < end_) ++pos_;
    }

    // Parse signed int64 starting at pos_. Advances pos_. Returns parsed value.
    std::int64_t parse_i64() noexcept {
        std::int64_t sign = 1;
        if (pos_ < end_ && ch() == '-') { sign = -1; ++pos_; }
        std::int64_t val = 0;
        while (pos_ < end_) {
            char c = ch();
            if (c < '0' || c > '9') break;
            val = val * 10 + (c - '0');
            ++pos_;
        }
        return val * sign;
    }

    std::uint64_t parse_u64() noexcept {
        std::uint64_t val = 0;
        while (pos_ < end_) {
            char c = ch();
            if (c < '0' || c > '9') break;
            val = val * 10 + static_cast<std::uint64_t>(c - '0');
            ++pos_;
        }
        return val;
    }

    NM_ALWAYS_INLINE
    bool expect_comma() noexcept {
        if (pos_ < end_ && ch() == ',') { ++pos_; return true; }
        return false;
    }

    [[nodiscard]] bool parse_row(InboundOrder& out) noexcept {
        if (pos_ >= end_) return false;

        // ── op
        char opc = ch(); ++pos_;
        switch (opc) {
            case 'A': out.op = Op::Add;     break;
            case 'C': out.op = Op::Cancel;  break;
            case 'M': out.op = Op::Modify;  break;
            case 'X': out.op = Op::Execute; break;
            case 'N': out.op = Op::Noop;    break;
            default:  skip_line(); return false;
        }
        if (!expect_comma()) { skip_line(); return false; }

        // ── id
        out.id = parse_u64();
        if (!expect_comma()) { skip_line(); return false; }

        // ── side
        char sc = ch(); ++pos_;
        out.side = (sc == 'S') ? Side::Sell : Side::Buy;
        if (!expect_comma()) { skip_line(); return false; }

        // ── price_ticks
        out.price = parse_i64();
        if (!expect_comma()) { skip_line(); return false; }

        // ── qty
        out.qty = static_cast<Quantity>(parse_u64());
        if (!expect_comma()) { skip_line(); return false; }

        // ── ts_ns
        out.ts_ns = parse_u64();
        if (!expect_comma()) { skip_line(); return false; }

        // ── flags
        out.flags = static_cast<std::uint8_t>(parse_u64());

        skip_line();   // consume trailing \n
        return true;
    }

    MappedFile       file_;
    const std::byte* pos_ = nullptr;
    const std::byte* end_ = nullptr;
};

} // namespace nanomatch