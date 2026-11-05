#ifndef PS2_VU1_NATIVE_H
#define PS2_VU1_NATIVE_H

// Native VU1 programs: C++ translations of whole VU1 microprograms, run from the MSCAL that
// starts one to its E bit without stepping through the microcode pair by pair.
// tools/translate_vu_programs.py writes them from the game's own microcode; this header holds
// what they share: the registry and the VU arithmetic on host SSE registers.
//
// PS2_VU_NATIVE selects them at run time: unset or 0, off; "shadow", the microcode still runs
// and draws while each translation runs on copies of its inputs and the two results are
// compared (GIF packets, VU1 data memory, registers); 1, the translations draw.

#include "runtime/ps2_vu1.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#if (defined(_M_X64) || defined(__x86_64__)) && defined(__AVX2__)
#define PS2_VU_NATIVE_AVAILABLE 1
#include <immintrin.h>
#else
#define PS2_VU_NATIVE_AVAILABLE 0
#endif

#if defined(_MSC_VER)
#define PS2_VU_NATIVE_INLINE __forceinline
#else
#define PS2_VU_NATIVE_INLINE inline __attribute__((always_inline))
#endif

namespace ps2_vu_native
{
// What a translation reads and changes. The registers it does not model (status, P, R) keep
// their values; GIF packets go to kick in the order the microcode kicks them.
struct Context
{
    uint8_t *data; // VU1 data memory, 16 KiB
    VU1State *state;
    void (*kick)(void *user, const uint8_t *packet, uint32_t bytes);
    void *user;
    uint64_t cycles; // set by the translation: the VU1 cycles the microcode takes, pipeline flush included
};

struct Program
{
    const char *name;
    uint32_t pc;         // MSCAL address
    uint32_t begin, end; // microcode bytes the translation was made from
    uint64_t hash;       // their FNV-1a 64
    const uint8_t *code; // the bytes themselves, compared at each new microcode generation
    bool (*run)(Context &);
};

// Called by the translations' static initializers.
void add(const Program &program);

enum class Mode
{
    Off,
    Shadow,
    On
};
Mode mode();
void setMode(Mode mode);

// The translation for the program an MSCAL at pc starts, if there is one: the code is checked
// once per microcode generation.
const Program *find(const uint8_t *code, uint32_t codeSize, uint32_t pc, uint64_t generation);

// Runs a translation as VU1Interpreter::run runs microcode: rounding toward zero, denormal
// operands read as zero, results below FLT_MIN flushed to zeros of their sign. false: the start
// holds a value the program jumps through that the translation does not have (an address it
// left in a register or in VU1 memory), nothing has changed and the microcode must run.
bool run(const Program &program, Context &context);

// Counts of shadow comparisons, one line per program; empty when nothing was compared.
std::string report();
// All programs together.
void totals(uint64_t &compared, uint64_t &identical);

// Shadow mode, called by VU1Interpreter: start() runs the translation on copies of the
// inputs, kick() collects what the microcode kicks meanwhile and finish() compares the
// two when the microcode reaches its E bit, cycles after it started. cancel(): the microcode
// did not end within its MSCAL's cycles, which the translation does not model.
void shadowStart(const Program &program, const uint8_t *data, const VU1State &state);
bool shadowActive();
void shadowKick(const uint8_t *packet, uint32_t bytes);
void shadowFinish(const uint8_t *data, const VU1State &state, uint64_t cycles);
void shadowCancel();

// What a kick of VU1 data memory at a qword address sends, as XGKICK reads it: GIF tags until
// the one with EOP, wrapping at the end of memory, into out (capacity bytes, as the
// interpreter's PATH1 buffer). qwords counts what PATH1 reads, one qword every two cycles; a
// packet that does not fit is dropped, as the interpreter drops it, after the tag that
// overflows.
struct GifPacket
{
    uint32_t bytes;  // 0 when dropped
    uint32_t qwords;
};
GifPacket gifPacket(const uint8_t *data, uint32_t qwordAddress, uint8_t *out, uint32_t capacity);

#if PS2_VU_NATIVE_AVAILABLE
// VU arithmetic as the interpreter does it (ps2_vu1_upper_engine.h). Rounding toward zero,
// denormal operands read as zero and results below FLT_MIN flushed to zero come from the MXCSR
// run() sets; rounding toward zero never overflows finite operands to infinity, so a result
// with exponent 255 means an operand had one, and the lanes are computed again from operands
// normalized as the interpreter reads them.
namespace ops
{
using V = __m128;

PS2_VU_NATIVE_INLINE V bits(uint32_t value) { return _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(value))); }
// A value the compiler cannot see: with VF0 or an I immediate known, it folds x + 0 or x * 1
// into x, whatever the floating-point model, where the VU reads a denormal x as zero.
PS2_VU_NATIVE_INLINE V opaque(V value)
{
    alignas(16) uint32_t lanes[4];
    _mm_store_si128(reinterpret_cast<__m128i *>(lanes), _mm_castps_si128(value));
    volatile uint32_t slot[4] = {lanes[0], lanes[1], lanes[2], lanes[3]};
    return _mm_castsi128_ps(_mm_setr_epi32(static_cast<int>(slot[0]), static_cast<int>(slot[1]), static_cast<int>(slot[2]),
                                           static_cast<int>(slot[3])));
}
PS2_VU_NATIVE_INLINE V splat(float value) { return _mm_set1_ps(value); }
PS2_VU_NATIVE_INLINE V load(const float *lanes) { return _mm_loadu_ps(lanes); }
PS2_VU_NATIVE_INLINE void store(float *lanes, V value) { _mm_storeu_ps(lanes, value); }

// VU dest masks have x in bit 3; SSE blends have x in bit 0.
constexpr int sseMask(int dest) { return ((dest >> 3) & 1) | ((dest >> 1) & 2) | ((dest << 1) & 4) | ((dest << 3) & 8); }
template <int Dest>
PS2_VU_NATIVE_INLINE V blend(V old, V value)
{
    if constexpr (Dest == 15)
        return value;
    else if constexpr (Dest == 0)
        return old;
    else
        return _mm_blend_ps(old, value, sseMask(Dest));
}
template <int Lane>
PS2_VU_NATIVE_INLINE V bc(V value) { return _mm_shuffle_ps(value, value, Lane * 0x55); }
template <int Lane>
PS2_VU_NATIVE_INLINE float lane(V value) { return _mm_cvtss_f32(bc<Lane>(value)); }
template <int Lane>
PS2_VU_NATIVE_INLINE uint32_t laneBits(V value) { return static_cast<uint32_t>(_mm_cvtsi128_si32(_mm_castps_si128(bc<Lane>(value)))); }

// Whether a lane has exponent 255 (infinity or NaN).
PS2_VU_NATIVE_INLINE bool special(V value)
{
    const __m128i exponent = _mm_and_si128(_mm_castps_si128(value), _mm_set1_epi32(0x7F800000));
    return _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(exponent, _mm_set1_epi32(0x7F800000)))) != 0;
}
// normalizeOperand: what arithmetic reads, denormals as zeros and exponent 255 as FLT_MAX.
PS2_VU_NATIVE_INLINE V normalize(V value)
{
    const __m128i raw = _mm_castps_si128(value);
    const __m128i exponent = _mm_and_si128(raw, _mm_set1_epi32(0x7F800000));
    const __m128i sign = _mm_and_si128(raw, _mm_set1_epi32(INT32_MIN));
    const __m128i zero = _mm_cmpeq_epi32(exponent, _mm_setzero_si128());
    const __m128i huge = _mm_cmpeq_epi32(exponent, _mm_set1_epi32(0x7F800000));
    __m128i result = _mm_blendv_epi8(raw, sign, zero);
    result = _mm_blendv_epi8(result, _mm_or_si128(sign, _mm_set1_epi32(0x7F7FFFFF)), huge);
    return _mm_castsi128_ps(result);
}
PS2_VU_NATIVE_INLINE V add(V a, V b)
{
    const V result = _mm_add_ps(a, b);
    return special(result) ? _mm_add_ps(normalize(a), normalize(b)) : result;
}
PS2_VU_NATIVE_INLINE V sub(V a, V b)
{
    const V result = _mm_sub_ps(a, b);
    return special(result) ? _mm_sub_ps(normalize(a), normalize(b)) : result;
}
PS2_VU_NATIVE_INLINE V mul(V a, V b)
{
    const V result = _mm_mul_ps(a, b);
    return special(result) ? _mm_mul_ps(normalize(a), normalize(b)) : result;
}

// acc + a * b (or acc - a * b) rounded once, as the interpreter's fused product sum. Where the
// interpreter takes the exact value's sign instead (an exact value of zero, below FLT_MIN or
// above FLT_MAX), rounding toward zero and flushing give the same zero or FLT_MAX.
template <bool Subtract>
PS2_VU_NATIVE_INLINE V fused(V acc, V a, V b) { return Subtract ? _mm_fnmadd_ps(a, b, acc) : _mm_fmadd_ps(a, b, acc); }
template <bool Subtract, int>
PS2_VU_NATIVE_INLINE V productSum(V acc, V a, V b)
{
    const V result = fused<Subtract>(acc, a, b);
    return special(result) ? fused<Subtract>(normalize(acc), normalize(a), normalize(b)) : result;
}
template <int Dest>
PS2_VU_NATIVE_INLINE V madd(V acc, V a, V b) { return productSum<false, Dest>(acc, a, b); }
template <int Dest>
PS2_VU_NATIVE_INLINE V msub(V acc, V a, V b) { return productSum<true, Dest>(acc, a, b); }
// OPMULA and OPMSUB pair fs.yzx with ft.zxy; w is zero.
PS2_VU_NATIVE_INLINE V outerLeft(V a) { return _mm_shuffle_ps(a, a, _MM_SHUFFLE(3, 0, 2, 1)); }
PS2_VU_NATIVE_INLINE V outerRight(V b) { return _mm_shuffle_ps(b, b, _MM_SHUFFLE(3, 1, 0, 2)); }

// MAX and MINI order the raw bit patterns as sign and magnitude.
PS2_VU_NATIVE_INLINE __m128i ordered(V value)
{
    const __m128i raw = _mm_castps_si128(value);
    return _mm_xor_si128(raw, _mm_srli_epi32(_mm_srai_epi32(raw, 31), 1));
}
PS2_VU_NATIVE_INLINE V max(V a, V b) { return _mm_blendv_ps(a, b, _mm_castsi128_ps(_mm_cmpgt_epi32(ordered(b), ordered(a)))); }
PS2_VU_NATIVE_INLINE V min(V a, V b) { return _mm_blendv_ps(a, b, _mm_castsi128_ps(_mm_cmplt_epi32(ordered(b), ordered(a)))); }

PS2_VU_NATIVE_INLINE V abs(V a) { return _mm_and_ps(a, bits(0x7FFFFFFFu)); }

template <int Shift>
PS2_VU_NATIVE_INLINE V itof(V a)
{
    const V value = _mm_cvtepi32_ps(_mm_castps_si128(a));
    if constexpr (Shift == 0)
        return value;
    else
        return _mm_mul_ps(value, _mm_set1_ps(1.0f / static_cast<float>(1 << Shift)));
}
// Truncates, saturating at INT32_MAX and INT32_MIN.
template <int Shift>
PS2_VU_NATIVE_INLINE V ftoi(V a)
{
    const V scaled = Shift == 0 ? a : _mm_mul_ps(a, _mm_set1_ps(static_cast<float>(1 << Shift)));
    const __m128i value = _mm_cvttps_epi32(scaled);
    const __m128i positiveOverflow = _mm_and_si128(_mm_cmpeq_epi32(value, _mm_set1_epi32(INT32_MIN)),
                                                   _mm_castps_si128(_mm_cmpgt_ps(scaled, _mm_setzero_ps())));
    return _mm_castsi128_ps(_mm_xor_si128(value, _mm_and_si128(positiveOverflow, _mm_set1_epi32(-1))));
}

// CLIP: +x, -x, +y, -y, +z, -z beyond |w|, as six bits.
PS2_VU_NATIVE_INLINE uint32_t clip(V fs, V ft)
{
    const uint32_t wBits = laneBits<3>(ft);
    const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;
    const __m128i raw = _mm_castps_si128(fs);
    const __m128i bound = _mm_set1_epi32(limit);
    const int positive = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(raw, bound)));
    const int negative = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(_mm_xor_si128(raw, _mm_set1_epi32(INT32_MIN)), bound)));
    return static_cast<uint32_t>((positive & 1) | (negative & 1) << 1 | (positive & 2) << 1 | (negative & 2) << 2 |
                                 (positive & 4) << 2 | (negative & 4) << 3);
}

// DIV: Q = fs / ft, both finite; a zero divisor gives FLT_MAX with the quotient's sign.
// Q and P as the interpreter computes them (ps2_vu1_lower.inl): operands with exponent 255 read
// as FLT_MAX, a zero divisor gives FLT_MAX with the quotient's sign.
PS2_VU_NATIVE_INLINE float operand(float value) { return _mm_cvtss_f32(normalize(_mm_set_ss(value))); }
PS2_VU_NATIVE_INLINE float root(float value) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(value))); }

PS2_VU_NATIVE_INLINE float div(float numerator, float denominator)
{
    numerator = operand(numerator);
    denominator = operand(denominator);
    if (denominator == 0.0f)
        return (std::signbit(numerator) != std::signbit(denominator)) ? -3.40282347e+38f : 3.40282347e+38f;
    return numerator / denominator;
}

PS2_VU_NATIVE_INLINE float sqrtQ(float value) { return root(std::fabs(operand(value))); }

PS2_VU_NATIVE_INLINE float rsqrtQ(float numerator, float radicand)
{
    numerator = operand(numerator);
    const float denominator = root(std::fabs(operand(radicand)));
    if (denominator == 0.0f)
        return std::signbit(numerator) ? -3.40282347e+38f : 3.40282347e+38f;
    return numerator / denominator;
}

PS2_VU_NATIVE_INLINE float esqrt(float value)
{
    value = operand(value);
    return value >= 0.0f ? root(value) : value;
}

PS2_VU_NATIVE_INLINE float ersqrt(float value)
{
    value = operand(value);
    if (value < 0.0f)
        return value;
    const float r = root(value);
    return r != 0.0f ? 1.0f / r : r;
}

PS2_VU_NATIVE_INLINE float ercpr(float value)
{
    value = operand(value);
    return value != 0.0f ? 1.0f / value : value;
}

// MAC flags, for the lanes in Dest: zero in bits 0-3, sign in 4-7, underflow in 8-11, overflow
// in 12-15, lane x highest in each group. A result in the normal range has only its sign, as
// in the interpreter's fast path; other lanes take their flags from the exact value (acc is
// unused by ADD, SUB and MUL).
enum class Arith
{
    Add,
    Sub,
    Mul,
    MAdd,
    MSub
};
template <Arith Op>
uint32_t exactFlags(V acc, V a, V b, int dest);
template <Arith Op, int Dest>
PS2_VU_NATIVE_INLINE uint32_t flags(V acc, V a, V b, V result)
{
    const __m128i raw = _mm_castps_si128(result);
    __m128i normal;
    if constexpr (Op == Arith::MAdd || Op == Arith::MSub)
    {
        // productSum's test: an exponent from 2 to 253 is where the exact value is too.
        const __m128i exponent = _mm_and_si128(_mm_srli_epi32(raw, 23), _mm_set1_epi32(0xFF));
        normal = _mm_and_si128(_mm_cmpgt_epi32(exponent, _mm_set1_epi32(1)), _mm_cmplt_epi32(exponent, _mm_set1_epi32(254)));
    }
    else
    {
        const __m128i magnitude = _mm_and_si128(raw, _mm_set1_epi32(0x7FFFFFFF));
        normal = _mm_and_si128(_mm_cmpgt_epi32(magnitude, _mm_set1_epi32(0x007FFFFF)), _mm_cmplt_epi32(magnitude, _mm_set1_epi32(0x7F7FFFFF)));
    }
    if ((~_mm_movemask_ps(_mm_castsi128_ps(normal)) & sseMask(Dest)) != 0)
        return exactFlags<Op>(acc, a, b, Dest);
    const int signs = _mm_movemask_ps(result) & sseMask(Dest); // SSE bit 0 is x
    return static_cast<uint32_t>(((signs & 1) << 3 | (signs & 2) << 1 | (signs & 4) >> 1 | (signs & 8) >> 3) << 4);
}

// VU1 data memory: qword addresses wrap at 16 KiB.
PS2_VU_NATIVE_INLINE V lq(const uint8_t *data, int32_t address) { return _mm_loadu_ps(reinterpret_cast<const float *>(data + (address & 0x3FF) * 16)); }
template <int Dest>
PS2_VU_NATIVE_INLINE void sq(uint8_t *data, int32_t address, V value)
{
    float *target = reinterpret_cast<float *>(data + (address & 0x3FF) * 16);
    _mm_storeu_ps(target, blend<Dest>(_mm_loadu_ps(target), value));
}
PS2_VU_NATIVE_INLINE int32_t ilw(const uint8_t *data, int32_t address, int component)
{
    uint32_t word;
    std::memcpy(&word, data + (address & 0x3FF) * 16 + component * 4, sizeof(word));
    return static_cast<int16_t>(word & 0xFFFFu);
}
template <int Dest>
PS2_VU_NATIVE_INLINE void isw(uint8_t *data, int32_t address, int32_t value)
{
    sq<Dest>(data, address, bits(static_cast<uint16_t>(value)));
}
PS2_VU_NATIVE_INLINE int32_t vi16(int32_t value) { return static_cast<int16_t>(value); }

// Sends the packet at once and returns the cycle PATH1 is free again, for an XGKICK issued at
// cycle: the microcode copies it progressively, which reads the same bytes unless the program
// stores into a packet while it is sent (a double-buffered program does not).
uint64_t xgkick(Context &context, int32_t qwordAddress, uint64_t cycle);
// A JR to an address no BAL linked, which the translator rules out.
void unexpectedJump(Context &context, uint32_t address);
} // namespace ops
#endif
} // namespace ps2_vu_native

#endif
