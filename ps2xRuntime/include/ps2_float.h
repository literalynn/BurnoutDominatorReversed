#ifndef PS2_FLOAT_H
#define PS2_FLOAT_H

// Floating point as the EE FPU (COP1) and the VUs compute it: there are no
// infinities, NaNs or denormals. An exponent of 255 is an ordinary (huge)
// value, results that overflow become +-FLT_MAX, denormals are zero, a
// division by zero gives +-FLT_MAX and a square root uses the absolute value.
// Host IEEE arithmetic otherwise produces Inf and NaN that then spread through
// game state. Follows PCSX2 (FPU.cpp, VU clamping) and the EE manuals.
// The game thread rounds toward zero, as the EE does (EeScheduler::run sets it).

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

static inline float ps2_fadd(float a, float b) { return ps2_clampf(ps2_clampf(a) + ps2_clampf(b)); }
static inline float ps2_fsub(float a, float b) { return ps2_clampf(ps2_clampf(a) - ps2_clampf(b)); }
static inline float ps2_fmul(float a, float b) { return ps2_clampf(ps2_clampf(a) * ps2_clampf(b)); }

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

static inline __m128 ps2_vadd(__m128 a, __m128 b) { return ps2_clamp_ps(_mm_add_ps(ps2_clamp_ps(a), ps2_clamp_ps(b))); }
static inline __m128 ps2_vsub(__m128 a, __m128 b) { return ps2_clamp_ps(_mm_sub_ps(ps2_clamp_ps(a), ps2_clamp_ps(b))); }
static inline __m128 ps2_vmul(__m128 a, __m128 b) { return ps2_clamp_ps(_mm_mul_ps(ps2_clamp_ps(a), ps2_clamp_ps(b))); }

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
