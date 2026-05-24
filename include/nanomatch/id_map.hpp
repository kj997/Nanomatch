#pragma once
#include "order.hpp"
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace nanomatch {

// ─────────────────────────────────────────────────────────────────────────
// OrderIdHashMap
//
// Open-addressed, linear-probed hash table. Fixed capacity. Power-of-2 size.
// Key = OrderId (uint64). Value = OrderIdx (uint32).
//
// Why not std::unordered_map?
//   - Node-based: every entry = heap alloc + pointer chase. Cache disaster.
//   - Bucket array of linked lists. Worst case = lots of indirection.
// Why linear probe?
//   - Cache friendly. Probe sequence walks contiguous memory. Prefetcher win.
//   - No tombstones needed for our pattern: insert at submit, erase at fill/cancel.
//     We use a sentinel-key "EMPTY" + tombstone "DELETED".
//
// Hash = splitmix64. OrderIds from exchanges may be sequential — bad mod-N
// distribution. splitmix64 fixes that with one mul + two xor-shifts.
// ─────────────────────────────────────────────────────────────────────────

template <std::size_t Capacity>
class OrderIdHashMap {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");

    static constexpr OrderId  EMPTY_KEY     = 0;                   // OrderId 0 reserved
    static constexpr OrderId  TOMBSTONE_KEY = 0xFFFFFFFFFFFFFFFFull;
    static constexpr std::size_t MASK = Capacity - 1;

    struct Entry {
        OrderId  key;
        OrderIdx val;
    };

public:
    OrderIdHashMap() noexcept : size_(0) {
        // Init all keys to EMPTY. Touch all pages now — predictable.
        for (std::size_t i = 0; i < Capacity; ++i) {
            table_[i].key = EMPTY_KEY;
            table_[i].val = INVALID_IDX;
        }
    }

    // Insert. Returns false if full or duplicate.
    // O(1) expected. O(N) worst case under heavy collisions (load factor < 0.7 in prod).
    bool insert(OrderId key, OrderIdx val) noexcept {
        if (key == EMPTY_KEY || key == TOMBSTONE_KEY) return false;
        std::size_t idx = hash(key) & MASK;
        for (std::size_t probe = 0; probe < Capacity; ++probe) {
            std::size_t slot = (idx + probe) & MASK;
            OrderId k = table_[slot].key;
            if (k == EMPTY_KEY || k == TOMBSTONE_KEY) {
                table_[slot].key = key;
                table_[slot].val = val;
                ++size_;
                return true;
            }
            if (k == key) return false; // duplicate
        }
        return false; // table full
    }

    // Lookup. Returns INVALID_IDX on miss.
    [[gnu::always_inline]]
    OrderIdx find(OrderId key) const noexcept {
        std::size_t idx = hash(key) & MASK;
        for (std::size_t probe = 0; probe < Capacity; ++probe) {
            std::size_t slot = (idx + probe) & MASK;
            OrderId k = table_[slot].key;
            if (k == key)       return table_[slot].val;
            if (k == EMPTY_KEY) return INVALID_IDX;  // never inserted here
            // tombstone: keep probing
        }
        return INVALID_IDX;
    }

    // Erase. Sets slot to tombstone (preserves probe chain for finds).
    bool erase(OrderId key) noexcept {
        std::size_t idx = hash(key) & MASK;
        for (std::size_t probe = 0; probe < Capacity; ++probe) {
            std::size_t slot = (idx + probe) & MASK;
            OrderId k = table_[slot].key;
            if (k == key) {
                table_[slot].key = TOMBSTONE_KEY;
                table_[slot].val = INVALID_IDX;
                --size_;
                return true;
            }
            if (k == EMPTY_KEY) return false;
        }
        return false;
    }

    std::size_t size() const noexcept { return size_; }
    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    // splitmix64 — fast, high-quality avalanche for sequential keys.
    [[gnu::always_inline]]
    static std::uint64_t hash(std::uint64_t x) noexcept {
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
        x =  x ^ (x >> 31);
        return x;
    }

    Entry       table_[Capacity];
    std::size_t size_;
};

} // namespace nanomatch