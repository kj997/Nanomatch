#pragma once
#include "platform.hpp"
#include "order.hpp"

namespace nanomatch {

// ─────────────────────────────────────────────────────────────────────────
// InboundOrder — single normalized message type consumed by the engine.
//
// Real feeds emit many message types (ITCH has 22+). We collapse them into
// one fixed-size struct with an Op discriminator. Engine handles four ops:
//   - Add     : new limit order
//   - Cancel  : remove resting order by id
//   - Modify  : change quantity of resting order (NASDAQ semantics: cancel+replace)
//   - Execute : exchange-side fill notification (informational; skipped in matching mode)
//
// Fixed 64 bytes — one cache line. SPSC ring carries these.
// ─────────────────────────────────────────────────────────────────────────

enum class Op : std::uint8_t {
    Add     = 0,
    Cancel  = 1,
    Modify  = 2,
    Execute = 3,
    Noop    = 0xFF,  // skipped (system events, trading halts, etc.)
};

struct alignas(CACHE_LINE_SIZE) InboundOrder {
    // 8-byte fields first to avoid implicit padding
    OrderId       id;              // 8 @ 0
    PriceTicks    price;           // 8 @ 8
    Timestamp     ts_ns;           // 8 @ 16
    Quantity      qty;             // 4 @ 24
    Op            op;              // 1 @ 28
    Side          side;            // 1 @ 29
    std::uint8_t  flags;           // 1 @ 30
    std::uint8_t  _pad1;           // 1 @ 31  → 32 bytes used
    std::uint8_t  _pad2[32];       // 32 @ 32 → total = 64
};

static_assert(sizeof(InboundOrder)  == 64, "InboundOrder must be one cache line");
static_assert(alignof(InboundOrder) == 64, "InboundOrder must be cache-line aligned");

} // namespace nanomatch