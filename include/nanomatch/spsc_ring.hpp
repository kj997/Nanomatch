#pragma once
#include "platform.hpp"
#include "order.hpp"      // CACHE_LINE_SIZE
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace nanomatch {

// ─────────────────────────────────────────────────────────────────────────
// SpscRing<T, Capacity>
//
// Single-Producer / Single-Consumer wait-free ring buffer.
// One thread calls try_push() / try_emplace(). One *different* thread
// calls try_pop() / front()/pop(). Crossing roles = UB.
//
// Layout (per cache line, no false sharing):
//   [ producer head + cached_tail | pad ]   ← producer thread reads/writes
//   [ consumer tail + cached_head | pad ]   ← consumer thread reads/writes
//   [ slots[Capacity]                  ]    ← payload
//
// Capacity must be a power of two — wrap = AND instead of modulo.
//
// Memory ordering:
//   producer: relaxed-load own head, acquire-load consumer's tail (cached),
//             release-store new head after writing slot.
//   consumer: relaxed-load own tail, acquire-load producer's head (cached),
//             release-store new tail after reading slot.
//
// This is the Vyukov SPSC pattern. Each side has only one global atomic
// to publish (release), and reads the other side's atomic only when its
// own cached value says full/empty.
// ─────────────────────────────────────────────────────────────────────────

template <typename T, std::size_t Capacity>
class SpscRing {
    static_assert(Capacity >= 2,                          "Capacity must be >= 2");
    static_assert((Capacity & (Capacity - 1)) == 0,       "Capacity must be power of 2");
    static_assert(std::is_trivially_destructible_v<T>,    "T must be trivially destructible (hot path)");
    static_assert(std::is_nothrow_move_constructible_v<T> ||
                  std::is_nothrow_copy_constructible_v<T>,
                  "T must be nothrow-constructible (hot path is noexcept)");

    static constexpr std::size_t MASK = Capacity - 1;

public:
    SpscRing() noexcept
        : head_(0), cached_tail_(0),
          tail_(0), cached_head_(0) {}

    SpscRing(const SpscRing&)            = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    // ─── Producer side ───────────────────────────────────────────────────

    // Returns false if ring is full. Otherwise copies/moves item into slot.
    template <typename U>
    [[nodiscard]] NM_ALWAYS_INLINE
    bool try_push(U&& item) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = head + 1;

        // Cheap check: use cached tail (set during prior push when we observed it)
        if (next - cached_tail_ > Capacity) {
            // Refresh — maybe consumer has advanced
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (next - cached_tail_ > Capacity) return false;   // truly full
        }

        new (slot_ptr(head & MASK)) T(std::forward<U>(item));
        head_.store(next, std::memory_order_release);
        return true;
    }

    // In-place construct — avoids one move/copy when caller can build args directly.
    template <typename... Args>
    [[nodiscard]] NM_ALWAYS_INLINE
    bool try_emplace(Args&&... args) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = head + 1;

        if (next - cached_tail_ > Capacity) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (next - cached_tail_ > Capacity) return false;
        }

        new (slot_ptr(head & MASK)) T(std::forward<Args>(args)...);
        head_.store(next, std::memory_order_release);
        return true;
    }

    // ─── Consumer side ───────────────────────────────────────────────────

    // Returns false if ring is empty. Otherwise moves item out.
    [[nodiscard]] NM_ALWAYS_INLINE
    bool try_pop(T& out) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);

        if (tail == cached_head_) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (tail == cached_head_) return false;   // truly empty
        }

        T* slot = slot_ptr(tail & MASK);
        out = std::move(*slot);
        // Trivially destructible by static_assert — no explicit dtor call needed.
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Peek at the next slot without removing. Returns nullptr if empty.
    // Pair with pop_consume() once you've consumed it. Zero-copy access.
    [[nodiscard]] NM_ALWAYS_INLINE
    T* front() noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == cached_head_) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (tail == cached_head_) return nullptr;
        }
        return slot_ptr(tail & MASK);
    }

    // Advance tail after caller is done with the front() pointer.
    NM_ALWAYS_INLINE
    void pop_consume() noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        tail_.store(tail + 1, std::memory_order_release);
    }

    // ─── Observers (safe from either thread, approximate) ───────────────
    // size_approx is not synchronized — may briefly read stale values.
    std::size_t size_approx() const noexcept {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return h - t;
    }
    bool        empty_approx() const noexcept { return size_approx() == 0; }
    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    NM_ALWAYS_INLINE
    T* slot_ptr(std::size_t idx) noexcept {
        return std::launder(reinterpret_cast<T*>(&slots_[idx].data));
    }

    // ─── Producer cache line ─────────────────────────────────────────────
    alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> head_;
    std::size_t cached_tail_;
    char _pad_p[CACHE_LINE_SIZE - sizeof(std::atomic<std::size_t>) - sizeof(std::size_t)];

    // ─── Consumer cache line ─────────────────────────────────────────────
    alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> tail_;
    std::size_t cached_head_;
    char _pad_c[CACHE_LINE_SIZE - sizeof(std::atomic<std::size_t>) - sizeof(std::size_t)];

    // ─── Payload ─────────────────────────────────────────────────────────
    struct alignas(T) Slot { std::byte data[sizeof(T)]; };
    alignas(CACHE_LINE_SIZE) Slot slots_[Capacity];
};

} // namespace nanomatch
