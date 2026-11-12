#include "runtime/ps2_ipu.h"
#include "ps2x/state_archive.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <vector>

#if defined(_M_X64) || defined(__SSE2__)
#define PS2X_IPU_SSE2 1
#include <emmintrin.h>
#endif

namespace
{
    // Byte FIFO for the IPU output: appended in macroblocks, consumed in qwords.
    class ByteFifo
    {
    public:
        size_t size() const { return m_bytes.size() - m_head; }
        bool empty() const { return size() == 0u; }
        void clear()
        {
            m_bytes.clear();
            m_head = 0u;
        }
        void push(const uint8_t *data, size_t count)
        {
            if (m_head != 0u && m_head >= m_bytes.size() / 2u)
            {
                m_bytes.erase(m_bytes.begin(), m_bytes.begin() + static_cast<std::ptrdiff_t>(m_head));
                m_head = 0u;
            }
            m_bytes.insert(m_bytes.end(), data, data + count);
        }
        const uint8_t *data() const { return m_bytes.data() + m_head; }
        void discard(size_t count)
        {
            m_head += std::min(count, size());
            if (m_head == m_bytes.size())
                clear();
        }
        // Copies min(count, size()) bytes out and zero-fills the rest.
        void pop(uint8_t *out, size_t count)
        {
            const size_t taken = std::min(count, size());
            std::memcpy(out, data(), taken);
            std::memset(out + taken, 0, count - taken);
            discard(taken);
        }

    private:
        std::vector<uint8_t> m_bytes;
        size_t m_head = 0u;
    };

    using Qword = std::array<uint8_t, 16>;

    // Input FIFO. Consumed qwords stay stored until compact(), so a command that runs out
    // of input is retried by rewinding to its starting head: the qwords it consumed come
    // back ahead of those DMA delivered meanwhile, without copying the FIFO per command.
    class QwordFifo
    {
    public:
        size_t size() const { return m_qwords.size() - m_head; }
        bool empty() const { return m_head == m_qwords.size(); }
        void clear()
        {
            m_qwords.clear();
            m_head = 0u;
        }
        void push(const Qword &qword) { m_qwords.push_back(qword); }
        const Qword &pop() { return m_qwords[m_head++]; }
        size_t head() const { return m_head; }
        void rewind(size_t head) { m_head = head; }
        void compact()
        {
            m_qwords.erase(m_qwords.begin(), m_qwords.begin() + static_cast<std::ptrdiff_t>(m_head));
            m_head = 0u;
        }
        std::vector<Qword> contents() const { return {m_qwords.begin() + static_cast<std::ptrdiff_t>(m_head), m_qwords.end()}; }

    private:
        std::vector<Qword> m_qwords;
        size_t m_head = 0u;
    };

    inline uint64_t loadBigEndian64(const uint8_t *bytes)
    {
        uint64_t value;
        std::memcpy(&value, bytes, sizeof(value));
#if defined(_MSC_VER)
        return _byteswap_uint64(value);
#else
        return __builtin_bswap64(value);
#endif
    }

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

    // Built flat, then split in two levels so that the 16-bit DCT tables fit the L1 cache:
    // the top 8 bits give the entry when they decide it, else a page indexed by the rest.
    class Lut
    {
    public:
        explicit Lut(uint32_t bits) : m_bits(bits), m_low(bits > 8u ? bits - 8u : 0u), m_pages(size_t(1) << bits) {}

        void add(uint32_t code, uint32_t length, int16_t value, uint8_t run = 0)
        {
            const uint32_t shift = m_bits - length;
            for (uint32_t suffix = 0; suffix < (1u << shift); ++suffix)
                m_pages[(static_cast<size_t>(code) << shift) | suffix] = {value, static_cast<uint8_t>(length), run};
        }
        void split()
        {
            const size_t pageSize = size_t(1) << m_low;
            std::vector<LutEntry> pages;
            m_top.resize(m_pages.size() / pageSize);
            for (size_t top = 0; top < m_top.size(); ++top)
            {
                const LutEntry *page = m_pages.data() + top * pageSize;
                const bool uniform = std::all_of(page, page + pageSize, [page](const LutEntry &entry)
                                                 { return entry.value == page->value && entry.length == page->length && entry.run == page->run; });
                if (uniform)
                {
                    m_top[top] = *page;
                    continue;
                }
                m_top[top] = {static_cast<int16_t>(pages.size() / pageSize), kPage, 0};
                pages.insert(pages.end(), page, page + pageSize);
            }
            m_pages = std::move(pages);
        }
        uint32_t bits() const { return m_bits; }
        LutEntry at(uint32_t index) const
        {
            const LutEntry top = m_top[index >> m_low];
            if (top.length != kPage)
                return top;
            return m_pages[(static_cast<size_t>(top.value) << m_low) | (index & ((1u << m_low) - 1u))];
        }

    private:
        static constexpr uint8_t kPage = 0xFF;
        uint32_t m_bits;
        uint32_t m_low;
        std::vector<LutEntry> m_top;
        std::vector<LutEntry> m_pages;
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

            for (Lut *lut : {&mba, &mbI, &mbP, &mbB, &mbD, &cbp, &motion, &dmv, &dcLuminance, &dcChrominance, &dctZero, &dctOne})
                lut->split();
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

    // Wraps on 32 bits like the int multiply compiled before, where extreme blocks
    // overflow it, without the undefined behaviour.
    inline int times181(int value)
    {
        return static_cast<int>(181u * static_cast<uint32_t>(value));
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
        x2 = (times181(x4 + x5) + 128) >> 8;
        x4 = (times181(x4 - x5) + 128) >> 8;
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
        x2 = (times181(x4 + x5) + 128) >> 8;
        x4 = (times181(x4 - x5) + 128) >> 8;
        blk[8 * 0] = clip256((x7 + x1) >> 14);
        blk[8 * 1] = clip256((x3 + x2) >> 14);
        blk[8 * 2] = clip256((x0 + x4) >> 14);
        blk[8 * 3] = clip256((x8 + x6) >> 14);
        blk[8 * 4] = clip256((x8 - x6) >> 14);
        blk[8 * 5] = clip256((x0 - x4) >> 14);
        blk[8 * 6] = clip256((x3 - x2) >> 14);
        blk[8 * 7] = clip256((x7 - x1) >> 14);
    }

    inline uint8_t clampByte(int value)
    {
        return static_cast<uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
    }

    void applyThresholds(uint32_t threshold0, uint32_t threshold1, uint8_t *rgba)
    {
        if (threshold0 == 0u && threshold1 == 0u)
            return;
        for (int i = 0; i < 256; ++i)
        {
            uint8_t *pixel = rgba + i * 4;
            if (threshold0 > 0u && pixel[0] < threshold0 && pixel[1] < threshold0 && pixel[2] < threshold0)
                pixel[0] = pixel[1] = pixel[2] = pixel[3] = 0u;
            else if (threshold1 > 0u && pixel[0] < threshold1 && pixel[1] < threshold1 && pixel[2] < threshold1)
                pixel[3] = 0x40;
        }
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

namespace ps2_ipu_kernels
{
    void inverseDctScalar(int16_t block[64])
    {
        for (int i = 0; i < 8; ++i)
            idctRow(block + 8 * i);
        for (int i = 0; i < 8; ++i)
            idctColumn(block + i);
    }

    void convertToRgb32Scalar(const uint8_t macroblock[384], uint32_t threshold0, uint32_t threshold1, uint8_t rgba[1024])
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
        applyThresholds(threshold0, threshold1, rgba);
    }

#if PS2X_IPU_SSE2
    namespace
    {
        void transpose(__m128i (&r)[8])
        {
            const __m128i a0 = _mm_unpacklo_epi16(r[0], r[1]), a1 = _mm_unpackhi_epi16(r[0], r[1]);
            const __m128i a2 = _mm_unpacklo_epi16(r[2], r[3]), a3 = _mm_unpackhi_epi16(r[2], r[3]);
            const __m128i a4 = _mm_unpacklo_epi16(r[4], r[5]), a5 = _mm_unpackhi_epi16(r[4], r[5]);
            const __m128i a6 = _mm_unpacklo_epi16(r[6], r[7]), a7 = _mm_unpackhi_epi16(r[6], r[7]);
            const __m128i b0 = _mm_unpacklo_epi32(a0, a2), b1 = _mm_unpackhi_epi32(a0, a2);
            const __m128i b2 = _mm_unpacklo_epi32(a1, a3), b3 = _mm_unpackhi_epi32(a1, a3);
            const __m128i b4 = _mm_unpacklo_epi32(a4, a6), b5 = _mm_unpackhi_epi32(a4, a6);
            const __m128i b6 = _mm_unpacklo_epi32(a5, a7), b7 = _mm_unpackhi_epi32(a5, a7);
            r[0] = _mm_unpacklo_epi64(b0, b4);
            r[1] = _mm_unpackhi_epi64(b0, b4);
            r[2] = _mm_unpacklo_epi64(b1, b5);
            r[3] = _mm_unpackhi_epi64(b1, b5);
            r[4] = _mm_unpacklo_epi64(b2, b6);
            r[5] = _mm_unpackhi_epi64(b2, b6);
            r[6] = _mm_unpacklo_epi64(b3, b7);
            r[7] = _mm_unpackhi_epi64(b3, b7);
        }

        // a * x + b * y for every pair (x, y) of 16-bit lanes.
        inline __m128i madd(__m128i pairs, int a, int b)
        {
            const auto x = static_cast<int16_t>(a), y = static_cast<int16_t>(b);
            return _mm_madd_epi16(pairs, _mm_set_epi16(y, x, y, x, y, x, y, x));
        }

        // times181 on 32-bit lanes, without SSE4.1's pmulld.
        inline __m128i times181(__m128i v)
        {
            return _mm_add_epi32(_mm_add_epi32(_mm_add_epi32(_mm_slli_epi32(v, 7), _mm_slli_epi32(v, 5)),
                                               _mm_add_epi32(_mm_slli_epi32(v, 4), _mm_slli_epi32(v, 2))),
                                 v);
        }

        // idctRow (or idctColumn) on eight rows at once: lane j of v[k] is coefficient k of
        // row j. Products are regrouped into exact 16-bit multiply-adds (W7 * (x4 + x5) +
        // (W1 - W7) * x4 is W1 * x4 + W7 * x5), and the scalar shortcuts for zero
        // coefficients give what the full computation gives.
        template <bool Column>
        void idctPass(__m128i (&v)[8])
        {
            __m128i result[2][8];
            for (int half = 0; half < 2; ++half)
            {
                const auto pair = [&](int a, int b)
                { return half == 0 ? _mm_unpacklo_epi16(v[a], v[b]) : _mm_unpackhi_epi16(v[a], v[b]); };
                const __m128i p04 = pair(0, 4), p17 = pair(1, 7), p53 = pair(5, 3), p26 = pair(2, 6);
                const int scale = Column ? 256 : 2048;
                const __m128i bias = _mm_set1_epi32(Column ? 8192 : 128);
                __m128i x8 = _mm_add_epi32(madd(p04, scale, scale), bias);
                __m128i x0 = _mm_add_epi32(madd(p04, scale, -scale), bias);
                __m128i x4 = madd(p17, W1, W7);
                __m128i x5 = madd(p17, W7, -W1);
                __m128i x6 = madd(p53, W5, W3);
                __m128i x7 = madd(p53, W3, -W5);
                __m128i x2 = madd(p26, W6, -W2);
                __m128i x3 = madd(p26, W2, W6);
                if (Column)
                {
                    const __m128i four = _mm_set1_epi32(4);
                    for (__m128i *x : {&x4, &x5, &x6, &x7, &x2, &x3})
                        *x = _mm_srai_epi32(_mm_add_epi32(*x, four), 3);
                }
                __m128i x1 = _mm_add_epi32(x4, x6);
                x4 = _mm_sub_epi32(x4, x6);
                x6 = _mm_add_epi32(x5, x7);
                x5 = _mm_sub_epi32(x5, x7);
                x7 = _mm_add_epi32(x8, x3);
                x8 = _mm_sub_epi32(x8, x3);
                x3 = _mm_add_epi32(x0, x2);
                x0 = _mm_sub_epi32(x0, x2);
                const __m128i round = _mm_set1_epi32(128);
                x2 = _mm_srai_epi32(_mm_add_epi32(times181(_mm_add_epi32(x4, x5)), round), 8);
                x4 = _mm_srai_epi32(_mm_add_epi32(times181(_mm_sub_epi32(x4, x5)), round), 8);
                const __m128i outputs[8] = {_mm_add_epi32(x7, x1), _mm_add_epi32(x3, x2), _mm_add_epi32(x0, x4),
                                            _mm_add_epi32(x8, x6), _mm_sub_epi32(x8, x6), _mm_sub_epi32(x0, x4),
                                            _mm_sub_epi32(x3, x2), _mm_sub_epi32(x7, x1)};
                for (int k = 0; k < 8; ++k)
                {
                    const __m128i out = _mm_srai_epi32(outputs[k], Column ? 14 : 8);
                    // The row pass keeps the low 16 bits, as the scalar cast to int16_t does.
                    result[half][k] = Column ? out : _mm_srai_epi32(_mm_slli_epi32(out, 16), 16);
                }
            }
            for (int k = 0; k < 8; ++k)
            {
                v[k] = _mm_packs_epi32(result[0][k], result[1][k]);
                if (Column)
                    v[k] = _mm_max_epi16(_mm_min_epi16(v[k], _mm_set1_epi16(255)), _mm_set1_epi16(-256));
            }
        }
    }

    void inverseDct(int16_t block[64])
    {
        __m128i v[8];
        for (int i = 0; i < 8; ++i)
            v[i] = _mm_loadu_si128(reinterpret_cast<const __m128i *>(block + 8 * i));
        transpose(v);
        idctPass<false>(v);
        transpose(v);
        idctPass<true>(v);
        for (int i = 0; i < 8; ++i)
            _mm_storeu_si128(reinterpret_cast<__m128i *>(block + 8 * i), v[i]);
    }

    // The scalar formulas on 16-bit lanes: every product fits but 0x102 * cb, and
    // 0x102 * cb >> 6 is 4 * cb + (cb >> 5).
    void convertToRgb32(const uint8_t macroblock[384], uint32_t threshold0, uint32_t threshold1, uint8_t rgba[1024])
    {
        const __m128i zero = _mm_setzero_si128();
        const __m128i one = _mm_set1_epi16(1);
        const __m128i bias = _mm_set1_epi16(128);
        const __m128i alpha = _mm_set1_epi8(static_cast<char>(0x80));
        for (int y = 0; y < 16; ++y)
        {
            const __m128i luminance = _mm_subs_epu8(_mm_loadu_si128(reinterpret_cast<const __m128i *>(macroblock + y * 16)),
                                                    _mm_set1_epi8(16));
            const __m128i cb8 = _mm_loadl_epi64(reinterpret_cast<const __m128i *>(macroblock + 256 + (y >> 1) * 8));
            const __m128i cr8 = _mm_loadl_epi64(reinterpret_cast<const __m128i *>(macroblock + 320 + (y >> 1) * 8));
            const __m128i cb16 = _mm_unpacklo_epi8(cb8, cb8);
            const __m128i cr16 = _mm_unpacklo_epi8(cr8, cr8);
            __m128i channels[3][2];
            for (int part = 0; part < 2; ++part)
            {
                const auto widen = [&](__m128i bytes)
                { return part == 0 ? _mm_unpacklo_epi8(bytes, zero) : _mm_unpackhi_epi8(bytes, zero); };
                const __m128i lum = _mm_srli_epi16(_mm_mullo_epi16(widen(luminance), _mm_set1_epi16(0x95)), 6);
                const __m128i cb = _mm_sub_epi16(widen(cb16), bias);
                const __m128i cr = _mm_sub_epi16(widen(cr16), bias);
                const __m128i red = _mm_srai_epi16(_mm_mullo_epi16(cr, _mm_set1_epi16(0xcc)), 6);
                const __m128i green = _mm_add_epi16(_mm_srai_epi16(_mm_mullo_epi16(cr, _mm_set1_epi16(-0x68)), 6),
                                                    _mm_srai_epi16(_mm_mullo_epi16(cb, _mm_set1_epi16(-0x32)), 6));
                const __m128i blue = _mm_add_epi16(_mm_slli_epi16(cb, 2), _mm_srai_epi16(cb, 5));
                channels[0][part] = _mm_srai_epi16(_mm_add_epi16(_mm_add_epi16(lum, red), one), 1);
                channels[1][part] = _mm_srai_epi16(_mm_add_epi16(_mm_add_epi16(lum, green), one), 1);
                channels[2][part] = _mm_srai_epi16(_mm_add_epi16(_mm_add_epi16(lum, blue), one), 1);
            }
            const __m128i r = _mm_packus_epi16(channels[0][0], channels[0][1]);
            const __m128i g = _mm_packus_epi16(channels[1][0], channels[1][1]);
            const __m128i b = _mm_packus_epi16(channels[2][0], channels[2][1]);
            const __m128i rg0 = _mm_unpacklo_epi8(r, g), rg1 = _mm_unpackhi_epi8(r, g);
            const __m128i ba0 = _mm_unpacklo_epi8(b, alpha), ba1 = _mm_unpackhi_epi8(b, alpha);
            __m128i *row = reinterpret_cast<__m128i *>(rgba + y * 64);
            _mm_storeu_si128(row + 0, _mm_unpacklo_epi16(rg0, ba0));
            _mm_storeu_si128(row + 1, _mm_unpackhi_epi16(rg0, ba0));
            _mm_storeu_si128(row + 2, _mm_unpacklo_epi16(rg1, ba1));
            _mm_storeu_si128(row + 3, _mm_unpackhi_epi16(rg1, ba1));
        }
        applyThresholds(threshold0, threshold1, rgba);
    }
#else
    void inverseDct(int16_t block[64])
    {
        inverseDctScalar(block);
    }

    void convertToRgb32(const uint8_t macroblock[384], uint32_t threshold0, uint32_t threshold1, uint8_t rgba[1024])
    {
        convertToRgb32Scalar(macroblock, threshold0, threshold1, rgba);
    }
#endif
}

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

    const Tables &vlc = tables();

    // Input: the FIFO (IFC) and the decoder's two-qword buffer (FP) read at bit BP,
    // which stays below 128 whenever bits are peeked.
    QwordFifo fifo;
    std::array<uint8_t, 32> internal{};
    uint32_t fp = 0u;
    uint32_t bp = 0u;

    // A command that runs out of input is retried from its start: everything it
    // consumed is restored, followed by the qwords DMA delivered meanwhile.
    struct Snapshot
    {
        size_t fifoHead = 0u;
        std::array<uint8_t, 32> internal{};
        uint32_t fp = 0u;
        uint32_t bp = 0u;
        int dcPredictor[3] = {};
    };
    Snapshot snapshot;

    // Output FIFO, drained by DMA channel 3 or reads of 0x10007000.
    ByteFifo output;

    // Decoder state.
    uint8_t intraMatrix[64] = {};    // as loaded by SETIQ (zigzag order)
    uint8_t nonIntraMatrix[64] = {}; //
    uint16_t vqClut[16] = {};
    uint32_t threshold0 = 0u;
    uint32_t threshold1 = 0u;
    int dcPredictor[3] = {128, 128, 128};
    uint32_t macroblocksDone = 0u; // CSC/PACK progress
    bool blockDecoded = false;

    // DMA channel 4: stop when the current transfer drains (last tag ended the chain).
    bool toIpuFinished = true;

    const bool traceEnabled = std::getenv("BDR_TRACE_IPU") != nullptr;
    uint32_t traceCommands = 0u;
    uint32_t traceDmas = 0u;
    void trace(const char *event, uint32_t value)
    {
        if (!traceEnabled)
            return;
        uint32_t &count = event[0] == 'c' ? traceCommands : traceDmas;
        ++count;
        if (count > 160u && count % 10000u != 0u)
            return;
        std::cerr << "[ipu] " << event << " #" << count << " value=0x" << std::hex << value
                  << " cmd=0x" << command << " ctrl=0x" << ctrlValue() << " data=0x" << data
                  << " bp=0x" << bpValue() << " top=0x" << top
                  << " to[chcr=0x" << reg(Ps2Ipu::kToIpuChannel, 0u)
                  << " madr=0x" << reg(Ps2Ipu::kToIpuChannel, kOffsetMadr)
                  << " qwc=0x" << reg(Ps2Ipu::kToIpuChannel, kOffsetQwc)
                  << "] from[qwc=0x" << reg(Ps2Ipu::kFromIpuChannel, kOffsetQwc) << "]" << std::dec << '\n';
    }

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
        while (output.size() >= 16u && (qwc & 0xFFFFu) != 0u)
        {
            const uint32_t qwords = static_cast<uint32_t>(std::min<size_t>(output.size() / 16u, qwc & 0xFFFFu));
            // The bus refuses a span it cannot write whole; qword by qword, the
            // scratchpad wraps and an unmapped qword is dropped alone.
            if (!bus.writeMemory(madr, output.data(), qwords * 16u))
                for (uint32_t i = 0; i < qwords; ++i)
                    bus.writeMemory(madr + i * 16u, output.data() + i * 16u, 16u);
            output.discard(qwords * 16u);
            madr += qwords * 16u;
            qwc = (qwc & ~0xFFFFu) | ((qwc - qwords) & 0xFFFFu);
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
            fifo.push(incoming);
        }
        qword = fifo.pop();
        return true;
    }

    // Loads the decoder buffer until `bits` bits past BP are present.
    bool fill(uint32_t bits)
    {
        return fp * 128u >= bp + bits || load(bits);
    }

    bool load(uint32_t bits)
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
        const uint64_t window = loadBigEndian64(internal.data() + (bp >> 3)) << (bp & 7u);
        uint32_t value = static_cast<uint32_t>(window >> (64u - bits));
        const uint32_t loaded = available();
        if (loaded < bits)
            value &= static_cast<uint32_t>(~uint64_t(0) << (bits - loaded));
        return value;
    }

    void advance(uint32_t bits)
    {
        bp += bits;
        while (bp >= 128u && fp > 0u)
        {
            std::memcpy(internal.data(), internal.data() + 16u, 16u);
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
        fifo.compact();
        snapshot.fifoHead = fifo.head();
        snapshot.internal = internal;
        snapshot.fp = fp;
        snapshot.bp = bp;
        std::memcpy(snapshot.dcPredictor, dcPredictor, sizeof(dcPredictor));
    }

    void rollback()
    {
        fifo.rewind(snapshot.fifoHead);
        internal = snapshot.internal;
        fp = snapshot.fp;
        bp = snapshot.bp;
        std::memcpy(dcPredictor, snapshot.dcPredictor, sizeof(dcPredictor));
    }

    void topUpFifo()
    {
        while (fifo.size() < kFifoQwords)
        {
            Qword qword{};
            if (!readToIpu(qword))
                break;
            fifo.push(qword);
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
        if (!decode(tableOne ? vlc.dctOne : vlc.dctZero, entry))
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
            if (!decode(component == 0 ? vlc.dcLuminance : vlc.dcChrominance, entry))
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
            const int weight = matrix[vlc.inverseZigzag[position]];
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
        ps2_ipu_kernels::inverseDct(block);
        return Result::Done;
    }

    // BDEC: one macroblock as RAW16 (16x16 luminance, then 8x8 Cb and Cr).
    Result blockDecode(uint32_t word, int16_t (&raw)[384])
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
            if (!decode(vlc.cbp, entry))
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
            // Intra samples are unsigned pixels; inter samples are signed residuals.
            if (intra)
                for (int16_t &sample : blocks[i])
                    sample = std::clamp(sample, int16_t{0}, int16_t{255});
        }

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
        return Result::Done;
    }

    // After BDEC, skip byte-aligned zero stuffing and expose the next start code.
    // This phase can wait for input after the output DMA has already completed.
    Result finishBlock()
    {
        if (!fill(8u))
            return Result::NeedData;
        if (peek(8u) == 0u)
        {
            if (!skip((8u - (bp & 7u)) & 7u))
                return Result::NeedData;
            for (;;)
            {
                if (!fill(24u))
                    return Result::NeedData;
                const uint32_t prefix = peek(24u);
                if (prefix != 0u)
                {
                    ctrl |= prefix == 1u ? kCtrlScd : kCtrlEcd;
                    break;
                }
                advance(8u);
            }
        }
        if (!fill(32u))
            return Result::NeedData;
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
            lut = &vlc.mba;
            break;
        case 1:
            switch (pictureType())
            {
            case 2:
                lut = &vlc.mbP;
                break;
            case 3:
                lut = &vlc.mbB;
                break;
            case 4:
                lut = &vlc.mbD;
                break;
            default:
                lut = &vlc.mbI;
                break;
            }
            break;
        case 2:
            lut = &vlc.motion;
            break;
        default:
            lut = &vlc.dmv;
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

    // As getBits(8) per byte; on a byte boundary, all the bytes loaded are taken at once.
    bool readBytes(uint8_t *destination, size_t count)
    {
        for (size_t i = 0; i < count;)
        {
            if (!fill(8u))
                return false;
            const size_t run = (bp & 7u) == 0u ? std::min<size_t>(count - i, available() / 8u) : 1u;
            if (run == 1u)
                destination[i] = static_cast<uint8_t>(peek(8u));
            else
                std::memcpy(destination + i, internal.data() + bp / 8u, run);
            advance(static_cast<uint32_t>(run) * 8u);
            i += run;
        }
        return true;
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
            uint8_t rgba[1024];
            ps2_ipu_kernels::convertToRgb32(macroblock, threshold0, threshold1, rgba);
            if (rgb16)
            {
                uint8_t packed[512];
                packToRgb16(rgba, dither, packed);
                output.push(packed, sizeof(packed));
            }
            else
            {
                output.push(rgba, sizeof(rgba));
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
            if (rgb16)
            {
                uint8_t packed[512];
                packToRgb16(rgba, dither, packed);
                output.push(packed, sizeof(packed));
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
                output.push(indices, sizeof(indices));
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
                if (!blockDecoded)
                {
                    int16_t macroblock[384];
                    beginTransaction();
                    result = blockDecode(command, macroblock);
                    if (result == Result::NeedData)
                        rollback();
                    if (result != Result::Done)
                        break;
                    output.push(reinterpret_cast<const uint8_t *>(macroblock), sizeof(macroblock));
                    blockDecoded = true;
                }
                drainOutput();
                if (!output.empty())
                {
                    result = Result::NeedData;
                    break;
                }
                beginTransaction();
                result = finishBlock();
                if (result == Result::NeedData)
                    rollback();
                break;
            }
            case 0x3: // VDEC
                beginTransaction();
                result = variableDecode(command);
                if (result == Result::NeedData)
                    rollback();
                break;
            case 0x4: // FDEC
                beginTransaction();
                result = fixedDecode(command);
                if (result == Result::NeedData)
                    rollback();
                break;
            case 0x5: // SETIQ
                beginTransaction();
                result = setIq(command);
                if (result == Result::NeedData)
                    rollback();
                break;
            case 0x6: // SETVQ
                beginTransaction();
                result = setVq();
                if (result == Result::NeedData)
                    rollback();
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
        blockDecoded = false;
        ctrl &= ~(kCtrlEcd | kCtrlScd);
        process();
        trace("cmd", word);
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
        macroblocksDone = 0u;
        blockDecoded = false;
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
    state.output.pop(qword, 16u);
    state.process();
}

void Ps2Ipu::writeInFifo(const uint8_t qword[16])
{
    State &state = *m_state;
    Qword incoming{};
    std::memcpy(incoming.data(), qword, 16u);
    state.fifo.push(incoming);
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
    state.trace("dma", channelBase);
}

bool Ps2Ipu::busy() const
{
    return m_state->busy;
}

void Ps2Ipu::serialize(ps2x::StateArchive &archive)
{
    State &state = *m_state;
    archive.tag("IPU.");
    // No command is midway through a retry at a save: the snapshot's FIFO and the qwords
    // recorded during a command, which the format keeps, are empty.
    std::vector<Qword> fifo = state.fifo.contents();
    std::vector<Qword> retry;
    bool recording = false;
    archive.value(state.command);
    archive.value(state.data);
    archive.value(state.busy);
    archive.value(state.ctrl);
    archive.value(state.top);
    archive.value(state.topBusy);
    archive.vector(fifo);
    if (archive.loading())
    {
        state.fifo.clear();
        for (const Qword &qword : fifo)
            state.fifo.push(qword);
    }
    archive.value(state.internal);
    archive.value(state.fp);
    archive.value(state.bp);
    archive.vector(retry);
    archive.value(state.snapshot.internal);
    archive.value(state.snapshot.fp);
    archive.value(state.snapshot.bp);
    archive.value(state.snapshot.dcPredictor);
    archive.value(recording);
    archive.vector(retry);
    std::vector<uint8_t> output;
    if (!archive.loading())
    {
        ByteFifo copy = state.output;
        output.resize(copy.size());
        copy.pop(output.data(), output.size());
    }
    archive.vector(output);
    if (archive.loading())
    {
        state.output.clear();
        state.output.push(output.data(), output.size());
    }
    archive.value(state.intraMatrix);
    archive.value(state.nonIntraMatrix);
    archive.value(state.vqClut);
    archive.value(state.threshold0);
    archive.value(state.threshold1);
    archive.value(state.dcPredictor);
    archive.value(state.macroblocksDone);
    archive.value(state.blockDecoded);
    archive.value(state.toIpuFinished);
}
