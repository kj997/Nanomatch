#pragma once
#include <cstdint>
#include <cstddef>   // offsetof

namespace nanomatch {

inline constexpr std::size_t CACHE_LINE_SIZE = 64;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

using OrderIdx      = std::uint32_t;
using PriceLevelIdx = std::uint32_t;
inline constexpr OrderIdx INVALID_IDX = 0xFFFFFFFFu;

using PriceTicks = std::int64_t;
using Quantity   = std::uint32_t;
using OrderId    = std::uint64_t;
using Timestamp  = std::uint64_t;

struct alignas(CACHE_LINE_SIZE) Order {
    // --- Hot fields: first 32 bytes ---
    OrderId       id;               // 8
    PriceTicks    price;            // 8
    Quantity      remaining_qty;    // 4
    Quantity      original_qty;     // 4
    OrderIdx      next;             // 4
    OrderIdx      prev;             // 4
    // ---- 32-byte boundary ----

    // --- Warm fields: bytes 32-64 ---
    PriceLevelIdx level_idx;        // 4  @ 32
    std::uint32_t _pad_warm;        // 4  @ 36  — explicit, aligns ts_ns to 8B boundary
    Timestamp     ts_ns;            // 8  @ 40
    Side          side;             // 1  @ 48
    std::uint8_t  flags;            // 1  @ 49
    std::uint16_t participant_id;   // 2  @ 50
    std::uint8_t  _pad[12];         // 12 @ 52  → total = 64
};

static_assert(sizeof(Order)    == 64, "Order must be exactly one cache line");
static_assert(alignof(Order)   == 64, "Order must be cache-line aligned");
static_assert(offsetof(Order, remaining_qty) == 16, "remaining_qty must be at byte 16");
static_assert(offsetof(Order, next)          == 24, "next must be at byte 24");

} // namespace nanomatch
