#include "MiniTest.h"
#include "runtime/ps2_memory.h"

#include <array>
#include <cstring>

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
            t.Equals(m.read32(bp) & 0x7Fu, 24u, "peeking does not consume the result");
            t.Equals(m.read64(top), uint64_t{0xB3123456}, "64-bit TOP and BUSY");
            m.write32(ctrl, 1u << 30);
            t.Equals(m.read32(bp), 0u, "reset empties the FIFO and buffer");
            t.Equals(m.read32(ctrl) & 0x800000FFu, 0u, "reset clears BUSY and FIFO counts");
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
    });
}
