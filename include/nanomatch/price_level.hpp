#pragma once
#include "order.hpp"
#include <cstddef>   // offsetof

namespace nanomatch {

struct alignas(CACHE_LINE_SIZE) PriceLevel {
    // --- Hot fields ---
    PriceTicks    price;            // 8
    Quantity      total_quantity;   // 4
    std::uint32_t order_count;      // 4
    OrderIdx      head;             // 4  (FIFO front — matches first)
    OrderIdx      tail;             // 4  (FIFO back  — appended here)
    // ---- 24 bytes ----

    // --- Warm fields ---
    PriceLevelIdx next_level;       // 4
    PriceLevelIdx prev_level;       // 4
    std::uint32_t _pad[8];          // 32 → total = 64
};

static_assert(sizeof(PriceLevel)  == 64, "PriceLevel must be exactly one cache line");
static_assert(alignof(PriceLevel) == 64, "PriceLevel must be cache-line aligned");
static_assert(offsetof(PriceLevel, head) == 16, "head must be at byte 16");

} // namespace nanomatch
