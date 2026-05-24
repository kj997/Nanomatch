#pragma once
#include "platform.hpp"
#include "price_level.hpp"
#include "memory_pool.hpp"
#include "id_map.hpp"
#include <algorithm>
#include <iterator>

namespace nanomatch {

inline constexpr std::size_t PRICE_WINDOW_SIZE = 1 << 16;  // 65536 ticks per side

// One side of the book (bids OR asks).
// price_index is a dense array: offset = price_ticks - base_tick
// Active levels are also chained in a sorted intrusive linked list.
struct alignas(CACHE_LINE_SIZE) BookSide {
    // Dense O(1) price lookup: PRICE_WINDOW_SIZE × 4B = 256 KB — fits in L2
    PriceLevelIdx price_index[PRICE_WINDOW_SIZE];

    PriceLevelIdx best_level;       // head of sorted active-level list
    PriceTicks    base_tick;        // price_index[0] corresponds to this tick value
    std::uint32_t active_levels;    // count of populated levels (telemetry)
    std::uint8_t  _pad[52];         // pad to next cache line after the array

    void init(PriceTicks base) noexcept {
        std::fill(std::begin(price_index), std::end(price_index), INVALID_IDX);
        best_level    = INVALID_IDX;
        base_tick     = base;
        active_levels = 0;
    }

    // Convert an absolute price tick to an array offset.
    // Returns INVALID_IDX if out of window — caller handles overflow path.
    NM_ALWAYS_INLINE
    std::uint32_t tick_to_offset(PriceTicks tick) const noexcept {
        std::int64_t offset = tick - base_tick;
        if (NM_UNLIKELY(offset < 0 || static_cast<std::size_t>(offset) >= PRICE_WINDOW_SIZE))
            return INVALID_IDX;
        return static_cast<std::uint32_t>(offset);
    }
};

// The full order book for one instrument.
struct OrderBook {
    BookSide bids;
    BookSide asks;

    // Pools — allocated once at startup, never touched by OS during trading
    MemoryPool<Order,      1 << 16>  order_pool;   // 64K orders  × 64B = 4 MB  (stub size)
    MemoryPool<PriceLevel, 1 << 12>  level_pool;   // 4K  levels  × 64B = 256 KB

    // OrderId → OrderIdx map for O(1) cancel/modify lookup
    // 2× expected live orders → load factor ~0.5 → fast probes
    OrderIdHashMap<1 << 17> id_map;                // 128K slots × 12B = 1.5 MB

    // Full sizes for production (uncomment when huge-page pool is ready in STEP 3):
    // MemoryPool<Order,      1 << 22>  order_pool;  // 4M  orders  × 64B = 256 MB
    // MemoryPool<PriceLevel, 1 << 17>  level_pool;  // 128K levels × 64B = 8 MB

    void init(PriceTicks bid_base, PriceTicks ask_base) noexcept {
        bids.init(bid_base);
        asks.init(ask_base);
    }
};

} // namespace nanomatch