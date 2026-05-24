#pragma once

// ─────────────────────────────────────────────────────────────────────────
// platform.hpp — compiler portability shims
//
// Centralises all compiler-specific attributes so headers stay clean.
// Supports: GCC, Clang, MSVC.
// ─────────────────────────────────────────────────────────────────────────

// ── Force-inline ──────────────────────────────────────────────────────────
#if defined(_MSC_VER)
  #define NM_ALWAYS_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
  #define NM_ALWAYS_INLINE [[gnu::always_inline]] inline
#else
  #define NM_ALWAYS_INLINE inline
#endif

// ── Branch prediction hints ───────────────────────────────────────────────
#if defined(__GNUC__) || defined(__clang__)
  #define NM_LIKELY(x)   __builtin_expect(!!(x), 1)
  #define NM_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
  // MSVC 19.26+ has [[likely]]/[[unlikely]] but not __builtin_expect.
  // Safe no-op fallback — optimiser still does a reasonable job.
  #define NM_LIKELY(x)   (x)
  #define NM_UNLIKELY(x) (x)
#endif

// ── Restrict ──────────────────────────────────────────────────────────────
#if defined(_MSC_VER)
  #define NM_RESTRICT __restrict
#else
  #define NM_RESTRICT __restrict__
#endif

// ── Prefetch ─────────────────────────────────────────────────────────────
#if defined(__GNUC__) || defined(__clang__)
  #define NM_PREFETCH_R(addr) __builtin_prefetch((addr), 0, 3)
  #define NM_PREFETCH_W(addr) __builtin_prefetch((addr), 1, 3)
#elif defined(_MSC_VER)
  #include <intrin.h>
  #define NM_PREFETCH_R(addr) _mm_prefetch((const char*)(addr), _MM_HINT_T0)
  #define NM_PREFETCH_W(addr) _mm_prefetch((const char*)(addr), _MM_HINT_T0)
#else
  #define NM_PREFETCH_R(addr) (void)(addr)
  #define NM_PREFETCH_W(addr) (void)(addr)
#endif

// ── Suppress unused-variable warnings ────────────────────────────────────
#define NM_UNUSED(x) (void)(x)