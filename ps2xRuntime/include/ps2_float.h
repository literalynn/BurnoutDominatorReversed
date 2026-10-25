#ifndef PS2_FLOAT_H
#define PS2_FLOAT_H

// Floating point as the EE FPU (COP1) and the VUs compute it: there are no
// infinities, NaNs or denormals. An exponent of 255 is an ordinary (huge)
// value, results that overflow become +-FLT_MAX, denormals are zero, a
// division by zero gives +-FLT_MAX and a square root uses the absolute value.
// Host IEEE arithmetic otherwise produces Inf and NaN that then spread through
// game state. Follows PCSX2 (FPU.cpp, VU clamping) and the EE manuals.
// The game thread rounds toward zero, as the EE does, and reads denormal operands
// as zeros (EeScheduler::run sets both).

#include <cstdint>
#include <cstring>
#include <cmath>

static inline uint32_t ps2_float_bits(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static inline float ps2_bits_float(uint32_t bits)
{
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// Maps a host float onto the values the EE can hold: exponent 255 -> +-FLT_MAX, denormal -> +-0.
static inline float ps2_clampf(float value)
{
    const uint32_t bits = ps2_float_bits(value);
    const uint32_t exponent = bits & 0x7F800000u;
    if (exponent == 0x7F800000u)
        return ps2_bits_float((bits & 0x80000000u) | 0x7F7FFFFFu);
    if (exponent == 0u)
        return ps2_bits_float(bits & 0x80000000u);
    return value;
}

static inline __m128 ps2_clamp_ps(__m128 value)
{
    const __m128i bits = _mm_castps_si128(value);
    const __m128i sign = _mm_and_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000u)));
    const __m128i exponent = _mm_and_si128(bits, _mm_set1_epi32(0x7F800000));
    const __m128i isMax = _mm_cmpeq_epi32(exponent, _mm_set1_epi32(0x7F800000));
    const __m128i isZero = _mm_cmpeq_epi32(exponent, _mm_setzero_si128());
    __m128i result = _mm_or_si128(_mm_and_si128(isMax, _mm_or_si128(sign, _mm_set1_epi32(0x7F7FFFFF))),
                                  _mm_andnot_si128(isMax, bits));
    result = _mm_or_si128(_mm_and_si128(isZero, sign), _mm_andnot_si128(isZero, result));
    return _mm_castsi128_ps(result);
}

#if defined(_M_X64) || defined(__x86_64__)
#if defined(_MSC_VER)
#define PS2_FLOAT_INLINE __forceinline
#else
#define PS2_FLOAT_INLINE inline __attribute__((always_inline))
#endif

// Add, subtract and multiply for the generated code, which runs on the game thread: MSVC kept
// the clamping versions (below) out of line, a call and three branchy clamps per operation.
// There denormal operands read as zeros of their sign, as ps2_clampf makes them, and finite
// operands cannot overflow into an infinity when results round toward zero. Two things are
// left, without branches: operands with exponent 255 and denormal results. The bits are those
// of the clamping versions under the game thread's MXCSR, and only there.

// Exponent 255 -> +-FLT_MAX: as integers, positive patterns above 0x7F7FFFFF and negative
// ones above 0xFF7FFFFF.
static PS2_FLOAT_INLINE __m128 ps2_saturate_ps(__m128 value)
{
    const __m128i bits = _mm_castps_si128(value);
    return _mm_castsi128_ps(_mm_min_epu32(_mm_min_epi32(bits, _mm_set1_epi32(0x7F7FFFFF)),
                                          _mm_set1_epi32(static_cast<int>(0xFF7FFFFFu))));
}

// Exponent 0 -> a zero of the same sign.
static PS2_FLOAT_INLINE __m128 ps2_flush_ps(__m128 value)
{
    const __m128i bits = _mm_castps_si128(value);
    const __m128i tiny = _mm_cmpeq_epi32(_mm_and_si128(bits, _mm_set1_epi32(0x7F800000)), _mm_setzero_si128());
    return _mm_castsi128_ps(_mm_andnot_si128(_mm_srli_epi32(tiny, 1), bits));
}

static PS2_FLOAT_INLINE float ps2_fadd(float a, float b)
{
    return _mm_cvtss_f32(ps2_flush_ps(_mm_add_ss(ps2_saturate_ps(_mm_set_ss(a)), ps2_saturate_ps(_mm_set_ss(b)))));
}
static PS2_FLOAT_INLINE float ps2_fsub(float a, float b)
{
    return _mm_cvtss_f32(ps2_flush_ps(_mm_sub_ss(ps2_saturate_ps(_mm_set_ss(a)), ps2_saturate_ps(_mm_set_ss(b)))));
}
static PS2_FLOAT_INLINE float ps2_fmul(float a, float b)
{
    return _mm_cvtss_f32(ps2_flush_ps(_mm_mul_ss(ps2_saturate_ps(_mm_set_ss(a)), ps2_saturate_ps(_mm_set_ss(b)))));
}
static PS2_FLOAT_INLINE __m128 ps2_vadd(__m128 a, __m128 b) { return ps2_flush_ps(_mm_add_ps(ps2_saturate_ps(a), ps2_saturate_ps(b))); }
static PS2_FLOAT_INLINE __m128 ps2_vsub(__m128 a, __m128 b) { return ps2_flush_ps(_mm_sub_ps(ps2_saturate_ps(a), ps2_saturate_ps(b))); }
static PS2_FLOAT_INLINE __m128 ps2_vmul(__m128 a, __m128 b) { return ps2_flush_ps(_mm_mul_ps(ps2_saturate_ps(a), ps2_saturate_ps(b))); }
#else
static inline float ps2_fadd(float a, float b) { return ps2_clampf(ps2_clampf(a) + ps2_clampf(b)); }
static inline float ps2_fsub(float a, float b) { return ps2_clampf(ps2_clampf(a) - ps2_clampf(b)); }
static inline float ps2_fmul(float a, float b) { return ps2_clampf(ps2_clampf(a) * ps2_clampf(b)); }
static inline __m128 ps2_vadd(__m128 a, __m128 b) { return ps2_clamp_ps(_mm_add_ps(ps2_clamp_ps(a), ps2_clamp_ps(b))); }
static inline __m128 ps2_vsub(__m128 a, __m128 b) { return ps2_clamp_ps(_mm_sub_ps(ps2_clamp_ps(a), ps2_clamp_ps(b))); }
static inline __m128 ps2_vmul(__m128 a, __m128 b) { return ps2_clamp_ps(_mm_mul_ps(ps2_clamp_ps(a), ps2_clamp_ps(b))); }
#endif

// a / b; a zero divisor gives +-FLT_MAX with the sign of a xor b (0/0 included).
static inline float ps2_fdiv(float a, float b)
{
    a = ps2_clampf(a);
    b = ps2_clampf(b);
    if ((ps2_float_bits(b) & 0x7F800000u) == 0u)
        return ps2_bits_float(((ps2_float_bits(a) ^ ps2_float_bits(b)) & 0x80000000u) | 0x7F7FFFFFu);
    return ps2_clampf(a / b);
}

// sqrt(|a|); zero keeps its sign.
static inline float ps2_fsqrt(float a)
{
    a = ps2_clampf(a);
    if ((ps2_float_bits(a) & 0x7F800000u) == 0u)
        return a;
    return std::sqrt(std::fabs(a));
}

// a / sqrt(|b|); a zero b gives +-FLT_MAX with the sign of a xor b.
static inline float ps2_frsqrt(float a, float b)
{
    a = ps2_clampf(a);
    b = ps2_clampf(b);
    if ((ps2_float_bits(b) & 0x7F800000u) == 0u)
        return ps2_bits_float(((ps2_float_bits(a) ^ ps2_float_bits(b)) & 0x80000000u) | 0x7F7FFFFFu);
    return ps2_clampf(a / std::sqrt(std::fabs(b)));
}

// VU MAX/MINI compare raw bit patterns as sign-magnitude integers (exact on
// denormals and exponent 255) and return an operand unchanged (PCSX2 fp_max/fp_min).
static inline __m128 ps2_vminmax(__m128 a, __m128 b, bool takeMax)
{
    const __m128i ia = _mm_castps_si128(a), ib = _mm_castps_si128(b);
    const __m128i bothNegative = _mm_srai_epi32(_mm_and_si128(ia, ib), 31);
    const __m128i greater = _mm_cmpgt_epi32(ia, ib), less = _mm_cmpgt_epi32(ib, ia);
    // Two negatives order in reverse: the larger integer is the smaller float.
    const __m128i aIsMax = _mm_or_si128(_mm_and_si128(bothNegative, less), _mm_andnot_si128(bothNegative, greater));
    const __m128i aIsMin = _mm_or_si128(_mm_and_si128(bothNegative, greater), _mm_andnot_si128(bothNegative, less));
    const __m128i pickA = takeMax ? aIsMax : aIsMin;
    return _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(pickA, ia), _mm_andnot_si128(pickA, ib)));
}
static inline __m128 ps2_vmax(__m128 a, __m128 b) { return ps2_vminmax(a, b, true); }
static inline __m128 ps2_vmin(__m128 a, __m128 b) { return ps2_vminmax(a, b, false); }

#endif // PS2_FLOAT_H
