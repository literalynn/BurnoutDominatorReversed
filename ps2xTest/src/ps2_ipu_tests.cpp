#include "MiniTest.h"
#include "runtime/ps2_ipu.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <random>
#include <vector>

namespace
{
    constexpr uint32_t cmd = 0x10002000u;
    constexpr uint32_t ctrl = cmd + 0x10u;
    constexpr uint32_t bp = cmd + 0x20u;
    constexpr uint32_t top = cmd + 0x30u;
    constexpr uint32_t from = 0x1000B000u;
    constexpr uint32_t to = 0x1000B400u;

    void input(PS2Memory &memory, const std::array<uint8_t, 16> &bytes)
    {
        memory.write128(0x10007010u, _mm_loadu_si128(reinterpret_cast<const __m128i *>(bytes.data())));
    }

    void dma(PS2Memory &memory, uint32_t channel, uint32_t address, uint32_t qwords, uint32_t chcr)
    {
        memory.write32(channel + 0x10u, address);
        memory.write32(channel + 0x20u, qwords);
        memory.write32(channel, chcr);
    }

    constexpr uint8_t kZigzag[64] = {
        0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
        12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
        35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
        58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

    struct BitWriter
    {
        std::vector<uint8_t> bytes;
        uint32_t bits = 0u;
        void put(uint32_t value, uint32_t length)
        {
            for (uint32_t i = length; i-- > 0u; ++bits)
            {
                if (bits % 8u == 0u)
                    bytes.push_back(0u);
                bytes.back() |= static_cast<uint8_t>(((value >> i) & 1u) << (7u - bits % 8u));
            }
        }
    };

    // An intra macroblock of six random blocks (DC size 0, AC as escape codes or as some
    // short and long table B.14 codes), after a SETIQ matrix of 16, and the RAW16 that
    // the reference dequantiser and IDCT make of it at quantiser_scale_code 1.
    struct Macroblock
    {
        std::vector<uint8_t> stream;
        int16_t raw[384] = {};
    };

    Macroblock makeMacroblock(std::mt19937 &rng)
    {
        struct TableCode
        {
            int run, level;
            uint32_t code, length;
        };
        static constexpr TableCode kCodes[] = {{0, 1, 0x3, 2}, {1, 1, 0x3, 3}, {0, 2, 0x4, 4}, {0, 40, 0x10, 15}, {31, 1, 0x1b, 16}};
        const auto random = [&](int low, int high) { return std::uniform_int_distribution<int>(low, high)(rng); };
        Macroblock result;
        BitWriter bits;
        for (int i = 0; i < 64; ++i)
            bits.put(16u, 8u);
        int16_t blocks[6][64] = {};
        for (int b = 0; b < 6; ++b)
        {
            int16_t *block = blocks[b];
            bits.put(b < 4 ? 0x4u : 0x0u, b < 4 ? 3u : 2u);
            block[0] = 1024; // DC predictor 128, precision 0
            int sum = 1024;
            const int kind = random(0, 3); // 0: DC only, 3: saturated levels
            for (int index = 1; kind != 0 && random(0, 15) != 0;)
            {
                int run = random(0, 3);
                int level = kind == 3 ? (random(0, 1) ? 2047 : -2047) : random(-2047, 2047);
                const TableCode *table = random(0, 2) == 0 ? &kCodes[random(0, 4)] : nullptr;
                if (table)
                {
                    run = table->run;
                    level = random(0, 1) ? table->level : -table->level;
                }
                if (level == 0 || index + run > 63)
                    break;
                if (table)
                {
                    bits.put(table->code, table->length);
                    bits.put(level < 0 ? 1u : 0u, 1u);
                }
                else
                {
                    bits.put(0x1u, 6u);
                    bits.put(static_cast<uint32_t>(run), 6u);
                    bits.put(static_cast<uint32_t>(level) & 0xFFFu, 12u);
                }
                index += run;
                const int value = std::clamp(level * 2, -2048, 2047);
                block[kZigzag[index]] = static_cast<int16_t>(value);
                sum += value;
                ++index;
            }
            bits.put(0x2u, 2u); // EOB
            if ((sum & 1) == 0)
                block[63] ^= 1;
            ps2_ipu_kernels::inverseDctScalar(block);
            for (int i = 0; i < 64; ++i)
                block[i] = std::clamp<int16_t>(block[i], 0, 255);
        }
        for (int i = 0; i < 4; ++i)
            for (int row = 0; row < 8; ++row)
                std::memcpy(&result.raw[(row + (i >> 1) * 8) * 16 + (i & 1) * 8], &blocks[i][row * 8], 16);
        std::memcpy(&result.raw[256], blocks[4], 128);
        std::memcpy(&result.raw[320], blocks[5], 128);
        result.stream = std::move(bits.bytes);
        for (uint8_t byte : {0x00, 0x00, 0x01, 0xB3})
            result.stream.push_back(byte);
        result.stream.resize((result.stream.size() + 31u) / 16u * 16u, 0u);
        return result;
    }

    // SETIQ then BDEC on `stream`, given to DMA channel 4 at once or one qword per transfer.
    bool decodeMacroblock(const std::vector<uint8_t> &stream, bool qwordByQword, int16_t (&raw)[384], uint32_t &ctrlAfter)
    {
        PS2Memory m;
        if (!m.initialize())
            return false;
        std::memcpy(m.getRDRAM() + 0x10000, stream.data(), stream.size());
        const uint32_t qwords = static_cast<uint32_t>(stream.size() / 16u);
        uint32_t sent = 0u;
        const auto run = [&](uint32_t command)
        {
            m.write32(cmd, command);
            for (int guard = 0; guard < 4096 && (m.read32(ctrl) >> 31) != 0u; ++guard)
            {
                if (sent == qwords || (m.read32(to) & 0x100u) != 0u)
                    return false;
                const uint32_t count = qwordByQword ? 1u : qwords - sent;
                dma(m, to, 0x10000u + sent * 16u, count, 0x101u);
                sent += count;
            }
            return true;
        };
        dma(m, from, 0x80001000u, 48u, 0x100u);
        if (!run(0x50000000u) || !run(0x2C010000u))
            return false;
        ctrlAfter = m.read32(ctrl);
        std::memcpy(raw, m.getScratchpad() + 0x1000, sizeof(raw));
        return m.read32(from + 0x20u) == 0u;
    }
}

void register_ps2_ipu_tests()
{
    MiniTest::Case("IPU", [](TestCase &test) {
        test.Run("FDEC waits for input and preserves the bit cursor", [](TestCase &t) {
            PS2Memory m;
            t.IsTrue(m.initialize(), "memory initialization");
            m.write32(cmd, 0x40000000u);
            t.IsTrue((m.read64(cmd) >> 63) != 0u, "CMD BUSY without input");
            input(m, {0x00, 0x00, 0x01, 0xB3, 0x12, 0x34, 0x56, 0x78});
            t.Equals(m.read64(cmd), uint64_t{0x1B3}, "sequence header prefix, BUSY cleared");
            m.write32(cmd, 0x40000018u);
            t.Equals(m.read32(cmd), 0xB3123456u, "FDEC skips 24 bits and peeks 32");
            t.Equals(m.read64(cmd), uint64_t{0xB3123456}, "DATA bit 31 does not set the CMD BUSY bit 63");
            t.Equals(m.read32(bp) & 0x7Fu, 24u, "peeking does not consume the result");
            t.Equals(m.read64(top), uint64_t{0xB3123456}, "64-bit TOP and BUSY");
            m.write32(ctrl, 1u << 30);
            t.Equals(m.read32(bp), 0u, "reset empties the FIFO and buffer");
            t.Equals(m.read32(ctrl) & 0x800000FFu, 0u, "reset clears BUSY and FIFO counts");
        });

        test.Run("64-bit reads reach the IPU through KSEG1, not through the scratchpad", [](TestCase &t) {
            PS2Memory m;
            t.IsTrue(m.initialize(), "memory initialization");
            m.write64(0x70002020u, 0x0123456789ABCDEFull);
            t.Equals(m.read64(0x70002020u), uint64_t{0x0123456789ABCDEFull}, "scratchpad 0x2020 is not IPU_BP");
            input(m, {0x00, 0x00, 0x01, 0xB3});
            m.write32(cmd, 0x40000000u);
            t.Equals(m.read64(cmd | 0xA0000000u), uint64_t{0x1B3}, "IPU_CMD through KSEG1");
        });

        test.Run("VDEC decodes MPEG values and reports invalid codes", [](TestCase &t) {
            PS2Memory m;
            t.IsTrue(m.initialize(), "memory initialization");
            input(m, {0x80}); // MBA increment 1: code '1'
            m.write32(cmd, 0x30000000u);
            t.Equals(m.read32(cmd), 0x00010001u, "MBA value and code length");
            m.write32(cmd, 0x30000000u); // only zero bits remain: invalid VLC
            t.Equals(m.read32(cmd), 0u, "invalid VLC data");
            t.IsTrue((m.read32(ctrl) & 0x4000u) != 0u, "invalid VLC raises ECD");
        });

        test.Run("input DMA is throttled by the FIFO and resumes on consumption", [](TestCase &t) {
            PS2Memory m;
            t.IsTrue(m.initialize(), "memory initialization");
            for (uint32_t i = 0; i < 256u; ++i)
                m.getRDRAM()[0x2000u + i] = static_cast<uint8_t>(i);
            dma(m, to, 0x2000u, 16u, 0x101u);
            t.IsTrue((m.read32(to) & 0x100u) != 0u, "DMA remains active while FIFO full");
            t.IsTrue(m.read32(to + 0x20u) > 0u, "not all input accepted immediately");
            t.Equals(m.read32(ctrl) & 0xFu, 8u, "eight qwords in input FIFO");
            m.write32(cmd, 0x40000000u);
            t.Equals(m.read32(cmd), 0x00010203u, "DMA data in stream order");
            for (int i = 0; i < 40; ++i)
                m.write32(cmd, 0x40000020u);
            t.Equals(m.read32(to + 0x20u), 0u, "input DMA fully consumed");
            t.Equals(m.read32(to) & 0x100u, 0u, "STR cleared on completion");
            t.IsTrue((m.read32(0x1000E010u) & (1u << 4)) != 0u, "channel 4 interrupt status");
        });

        test.Run("CSC waits for a split DMA input and writes a neutral macroblock", [](TestCase &t) {
            PS2Memory m;
            t.IsTrue(m.initialize(), "memory initialization");
            std::memset(m.getRDRAM() + 0x2000, 16, 256);
            std::memset(m.getRDRAM() + 0x2100, 128, 128);
            dma(m, from, 0x4000u, 64u, 0x100u);
            m.write32(cmd, 0x70000001u);
            dma(m, to, 0x2000u, 12u, 0x101u);
            t.IsTrue((m.read32(ctrl) >> 31) != 0u, "CSC remains busy with half the input");
            t.Equals(m.read32(from + 0x20u), 64u, "no partial output or fabricated completion");
            dma(m, to, 0x20C0u, 12u, 0x101u);
            t.Equals(m.read32(ctrl) >> 31, 0u, "CSC resumes on DMA refill");
            t.Equals(m.read32(from + 0x20u), 0u, "64 output qwords delivered");
            for (int i = 0; i < 256; ++i)
                t.Equals(m.read32(0x4000u + i * 4u), 0x80000000u, "black RGB with IPU alpha");
        });

        test.Run("IPU input follows REF and REFE source tags", [](TestCase &t) {
            PS2Memory m;
            t.IsTrue(m.initialize(), "memory initialization");
            std::memset(m.getRDRAM() + 0x2000, 0x11, 16);
            std::memset(m.getRDRAM() + 0x2100, 0x22, 16);
            m.write64(0x3000, (uint64_t{0x2000} << 32) | 0x30000001u);
            m.write64(0x3010, (uint64_t{0x2100} << 32) | 0x00000001u);
            m.write32(to + 0x30, 0x3000u);
            m.write32(to + 0x20, 0u);
            m.write32(to, 0x105u);
            m.write32(cmd, 0x40000000u);
            t.Equals(m.read32(cmd), 0x11111111u, "first REF payload");
            for (int i = 0; i < 4; ++i)
                m.write32(cmd, 0x40000020u);
            t.Equals(m.read32(cmd), 0x22222222u, "REFE payload after crossing the qword");
            t.Equals(m.read32(to) & 0x100u, 0u, "chain completed after REFE");
            t.Equals(m.read32(to + 0x30), 0x3020u, "TADR advanced beyond the terminal tag");
        });

        test.Run("BDEC waits for output DMA and then a split start code", [](TestCase &t) {
            PS2Memory m;
            t.IsTrue(m.initialize(), "memory initialization");
            input(m, {0x94, 0xA5, 0x22, 0x20});
            m.write32(cmd, 0x2C010000u);
            t.IsTrue((m.read32(ctrl) >> 31) != 0u, "output waiting without channel 3");
            dma(m, from, 0x80001000u, 48u, 0x100u);
            t.Equals(m.read32(from + 0x20), 0u, "output completes before the next input arrives");
            t.IsTrue((m.read32(ctrl) >> 31) != 0u, "BDEC waits for a header after zero stuffing");
            input(m, {0x00, 0x00, 0x01, 0xB3});
            t.Equals(m.read64(top), uint64_t{0x1B3}, "header found after refill");
            t.Equals(m.read32(ctrl) >> 31, 0u, "command completed");
            t.IsTrue((m.read32(ctrl) & 0x8000u) != 0u, "SCD set on the sequence header");
            t.Equals(m.read32(ctrl) & 0xF0u, 0u, "macroblock output not duplicated during retry");
            t.Equals(m.read32(from + 0x10), 0x80001300u, "output cursor advanced exactly once");
        });

        test.Run("SPR_TO source chain gathers blocks and wraps scratchpad", [](TestCase &t) {
            PS2Memory m;
            t.IsTrue(m.initialize(), "memory initialization");
            for (uint32_t i = 0; i < 32; ++i)
                m.getRDRAM()[0x2000 + i] = static_cast<uint8_t>(i + 1);
            m.write64(0x3000, (uint64_t{0x2000} << 32) | 0x30000001u); // REF
            m.write64(0x3010, (uint64_t{0x2010} << 32) | 0x00000001u); // REFE
            m.write32(0x1000D480u, 0x3FF0u);
            m.write32(0x1000D430u, 0x3000u);
            m.write32(0x1000D420u, 0u);
            m.write32(0x1000D400u, 0x105u);
            for (uint32_t i = 0; i < 32; ++i)
                t.Equals(m.getScratchpad()[(0x3FF0 + i) & 0x3FFF], static_cast<uint8_t>(i + 1), "gathered reference blocks");
            t.Equals(m.read32(0x1000D400u) & 0x100u, 0u, "SPR_TO STR cleared");
            t.Equals(m.read32(0x1000D480u), 0x10u, "wrapped SADR");
            const auto causes = m.consumeCompletedDmacCauses();
            t.Equals(causes.size(), size_t{1}, "one IRQ for the complete chain");
            if (!causes.empty())
                t.Equals(causes[0], 9u, "channel 9 IRQ");
        });

        test.Run("BDEC decodes six intra blocks to RAW16 in scratchpad", [](TestCase &t) {
            PS2Memory m;
            t.IsTrue(m.initialize(), "memory initialization");
            // Four luma blocks: DC size 0 ('100'), EOB ('10'); two chroma:
            // DC size 0 ('00'), EOB ('10'). Constant 128 in all components.
            input(m, {0x94, 0xA5, 0x22, 0x20, 0x00, 0x00, 0x01, 0xB3});
            dma(m, from, 0x80001000u, 48u, 0x100u);
            m.write32(cmd, 0x2C010000u);
            t.Equals(m.read32(ctrl) & 0x4000u, 0u, "valid macroblock without ECD");
            t.Equals(m.read32(from + 0x20u), 0u, "768 RAW16 bytes transferred");
            t.IsTrue((m.read32(ctrl) & 0x8000u) != 0u, "start code detection after byte alignment");
            t.Equals(m.read64(top), uint64_t{0x1B3}, "next MPEG header in TOP");
            for (int i = 0; i < 384; ++i)
                t.Equals(m.read16(0x70001000u + i * 2u), uint16_t{128}, "constant intra samples");
        });

        test.Run("vector IDCT matches the scalar reference bit for bit", [](TestCase &t) {
            std::mt19937 rng(12345u);
            const auto random = [&](int low, int high) { return std::uniform_int_distribution<int>(low, high)(rng); };
            int differing = 0;
            for (int trial = 0; trial < 50000; ++trial)
            {
                int16_t block[64] = {};
                switch (trial % 5)
                {
                case 0: // dense, over the dequantiser's range
                    for (int16_t &value : block)
                        value = static_cast<int16_t>(random(-2048, 2047));
                    break;
                case 1: // a few coefficients
                    for (int i = random(1, 6); i > 0; --i)
                        block[random(0, 63)] = static_cast<int16_t>(random(-2048, 2047));
                    break;
                case 2: // DC only
                    block[0] = static_cast<int16_t>(random(-2048, 2047));
                    break;
                case 3: // saturated: the row pass overflows 16 bits and the output clips
                    for (int16_t &value : block)
                        value = static_cast<int16_t>(random(0, 1) ? 2047 : -2048);
                    break;
                default: // any 16-bit value
                    for (int16_t &value : block)
                        value = static_cast<int16_t>(random(-32768, 32767));
                    break;
                }
                int16_t vector[64];
                int16_t scalar[64];
                std::memcpy(vector, block, sizeof(block));
                std::memcpy(scalar, block, sizeof(block));
                ps2_ipu_kernels::inverseDct(vector);
                ps2_ipu_kernels::inverseDctScalar(scalar);
                differing += std::memcmp(vector, scalar, sizeof(block)) != 0;
            }
            t.Equals(differing, 0, "blocks that differ");
        });

        test.Run("vector colour conversion matches the scalar reference bit for bit", [](TestCase &t) {
            std::mt19937 rng(54321u);
            static constexpr uint32_t kThresholds[][2] = {{0, 0}, {0, 0}, {40, 0}, {0, 90}, {60, 120}, {300, 511}};
            int differing = 0;
            for (int trial = 0; trial < 2048; ++trial)
            {
                uint8_t macroblock[384];
                for (uint8_t &byte : macroblock)
                    byte = static_cast<uint8_t>(rng());
                if (trial < 1024) // every (Cb, Cr) pair once, every luminance in each macroblock
                {
                    for (int i = 0; i < 256; ++i)
                        macroblock[i] = static_cast<uint8_t>(i + trial);
                    for (int i = 0; i < 64; ++i)
                    {
                        macroblock[256 + i] = static_cast<uint8_t>(trial * 64 + i);
                        macroblock[320 + i] = static_cast<uint8_t>((trial * 64 + i) >> 8);
                    }
                }
                const uint32_t *threshold = kThresholds[trial % 6];
                uint8_t vector[1024];
                uint8_t scalar[1024];
                ps2_ipu_kernels::convertToRgb32(macroblock, threshold[0], threshold[1], vector);
                ps2_ipu_kernels::convertToRgb32Scalar(macroblock, threshold[0], threshold[1], scalar);
                differing += std::memcmp(vector, scalar, sizeof(vector)) != 0;
            }
            t.Equals(differing, 0, "macroblocks that differ");
        });

        test.Run("BDEC decodes random escape and table codes as the reference, whole or a qword at a time", [](TestCase &t) {
            std::mt19937 rng(777u);
            int differing = 0;
            int failed = 0;
            for (int trial = 0; trial < 200; ++trial)
            {
                const Macroblock macroblock = makeMacroblock(rng);
                for (const bool qwordByQword : {false, true})
                {
                    int16_t raw[384] = {};
                    uint32_t ctrlAfter = 0u;
                    if (!decodeMacroblock(macroblock.stream, qwordByQword, raw, ctrlAfter) || (ctrlAfter & 0xC000u) != 0x8000u)
                        ++failed;
                    differing += std::memcmp(raw, macroblock.raw, sizeof(raw)) != 0;
                }
            }
            t.Equals(failed, 0, "decodes without completion, ECD or the next start code");
            t.Equals(differing, 0, "macroblocks that differ from the reference");
        });
    });
}
