#ifndef PS2_MMI_H
#define PS2_MMI_H

// EE MMI instructions that use the 128-bit HI/LO pair or move elements in
// ways SSE shuffles got wrong. Semantics follow the EE Core Instruction Set
// Manual and PCSX2's interpreter (pcsx2/MMI.cpp). HI = {hi, hi1} and
// LO = {lo, lo1}: the upper 64 bits belong to pipeline 1 (MULT1, DIV1...).
// Included by ps2_runtime_macros.h, after R5900Context is defined.

#include <cstdint>
#include <cstring>

namespace ps2_mmi
{
    union Q
    {
        __m128i v;
        int16_t SS[8];
        uint16_t US[8];
        int32_t SL[4];
        uint32_t UL[4];
        int64_t SD[2];
        uint64_t UD[2];
    };

    inline Q gpr(const R5900Context *ctx, uint32_t r)
    {
        Q q;
        q.v = r ? ctx->r[r] : _mm_setzero_si128();
        return q;
    }
    inline void setGpr(R5900Context *ctx, uint32_t r, const Q &q)
    {
        if (r)
            ctx->r[r] = q.v;
    }
    inline Q lo(const R5900Context *ctx)
    {
        Q q;
        q.UD[0] = ctx->lo;
        q.UD[1] = ctx->lo1;
        return q;
    }
    inline Q hi(const R5900Context *ctx)
    {
        Q q;
        q.UD[0] = ctx->hi;
        q.UD[1] = ctx->hi1;
        return q;
    }
    inline void setLoHi(R5900Context *ctx, const Q &l, const Q &h)
    {
        ctx->lo = l.UD[0];
        ctx->lo1 = l.UD[1];
        ctx->hi = h.UD[0];
        ctx->hi1 = h.UD[1];
    }
    inline int16_t sat16(int32_t v)
    {
        return static_cast<int16_t>(v > 0x7FFF ? 0x7FFF : (v < -0x8000 ? -0x8000 : v));
    }

    inline void pmfhi(R5900Context *ctx, uint32_t rd) { setGpr(ctx, rd, hi(ctx)); }
    inline void pmflo(R5900Context *ctx, uint32_t rd) { setGpr(ctx, rd, lo(ctx)); }
    inline void pmthi(R5900Context *ctx, uint32_t rs)
    {
        const Q s = gpr(ctx, rs);
        ctx->hi = s.UD[0];
        ctx->hi1 = s.UD[1];
    }
    inline void pmtlo(R5900Context *ctx, uint32_t rs)
    {
        const Q s = gpr(ctx, rs);
        ctx->lo = s.UD[0];
        ctx->lo1 = s.UD[1];
    }

    // PMFHL.fmt: 0 LW, 1 UW, 2 SLW, 3 LH, 4 SH.
    inline void pmfhl(R5900Context *ctx, uint32_t rd, uint32_t fmt)
    {
        const Q l = lo(ctx), h = hi(ctx);
        Q d{};
        switch (fmt)
        {
        case 0:
            d.UL[0] = l.UL[0], d.UL[1] = h.UL[0], d.UL[2] = l.UL[2], d.UL[3] = h.UL[2];
            break;
        case 1:
            d.UL[0] = l.UL[1], d.UL[1] = h.UL[1], d.UL[2] = l.UL[3], d.UL[3] = h.UL[3];
            break;
        case 2:
            for (int i = 0; i < 2; ++i)
            {
                const int64_t v = static_cast<int64_t>((static_cast<uint64_t>(h.UL[2 * i]) << 32) | l.UL[2 * i]);
                d.SD[i] = v >= 0x7FFFFFFFLL ? 0x7FFFFFFFLL : (v <= -0x80000000LL ? -0x80000000LL : static_cast<int64_t>(l.SL[2 * i]));
            }
            break;
        case 3:
            d.US[0] = l.US[0], d.US[1] = l.US[2], d.US[2] = h.US[0], d.US[3] = h.US[2];
            d.US[4] = l.US[4], d.US[5] = l.US[6], d.US[6] = h.US[4], d.US[7] = h.US[6];
            break;
        case 4:
            d.SS[0] = sat16(l.SL[0]), d.SS[1] = sat16(l.SL[1]), d.SS[2] = sat16(h.SL[0]), d.SS[3] = sat16(h.SL[1]);
            d.SS[4] = sat16(l.SL[2]), d.SS[5] = sat16(l.SL[3]), d.SS[6] = sat16(h.SL[2]), d.SS[7] = sat16(h.SL[3]);
            break;
        default:
            return;
        }
        setGpr(ctx, rd, d);
    }

    // PMTHL.LW: words 0 and 2 of LO and HI from rs; words 1 and 3 are kept.
    inline void pmthl(R5900Context *ctx, uint32_t rs)
    {
        const Q s = gpr(ctx, rs);
        Q l = lo(ctx), h = hi(ctx);
        l.UL[0] = s.UL[0], h.UL[0] = s.UL[1], l.UL[2] = s.UL[2], h.UL[2] = s.UL[3];
        setLoHi(ctx, l, h);
    }

    // Word products: element 0 goes to HI/LO doubleword 0, element 2 to doubleword 1.
    inline void pmultw(R5900Context *ctx, uint32_t rd, uint32_t rs, uint32_t rt, int accumulate)
    {
        const Q a = gpr(ctx, rs), b = gpr(ctx, rt);
        Q l = lo(ctx), h = hi(ctx), d = gpr(ctx, rd);
        for (int i = 0; i < 2; ++i)
        {
            const int s = 2 * i;
            const int64_t acc = static_cast<int64_t>((static_cast<uint64_t>(h.UL[s]) << 32) | l.UL[s]);
            const int64_t product = static_cast<int64_t>(a.SL[s]) * b.SL[s];
            const int64_t t = accumulate > 0 ? acc + product : (accumulate < 0 ? acc - product : product);
            l.SD[i] = static_cast<int32_t>(t);
            h.SD[i] = static_cast<int32_t>(t >> 32);
            d.SD[i] = t;
        }
        setLoHi(ctx, l, h);
        setGpr(ctx, rd, d);
    }
    inline void pmultuw(R5900Context *ctx, uint32_t rd, uint32_t rs, uint32_t rt, bool accumulate)
    {
        const Q a = gpr(ctx, rs), b = gpr(ctx, rt);
        Q l = lo(ctx), h = hi(ctx), d = gpr(ctx, rd);
        for (int i = 0; i < 2; ++i)
        {
            const int s = 2 * i;
            const uint64_t acc = (static_cast<uint64_t>(h.UL[s]) << 32) | l.UL[s];
            const uint64_t t = (accumulate ? acc : 0u) + static_cast<uint64_t>(a.UL[s]) * b.UL[s];
            l.SD[i] = static_cast<int32_t>(t);
            h.SD[i] = static_cast<int32_t>(t >> 32);
            d.UD[i] = t;
        }
        setLoHi(ctx, l, h);
        setGpr(ctx, rd, d);
    }
    inline void pdivw(R5900Context *ctx, uint32_t rs, uint32_t rt)
    {
        const Q a = gpr(ctx, rs), b = gpr(ctx, rt);
        Q l = lo(ctx), h = hi(ctx);
        for (int i = 0; i < 2; ++i)
        {
            const int s = 2 * i;
            if (a.UL[s] == 0x80000000u && b.UL[s] == 0xFFFFFFFFu)
                l.SD[i] = INT32_MIN, h.SD[i] = 0;
            else if (b.SL[s] != 0)
                l.SD[i] = a.SL[s] / b.SL[s], h.SD[i] = a.SL[s] % b.SL[s];
            else
                l.SD[i] = a.SL[s] < 0 ? 1 : -1, h.SD[i] = a.SL[s];
        }
        setLoHi(ctx, l, h);
    }
    inline void pdivuw(R5900Context *ctx, uint32_t rs, uint32_t rt)
    {
        const Q a = gpr(ctx, rs), b = gpr(ctx, rt);
        Q l = lo(ctx), h = hi(ctx);
        for (int i = 0; i < 2; ++i)
        {
            const int s = 2 * i;
            if (b.UL[s] != 0)
                l.SD[i] = static_cast<int32_t>(a.UL[s] / b.UL[s]), h.SD[i] = static_cast<int32_t>(a.UL[s] % b.UL[s]);
            else
                l.SD[i] = -1, h.SD[i] = a.SL[s];
        }
        setLoHi(ctx, l, h);
    }
    // PDIVBW: the four words of rs divided by halfword 0 of rt.
    inline void pdivbw(R5900Context *ctx, uint32_t rs, uint32_t rt)
    {
        const Q a = gpr(ctx, rs), b = gpr(ctx, rt);
        const int32_t divisor = b.SS[0];
        Q l = lo(ctx), h = hi(ctx);
        for (int i = 0; i < 4; ++i)
        {
            if (a.UL[i] == 0x80000000u && divisor == -1)
                l.SL[i] = INT32_MIN, h.SL[i] = 0;
            else if (divisor != 0)
                l.SL[i] = a.SL[i] / divisor, h.SL[i] = a.SL[i] % divisor;
            else
                l.SL[i] = a.SL[i] < 0 ? 1 : -1, h.SL[i] = a.SL[i];
        }
        setLoHi(ctx, l, h);
    }

    // Halfword products: pairs land in LO.w0/w1, HI.w0/w1, LO.w2/w3, HI.w2/w3;
    // rd gets LO.w0, HI.w0, LO.w2, HI.w2. accumulate: 0 PMULTH, 1 PMADDH, -1 PMSUBH.
    inline void pmulth(R5900Context *ctx, uint32_t rd, uint32_t rs, uint32_t rt, int accumulate)
    {
        const Q a = gpr(ctx, rs), b = gpr(ctx, rt);
        Q l = lo(ctx), h = hi(ctx);
        static constexpr int kHi[8] = {0, 0, 1, 1, 0, 0, 1, 1};
        static constexpr int kWord[8] = {0, 1, 0, 1, 2, 3, 2, 3};
        for (int n = 0; n < 8; ++n)
        {
            Q &target = kHi[n] ? h : l;
            const int32_t product = static_cast<int32_t>(a.SS[n]) * b.SS[n];
            const uint32_t old = target.UL[kWord[n]];
            target.UL[kWord[n]] = accumulate > 0 ? old + static_cast<uint32_t>(product)
                                  : accumulate < 0 ? old - static_cast<uint32_t>(product)
                                                   : static_cast<uint32_t>(product);
        }
        setLoHi(ctx, l, h);
        Q d;
        d.UL[0] = l.UL[0], d.UL[1] = h.UL[0], d.UL[2] = l.UL[2], d.UL[3] = h.UL[2];
        setGpr(ctx, rd, d);
    }
    // PHMADH / PHMSBH: odd product plus (or minus) even product per word pair.
    inline void phmadh(R5900Context *ctx, uint32_t rd, uint32_t rs, uint32_t rt, bool subtract)
    {
        const Q a = gpr(ctx, rs), b = gpr(ctx, rt);
        Q l = lo(ctx), h = hi(ctx), d;
        for (int n = 0; n < 4; ++n)
        {
            const int32_t odd = static_cast<int32_t>(a.SS[2 * n + 1]) * b.SS[2 * n + 1];
            const int32_t even = static_cast<int32_t>(a.SS[2 * n]) * b.SS[2 * n];
            const uint32_t t = subtract ? static_cast<uint32_t>(odd) - static_cast<uint32_t>(even)
                                        : static_cast<uint32_t>(odd) + static_cast<uint32_t>(even);
            Q &target = (n & 1) ? h : l;
            const int word = (n & 2) ? 2 : 0;
            target.UL[word] = t;
            target.UL[word + 1] = subtract ? ~static_cast<uint32_t>(odd) : static_cast<uint32_t>(odd);
            d.UL[n] = t;
        }
        setLoHi(ctx, l, h);
        setGpr(ctx, rd, d);
    }

    // PSLLVW/PSRLVW/PSRAVW rd, rt, rs: words 0 and 2 of rt shifted by rs, sign-extended to 64 bits.
    inline void pshiftvw(R5900Context *ctx, uint32_t rd, uint32_t rs, uint32_t rt, int kind)
    {
        const Q value = gpr(ctx, rt), amount = gpr(ctx, rs);
        Q d;
        for (int i = 0; i < 2; ++i)
        {
            const uint32_t sh = amount.UL[2 * i] & 0x1Fu;
            const uint32_t v = value.UL[2 * i];
            const int32_t r = kind == 0 ? static_cast<int32_t>(v << sh)
                              : kind == 1 ? static_cast<int32_t>(v >> sh)
                                          : (static_cast<int32_t>(v) >> sh);
            d.SD[i] = r;
        }
        setGpr(ctx, rd, d);
    }

    // Element permutations, each listing the source element of every destination element.
    inline __m128i permuteH(__m128i v, const int (&order)[8])
    {
        Q s, d;
        s.v = v;
        for (int i = 0; i < 8; ++i)
            d.US[i] = s.US[order[i]];
        return d.v;
    }
    inline __m128i permuteW(__m128i v, const int (&order)[4])
    {
        Q s, d;
        s.v = v;
        for (int i = 0; i < 4; ++i)
            d.UL[i] = s.UL[order[i]];
        return d.v;
    }
    inline __m128i pexeh(__m128i rt) { return permuteH(rt, {2, 1, 0, 3, 6, 5, 4, 7}); }
    inline __m128i prevh(__m128i rt) { return permuteH(rt, {3, 2, 1, 0, 7, 6, 5, 4}); }
    inline __m128i pexch(__m128i rt) { return permuteH(rt, {0, 2, 1, 3, 4, 6, 5, 7}); }
    inline __m128i pexew(__m128i rt) { return permuteW(rt, {2, 1, 0, 3}); }
    inline __m128i prot3w(__m128i rt) { return permuteW(rt, {1, 2, 0, 3}); }
    inline __m128i pexcw(__m128i rt) { return permuteW(rt, {0, 2, 1, 3}); }
    // PINTH: low halfwords of rt interleaved with high halfwords of rs.
    inline __m128i pinth(__m128i rs, __m128i rt) { return _mm_unpacklo_epi16(rt, _mm_unpackhi_epi64(rs, rs)); }
    // PINTEH: even halfwords of rt and rs, rt first.
    inline __m128i pinteh(__m128i rs, __m128i rt)
    {
        return _mm_or_si128(_mm_and_si128(rt, _mm_set1_epi32(0xFFFF)), _mm_slli_epi32(rs, 16));
    }
}

#endif // PS2_MMI_H
