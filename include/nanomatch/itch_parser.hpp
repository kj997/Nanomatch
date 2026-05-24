#pragma once
#include "platform.hpp"
#include "inbound_order.hpp"
#include "mapped_file.hpp"
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace nanomatch {

// ─────────────────────────────────────────────────────────────────────────
// ItchParser — NASDAQ TotalView-ITCH 5.0 binary stream parser (subset).
//
// File framing: each message is prefixed with a 2-byte big-endian length,
// then the message body. First body byte = message type (ASCII).
// We handle the matching-relevant subset:
//
//   'A' Add Order              → Op::Add
//   'F' Add Order w/ MPID      → Op::Add (MPID ignored)
//   'E' Order Executed         → Op::Execute
//   'X' Order Cancel (partial) → Op::Cancel (treated as quantity-only)
//   'D' Order Delete (full)    → Op::Cancel
//   'U' Order Replace          → Op::Modify (cancel old id + add new id)
//
// Everything else → Op::Noop (system events, halts, mood imbalance, etc.)
//
// All multi-byte fields are big-endian (per NASDAQ spec). We byte-swap
// inline. Stock symbol field exists but is ignored at this stage —
// matching engine is single-instrument.
// ─────────────────────────────────────────────────────────────────────────

class ItchParser {
public:
    ItchParser() noexcept = default;

    [[nodiscard]] bool open(const char* path) noexcept {
        if (!file_.open(path)) return false;
        pos_ = file_.data();
        end_ = file_.data() + file_.size();
        return true;
    }

    void close() noexcept { file_.close(); pos_ = end_ = nullptr; }

    [[nodiscard]] bool is_open() const noexcept { return file_.is_open(); }

    // Pull the next message into `out`. Returns false at EOF or malformed frame.
    // For non-matching message types, sets out.op = Op::Noop and returns true
    // so the caller can keep iterating without state loss.
    [[nodiscard]] bool next(InboundOrder& out) noexcept {
        if (pos_ + 2 > end_) return false;   // not enough for length prefix

        // Read big-endian 2-byte length
        std::uint16_t len = read_be16(pos_);
        pos_ += 2;
        if (len == 0 || pos_ + len > end_) return false;  // truncated

        const std::byte* body = pos_;
        pos_ += len;   // advance past this message regardless of outcome

        if (len < 1) { out.op = Op::Noop; return true; }
        char type = static_cast<char>(body[0]);

        switch (type) {
            case 'A': return parse_add(body, len, out, /*has_mpid=*/false);
            case 'F': return parse_add(body, len, out, /*has_mpid=*/true);
            case 'E': return parse_executed(body, len, out);
            case 'X': return parse_cancel_partial(body, len, out);
            case 'D': return parse_delete(body, len, out);
            case 'U': return parse_replace(body, len, out);
            default:  out.op = Op::Noop; return true;
        }
    }

private:
    // ── Big-endian readers ───────────────────────────────────────────────
    NM_ALWAYS_INLINE
    static std::uint16_t read_be16(const std::byte* p) noexcept {
        return (static_cast<std::uint16_t>(p[0]) << 8) |
                static_cast<std::uint16_t>(p[1]);
    }
    NM_ALWAYS_INLINE
    static std::uint32_t read_be32(const std::byte* p) noexcept {
        return  (static_cast<std::uint32_t>(p[0]) << 24)
              | (static_cast<std::uint32_t>(p[1]) << 16)
              | (static_cast<std::uint32_t>(p[2]) <<  8)
              |  static_cast<std::uint32_t>(p[3]);
    }
    NM_ALWAYS_INLINE
    static std::uint64_t read_be48(const std::byte* p) noexcept {
        return  (static_cast<std::uint64_t>(p[0]) << 40)
              | (static_cast<std::uint64_t>(p[1]) << 32)
              | (static_cast<std::uint64_t>(p[2]) << 24)
              | (static_cast<std::uint64_t>(p[3]) << 16)
              | (static_cast<std::uint64_t>(p[4]) <<  8)
              |  static_cast<std::uint64_t>(p[5]);
    }
    NM_ALWAYS_INLINE
    static std::uint64_t read_be64(const std::byte* p) noexcept {
        return  (static_cast<std::uint64_t>(p[0]) << 56)
              | (static_cast<std::uint64_t>(p[1]) << 48)
              | (static_cast<std::uint64_t>(p[2]) << 40)
              | (static_cast<std::uint64_t>(p[3]) << 32)
              | (static_cast<std::uint64_t>(p[4]) << 24)
              | (static_cast<std::uint64_t>(p[5]) << 16)
              | (static_cast<std::uint64_t>(p[6]) <<  8)
              |  static_cast<std::uint64_t>(p[7]);
        }

    // ── Message body parsers ─────────────────────────────────────────────
    // ITCH 5.0 Add Order layout (type 'A', 36 bytes; 'F' adds 4-byte MPID = 40 bytes):
    //   [0]    type 'A'
    //   [1-2]  stock locate (u16)
    //   [3-4]  tracking number (u16)
    //   [5-10] timestamp (u48, ns since midnight)
    //   [11-18] order reference number (u64)
    //   [19]    buy/sell ('B'/'S')
    //   [20-23] shares (u32)
    //   [24-31] stock symbol (8 chars, space-padded)
    //   [32-35] price (u32, fixed-point /10000)
    //   [36-39] attribution / MPID (4 chars)  [only in 'F']
    [[nodiscard]] static bool parse_add(const std::byte* b, std::size_t len,
                                        InboundOrder& out, bool has_mpid) noexcept {
        const std::size_t need = has_mpid ? 40 : 36;
        if (len < need) { out.op = Op::Noop; return true; }
        out.op    = Op::Add;
        out.ts_ns = read_be48(b + 5);
        out.id    = read_be64(b + 11);
        out.side  = (static_cast<char>(b[19]) == 'S') ? Side::Sell : Side::Buy;
        out.qty   = read_be32(b + 20);
        out.price = static_cast<PriceTicks>(read_be32(b + 32));  // already ticks×10^4
        out.flags = 0;
        return true;
    }

    // 'E' Order Executed — 31 bytes
    //   [11-18] order ref  → which order is filling
    //   [19-22] executed shares (u32)
    //   [23-30] match number (ignored)
    [[nodiscard]] static bool parse_executed(const std::byte* b, std::size_t len,
                                              InboundOrder& out) noexcept {
        if (len < 31) { out.op = Op::Noop; return true; }
        out.op    = Op::Execute;
        out.ts_ns = read_be48(b + 5);
        out.id    = read_be64(b + 11);
        out.qty   = read_be32(b + 19);
        out.price = 0;
        out.side  = Side::Buy;  // not meaningful for Execute
        out.flags = 0;
        return true;
    }

    // 'X' Order Cancel (partial) — 23 bytes
    //   [11-18] order ref
    //   [19-22] cancelled shares (u32)  ← reduce remaining qty
    [[nodiscard]] static bool parse_cancel_partial(const std::byte* b, std::size_t len,
                                                    InboundOrder& out) noexcept {
        if (len < 23) { out.op = Op::Noop; return true; }
        out.op    = Op::Modify;   // partial cancel = reduce qty = Modify down
        out.ts_ns = read_be48(b + 5);
        out.id    = read_be64(b + 11);
        out.qty   = read_be32(b + 19);   // qty to reduce by (engine subtracts)
        out.price = 0;
        out.side  = Side::Buy;
        out.flags = 0;
        return true;
    }

    // 'D' Order Delete — 19 bytes (full cancel)
    [[nodiscard]] static bool parse_delete(const std::byte* b, std::size_t len,
                                            InboundOrder& out) noexcept {
        if (len < 19) { out.op = Op::Noop; return true; }
        out.op    = Op::Cancel;
        out.ts_ns = read_be48(b + 5);
        out.id    = read_be64(b + 11);
        out.qty   = 0;
        out.price = 0;
        out.side  = Side::Buy;
        out.flags = 0;
        return true;
    }

    // 'U' Order Replace — 35 bytes (cancel old id + new order)
    // We emit as Modify carrying NEW id and NEW price/qty. Engine layer
    // handles the cancel-old half (we punt this detail; CSV path doesn't
    // need it).
    [[nodiscard]] static bool parse_replace(const std::byte* b, std::size_t len,
                                             InboundOrder& out) noexcept {
        if (len < 35) { out.op = Op::Noop; return true; }
        out.op    = Op::Modify;
        out.ts_ns = read_be48(b + 5);
        out.id    = read_be64(b + 19);          // new order ref
        out.qty   = read_be32(b + 27);          // new shares
        out.price = static_cast<PriceTicks>(read_be32(b + 31));
        out.side  = Side::Buy;
        out.flags = 0;
        return true;
    }

    MappedFile       file_;
    const std::byte* pos_ = nullptr;
    const std::byte* end_ = nullptr;
};

} // namespace nanomatch