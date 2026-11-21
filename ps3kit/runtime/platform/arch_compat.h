/* Portable replacements for the x86-only intrinsics the runtime used directly.
 * x86-64 keeps the original SSE code paths; AArch64 (Android) gets NEON.
 * Include after <stdint.h>. */
#ifndef PS3_ARCH_COMPAT_H
#define PS3_ARCH_COMPAT_H
#include <stdint.h>
#include <string.h>

#if defined(__aarch64__)
#include <arm_neon.h>
typedef uint8x16_t ps3_v128;
static inline void ps3_cpu_pause(void) { __asm__ volatile("isb" ::: "memory"); }
static inline ps3_v128 ps3_v128_loadu(const void* p) { return vld1q_u8((const uint8_t*)p); }
static inline void ps3_v128_storeu(void* p, ps3_v128 v) { vst1q_u8((uint8_t*)p, v); }
static inline ps3_v128 ps3_v128_xor(ps3_v128 a, ps3_v128 b) { return veorq_u8(a, b); }
static inline ps3_v128 ps3_v128_or(ps3_v128 a, ps3_v128 b) { return vorrq_u8(a, b); }
static inline ps3_v128 ps3_v128_zero(void) { return vdupq_n_u8(0); }
/* true when every byte of v is 0 */
static inline int ps3_v128_is_zero(ps3_v128 v) { return vmaxvq_u8(v) == 0; }
/* cheap monotonic tick counter (cntvct on ARM64: 19.2 MHz class, not core cycles) */
static inline uint64_t ps3_tick(void) { uint64_t v; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v)); return v; }
#elif defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#include <x86intrin.h>
typedef __m128i ps3_v128;
static inline void ps3_cpu_pause(void) { _mm_pause(); }
static inline uint64_t ps3_tick(void) { return __rdtsc(); }
static inline ps3_v128 ps3_v128_loadu(const void* p) { return _mm_loadu_si128((const __m128i*)p); }
static inline void ps3_v128_storeu(void* p, ps3_v128 v) { _mm_storeu_si128((__m128i*)p, v); }
static inline ps3_v128 ps3_v128_xor(ps3_v128 a, ps3_v128 b) { return _mm_xor_si128(a, b); }
static inline ps3_v128 ps3_v128_or(ps3_v128 a, ps3_v128 b) { return _mm_or_si128(a, b); }
static inline ps3_v128 ps3_v128_zero(void) { return _mm_setzero_si128(); }
static inline int ps3_v128_is_zero(ps3_v128 v) { return _mm_movemask_epi8(_mm_cmpeq_epi8(v, _mm_setzero_si128())) == 0xFFFF; }
#else
#error "arch_compat.h: unsupported architecture"
#endif
#endif
