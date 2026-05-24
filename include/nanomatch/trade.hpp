#pragma once
#include "order.hpp"

namespace nanomatch {

// Trade report — emitted per fill. Consumer reads via ring buffer (STEP 4).
// 64 bytes = one cache line. No false sharing in trade stream.
struct alignas(CACHE_LINE_SIZE) TradeReport {
    OrderId    aggressor_id;   // 8  — incoming (taker) order id
    OrderId    resting_id;     // 8  — book (maker) order id
    PriceTicks price;          // 8  — fill price (maker price by convention)
    Quantity   quantity;       // 4  — fill qty
    Side       aggressor_side; // 1  — Buy = taker hit ask; Sell = taker hit bid
    std::uint8_t  _pad1[3];    // 3
    Timestamp  ts_ns;          // 8  — fill timestamp
    std::uint8_t  _pad2[24];   // 24 → total = 64
};

static_assert(sizeof(TradeReport)  == 64, "TradeReport must be one cache line");
static_assert(alignof(TradeReport) == 64, "TradeReport must be cache-line aligned");

} // namespace nanomatch
