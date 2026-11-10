#ifndef PS2_FLOAT_H
#define PS2_FLOAT_H

// Floating point as the EE FPU (COP1) and the VUs compute it: there are no
// infinities, NaNs or denormals. An exponent of 255 is an ordinary (huge)
// value, results that overflow become +-FLT_MAX, denormals are zero, a
// division by zero gives +-FLT_MAX and a square root uses the absolute value.
// Host IEEE arithmetic otherwise produces Inf and NaN that then spread through
// game state. Follows PCSX2 (FPU.cpp, VU clamping) and the EE manuals.
// The game thread rounds toward zero, as the EE's adder and multiplier do, and reads
// denormal operands as zeros (EeScheduler::run sets both). The EE's divider has its own
// results (ps2_ee_divider_div below). What still differs from a console is listed in
// docs/FINDINGS.md (E024).

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

#if defined(_MSC_VER)
#define PS2_FLOAT_NOINLINE __declspec(noinline)
#else
#define PS2_FLOAT_NOINLINE __attribute__((noinline))
#endif

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

// VDIV, VSQRT and VRSQRT of VU0's macro mode (the next three): the host's operations under the
// game thread's rounding, as PCSX2 computes the VUs' and as the VU interpreter here does. No
// console results were found to check the VUs' divider against.

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

// DIV.S, SQRT.S and RSQRT.S as the EE computes them (used when PS2X_EE_EXACT_DIVIDER is 1, see
// below). Its divider is a radix-2 SRT unit: the remainder is kept as a sum word and a carry
// word, each of the 25 quotient digits (-1, 0 or 1) is chosen from an estimate of the remainder,
// and nothing corrects the last digit. The 24 bits kept are therefore neither the rounded nor
// the truncated quotient: of random operands they are the nearest float in 82% of divisions and
// the truncated one in 62% (square roots: 74% and 76%). The algorithm is the one of PCSX2's
// soft-float work (pull request 12001, PS2Float.cpp, GPL-3.0+); it gives every DIV.S, SQRT.S
// and RSQRT.S result that ps2autotests printed on a console (tests/cpu/ee_fpu). Integer code:
// the result does not depend on MXCSR.
// Operands are read as the EE reads them (exponent 255 is an ordinary exponent, exponent 0 is
// zero). A result that needs exponent 255 is given as +-FLT_MAX, like every other result here;
// the EE would keep it, up to 0x7FFFFFFF.

// The next quotient digit. The binary point is between bits 24 and 25; the carry word is added
// to the sum's high bits only, the sum's low 24 bits are OR-ed in.
static inline int32_t ps2_srt_digit(uint32_t sum, uint32_t carry)
{
    const int32_t estimate = static_cast<int32_t>(((sum & 0xFF000000u) + carry) | (sum & 0x00FFFFFFu));
    return estimate >= (1 << 23) ? 1 : estimate < -(1 << 24) ? -1 : 0;
}

// fs / ft; a zero divisor gives +-FLT_MAX with the sign of fs xor ft (0/0 included).
PS2_FLOAT_NOINLINE static inline float ps2_ee_divider_div(float fs, float ft)
{
    const uint32_t a = ps2_float_bits(fs), b = ps2_float_bits(ft);
    const uint32_t sign = (a ^ b) & 0x80000000u;
    const int32_t exponentA = static_cast<int32_t>((a >> 23) & 0xFFu), exponentB = static_cast<int32_t>((b >> 23) & 0xFFu);
    if (exponentB == 0)
        return ps2_bits_float(sign | 0x7F7FFFFFu);
    if (exponentA == 0)
        return ps2_bits_float(sign);
    const uint32_t divisor = ((b & 0x007FFFFFu) | 0x00800000u) << 2;
    uint32_t sum = ((a & 0x007FFFFFu) | 0x00800000u) << 2, carry = 0u, quotient = 0u;
    int32_t digit = 1;
    for (int i = 0; i < 25; ++i)
    {
        quotient = (quotient << 1) + static_cast<uint32_t>(digit);
        // Subtract the divisor for a digit of 1 (its complement, plus one through the carry word), add it for -1.
        const uint32_t addend = digit > 0 ? ~divisor : digit < 0 ? divisor : 0u;
        carry += digit > 0 ? 1u : 0u;
        const uint32_t partial = sum ^ carry;
        const uint32_t nextSum = partial ^ addend;
        const uint32_t nextCarry = ((sum & carry) | (partial & addend)) << 1;
        digit = digit != 0 ? ps2_srt_digit(nextSum, nextCarry) : ps2_srt_digit(sum, carry);
        sum = nextSum << 1;
        carry = nextCarry << 1;
    }
    int32_t exponent = exponentA - exponentB + 126;
    if (quotient >= (1u << 24))
    {
        ++exponent;
        quotient >>= 1;
    }
    if (exponent > 254)
        return ps2_bits_float(sign | 0x7F7FFFFFu);
    if (exponent < 1)
        return ps2_bits_float(sign);
    return ps2_bits_float(sign | (static_cast<uint32_t>(exponent) << 23) | (quotient & 0x007FFFFFu));
}

// sqrt(|ft|); a zero of either sign gives +0.
PS2_FLOAT_NOINLINE static inline float ps2_ee_divider_sqrt(float ft)
{
    const uint32_t a = ps2_float_bits(ft);
    const uint32_t exponent = (a >> 23) & 0xFFu;
    if (exponent == 0u)
        return 0.0f;
    // The root of the mantissa times 2 or 4, so that the exponent left over is even.
    uint32_t sum = ((a & 0x007FFFFFu) | 0x00800000u) << ((a & 0x00800000u) ? 1 : 2), carry = 0u, root = 0u;
    int32_t digit = 1;
    for (int i = 0; i < 25; ++i)
    {
        // A digit d at this place adds d * (2 * root + d) to the root's square, which leaves the remainder.
        const uint32_t adjust = root + (static_cast<uint32_t>(digit) << (24 - i));
        root += static_cast<uint32_t>(digit) << (25 - i);
        const uint32_t addend = digit > 0 ? ~adjust : digit < 0 ? adjust : 0u;
        carry += digit > 0 ? 1u : 0u;
        const uint32_t partial = sum ^ carry;
        const uint32_t nextSum = partial ^ addend;
        const uint32_t nextCarry = ((sum & carry) | (partial & addend)) << 1;
        digit = digit != 0 ? ps2_srt_digit(nextSum, nextCarry) : ps2_srt_digit(sum, carry);
        sum = nextSum << 1;
        carry = nextCarry << 1;
    }
    return ps2_bits_float((((exponent + 127u) >> 1) << 23) | ((root >> 2) & 0x007FFFFFu));
}

// fs / sqrt(|ft|): the divider's two results in a row. A zero ft gives +-FLT_MAX with the sign of fs.
PS2_FLOAT_NOINLINE static inline float ps2_ee_divider_rsqrt(float fs, float ft)
{
    return ps2_ee_divider_div(fs, ps2_ee_divider_sqrt(ft));
}

// What DIV.S, SQRT.S and RSQRT.S give in the generated code. 1: the EE divider's results above,
// a console's bit for bit; each takes about 100 ns on a Ryzen 7 9800X3D, where the host's takes
// under 2, and the game thread's CPU time in a race rose by about 8% (docs/CHANGES.md,
// November 10, 2026). 0, the default: the host's division and square root rounded toward zero,
// which are the console's bits for 62% of divisions and 76% of square roots and one unit in the
// last place away for the others; zeros are handled as on the console either way.
#ifndef PS2X_EE_EXACT_DIVIDER
#define PS2X_EE_EXACT_DIVIDER 0
#endif
#if PS2X_EE_EXACT_DIVIDER
static inline float ps2_ee_fdiv(float fs, float ft) { return ps2_ee_divider_div(fs, ft); }
static inline float ps2_ee_fsqrt(float ft) { return ps2_ee_divider_sqrt(ft); }
static inline float ps2_ee_frsqrt(float fs, float ft) { return ps2_ee_divider_rsqrt(fs, ft); }
#else
static inline float ps2_ee_fdiv(float fs, float ft) { return ps2_fdiv(fs, ft); }
// A zero of either sign gives +0.
static inline float ps2_ee_fsqrt(float ft)
{
    return (ps2_float_bits(ft) & 0x7F800000u) == 0u ? 0.0f : ps2_fsqrt(ft);
}
// A zero ft gives +-FLT_MAX with the sign of fs alone.
static inline float ps2_ee_frsqrt(float fs, float ft)
{
    if ((ps2_float_bits(ft) & 0x7F800000u) == 0u)
        return ps2_bits_float((ps2_float_bits(fs) & 0x80000000u) | 0x7F7FFFFFu);
    return ps2_frsqrt(fs, ft);
}
#endif

// C.EQ.S, C.LT.S and C.LE.S order every bit pattern: exponent 255 is an ordinary exponent and
// exponent 0 is zero (ps2autotests, tests/cpu/ee_fpu/compare). Under the game thread's DAZ the
// host's comparison gives that order unless an operand is one of its NaNs (exponent 255 with a
// mantissa); then the patterns are compared as integers, sign and magnitude. The NaN is looked
// for in the bits: the release build is compiled with /fp:fast (ReleaseMode.cmake), under which
// a float test for NaN is folded away and a comparison with one gives whatever its instructions
// leave in the flags.
static inline int32_t ps2_ee_order(float value)
{
    const uint32_t bits = ps2_float_bits(value);
    if ((bits & 0x7F800000u) == 0u)
        return 0;
    const int32_t magnitude = static_cast<int32_t>(bits & 0x7FFFFFFFu);
    return (bits & 0x80000000u) ? -magnitude : magnitude;
}
PS2_FLOAT_NOINLINE static inline bool ps2_ee_feq_patterns(float a, float b) { return ps2_ee_order(a) == ps2_ee_order(b); }
PS2_FLOAT_NOINLINE static inline bool ps2_ee_flt_patterns(float a, float b) { return ps2_ee_order(a) < ps2_ee_order(b); }
PS2_FLOAT_NOINLINE static inline bool ps2_ee_fle_patterns(float a, float b) { return ps2_ee_order(a) <= ps2_ee_order(b); }
#if defined(_M_X64) || defined(__x86_64__)
static inline bool ps2_ee_has_nan_pattern(float a, float b)
{
    // Doubling drops the sign.
    return (ps2_float_bits(a) * 2u > 0xFF000000u) | (ps2_float_bits(b) * 2u > 0xFF000000u);
}
static inline bool ps2_ee_feq(float a, float b) { return ps2_ee_has_nan_pattern(a, b) ? ps2_ee_feq_patterns(a, b) : a == b; }
static inline bool ps2_ee_flt(float a, float b) { return ps2_ee_has_nan_pattern(a, b) ? ps2_ee_flt_patterns(a, b) : a < b; }
static inline bool ps2_ee_fle(float a, float b) { return ps2_ee_has_nan_pattern(a, b) ? ps2_ee_fle_patterns(a, b) : a <= b; }
#else
// Without DAZ the host's comparison does not read a denormal as zero.
static inline bool ps2_ee_feq(float a, float b) { return ps2_ee_feq_patterns(a, b); }
static inline bool ps2_ee_flt(float a, float b) { return ps2_ee_flt_patterns(a, b); }
static inline bool ps2_ee_fle(float a, float b) { return ps2_ee_fle_patterns(a, b); }
#endif

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

// MAX.S and MIN.S do the same on the EE: +0 is above -0 and a denormal is not zero
// (ps2autotests, tests/cpu/ee_fpu/arithmetic).
static inline float ps2_ee_fmax(float a, float b) { return _mm_cvtss_f32(ps2_vmax(_mm_set_ss(a), _mm_set_ss(b))); }
static inline float ps2_ee_fmin(float a, float b) { return _mm_cvtss_f32(ps2_vmin(_mm_set_ss(a), _mm_set_ss(b))); }

#endif // PS2_FLOAT_H
