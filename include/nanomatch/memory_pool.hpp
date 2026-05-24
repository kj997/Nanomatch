#pragma once
#include "order.hpp"
#include <cstddef>
#include <cstdint>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <new>

// ─── Platform detection ──────────────────────────────────────────────────
#if defined(__linux__)
  #include <sys/mman.h>
  #include <unistd.h>
  #define NANOMATCH_HAS_MMAP 1
  #ifndef MAP_HUGETLB
    #define MAP_HUGETLB 0x40000   // fallback if old headers
  #endif
  #ifndef MAP_HUGE_2MB
    #define MAP_HUGE_2MB (21 << 26)
  #endif
  #ifndef MADV_HUGEPAGE
    #define MADV_HUGEPAGE 14
  #endif
#else
  #define NANOMATCH_HAS_MMAP 0
  #if defined(_WIN32)
    #include <malloc.h>   // _aligned_malloc / _aligned_free
  #endif
#endif

namespace nanomatch {

// ─────────────────────────────────────────────────────────────────────────
// Storage policy — back-end for the pool's byte block.
// Two impls below: StaticStorage (compile-time array, default) and
// HugePageStorage (mmap-backed with 2MB pages + MAP_POPULATE).
//
// Both expose:   data() → std::byte*    bytes() → size_t
// ─────────────────────────────────────────────────────────────────────────

// ─── Static storage (compile-time array, no syscalls) ────────────────────
// Use for tests, small pools, non-Linux builds. Embeds memory in the
// containing object. Sizes up to a few MB are fine.
template <typename T, std::size_t Capacity>
class StaticStorage {
public:
    StaticStorage() noexcept = default;
    StaticStorage(const StaticStorage&) = delete;
    StaticStorage& operator=(const StaticStorage&) = delete;

    [[gnu::always_inline]] std::byte* data()    noexcept { return slots_[0].data; }
    [[gnu::always_inline]] std::size_t bytes()  const noexcept { return Capacity * SlotSize; }
    [[gnu::always_inline]] std::byte* slot_at(std::size_t i) noexcept {
        return slots_[i].data;
    }
    static constexpr const char* backend_name() { return "static"; }

private:
    static constexpr std::size_t SlotSize = sizeof(T);
    struct alignas(T) Slot { std::byte data[SlotSize]; };
    Slot slots_[Capacity];
};

// ─── Huge-page mmap storage ──────────────────────────────────────────────
// Tries strategies in order:
//   1. mmap(MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB | MAP_POPULATE)
//   2. mmap(MAP_ANONYMOUS | MAP_POPULATE) + madvise(MADV_HUGEPAGE)   (THP)
//   3. aligned alloc fallback (non-Linux or no huge-page support)
//
// On success records which strategy succeeded → reportable via backend_name().
// Pre-faults all pages (MAP_POPULATE / explicit touch) so no page faults
// occur on the hot path during trading.
template <typename T, std::size_t Capacity>
class HugePagePool_Storage {
public:
    static constexpr std::size_t SlotSize  = sizeof(T);
    static constexpr std::size_t SlotAlign = alignof(T);
    static constexpr std::size_t TotalBytes = Capacity * SlotSize;
    static constexpr std::size_t HUGE_PAGE_SIZE = 2 * 1024 * 1024;

    HugePagePool_Storage() noexcept : data_(nullptr), bytes_(TotalBytes), backend_("uninit") {
        // Round up to multiple of 2 MB for hugetlb allocator
        std::size_t map_bytes =
            (TotalBytes + HUGE_PAGE_SIZE - 1) & ~(HUGE_PAGE_SIZE - 1);

#if NANOMATCH_HAS_MMAP
        // Strategy 1: explicit hugetlb 2 MB pages, pre-faulted
        void* p = ::mmap(
            nullptr, map_bytes,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB | MAP_POPULATE,
            -1, 0);

        if (p != MAP_FAILED) {
            data_    = static_cast<std::byte*>(p);
            backend_ = "mmap(MAP_HUGETLB|2MB|POPULATE)";
            return;
        }

        // Strategy 2: regular mmap + transparent huge pages hint
        p = ::mmap(
            nullptr, map_bytes,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
            -1, 0);

        if (p != MAP_FAILED) {
            ::madvise(p, map_bytes, MADV_HUGEPAGE);
            // Explicit pre-touch — POPULATE may not cover all THP-promoted pages
            std::byte* bp = static_cast<std::byte*>(p);
            for (std::size_t off = 0; off < map_bytes; off += 4096) {
                bp[off] = std::byte{0};
            }
            data_    = bp;
            backend_ = "mmap(POPULATE)+madvise(HUGEPAGE)";
            return;
        }
#endif
        // Strategy 3: portable fallback (Linux non-mmap, macOS, Windows)
        void* p3 = nullptr;
#if defined(_WIN32)
        // MSVC / MinGW: use _aligned_malloc, freed with _aligned_free
        p3 = _aligned_malloc(
            (map_bytes + SlotAlign - 1) & ~(SlotAlign - 1),
            SlotAlign < 64 ? 64 : SlotAlign);
#elif defined(__APPLE__) || defined(_POSIX_C_SOURCE)
        // POSIX path — posix_memalign always available
        if (::posix_memalign(&p3,
                             SlotAlign < 64 ? 64 : SlotAlign,
                             (map_bytes + SlotAlign - 1) & ~(SlotAlign - 1)) != 0) {
            p3 = nullptr;
        }
#else
        // C11 aligned_alloc — Linux glibc, etc.
        p3 = ::aligned_alloc(SlotAlign < 64 ? 64 : SlotAlign,
                             (map_bytes + SlotAlign - 1) & ~(SlotAlign - 1));
#endif
        if (p3) {
            std::memset(p3, 0, map_bytes);   // pre-fault via touch
            data_    = static_cast<std::byte*>(p3);
            backend_ = "aligned_alloc(fallback)";
            return;
        }

        // Total failure — leave data_ null, caller's allocate() will fail safely.
        backend_ = "ALLOCATION FAILED";
    }

    ~HugePagePool_Storage() noexcept {
        if (!data_) return;
#if NANOMATCH_HAS_MMAP
        if (backend_[0] == 'm') {       // "mmap..." → munmap
            std::size_t map_bytes =
                (TotalBytes + HUGE_PAGE_SIZE - 1) & ~(HUGE_PAGE_SIZE - 1);
            ::munmap(data_, map_bytes);
            return;
        }
#endif
#if defined(_WIN32)
        _aligned_free(data_);
#else
        std::free(data_);
#endif
    }

    HugePagePool_Storage(const HugePagePool_Storage&) = delete;
    HugePagePool_Storage& operator=(const HugePagePool_Storage&) = delete;

    [[gnu::always_inline]] std::byte* data()   noexcept { return data_; }
    [[gnu::always_inline]] std::size_t bytes() const noexcept { return bytes_; }
    [[gnu::always_inline]] std::byte* slot_at(std::size_t i) noexcept {
        return data_ + (i * SlotSize);
    }
    const char* backend_name() const noexcept { return backend_; }

private:
    std::byte*  data_;
    std::size_t bytes_;
    const char* backend_;
};

// ─────────────────────────────────────────────────────────────────────────
// MemoryPool — generic over storage policy.
//
// Default: StaticStorage. Switch with MemoryPoolHuge alias below.
// Public API identical across both.
// ─────────────────────────────────────────────────────────────────────────
template <typename T, std::size_t Capacity,
          template <typename, std::size_t> class Storage = StaticStorage>
class MemoryPool {
    static_assert(sizeof(T) >= sizeof(std::uint32_t),
                  "T must hold a free-list link (>= 4 bytes)");
    static_assert(Capacity <= 0xFFFFFFFEu,
                  "Capacity must leave room for INVALID_IDX sentinel");

public:
    MemoryPool() noexcept : free_head_(INVALID_IDX), high_water_(0) {}

    // O(1) — pops free-list, else bumps high-water mark.
    [[nodiscard]] std::uint32_t allocate() noexcept {
        if (free_head_ != INVALID_IDX) {
            std::uint32_t idx = free_head_;
            free_head_ = *reinterpret_cast<std::uint32_t*>(storage_.slot_at(idx));
            return idx;
        }
        if (high_water_ < Capacity) return high_water_++;
        return INVALID_IDX;
    }

    // O(1) — push slot onto intrusive free list (LIFO, cache-hot).
    void deallocate(std::uint32_t idx) noexcept {
        assert(idx < Capacity);
        *reinterpret_cast<std::uint32_t*>(storage_.slot_at(idx)) = free_head_;
        free_head_ = idx;
    }

    [[gnu::always_inline]] T& operator[](std::uint32_t idx) noexcept {
        assert(idx < Capacity);
        return *std::launder(reinterpret_cast<T*>(storage_.slot_at(idx)));
    }
    [[gnu::always_inline]] const T& operator[](std::uint32_t idx) const noexcept {
        assert(idx < Capacity);
        return *std::launder(reinterpret_cast<const T*>(
            const_cast<Storage<T, Capacity>&>(storage_).slot_at(idx)));
    }

    std::size_t capacity()   const noexcept { return Capacity; }
    std::size_t high_water() const noexcept { return high_water_; }
    const char* backend()    const noexcept { return storage_.backend_name(); }

private:
    Storage<T, Capacity> storage_;
    std::uint32_t        free_head_;
    std::uint32_t        high_water_;
};

// Convenience alias — huge-page-backed pool for production
template <typename T, std::size_t Capacity>
using MemoryPoolHuge = MemoryPool<T, Capacity, HugePagePool_Storage>;

} // namespace nanomatch