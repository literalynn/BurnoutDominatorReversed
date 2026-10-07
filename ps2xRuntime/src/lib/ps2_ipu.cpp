#include "runtime/ps2_ipu.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <iostream>
#include <vector>

namespace
{
    using Qword = std::array<uint8_t, 16>;

    // ------------------------------------------------------------------ VLC tables
    // MPEG-2 (ISO/IEC 13818-2) annex B. Codes are given without their sign bit.

    struct VlcCode
    {
        uint16_t code;
        uint8_t length;
    };

    // B.1 macroblock_address_increment, values 1 to 33.
    constexpr VlcCode kMbaCodes[33] = {
        {0x1, 1}, {0x3, 3}, {0x2, 3}, {0x3, 4}, {0x2, 4}, {0x3, 5}, {0x2, 5}, {0x7, 7}, {0x6, 7},
        {0xb, 8}, {0xa, 8}, {0x9, 8}, {0x8, 8}, {0x7, 8}, {0x6, 8}, {0x17, 10}, {0x16, 10}, {0x15, 10},
        {0x14, 10}, {0x13, 10}, {0x12, 10}, {0x23, 11}, {0x22, 11}, {0x21, 11}, {0x20, 11}, {0x1f, 11},
        {0x1e, 11}, {0x1d, 11}, {0x1c, 11}, {0x1b, 11}, {0x1a, 11}, {0x19, 11}, {0x18, 11}};
    constexpr VlcCode kMbaEscape = {0x8, 11};   // reported as 0x23
    constexpr VlcCode kMbaStuffing = {0xf, 11}; // reported as 0x22

    // B.9 coded_block_pattern, indexed by the pattern.
    constexpr VlcCode kCbpCodes[64] = {
        {0x1, 9}, {0xb, 5}, {0x9, 5}, {0xd, 6}, {0xd, 4}, {0x17, 7}, {0x13, 7}, {0x1f, 8},
        {0xc, 4}, {0x16, 7}, {0x12, 7}, {0x1e, 8}, {0x13, 5}, {0x1b, 8}, {0x17, 8}, {0x13, 8},
        {0xb, 4}, {0x15, 7}, {0x11, 7}, {0x1d, 8}, {0x11, 5}, {0x19, 8}, {0x15, 8}, {0x11, 8},
        {0xf, 6}, {0xf, 8}, {0xd, 8}, {0x3, 9}, {0xf, 5}, {0xb, 8}, {0x7, 8}, {0x7, 9},
        {0xa, 4}, {0x14, 7}, {0x10, 7}, {0x1c, 8}, {0xe, 6}, {0xe, 8}, {0xc, 8}, {0x2, 9},
        {0x10, 5}, {0x18, 8}, {0x14, 8}, {0x10, 8}, {0xe, 5}, {0xa, 8}, {0x6, 8}, {0x6, 9},
        {0x12, 5}, {0x1a, 8}, {0x16, 8}, {0x12, 8}, {0xd, 5}, {0x9, 8}, {0x5, 8}, {0x5, 9},
        {0xc, 5}, {0x8, 8}, {0x4, 8}, {0x4, 9}, {0x7, 3}, {0xa, 5}, {0x8, 5}, {0xc, 6}};

    // B.10 motion_code magnitudes 0 to 16; a sign bit follows every code but 0.
    constexpr VlcCode kMotionCodes[17] = {
        {0x1, 1}, {0x1, 2}, {0x1, 3}, {0x1, 4}, {0x3, 6}, {0x5, 7}, {0x4, 7}, {0x3, 7},
        {0xb, 9}, {0xa, 9}, {0x9, 9}, {0x11, 10}, {0x10, 10}, {0xf, 10}, {0xe, 10}, {0xd, 10}, {0xc, 10}};

    // B.12 and B.13 dct_dc_size, indexed by the size.
    constexpr VlcCode kDcLuminanceCodes[12] = {
        {0x4, 3}, {0x0, 2}, {0x1, 2}, {0x5, 3}, {0x6, 3}, {0xe, 4}, {0x1e, 5}, {0x3e, 6}, {0x7e, 7}, {0xfe, 8}, {0x1fe, 9}, {0x1ff, 9}};
    constexpr VlcCode kDcChrominanceCodes[12] = {
        {0x0, 2}, {0x1, 2}, {0x2, 2}, {0x6, 3}, {0xe, 4}, {0x1e, 5}, {0x3e, 6}, {0x7e, 7}, {0xfe, 8}, {0x1fe, 9}, {0x3fe, 10}, {0x3ff, 10}};

    // B.14 (table zero) and B.15 (table one) dct_coef_first/next, in run/level order:
    // run 0 levels 1-40, run 1 levels 1-18, run 2 levels 1-5, run 3 levels 1-4,
    // runs 4-6 levels 1-3, runs 7-16 levels 1-2, runs 17-31 level 1.
    constexpr VlcCode kDctZero[111] = {
        {0x3, 2}, {0x4, 4}, {0x5, 5}, {0x6, 7}, {0x26, 8}, {0x21, 8}, {0xa, 10}, {0x1d, 12},
        {0x18, 12}, {0x13, 12}, {0x10, 12}, {0x1a, 13}, {0x19, 13}, {0x18, 13}, {0x17, 13}, {0x1f, 14},
        {0x1e, 14}, {0x1d, 14}, {0x1c, 14}, {0x1b, 14}, {0x1a, 14}, {0x19, 14}, {0x18, 14}, {0x17, 14},
        {0x16, 14}, {0x15, 14}, {0x14, 14}, {0x13, 14}, {0x12, 14}, {0x11, 14}, {0x10, 14}, {0x18, 15},
        {0x17, 15}, {0x16, 15}, {0x15, 15}, {0x14, 15}, {0x13, 15}, {0x12, 15}, {0x11, 15}, {0x10, 15},
        {0x3, 3}, {0x6, 6}, {0x25, 8}, {0xc, 10}, {0x1b, 12}, {0x16, 13}, {0x15, 13}, {0x1f, 15},
        {0x1e, 15}, {0x1d, 15}, {0x1c, 15}, {0x1b, 15}, {0x1a, 15}, {0x19, 15}, {0x13, 16}, {0x12, 16},
        {0x11, 16}, {0x10, 16},
        {0x5, 4}, {0x4, 7}, {0xb, 10}, {0x14, 12}, {0x14, 13},
        {0x7, 5}, {0x24, 8}, {0x1c, 12}, {0x13, 13},
        {0x6, 5}, {0xf, 10}, {0x12, 12},
        {0x7, 6}, {0x9, 10}, {0x12, 13},
        {0x5, 6}, {0x1e, 12}, {0x14, 16},
        {0x4, 6}, {0x15, 12}, {0x7, 7}, {0x11, 12}, {0x5, 7}, {0x11, 13}, {0x27, 8}, {0x10, 13},
        {0x23, 8}, {0x1a, 16}, {0x22, 8}, {0x19, 16}, {0x20, 8}, {0x18, 16}, {0xe, 10}, {0x17, 16},
        {0xd, 10}, {0x16, 16}, {0x8, 10}, {0x15, 16},
        {0x1f, 12}, {0x1a, 12}, {0x19, 12}, {0x17, 12}, {0x16, 12},
        {0x1f, 13}, {0x1e, 13}, {0x1d, 13}, {0x1c, 13}, {0x1b, 13},
        {0x1f, 16}, {0x1e, 16}, {0x1d, 16}, {0x1c, 16}, {0x1b, 16}};
    constexpr VlcCode kDctOne[111] = {
        {0x2, 2}, {0x6, 3}, {0x7, 4}, {0x1c, 5}, {0x1d, 5}, {0x5, 6}, {0x4, 6}, {0x7b, 7},
        {0x7c, 7}, {0x23, 8}, {0x22, 8}, {0xfa, 8}, {0xfb, 8}, {0xfe, 8}, {0xff, 8}, {0x1f, 14},
        {0x1e, 14}, {0x1d, 14}, {0x1c, 14}, {0x1b, 14}, {0x1a, 14}, {0x19, 14}, {0x18, 14}, {0x17, 14},
        {0x16, 14}, {0x15, 14}, {0x14, 14}, {0x13, 14}, {0x12, 14}, {0x11, 14}, {0x10, 14}, {0x18, 15},
        {0x17, 15}, {0x16, 15}, {0x15, 15}, {0x14, 15}, {0x13, 15}, {0x12, 15}, {0x11, 15}, {0x10, 15},
        {0x2, 3}, {0x6, 5}, {0x79, 7}, {0x27, 8}, {0x20, 8}, {0x16, 13}, {0x15, 13}, {0x1f, 15},
        {0x1e, 15}, {0x1d, 15}, {0x1c, 15}, {0x1b, 15}, {0x1a, 15}, {0x19, 15}, {0x13, 16}, {0x12, 16},
        {0x11, 16}, {0x10, 16},
        {0x5, 5}, {0x7, 7}, {0xfc, 8}, {0xc, 10}, {0x14, 13},
        {0x7, 5}, {0x26, 8}, {0x1c, 12}, {0x13, 13},
        {0x6, 6}, {0xfd, 8}, {0x12, 12},
        {0x7, 6}, {0x4, 9}, {0x12, 13},
        {0x6, 7}, {0x1e, 12}, {0x14, 16},
        {0x4, 7}, {0x15, 12}, {0x5, 7}, {0x11, 12}, {0x78, 7}, {0x11, 13}, {0x7a, 7}, {0x10, 13},
        {0x21, 8}, {0x1a, 16}, {0x25, 8}, {0x19, 16}, {0x24, 8}, {0x18, 16}, {0x5, 9}, {0x17, 16},
        {0x7, 9}, {0x16, 16}, {0xd, 10}, {0x15, 16},
        {0x1f, 12}, {0x1a, 12}, {0x19, 12}, {0x17, 12}, {0x16, 12},
        {0x1f, 13}, {0x1e, 13}, {0x1d, 13}, {0x1c, 13}, {0x1b, 13},
        {0x1f, 16}, {0x1e, 16}, {0x1d, 16}, {0x1c, 16}, {0x1b, 16}};
    constexpr VlcCode kDctZeroEob = {0x2, 2};
    constexpr VlcCode kDctOneEob = {0x6, 4};
    constexpr VlcCode kDctEscape = {0x1, 6};

    // Macroblock type bits as VDEC table 1 returns them (MPEG-2 macroblock_type).
    constexpr int kMbQuant = 0x10;
    constexpr int kMbForward = 0x08;
    constexpr int kMbBackward = 0x04;
    constexpr int kMbPattern = 0x02;
    constexpr int kMbIntra = 0x01;

    constexpr uint8_t kZigzag[64] = {
        0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
        12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
        35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
        58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
    constexpr uint8_t kAlternate[64] = {
        0, 8, 16, 24, 1, 9, 2, 10, 17, 25, 32, 40, 48, 56, 57, 49,
        41, 33, 26, 18, 3, 11, 4, 12, 19, 27, 34, 42, 50, 58, 35, 43,
        51, 59, 20, 28, 5, 13, 6, 14, 21, 29, 36, 44, 52, 60, 37, 45,
        53, 61, 22, 30, 7, 15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63};
    constexpr uint8_t kNonLinearQuantiser[32] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 18, 20, 22,
        24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112};

    // Lookup table indexed by the next `bits` bits of the stream.
    struct LutEntry
    {
        int16_t value = 0;
        uint8_t length = 0; // 0: no code starts with these bits
        uint8_t run = 0;    // DCT tables: run; 0xFF EOB, 0xFE escape
    };

    class Lut
    {
    public:
        explicit Lut(uint32_t bits) : m_bits(bits), m_entries(size_t(1) << bits) {}

        void add(uint32_t code, uint32_t length, int16_t value, uint8_t run = 0)
        {
            const uint32_t shift = m_bits - length;
            for (uint32_t suffix = 0; suffix < (1u << shift); ++suffix)
            {
                LutEntry &entry = m_entries[(static_cast<size_t>(code) << shift) | suffix];
                if (entry.length != 0u)
                    ++m_conflicts;
                entry = {value, static_cast<uint8_t>(length), run};
            }
        }
        uint32_t bits() const { return m_bits; }
        const LutEntry &at(uint32_t index) const { return m_entries[index]; }

    private:
        uint32_t m_bits;
        std::vector<LutEntry> m_entries;
        uint32_t m_conflicts = 0;
    };

    struct Tables
    {
        Lut mba{11};
        Lut mbI{6};
        Lut mbP{6};
        Lut mbB{6};
        Lut mbD{6};
        Lut cbp{9};
        Lut motion{11};
        Lut dmv{2};
        Lut dcLuminance{9};
        Lut dcChrominance{10};
        Lut dctZero{16};
        Lut dctOne{16};
        uint8_t inverseZigzag[64] = {};

        Tables()
        {
            for (int i = 0; i < 33; ++i)
                mba.add(kMbaCodes[i].code, kMbaCodes[i].length, static_cast<int16_t>(i + 1));
            mba.add(kMbaEscape.code, kMbaEscape.length, 0x23);
            mba.add(kMbaStuffing.code, kMbaStuffing.length, 0x22);

            mbI.add(0x1, 1, kMbIntra);
            mbI.add(0x1, 2, kMbQuant | kMbIntra);
            mbP.add(0x1, 1, kMbForward | kMbPattern);
            mbP.add(0x1, 2, kMbPattern);
            mbP.add(0x1, 3, kMbForward);
            mbP.add(0x3, 5, kMbIntra);
            mbP.add(0x2, 5, kMbQuant | kMbForward | kMbPattern);
            mbP.add(0x1, 5, kMbQuant | kMbPattern);
            mbP.add(0x1, 6, kMbQuant | kMbIntra);
            mbB.add(0x2, 2, kMbForward | kMbBackward);
            mbB.add(0x3, 2, kMbForward | kMbBackward | kMbPattern);
            mbB.add(0x2, 3, kMbBackward);
            mbB.add(0x3, 3, kMbBackward | kMbPattern);
            mbB.add(0x2, 4, kMbForward);
            mbB.add(0x3, 4, kMbForward | kMbPattern);
            mbB.add(0x3, 5, kMbIntra);
            mbB.add(0x2, 5, kMbQuant | kMbForward | kMbBackward | kMbPattern);
            mbB.add(0x3, 6, kMbQuant | kMbForward | kMbPattern);
            mbB.add(0x2, 6, kMbQuant | kMbBackward | kMbPattern);
            mbB.add(0x1, 6, kMbQuant | kMbIntra);
            mbD.add(0x1, 1, kMbIntra);

            for (int i = 0; i < 64; ++i)
                cbp.add(kCbpCodes[i].code, kCbpCodes[i].length, static_cast<int16_t>(i));

            motion.add(kMotionCodes[0].code, kMotionCodes[0].length, 0);
            for (int i = 1; i <= 16; ++i)
            {
                const uint32_t code = static_cast<uint32_t>(kMotionCodes[i].code) << 1;
                const uint32_t length = kMotionCodes[i].length + 1u;
                motion.add(code, length, static_cast<int16_t>(i));
                motion.add(code | 1u, length, static_cast<int16_t>(-i));
            }

            dmv.add(0x0, 1, 0);
            dmv.add(0x2, 2, 1);
            dmv.add(0x3, 2, -1);

            for (int i = 0; i < 12; ++i)
            {
                dcLuminance.add(kDcLuminanceCodes[i].code, kDcLuminanceCodes[i].length, static_cast<int16_t>(i));
                dcChrominance.add(kDcChrominanceCodes[i].code, kDcChrominanceCodes[i].length, static_cast<int16_t>(i));
            }

            addDct(dctZero, kDctZero, kDctZeroEob);
            addDct(dctOne, kDctOne, kDctOneEob);

            for (int i = 0; i < 64; ++i)
                inverseZigzag[kZigzag[i]] = static_cast<uint8_t>(i);
        }

        static void addDct(Lut &lut, const VlcCode (&codes)[111], VlcCode eob)
        {
            static constexpr int kLevels[32] = {40, 18, 5, 4, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
                                                1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
            int index = 0;
            for (int run = 0; run < 32; ++run)
            {
                for (int level = 1; level <= kLevels[run]; ++level, ++index)
                    lut.add(codes[index].code, codes[index].length, static_cast<int16_t>(level), static_cast<uint8_t>(run));
            }
            lut.add(eob.code, eob.length, 0, 0xFF);
            lut.add(kDctEscape.code, kDctEscape.length, 0, 0xFE);
        }
    };

    const Tables &tables()
    {
        static const Tables instance;
        return instance;
    }

    // ------------------------------------------------------------------ IDCT
    // Chen-Wang IDCT of the MPEG Software Simulation Group's reference decoder
    // (IEEE 1180 compliant), as used by most MPEG-2 decoders.
    constexpr int W1 = 2841;
    constexpr int W2 = 2676;
    constexpr int W3 = 2408;
    constexpr int W5 = 1609;
    constexpr int W6 = 1108;
    constexpr int W7 = 565;

    inline int16_t clip256(int value)
    {
        return static_cast<int16_t>(value < -256 ? -256 : (value > 255 ? 255 : value));
    }

    void idctRow(int16_t *blk)
    {
        int x0, x1, x2, x3, x4, x5, x6, x7, x8;
        if (!((x1 = blk[4] * 2048) | (x2 = blk[6]) | (x3 = blk[2]) | (x4 = blk[1]) | (x5 = blk[7]) | (x6 = blk[5]) |
              (x7 = blk[3])))
        {
            const int16_t value = static_cast<int16_t>(blk[0] * 8);
            for (int i = 0; i < 8; ++i)
                blk[i] = value;
            return;
        }
        x0 = blk[0] * 2048 + 128;
        x8 = W7 * (x4 + x5);
        x4 = x8 + (W1 - W7) * x4;
        x5 = x8 - (W1 + W7) * x5;
        x8 = W3 * (x6 + x7);
        x6 = x8 - (W3 - W5) * x6;
        x7 = x8 - (W3 + W5) * x7;
        x8 = x0 + x1;
        x0 -= x1;
        x1 = W6 * (x3 + x2);
        x2 = x1 - (W2 + W6) * x2;
        x3 = x1 + (W2 - W6) * x3;
        x1 = x4 + x6;
        x4 -= x6;
        x6 = x5 + x7;
        x5 -= x7;
        x7 = x8 + x3;
        x8 -= x3;
        x3 = x0 + x2;
        x0 -= x2;
        x2 = (181 * (x4 + x5) + 128) >> 8;
        x4 = (181 * (x4 - x5) + 128) >> 8;
        blk[0] = static_cast<int16_t>((x7 + x1) >> 8);
        blk[1] = static_cast<int16_t>((x3 + x2) >> 8);
        blk[2] = static_cast<int16_t>((x0 + x4) >> 8);
        blk[3] = static_cast<int16_t>((x8 + x6) >> 8);
        blk[4] = static_cast<int16_t>((x8 - x6) >> 8);
        blk[5] = static_cast<int16_t>((x0 - x4) >> 8);
        blk[6] = static_cast<int16_t>((x3 - x2) >> 8);
        blk[7] = static_cast<int16_t>((x7 - x1) >> 8);
    }

    void idctColumn(int16_t *blk)
    {
        int x0, x1, x2, x3, x4, x5, x6, x7, x8;
        if (!((x1 = blk[8 * 4] * 256) | (x2 = blk[8 * 6]) | (x3 = blk[8 * 2]) | (x4 = blk[8 * 1]) | (x5 = blk[8 * 7]) |
              (x6 = blk[8 * 5]) | (x7 = blk[8 * 3])))
        {
            const int16_t value = clip256((blk[0] + 32) >> 6);
            for (int i = 0; i < 8; ++i)
                blk[8 * i] = value;
            return;
        }
        x0 = blk[0] * 256 + 8192;
        x8 = W7 * (x4 + x5) + 4;
        x4 = (x8 + (W1 - W7) * x4) >> 3;
        x5 = (x8 - (W1 + W7) * x5) >> 3;
        x8 = W3 * (x6 + x7) + 4;
        x6 = (x8 - (W3 - W5) * x6) >> 3;
        x7 = (x8 - (W3 + W5) * x7) >> 3;
        x8 = x0 + x1;
        x0 -= x1;
        x1 = W6 * (x3 + x2) + 4;
        x2 = (x1 - (W2 + W6) * x2) >> 3;
        x3 = (x1 + (W2 - W6) * x3) >> 3;
        x1 = x4 + x6;
        x4 -= x6;
        x6 = x5 + x7;
        x5 -= x7;
        x7 = x8 + x3;
        x8 -= x3;
        x3 = x0 + x2;
        x0 -= x2;
        x2 = (181 * (x4 + x5) + 128) >> 8;
        x4 = (181 * (x4 - x5) + 128) >> 8;
        blk[8 * 0] = clip256((x7 + x1) >> 14);
        blk[8 * 1] = clip256((x3 + x2) >> 14);
        blk[8 * 2] = clip256((x0 + x4) >> 14);
        blk[8 * 3] = clip256((x8 + x6) >> 14);
        blk[8 * 4] = clip256((x8 - x6) >> 14);
        blk[8 * 5] = clip256((x0 - x4) >> 14);
        blk[8 * 6] = clip256((x3 - x2) >> 14);
        blk[8 * 7] = clip256((x7 - x1) >> 14);
    }

    void inverseDct(int16_t *block)
    {
        for (int i = 0; i < 8; ++i)
            idctRow(block + 8 * i);
        for (int i = 0; i < 8; ++i)
            idctColumn(block + i);
    }

    inline uint8_t clampByte(int value)
    {
        return static_cast<uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
    }

    constexpr uint32_t kChcrStart = 0x100u;
    constexpr uint32_t kChcrTie = 0x80u;
    constexpr uint32_t kChcrDirFromMemory = 0x1u;
    constexpr uint32_t kOffsetMadr = 0x10u;
    constexpr uint32_t kOffsetQwc = 0x20u;
    constexpr uint32_t kOffsetTadr = 0x30u;
    constexpr uint32_t kOffsetAsr0 = 0x40u;
    constexpr uint32_t kOffsetAsr1 = 0x50u;

    // IPU_CTRL
    constexpr uint32_t kCtrlEcd = 1u << 14;
    constexpr uint32_t kCtrlScd = 1u << 15;
    constexpr uint32_t kCtrlReset = 1u << 30;
    constexpr uint32_t kCtrlBusy = 1u << 31;

    constexpr size_t kFifoQwords = 8u;
} // namespace

struct Ps2Ipu::State
{
    explicit State(Bus busIn) : bus(std::move(busIn)) {}

    Bus bus;

    // Registers.
    uint32_t command = 0u; // command being executed
    uint32_t data = 0u;    // IPU_CMD DATA
    bool busy = false;
    uint32_t ctrl = 0u; // IPU_CTRL except IFC, OFC and BUSY
    uint32_t top = 0u;
    bool topBusy = true;

    // Input: the FIFO (IFC) and the decoder's two-qword buffer (FP) read at bit BP.
    std::deque<Qword> fifo;
    std::array<uint8_t, 32> internal{};
    uint32_t fp = 0u;
    uint32_t bp = 0u;

    // A command that runs out of input is retried from its start: everything it
    // consumed is restored, followed by the qwords DMA delivered meanwhile.
    struct Snapshot
    {
        std::deque<Qword> fifo;
        std::array<uint8_t, 32> internal{};
        uint32_t fp = 0u;
        uint32_t bp = 0u;
        int dcPredictor[3] = {};
    };
    Snapshot snapshot;
    bool recording = false;
    std::vector<Qword> delivered;

    // Output FIFO, drained by DMA channel 3 or reads of 0x10007000.
    std::deque<uint8_t> output;

    // Decoder state.
    uint8_t intraMatrix[64] = {};    // as loaded by SETIQ (zigzag order)
    uint8_t nonIntraMatrix[64] = {}; //
    uint16_t vqClut[16] = {};
    uint32_t threshold0 = 0u;
    uint32_t threshold1 = 0u;
    int dcPredictor[3] = {128, 128, 128};
    uint32_t macroblocksDone = 0u; // CSC/PACK progress

    // DMA channel 4: stop when the current transfer drains (last tag ended the chain).
    bool toIpuFinished = true;

    bool warnedIdec = false;
    bool warnedCommandWhileBusy = false;

    // ------------------------------------------------------------ DMA
    uint32_t &reg(uint32_t channel, uint32_t offset) { return bus.registerRef(channel + offset); }

    void completeToIpu()
    {
        bus.completeChannel(Ps2Ipu::kToIpuChannel, 4u);
    }

    void completeFromIpu()
    {
        bus.completeChannel(Ps2Ipu::kFromIpuChannel, 3u);
    }

    // Next qword of DMA channel 4, following the source chain.
    bool readToIpu(Qword &qword)
    {
        uint32_t &chcr = reg(Ps2Ipu::kToIpuChannel, 0u);
        for (int guard = 0; guard < 64 && (chcr & kChcrStart) != 0u; ++guard)
        {
            uint32_t &qwc = reg(Ps2Ipu::kToIpuChannel, kOffsetQwc);
            uint32_t &madr = reg(Ps2Ipu::kToIpuChannel, kOffsetMadr);
            if ((qwc & 0xFFFFu) != 0u)
            {
                if (!bus.readMemory(madr, qword.data(), 16u))
                    qword.fill(0u);
                madr += 16u;
                qwc = (qwc & ~0xFFFFu) | ((qwc - 1u) & 0xFFFFu);
                if ((qwc & 0xFFFFu) == 0u && (((chcr >> 2) & 3u) == 0u || toIpuFinished))
                    completeToIpu();
                return true;
            }
            const uint32_t mode = (chcr >> 2) & 3u;
            if (mode != 1u || toIpuFinished)
            {
                completeToIpu();
                return false;
            }

            // Source chain: fetch the next tag.
            uint32_t &tadr = reg(Ps2Ipu::kToIpuChannel, kOffsetTadr);
            uint32_t tag[2] = {};
            if (!bus.readMemory(tadr, tag, sizeof(tag)))
            {
                completeToIpu();
                return false;
            }
            chcr = (chcr & 0xFFFFu) | (tag[0] & 0xFFFF0000u);
            const uint32_t id = (tag[0] >> 28) & 7u;
            const uint32_t count = tag[0] & 0xFFFFu;
            const uint32_t address = tag[1];
            const bool irq = (tag[0] & 0x80000000u) != 0u;
            uint32_t asp = (chcr >> 4) & 3u;
            toIpuFinished = false;
            switch (id)
            {
            case 0: // REFE
                madr = address;
                tadr += 16u;
                toIpuFinished = true;
                break;
            case 1: // CNT
                madr = tadr + 16u;
                tadr = madr + count * 16u;
                break;
            case 2: // NEXT
                madr = tadr + 16u;
                tadr = address;
                break;
            case 3: // REF
            case 4: // REFS
                madr = address;
                tadr += 16u;
                break;
            case 5: // CALL
                madr = tadr + 16u;
                if (asp < 2u)
                {
                    reg(Ps2Ipu::kToIpuChannel, asp == 0u ? kOffsetAsr0 : kOffsetAsr1) = madr + count * 16u;
                    ++asp;
                }
                tadr = address;
                break;
            case 6: // RET
                madr = tadr + 16u;
                if (asp > 0u)
                {
                    --asp;
                    tadr = reg(Ps2Ipu::kToIpuChannel, asp == 0u ? kOffsetAsr0 : kOffsetAsr1);
                }
                else
                {
                    toIpuFinished = true;
                }
                break;
            default: // END
                madr = tadr + 16u;
                toIpuFinished = true;
                break;
            }
            chcr = (chcr & ~0x30u) | (asp << 4);
            qwc = (qwc & ~0xFFFFu) | count;
            if (irq && (chcr & kChcrTie) != 0u)
                toIpuFinished = true;
        }
        return false;
    }

    void drainOutput()
    {
        uint32_t &chcr = reg(Ps2Ipu::kFromIpuChannel, 0u);
        if ((chcr & kChcrStart) == 0u)
            return;
        uint32_t &qwc = reg(Ps2Ipu::kFromIpuChannel, kOffsetQwc);
        uint32_t &madr = reg(Ps2Ipu::kFromIpuChannel, kOffsetMadr);
        if ((qwc & 0xFFFFu) == 0u)
        {
            completeFromIpu();
            return;
        }
        uint8_t qword[16];
        while (output.size() >= 16u && (qwc & 0xFFFFu) != 0u)
        {
            for (uint8_t &byte : qword)
            {
                byte = output.front();
                output.pop_front();
            }
            bus.writeMemory(madr, qword, 16u);
            madr += 16u;
            qwc = (qwc & ~0xFFFFu) | ((qwc - 1u) & 0xFFFFu);
        }
        if ((qwc & 0xFFFFu) == 0u)
            completeFromIpu();
    }

    // ------------------------------------------------------------ bit reader
    bool pullQword(Qword &qword)
    {
        if (fifo.empty())
        {
            Qword incoming{};
            if (!readToIpu(incoming))
                return false;
            if (recording)
                delivered.push_back(incoming);
            fifo.push_back(incoming);
        }
        qword = fifo.front();
        fifo.pop_front();
        return true;
    }

    // Loads the decoder buffer until `bits` bits past BP are present.
    bool fill(uint32_t bits)
    {
        while (fp * 128u < bp + bits)
        {
            if (fp >= 2u)
                return false;
            Qword qword{};
            if (!pullQword(qword))
                return false;
            std::memcpy(internal.data() + fp * 16u, qword.data(), 16u);
            ++fp;
        }
        return true;
    }

    uint32_t available() const
    {
        return fp * 128u > bp ? fp * 128u - bp : 0u;
    }

    // Up to 32 bits at BP, MSB first, without consuming them. Bits past the loaded
    // data read as zero.
    uint32_t peek(uint32_t bits) const
    {
        if (bits == 0u)
            return 0u;
        uint64_t window = 0u;
        const uint32_t firstByte = bp >> 3;
        for (uint32_t i = 0; i < 5u; ++i)
        {
            const uint32_t index = firstByte + i;
            const uint8_t byte = index < fp * 16u ? internal[index] : 0u;
            window = (window << 8) | byte;
        }
        const uint32_t shift = 40u - (bp & 7u) - bits;
        return static_cast<uint32_t>((window >> shift) & ((uint64_t(1) << bits) - 1u));
    }

    void advance(uint32_t bits)
    {
        bp += bits;
        while (bp >= 128u && fp > 0u)
        {
            std::memmove(internal.data(), internal.data() + 16u, 16u);
            --fp;
            bp -= 128u;
        }
    }

    bool skip(uint32_t bits)
    {
        while (bits > 0u)
        {
            const uint32_t chunk = std::min(bits, 32u);
            if (!fill(chunk))
                return false;
            advance(chunk);
            bits -= chunk;
        }
        return true;
    }

    bool getBits(uint32_t bits, uint32_t &value)
    {
        if (!fill(bits))
            return false;
        value = peek(bits);
        advance(bits);
        return true;
    }

    // Variable length code through `lut`. Returns false when the input ran out
    // before the code was complete; `entry.length == 0` marks an invalid code.
    bool decode(const Lut &lut, LutEntry &entry)
    {
        const uint32_t bits = lut.bits();
        fill(bits); // the code may be shorter than the table width near the end of the data
        entry = lut.at(peek(bits));
        if (entry.length == 0u)
            return available() >= bits;
        if (available() < entry.length)
            return false;
        advance(entry.length);
        return true;
    }

    void beginTransaction()
    {
        snapshot.fifo = fifo;
        snapshot.internal = internal;
        snapshot.fp = fp;
        snapshot.bp = bp;
        std::memcpy(snapshot.dcPredictor, dcPredictor, sizeof(dcPredictor));
        delivered.clear();
        recording = true;
    }

    void commit()
    {
        recording = false;
        delivered.clear();
    }

    void rollback()
    {
        fifo = snapshot.fifo;
        for (const Qword &qword : delivered)
            fifo.push_back(qword);
        internal = snapshot.internal;
        fp = snapshot.fp;
        bp = snapshot.bp;
        std::memcpy(dcPredictor, snapshot.dcPredictor, sizeof(dcPredictor));
        recording = false;
        delivered.clear();
    }

    void topUpFifo()
    {
        while (fifo.size() < kFifoQwords)
        {
            Qword qword{};
            if (!readToIpu(qword))
                break;
            fifo.push_back(qword);
        }
    }

    void refreshTop()
    {
        fill(32u);
        const uint32_t bits = std::min(available(), 32u);
        top = bits == 0u ? 0u : peek(bits) << (32u - bits);
        topBusy = bits < 32u;
    }

    // ------------------------------------------------------------ decoding
    uint32_t pictureType() const { return (ctrl >> 24) & 7u; }
    uint32_t intraDcPrecision() const { return (ctrl >> 16) & 3u; }
    bool alternateScan() const { return (ctrl & (1u << 20)) != 0u; }
    bool intraVlcFormat() const { return (ctrl & (1u << 21)) != 0u; }
    bool quantiserScaleType() const { return (ctrl & (1u << 22)) != 0u; }
    bool mpeg1() const { return (ctrl & (1u << 23)) != 0u; }

    enum class Result
    {
        Done,
        NeedData,
        Error,
    };

    // One (run, level) pair; run 0xFF is end of block.
    Result decodeCoefficient(bool tableOne, bool first, bool nonIntra, int &run, int &level)
    {
        if (first && nonIntra && fill(1u) && peek(1u) == 1u)
        {
            // dct_coef_first of non-intra blocks: '1s' is run 0, level 1.
            if (!fill(2u))
                return Result::NeedData;
            level = peek(2u) & 1u ? -1 : 1;
            run = 0;
            advance(2u);
            return Result::Done;
        }
        LutEntry entry{};
        if (!decode(tableOne ? tables().dctOne : tables().dctZero, entry))
            return Result::NeedData;
        if (entry.length == 0u)
            return Result::Error;
        if (entry.run == 0xFFu)
        {
            run = 0xFF;
            return Result::Done;
        }
        if (entry.run == 0xFEu)
        {
            uint32_t escapedRun = 0u;
            if (!getBits(6u, escapedRun))
                return Result::NeedData;
            run = static_cast<int>(escapedRun);
            if (mpeg1())
            {
                uint32_t byte = 0u;
                if (!getBits(8u, byte))
                    return Result::NeedData;
                if (byte == 0x00u || byte == 0x80u)
                {
                    uint32_t extension = 0u;
                    if (!getBits(8u, extension))
                        return Result::NeedData;
                    level = byte == 0x00u ? static_cast<int>(extension) : static_cast<int>(extension) - 256;
                }
                else
                {
                    level = static_cast<int8_t>(byte);
                }
            }
            else
            {
                uint32_t value = 0u;
                if (!getBits(12u, value))
                    return Result::NeedData;
                level = static_cast<int>(value << 20) >> 20;
            }
            if (level == 0)
                return Result::Error;
            return Result::Done;
        }
        uint32_t sign = 0u;
        if (!getBits(1u, sign))
            return Result::NeedData;
        run = entry.run;
        level = sign ? -entry.value : entry.value;
        return Result::Done;
    }

    // Decodes, dequantises and transforms one 8x8 block; `component` 0 is luminance,
    // 1 Cb, 2 Cr. Coefficients land in raster order.
    Result decodeBlock(bool intra, int component, int quantiserScale, int16_t (&block)[64])
    {
        std::fill(std::begin(block), std::end(block), int16_t{0});
        const uint8_t *scan = alternateScan() ? kAlternate : kZigzag;
        const uint8_t *matrix = intra ? intraMatrix : nonIntraMatrix;
        int sum = 0;
        int index = 0;
        bool first = true;

        if (intra)
        {
            LutEntry entry{};
            if (!decode(component == 0 ? tables().dcLuminance : tables().dcChrominance, entry))
                return Result::NeedData;
            if (entry.length == 0u)
                return Result::Error;
            const uint32_t size = static_cast<uint32_t>(entry.value);
            int differential = 0;
            if (size != 0u)
            {
                uint32_t bits = 0u;
                if (!getBits(size, bits))
                    return Result::NeedData;
                differential = (bits & (1u << (size - 1u))) ? static_cast<int>(bits)
                                                            : static_cast<int>(bits) - (1 << size) + 1;
            }
            dcPredictor[component] += differential;
            const int dc = mpeg1() ? dcPredictor[component] * 8 : dcPredictor[component] << (3 - intraDcPrecision());
            block[0] = static_cast<int16_t>(dc);
            sum = dc;
            index = 1;
            first = false;
        }

        const bool tableOne = intra && intraVlcFormat() && !mpeg1();
        for (;;)
        {
            int run = 0;
            int level = 0;
            const Result result = decodeCoefficient(tableOne, first, !intra, run, level);
            if (result != Result::Done)
                return result;
            if (run == 0xFF)
                break;
            first = false;
            index += run;
            if (index > 63)
                return Result::Error;
            const int position = scan[index];
            const int weight = matrix[tables().inverseZigzag[position]];
            const int magnitude = level < 0 ? -level : level;
            int value;
            if (mpeg1())
            {
                value = intra ? (magnitude * quantiserScale * weight) >> 3
                              : ((2 * magnitude + 1) * quantiserScale * weight) >> 4;
                if (value != 0 && (value & 1) == 0)
                    value -= 1;
            }
            else
            {
                value = intra ? (magnitude * quantiserScale * weight) >> 4
                              : ((2 * magnitude + 1) * quantiserScale * weight) >> 5;
            }
            if (level < 0)
                value = -value;
            value = std::clamp(value, -2048, 2047);
            block[position] = static_cast<int16_t>(value);
            sum += value;
            ++index;
        }
        if (!mpeg1() && (sum & 1) == 0)
            block[63] = static_cast<int16_t>(block[63] ^ 1);
        inverseDct(block);
        return Result::Done;
    }

    // BDEC: one macroblock as RAW16 (16x16 luminance, then 8x8 Cb and Cr).
    Result blockDecode(uint32_t word, std::vector<uint8_t> &out)
    {
        const uint32_t forwardBits = word & 0x3Fu;
        const int quantiserCode = static_cast<int>((word >> 16) & 0x1Fu);
        const bool fieldDct = (word & (1u << 25)) != 0u;
        const bool resetDc = (word & (1u << 26)) != 0u;
        const bool intra = (word & (1u << 27)) != 0u;

        if (!skip(forwardBits))
            return Result::NeedData;
        if (resetDc)
        {
            const int reset = mpeg1() ? 128 : 128 << intraDcPrecision();
            dcPredictor[0] = dcPredictor[1] = dcPredictor[2] = reset;
        }
        const int quantiserScale = mpeg1() ? quantiserCode
                                           : (quantiserScaleType() ? kNonLinearQuantiser[quantiserCode] : quantiserCode * 2);

        int pattern = 0x3F;
        if (!intra)
        {
            LutEntry entry{};
            if (!decode(tables().cbp, entry))
                return Result::NeedData;
            if (entry.length == 0u)
                return Result::Error;
            pattern = entry.value;
        }
        ctrl = (ctrl & ~(0x3Fu << 8)) | (static_cast<uint32_t>(pattern) << 8);

        int16_t blocks[6][64] = {};
        for (int i = 0; i < 6; ++i)
        {
            if ((pattern & (1 << (5 - i))) == 0)
                continue;
            const int component = i < 4 ? 0 : i - 3;
            const Result result = decodeBlock(intra, component, quantiserScale, blocks[i]);
            if (result != Result::Done)
                return result;
        }

        int16_t raw[384];
        for (int i = 0; i < 4; ++i)
        {
            const int column = (i & 1) * 8;
            for (int row = 0; row < 8; ++row)
            {
                const int y = fieldDct ? row * 2 + (i >> 1) : row + (i >> 1) * 8;
                std::memcpy(&raw[y * 16 + column], &blocks[i][row * 8], 8 * sizeof(int16_t));
            }
        }
        std::memcpy(&raw[256], blocks[4], 64 * sizeof(int16_t));
        std::memcpy(&raw[320], blocks[5], 64 * sizeof(int16_t));
        out.resize(sizeof(raw));
        std::memcpy(out.data(), raw, sizeof(raw));
        return Result::Done;
    }

    // VDEC: the value in DATA[15:0], the code length in DATA[21:16].
    Result variableDecode(uint32_t word)
    {
        if (!skip(word & 0x3Fu))
            return Result::NeedData;
        const Lut *lut = nullptr;
        switch ((word >> 26) & 3u)
        {
        case 0:
            lut = &tables().mba;
            break;
        case 1:
            switch (pictureType())
            {
            case 2:
                lut = &tables().mbP;
                break;
            case 3:
                lut = &tables().mbB;
                break;
            case 4:
                lut = &tables().mbD;
                break;
            default:
                lut = &tables().mbI;
                break;
            }
            break;
        case 2:
            lut = &tables().motion;
            break;
        default:
            lut = &tables().dmv;
            break;
        }
        LutEntry entry{};
        if (!decode(*lut, entry))
            return Result::NeedData;
        data = entry.length == 0u ? 0u : (static_cast<uint16_t>(entry.value) | (static_cast<uint32_t>(entry.length) << 16));
        if (entry.length == 0u)
            ctrl |= kCtrlEcd;
        return Result::Done;
    }

    // FDEC: skip FB bits, then the next 32 bits without consuming them.
    Result fixedDecode(uint32_t word)
    {
        if (!skip(word & 0x3Fu) || !fill(32u))
            return Result::NeedData;
        data = peek(32u);
        return Result::Done;
    }

    Result setIq(uint32_t word)
    {
        if (!skip(word & 0x3Fu))
            return Result::NeedData;
        uint8_t matrix[64];
        for (uint8_t &value : matrix)
        {
            uint32_t bits = 0u;
            if (!getBits(8u, bits))
                return Result::NeedData;
            value = static_cast<uint8_t>(bits);
        }
        std::memcpy((word & (1u << 27)) ? nonIntraMatrix : intraMatrix, matrix, sizeof(matrix));
        return Result::Done;
    }

    Result setVq()
    {
        uint8_t bytes[32];
        for (uint8_t &value : bytes)
        {
            uint32_t bits = 0u;
            if (!getBits(8u, bits))
                return Result::NeedData;
            value = static_cast<uint8_t>(bits);
        }
        std::memcpy(vqClut, bytes, sizeof(bytes));
        return Result::Done;
    }

    bool readBytes(uint8_t *destination, size_t count)
    {
        for (size_t i = 0; i < count; ++i)
        {
            uint32_t bits = 0u;
            if (!getBits(8u, bits))
                return false;
            destination[i] = static_cast<uint8_t>(bits);
        }
        return true;
    }

    // YCbCr 4:2:0 macroblock to RGBA, with the IPU's coefficients.
    void convertToRgb32(const uint8_t *macroblock, uint8_t *rgba) const
    {
        const uint8_t *luminance = macroblock;
        const uint8_t *cb = macroblock + 256;
        const uint8_t *cr = macroblock + 320;
        for (int y = 0; y < 16; ++y)
        {
            for (int x = 0; x < 16; ++x)
            {
                const int chroma = (y >> 1) * 8 + (x >> 1);
                const int lum = (0x95 * std::max(0, luminance[y * 16 + x] - 16)) >> 6;
                const int red = (0xcc * (cr[chroma] - 128)) >> 6;
                const int greenCr = (-0x68 * (cr[chroma] - 128)) >> 6;
                const int greenCb = (-0x32 * (cb[chroma] - 128)) >> 6;
                const int blue = (0x102 * (cb[chroma] - 128)) >> 6;
                uint8_t *pixel = rgba + (y * 16 + x) * 4;
                pixel[0] = clampByte((lum + red + 1) >> 1);
                pixel[1] = clampByte((lum + greenCr + greenCb + 1) >> 1);
                pixel[2] = clampByte((lum + blue + 1) >> 1);
                pixel[3] = 0x80;
            }
        }
        for (int i = 0; i < 256; ++i)
        {
            uint8_t *pixel = rgba + i * 4;
            if (threshold0 > 0u && pixel[0] < threshold0 && pixel[1] < threshold0 && pixel[2] < threshold0)
                pixel[0] = pixel[1] = pixel[2] = pixel[3] = 0u;
            else if (threshold1 > 0u && pixel[0] < threshold1 && pixel[1] < threshold1 && pixel[2] < threshold1)
                pixel[3] = 0x40;
        }
    }

    void packToRgb16(const uint8_t *rgba, bool dither, uint8_t *out) const
    {
        static constexpr int kDither[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};
        for (int y = 0; y < 16; ++y)
        {
            for (int x = 0; x < 16; ++x)
            {
                const uint8_t *pixel = rgba + (y * 16 + x) * 4;
                const int offset = dither ? kDither[y & 3][x & 3] : 0;
                const uint32_t red = clampByte(pixel[0] + offset) >> 3;
                const uint32_t green = clampByte(pixel[1] + offset) >> 3;
                const uint32_t blue = clampByte(pixel[2] + offset) >> 3;
                const uint16_t value = static_cast<uint16_t>(red | (green << 5) | (blue << 10) | ((pixel[3] >> 7) << 15));
                out[(y * 16 + x) * 2] = static_cast<uint8_t>(value);
                out[(y * 16 + x) * 2 + 1] = static_cast<uint8_t>(value >> 8);
            }
        }
    }

    // CSC: MBC macroblocks of YCbCr to RGB32 or RGB16.
    Result colourConvert(uint32_t word)
    {
        const uint32_t count = word & 0x7FFu;
        const bool dither = (word & (1u << 26)) != 0u;
        const bool rgb16 = (word & (1u << 27)) != 0u;
        while (macroblocksDone < count)
        {
            beginTransaction();
            uint8_t macroblock[384];
            if (!readBytes(macroblock, sizeof(macroblock)))
            {
                rollback();
                return Result::NeedData;
            }
            commit();
            uint8_t rgba[1024];
            convertToRgb32(macroblock, rgba);
            if (rgb16)
            {
                uint8_t packed[512];
                packToRgb16(rgba, dither, packed);
                output.insert(output.end(), std::begin(packed), std::end(packed));
            }
            else
            {
                output.insert(output.end(), std::begin(rgba), std::end(rgba));
            }
            ++macroblocksDone;
            drainOutput();
        }
        return Result::Done;
    }

    // PACK: MBC macroblocks of RGB32 to RGB16 or 4-bit indices into the VQ CLUT.
    Result pack(uint32_t word)
    {
        const uint32_t count = word & 0x7FFu;
        const bool dither = (word & (1u << 26)) != 0u;
        const bool rgb16 = (word & (1u << 27)) != 0u;
        while (macroblocksDone < count)
        {
            beginTransaction();
            uint8_t rgba[1024];
            if (!readBytes(rgba, sizeof(rgba)))
            {
                rollback();
                return Result::NeedData;
            }
            commit();
            if (rgb16)
            {
                uint8_t packed[512];
                packToRgb16(rgba, dither, packed);
                output.insert(output.end(), std::begin(packed), std::end(packed));
            }
            else
            {
                uint8_t indices[128] = {};
                for (int i = 0; i < 256; ++i)
                {
                    const uint8_t *pixel = rgba + i * 4;
                    int best = 0;
                    int bestDistance = 1 << 30;
                    for (int entry = 0; entry < 16; ++entry)
                    {
                        const int red = (vqClut[entry] & 0x1F) << 3;
                        const int green = ((vqClut[entry] >> 5) & 0x1F) << 3;
                        const int blue = ((vqClut[entry] >> 10) & 0x1F) << 3;
                        const int distance = (red - pixel[0]) * (red - pixel[0]) + (green - pixel[1]) * (green - pixel[1]) +
                                             (blue - pixel[2]) * (blue - pixel[2]);
                        if (distance < bestDistance)
                        {
                            bestDistance = distance;
                            best = entry;
                        }
                    }
                    indices[i >> 1] |= static_cast<uint8_t>(best << ((i & 1) * 4));
                }
                output.insert(output.end(), std::begin(indices), std::end(indices));
            }
            ++macroblocksDone;
            drainOutput();
        }
        return Result::Done;
    }

    // Runs the current command as far as the input allows.
    void process()
    {
        if (busy)
        {
            const uint32_t opcode = command >> 28;
            Result result = Result::Done;
            switch (opcode)
            {
            case 0x0: // BCLR
                fifo.clear();
                fp = 0u;
                bp = command & 0x7Fu;
                break;
            case 0x1: // IDEC
                if (!warnedIdec)
                {
                    warnedIdec = true;
                    std::cerr << "[IPU] IDEC is not implemented (command 0x" << std::hex << command << std::dec << ")" << std::endl;
                }
                result = Result::Error;
                break;
            case 0x2: // BDEC
            {
                std::vector<uint8_t> macroblock;
                beginTransaction();
                result = blockDecode(command, macroblock);
                if (result == Result::NeedData)
                    rollback();
                else
                    commit();
                if (result == Result::Done)
                    output.insert(output.end(), macroblock.begin(), macroblock.end());
                break;
            }
            case 0x3: // VDEC
                beginTransaction();
                result = variableDecode(command);
                if (result == Result::NeedData)
                    rollback();
                else
                    commit();
                break;
            case 0x4: // FDEC
                beginTransaction();
                result = fixedDecode(command);
                if (result == Result::NeedData)
                    rollback();
                else
                    commit();
                break;
            case 0x5: // SETIQ
                beginTransaction();
                result = setIq(command);
                if (result == Result::NeedData)
                    rollback();
                else
                    commit();
                break;
            case 0x6: // SETVQ
                beginTransaction();
                result = setVq();
                if (result == Result::NeedData)
                    rollback();
                else
                    commit();
                break;
            case 0x7: // CSC
                result = colourConvert(command);
                break;
            case 0x8: // PACK
                result = pack(command);
                break;
            case 0x9: // SETTH
                threshold0 = command & 0x1FFu;
                threshold1 = (command >> 16) & 0x1FFu;
                break;
            default:
                result = Result::Error;
                break;
            }

            if (result == Result::Error)
                ctrl |= kCtrlEcd;
            if (result != Result::NeedData)
                busy = false;
        }
        drainOutput();
        if (!busy)
        {
            refreshTop();
            topUpFifo();
        }
    }

    void start(uint32_t word)
    {
        if (busy && !warnedCommandWhileBusy)
        {
            warnedCommandWhileBusy = true;
            std::cerr << "[IPU] command 0x" << std::hex << word << " written while 0x" << command
                      << " is still busy" << std::dec << std::endl;
        }
        command = word;
        busy = true;
        macroblocksDone = 0u;
        ctrl &= ~(kCtrlEcd | kCtrlScd);
        process();
    }

    void reset()
    {
        command = 0u;
        data = 0u;
        busy = false;
        ctrl &= 0x07F33F00u;
        top = 0u;
        topBusy = true;
        fifo.clear();
        internal.fill(0u);
        fp = 0u;
        bp = 0u;
        output.clear();
        recording = false;
        delivered.clear();
        macroblocksDone = 0u;
    }

    uint32_t ctrlValue() const
    {
        const uint32_t ifc = static_cast<uint32_t>(std::min(fifo.size(), kFifoQwords));
        const uint32_t ofc = static_cast<uint32_t>(std::min(output.size() / 16u, kFifoQwords));
        return (ctrl & ~(kCtrlBusy | 0xFFu)) | ifc | (ofc << 4) | (busy ? kCtrlBusy : 0u);
    }

    uint32_t bpValue() const
    {
        const uint32_t ifc = static_cast<uint32_t>(std::min(fifo.size(), kFifoQwords));
        return (bp & 0x7Fu) | (ifc << 8) | (fp << 16);
    }
};

Ps2Ipu::Ps2Ipu(Bus bus)
    : m_state(std::make_unique<State>(std::move(bus)))
{
}

Ps2Ipu::~Ps2Ipu() = default;

void Ps2Ipu::reset()
{
    m_state->reset();
    m_state->ctrl = 0u;
    m_state->threshold0 = 0u;
    m_state->threshold1 = 0u;
    m_state->toIpuFinished = true;
    m_state->dcPredictor[0] = m_state->dcPredictor[1] = m_state->dcPredictor[2] = 128;
}

uint32_t Ps2Ipu::read32(uint32_t address)
{
    State &state = *m_state;
    switch (address & 0x3Cu)
    {
    case 0x00:
        return state.data;
    case 0x04:
        return state.busy ? 0x80000000u : 0u;
    case 0x10:
        return state.ctrlValue();
    case 0x20:
        return state.bpValue();
    case 0x30:
        if (!state.busy)
            state.refreshTop();
        return state.top;
    case 0x34:
        if (!state.busy)
            state.refreshTop();
        return (state.busy || state.topBusy) ? 0x80000000u : 0u;
    default:
        return 0u;
    }
}

uint64_t Ps2Ipu::read64(uint32_t address)
{
    const uint32_t base = address & ~7u;
    return static_cast<uint64_t>(read32(base)) | (static_cast<uint64_t>(read32(base + 4u)) << 32);
}

void Ps2Ipu::write32(uint32_t address, uint32_t value)
{
    State &state = *m_state;
    switch (address & 0x3Cu)
    {
    case 0x00:
        state.start(value);
        break;
    case 0x10:
        state.ctrl = (value & 0x47F30000u) | (state.ctrl & 0x8000FFFFu);
        if ((value & kCtrlReset) != 0u)
            state.reset();
        break;
    default:
        break;
    }
}

void Ps2Ipu::readOutFifo(uint8_t qword[16])
{
    State &state = *m_state;
    for (int i = 0; i < 16; ++i)
    {
        if (state.output.empty())
        {
            qword[i] = 0u;
            continue;
        }
        qword[i] = state.output.front();
        state.output.pop_front();
    }
    state.process();
}

void Ps2Ipu::writeInFifo(const uint8_t qword[16])
{
    State &state = *m_state;
    Qword incoming{};
    std::memcpy(incoming.data(), qword, 16u);
    state.fifo.push_back(incoming);
    state.process();
}

void Ps2Ipu::onDmaStart(uint32_t channelBase)
{
    State &state = *m_state;
    if (channelBase == kToIpuChannel)
    {
        const uint32_t chcr = state.reg(kToIpuChannel, 0u);
        const uint32_t qwc = state.reg(kToIpuChannel, kOffsetQwc) & 0xFFFFu;
        if (((chcr >> 2) & 3u) == 1u)
        {
            // Chain mode resumes the transfer the CHCR tag describes, or fetches a tag.
            const uint32_t tagId = (chcr >> 28) & 7u;
            const bool tagIrq = (chcr & 0x80000000u) != 0u;
            state.toIpuFinished = qwc != 0u && (tagId == 0u || tagId == 7u || (tagIrq && (chcr & kChcrTie) != 0u));
        }
        else
        {
            state.toIpuFinished = true;
            if ((chcr & kChcrDirFromMemory) == 0u)
                return; // the IPU only reads channel 4
        }
    }
    state.process();
}

bool Ps2Ipu::busy() const
{
    return m_state->busy;
}
