// Based on Blackline Interactive implementation
#include "runtime/ps2_memory.h"
#include <cstring>

enum VIFCmd : uint8_t
{
    VIF_NOP = 0x00,
    VIF_STCYCL = 0x01,
    VIF_OFFSET = 0x02,
    VIF_BASE = 0x03,
    VIF_ITOP = 0x04,
    VIF_STMOD = 0x05,
    VIF_MSKPATH3 = 0x06,
    VIF_MARK = 0x07,
    VIF_FLUSHE = 0x10,
    VIF_FLUSH = 0x11,
    VIF_FLUSHA = 0x13,
    VIF_MSCAL = 0x14,
    VIF_MSCALF = 0x15,
    VIF_MSCNT = 0x17,
    VIF_STMASK = 0x20,
    VIF_STROW = 0x30,
    VIF_STCOL = 0x31,
    VIF_MPG = 0x4A,
    VIF_DIRECT = 0x50,
    VIF_DIRECTHL = 0x51,
};

namespace
{
    // V1-V4 of 16 or 8 bits without mask or mode, every write with its source vector
    // (CL >= WL): normals, colours and texture coordinates. V2 writes x y x y; V3's w
    // takes the element after z, or keeps its value when the data ends there. One
    // instance per format, so no element tests the format again.
    template <uint32_t Vl, uint32_t Vn, bool ZeroExtend>
    void unpackNarrow(uint8_t *vuData, uint32_t qwordMask, uint32_t vuAddr, uint32_t cl, uint32_t wl,
                      const uint8_t *srcBase, const uint8_t *data, uint32_t sizeBytes, uint32_t writeVectorCount)
    {
        constexpr uint32_t elementBytes = 4u >> Vl;
        constexpr uint32_t bytesPerVector = (Vn + 1u) * elementBytes;
        const auto element = [](const uint8_t *source) -> uint32_t
        {
            if constexpr (Vl == 1u)
            {
                uint16_t raw;
                std::memcpy(&raw, source, sizeof(raw));
                return ZeroExtend ? raw : static_cast<uint32_t>(static_cast<int16_t>(raw));
            }
            else
                return ZeroExtend ? *source : static_cast<uint32_t>(static_cast<int8_t>(*source));
        };
        uint32_t blockStart = vuAddr, row = 0u;
        for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
        {
            uint8_t *destination = vuData + ((blockStart + row) & qwordMask) * 16u;
            if (++row == wl)
            {
                row = 0u;
                blockStart += cl;
            }
            const uint8_t *srcVec = srcBase + writeIndex * bytesPerVector;
            uint32_t lanes[4];
            if constexpr (Vn == 0u)
                lanes[0] = lanes[1] = lanes[2] = lanes[3] = element(srcVec);
            else if constexpr (Vn == 1u)
            {
                lanes[0] = lanes[2] = element(srcVec);
                lanes[1] = lanes[3] = element(srcVec + elementBytes);
            }
            else
            {
                if constexpr (Vn == 2u)
                    std::memcpy(&lanes[3], destination + 12u, sizeof(lanes[3]));
                lanes[0] = element(srcVec);
                lanes[1] = element(srcVec + elementBytes);
                lanes[2] = element(srcVec + 2u * elementBytes);
                if (Vn == 3u || static_cast<size_t>(srcVec - data) + 4u * elementBytes <= sizeBytes)
                    lanes[3] = element(srcVec + 3u * elementBytes);
            }
            std::memcpy(destination, lanes, sizeof(lanes));
        }
    }

    using NarrowUnpack = void (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, const uint8_t *, const uint8_t *,
                                  uint32_t, uint32_t);
    template <uint32_t Vl, uint32_t Vn>
    constexpr NarrowUnpack narrowPair[2] = {&unpackNarrow<Vl, Vn, false>, &unpackNarrow<Vl, Vn, true>};
    // [vl - 1][vn][zero extension]
    constexpr const NarrowUnpack *narrowUnpacks[2][4] = {
        {narrowPair<1, 0>, narrowPair<1, 1>, narrowPair<1, 2>, narrowPair<1, 3>},
        {narrowPair<2, 0>, narrowPair<2, 1>, narrowPair<2, 2>, narrowPair<2, 3>},
    };

    // UNPACK into VU data memory (PCSX2 Vif_Unpack.cpp), shared by VIF0 and VIF1. qwordMask is
    // 0x3FF for VU1 and 0xFF for VU0; only VIF1 adds TOPS (FLG). Returns the source bytes used.
    uint32_t unpackToVu(VIFRegisters &regs, uint8_t *vuData, uint32_t qwordMask, bool useTops,
                        const uint8_t *data, uint32_t sizeBytes, uint32_t pos,
                        uint8_t opcode, uint16_t imm, uint8_t num, bool trace)
    {
        uint8_t vn = (opcode >> 2) & 0x3;
        uint8_t vl = opcode & 0x3;
        const bool maskEnable = (opcode & 0x10u) != 0u;
        int components = vn + 1;
        int bitsPerComponent = 32;
        switch (vl)
        {
        case 0:
            bitsPerComponent = 32;
            break;
        case 1:
            bitsPerComponent = 16;
            break;
        case 2:
            bitsPerComponent = 8;
            break;
        case 3:
            bitsPerComponent = (vn == 3) ? 4 : 16;
            break;
        default:
            break;
        }
        int bitsPerVector = (vl == 3 && vn == 3) ? 16 : (components * bitsPerComponent);
        uint32_t bytesPerVector = (bitsPerVector + 7) / 8;
        // UNPACK semantics: NUM is 8-bit and NUM==0 means 256 vectors (writes).
        const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);

        // STCYCL controls write cycles for UNPACK.
        uint32_t cl = regs.cycle & 0xFFu;
        uint32_t wl = (regs.cycle >> 8) & 0xFFu;
        if (cl == 0u)
            cl = 1u;
        if (wl == 0u)
            wl = 1u;

        uint32_t sourceVectorCount = writeVectorCount;
        if (cl < wl)
        {
            const uint32_t fullBlocks = writeVectorCount / wl;
            uint32_t remainder = writeVectorCount % wl;
            if (remainder > cl)
                remainder = cl;
            sourceVectorCount = fullBlocks * cl + remainder;
        }

        uint32_t totalBytes = sourceVectorCount * bytesPerVector;
        totalBytes = (totalBytes + 3) & ~3u;

        uint32_t vuAddr = (uint32_t)imm & qwordMask;
        if (useTops && (imm & 0x8000u) != 0u)
            vuAddr = (vuAddr + (regs.tops & qwordMask)) & qwordMask;

        const bool zeroExtend = (imm & 0x4000u) != 0u;

        if (pos + totalBytes > sizeBytes && trace)
            std::printf("[vif1] UNPACK truncated: needs 0x%x bytes, 0x%x left\n", totalBytes, sizeBytes - pos);
        if (vuData && totalBytes > 0 && pos + totalBytes <= sizeBytes)
        {
            const uint8_t *srcBase = data + pos;
            // V4-32 and V3-32 without mask or mode, the vertex streams: every write
            // has its source vector (CL >= WL), copied whole. A V3's w takes the
            // source word after z, or keeps its value when the data ends there.
            if (vl == 0u && vn >= 2u && !maskEnable && (regs.mode & 3u) == 0u && cl >= wl)
            {
                // Write `writeIndex` goes to block writeIndex / WL, row writeIndex % WL,
                // both kept as counters: two divisions a vector cost more than its copy.
                uint32_t blockStart = vuAddr, row = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    uint8_t *destination = vuData + ((blockStart + row) & qwordMask) * 16u;
                    if (++row == wl)
                    {
                        row = 0u;
                        blockStart += cl;
                    }
                    const uint8_t *srcVec = srcBase + writeIndex * bytesPerVector;
                    // Constant sizes, which the compiler copies inline.
                    if (vn == 3u || static_cast<size_t>(srcVec - data) + 16u <= sizeBytes)
                        std::memcpy(destination, srcVec, 16u);
                    else
                        std::memcpy(destination, srcVec, 12u);
                }
                return totalBytes;
            }
            if ((vl == 1u || vl == 2u) && !maskEnable && (regs.mode & 3u) == 0u && cl >= wl)
            {
                narrowUnpacks[vl - 1u][vn][zeroExtend ? 1 : 0](vuData, qwordMask, vuAddr, cl, wl, srcBase, data, sizeBytes,
                                                               writeVectorCount);
                return totalBytes;
            }
            uint32_t srcIndex = 0u;
            for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
            {
                const uint32_t cyclePos = writeIndex % wl;
                const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);

                uint32_t destVec = 0;
                if (cl >= wl)
                {
                    destVec = (vuAddr + (writeIndex / wl) * cl + cyclePos) & qwordMask;
                }
                else
                {
                    destVec = (vuAddr + writeIndex) & qwordMask;
                }

                uint32_t destOff = destVec * 16u;
                if (destOff + 16u > (qwordMask + 1u) * 16u)
                {
                    if (sourceAvailable && srcIndex < sourceVectorCount)
                        ++srcIndex;
                    continue;
                }

                uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                std::memcpy(lanes, vuData + destOff, sizeof(lanes));
                uint32_t decompressed[4] = {lanes[0], lanes[1], lanes[2], lanes[3]};
                bool decoded = false;

                const uint8_t *srcVec = nullptr;
                if (sourceAvailable && srcIndex < sourceVectorCount)
                {
                    srcVec = srcBase + srcIndex * bytesPerVector;
                    ++srcIndex;
                    decoded = true;
                }
                else if (!sourceAvailable && pos + (srcIndex + 1u) * bytesPerVector <= sizeBytes)
                {
                    // Filling write (WL > CL): the console writes the next source
                    // vector again without consuming it (PCSX2 Vif_Unpack.cpp).
                    srcVec = srcBase + srcIndex * bytesPerVector;
                    decoded = true;
                }

                auto extend16 = [&](uint16_t raw) -> uint32_t
                {
                    if (zeroExtend)
                        return static_cast<uint32_t>(raw);
                    return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                };

                auto extend8 = [&](uint8_t raw) -> uint32_t
                {
                    if (zeroExtend)
                        return static_cast<uint32_t>(raw);
                    return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
                };

                bool handledFormat = true;
                if (!decoded)
                {
                    handledFormat = false;
                }
                else if (vl == 0u)
                {
                    if (components == 1)
                    {
                        uint32_t scalar = 0;
                        std::memcpy(&scalar, srcVec, sizeof(scalar));
                        decompressed[0] = scalar;
                        decompressed[1] = scalar;
                        decompressed[2] = scalar;
                        decompressed[3] = scalar;
                    }
                    else
                    {
                        const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                        for (uint32_t c = 0; c < limit; ++c)
                        {
                            uint32_t scalar = 0;
                            std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                            decompressed[c] = scalar;
                        }
                    }
                }
                else if (vl == 1u)
                {
                    if (components == 1)
                    {
                        uint16_t raw = 0;
                        std::memcpy(&raw, srcVec, sizeof(raw));
                        const uint32_t scalar = extend16(raw);
                        decompressed[0] = scalar;
                        decompressed[1] = scalar;
                        decompressed[2] = scalar;
                        decompressed[3] = scalar;
                    }
                    else
                    {
                        const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                        for (uint32_t c = 0; c < limit; ++c)
                        {
                            uint16_t raw = 0;
                            std::memcpy(&raw, srcVec + c * 2u, sizeof(raw));
                            decompressed[c] = extend16(raw);
                        }
                    }
                }
                else if (vl == 2u)
                {
                    if (components == 1)
                    {
                        const uint32_t scalar = extend8(srcVec[0]);
                        decompressed[0] = scalar;
                        decompressed[1] = scalar;
                        decompressed[2] = scalar;
                        decompressed[3] = scalar;
                    }
                    else
                    {
                        const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                        for (uint32_t c = 0; c < limit; ++c)
                        {
                            decompressed[c] = extend8(srcVec[c]);
                        }
                    }
                }
                else if (vl == 3u && vn == 3u)
                {
                    // V4-5: packed color-like format in a single 16-bit value.
                    uint16_t packed = 0;
                    std::memcpy(&packed, srcVec, sizeof(packed));
                    decompressed[0] = (packed & 0x1Fu) << 3;
                    decompressed[1] = ((packed >> 5) & 0x1Fu) << 3;
                    decompressed[2] = ((packed >> 10) & 0x1Fu) << 3;
                    decompressed[3] = (packed >> 8) & 0x80u;
                }
                else
                {
                    handledFormat = false;
                }

                // As on the console (PCSX2 Vif_Unpack.cpp): V2 writes x y x y, and V3
                // is unpacked like V4, so w takes the source element after z.
                if (handledFormat && vl < 3u && components == 2)
                {
                    decompressed[2] = decompressed[0];
                    decompressed[3] = decompressed[1];
                }
                else if (handledFormat && vl < 3u && components == 3)
                {
                    const uint32_t elementBytes = 4u >> vl;
                    const size_t offset = static_cast<size_t>(srcVec - data) + 3u * elementBytes;
                    if (offset + elementBytes <= sizeBytes)
                    {
                        if (vl == 0u)
                            std::memcpy(&decompressed[3], data + offset, 4u);
                        else if (vl == 1u)
                        {
                            uint16_t raw = 0;
                            std::memcpy(&raw, data + offset, sizeof(raw));
                            decompressed[3] = extend16(raw);
                        }
                        else
                            decompressed[3] = extend8(data[offset]);
                    }
                }

                // Unknown compressed format fallback: preserve legacy raw-copy behavior.
                if (!handledFormat && decoded && !maskEnable && (regs.mode == 0u || regs.mode == 3u))
                {
                    uint32_t copyBytes = (bytesPerVector < 16u) ? bytesPerVector : 16u;
                    std::memcpy(vuData + destOff, srcVec, copyBytes);
                    continue;
                }

                const bool canAdd = (vl != 3u || vn != 3u);
                const uint32_t mode = regs.mode & 3u;
                const uint32_t colIdx = (cyclePos > 3u) ? 3u : cyclePos;
                const uint32_t maskCycle = (cyclePos > 3u) ? 3u : cyclePos;

                for (uint32_t field = 0u; field < 4u; ++field)
                {
                    uint32_t maskSpec = 0u;
                    if (maskEnable)
                    {
                        const uint32_t shift = ((maskCycle * 4u) + field) * 2u;
                        maskSpec = (regs.mask >> shift) & 0x3u;
                    }

                    // In fill-write cycles with suspended source reads, treat raw-data selections as row-fill.
                    if (!decoded && maskSpec == 0u)
                        maskSpec = 1u;

                    uint32_t writeVal = lanes[field];
                    if (maskSpec == 0u)
                    {
                        if (handledFormat)
                        {
                            writeVal = decompressed[field];
                            if (canAdd && (mode == 1u || mode == 2u))
                            {
                                writeVal = writeVal + regs.row[field];
                                if (mode == 2u)
                                    regs.row[field] = writeVal;
                            }
                            else if (canAdd && mode == 3u)
                            {
                                regs.row[field] = writeVal; // mode 3 also stores the data in ROW
                            }
                        }
                    }
                    else if (maskSpec == 1u)
                    {
                        writeVal = regs.row[field];
                    }
                    else if (maskSpec == 2u)
                    {
                        writeVal = regs.col[colIdx];
                    }
                    else
                    {
                        continue; // write-protect
                    }

                    lanes[field] = writeVal;
                }

                std::memcpy(vuData + destOff, lanes, sizeof(lanes));
            }
        }
        return totalBytes;
    }

    constexpr uint8_t kGifFmtImage = 2u;

    uint32_t pendingGifImageQwc(const uint8_t *data, uint32_t sizeBytes)
    {
        if (!data || sizeBytes < 16u)
            return 0u;

        uint32_t offset = 0u;
        while (offset + 16u <= sizeBytes)
        {
            uint64_t tagLo = 0u;
            std::memcpy(&tagLo, data + offset, sizeof(tagLo));
            offset += 16u;

            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t payloadBytes = 0u;
            if (flg == 0u) // PACKED
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 16ull;
            }
            else if (flg == 1u) // REGLIST, padded to a quadword
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 8ull;
                payloadBytes = (payloadBytes + 15ull) & ~15ull;
            }
            else // IMAGE, and FLG 3, which works like IMAGE
            {
                payloadBytes = static_cast<uint64_t>(nloop) * 16ull;
                const uint64_t availableBytes = sizeBytes - offset;
                if (payloadBytes > availableBytes)
                {
                    return static_cast<uint32_t>((payloadBytes - availableBytes) / 16ull);
                }
            }

            if (payloadBytes > static_cast<uint64_t>(sizeBytes - offset))
                return 0u;
            offset += static_cast<uint32_t>(payloadBytes);
        }

        return 0u;
    }
}

void PS2Memory::processVIF0Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF0Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF0Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;

    uint32_t pos = 0;
    while (pos + 4 <= sizeBytes)
    {
        uint32_t cmd = 0u;
        std::memcpy(&cmd, data + pos, sizeof(cmd));
        pos += 4u;

        const uint8_t opcode = static_cast<uint8_t>((cmd >> 24) & 0x7Fu);
        const uint16_t imm = static_cast<uint16_t>(cmd & 0xFFFFu);
        const uint8_t num = static_cast<uint8_t>((cmd >> 16) & 0xFFu);
        const bool irq = (cmd & 0x80000000u) != 0u;

        vif0_regs.code = cmd;
        vif0_regs.num = num;
        if (irq)
            vif0_regs.stat |= (1u << 11);

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif0_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            vif0_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif0_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif0_regs.mark = imm;
            vif0_regs.stat |= (1u << 6);
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4u > sizeBytes)
                break;
            std::memcpy(&vif0_regs.mask, data + pos, sizeof(vif0_regs.mask));
            pos += 4u;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.row, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.col, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            const uint32_t destAddr = static_cast<uint32_t>(imm & 0x1FFu) * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            uint32_t copyBytes = 0u;
            if (m_vu0Code && destAddr < PS2_VU0_CODE_SIZE && mpgBytes > 0u)
            {
                copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU0_CODE_SIZE)
                    copyBytes = PS2_VU0_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu0Code + destAddr, data + pos, copyBytes);
                    markVU0CodeModified();
                }
            }

            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if ((opcode & 0x60u) == 0x60u)
        {
            const uint32_t totalBytes = unpackToVu(vif0_regs, m_vu0Data, 0xFFu, false, data, sizeBytes, pos,
                                                   opcode, imm, num, false);
            pos += totalBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            break;
        }
    }
}

void PS2Memory::processVIF1Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF1Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF1Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;

    uint32_t pos = 0;

    while (pos + 4 <= sizeBytes)
    {
        if (m_vif1PendingPath2ImageQwc != 0u)
        {
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            if (availableQw == 0u)
            {
                break;
            }

            const uint32_t chunkQw = std::min<uint32_t>(m_vif1PendingPath2ImageQwc, availableQw);
            std::vector<uint8_t> imagePacket(16u + static_cast<size_t>(chunkQw) * 16u, 0u);
            const uint64_t imageTag =
                static_cast<uint64_t>(chunkQw & 0x7FFFu) |
                ((m_vif1PendingPath2ImageQwc == chunkQw) ? (1ull << 15) : 0ull) |
                (static_cast<uint64_t>(kGifFmtImage) << 58);
            std::memcpy(imagePacket.data(), &imageTag, sizeof(imageTag));
            std::memcpy(imagePacket.data() + 16u, data + pos, static_cast<size_t>(chunkQw) * 16u);
            submitGifPacket(GifPathId::Path2, imagePacket.data(), static_cast<uint32_t>(imagePacket.size()), true, m_vif1PendingPath2DirectHl);

            pos += chunkQw * 16u;
            m_vif1PendingPath2ImageQwc -= chunkQw;
            if (m_vif1PendingPath2ImageQwc == 0u)
            {
                m_vif1PendingPath2DirectHl = false;
            }
            continue;
        }

        uint32_t cmd;
        memcpy(&cmd, data + pos, 4);
        pos += 4;

        uint8_t opcode = (cmd >> 24) & 0x7F;
        uint16_t imm = cmd & 0xFFFF;
        uint8_t num = (cmd >> 16) & 0xFF;
        const bool irq = (cmd & 0x80000000u) != 0u;

        // In a PATH3 unmask window PATH3 advances about one qword per VIF1 command; a
        // command other than NOP or MSKPATH3 first lets out the packets due so far.
        if (m_path3Window && !m_path3Masked)
        {
            ++m_path3WindowBudget;
            if (opcode == VIF_FLUSHA)
                flushMaskedPath3Packets(); // waits for PATH3 to go idle
            else if (opcode != VIF_NOP && opcode != VIF_MSKPATH3)
                releasePath3Window();
        }

        // BDR_TRACE_VIF1=first,count: every VIF1 code of VSync ticks first..first+count,
        // with the TOPS/ITOPS/BASE/OFST state it runs with.
        static const std::pair<uint64_t, uint64_t> traceWindow = []
        {
            const char *value = std::getenv("BDR_TRACE_VIF1");
            uint64_t first = 0, count = 0;
            if (value && std::sscanf(value, "%llu,%llu", reinterpret_cast<unsigned long long *>(&first),
                                     reinterpret_cast<unsigned long long *>(&count)) == 2)
                return std::make_pair(first, first + count);
            return std::make_pair(uint64_t{1}, uint64_t{0});
        }();
        if (const uint64_t tick = gs_regs.vsyncTick.load(std::memory_order_relaxed);
            tick >= traceWindow.first && tick <= traceWindow.second)
            std::printf("[vif1] v=%llu pos=0x%x/0x%x cmd=%08x op=%02x num=%u imm=%04x tops=%03x itops=%03x base=%03x ofst=%03x dbf=%u cycle=%04x mode=%u\n",
                        static_cast<unsigned long long>(tick), pos - 4u, sizeBytes, cmd, opcode, num, imm,
                        vif1_regs.tops & 0x3FFu, vif1_regs.itops & 0x3FFu, vif1_regs.base & 0x3FFu, vif1_regs.ofst & 0x3FFu,
                        (vif1_regs.stat >> 7) & 1u, vif1_regs.cycle & 0xFFFFu, vif1_regs.mode & 3u);
        // BDR_TRACE_VIF1_DATA=1: also the first qword of short (1 to 5 qword) V4-32 UNPACKs, the microcode's parameters;
        // =2: the first 16 bytes of every UNPACK.
        static const int traceDataLevel = std::getenv("BDR_TRACE_VIF1_DATA") ? std::atoi(std::getenv("BDR_TRACE_VIF1_DATA")) : 0;
        static const bool traceData = traceDataLevel != 0;
        if (const uint64_t tick = gs_regs.vsyncTick.load(std::memory_order_relaxed);
            traceData && tick >= traceWindow.first && tick <= traceWindow.second && pos + 16u <= sizeBytes &&
            (traceDataLevel >= 2 ? (opcode & 0x60u) == 0x60u : ((opcode & 0x6Fu) == 0x6Cu && num >= 1u && num <= 5u)))
        {
            uint32_t words[4];
            std::memcpy(words, data + pos, sizeof(words));
            std::printf("[vif1-data] v=%llu op=%02x addr=%03x num=%u %08x %08x %08x %08x\n",
                        static_cast<unsigned long long>(tick), opcode, imm & 0x3FFu, num,
                        words[0], words[1], words[2], words[3]);
            // =3: up to 8 more qwords of V4-32 UNPACKs.
            for (uint32_t q = 1u; traceDataLevel >= 3 && (opcode & 0x6Fu) == 0x6Cu && q < 9u && q < num && pos + (q + 1u) * 16u <= sizeBytes; ++q)
            {
                std::memcpy(words, data + pos + q * 16u, sizeof(words));
                std::printf("[vif1-data]   +%u %08x %08x %08x %08x\n", q, words[0], words[1], words[2], words[3]);
            }
        }

        // Track most-recent command for VIFn_CODE emulation.
        vif1_regs.code = cmd;
        vif1_regs.num = num;
        if (irq)
            vif1_regs.stat |= (1u << 11); // INT

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif1_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_OFFSET)
        {
            // VIF double-buffer setup. OFFSET clears DBF and resets TOPS to BASE.
            // Do not rewrite BASE from the previous TOPS value.
            vif1_regs.ofst = imm & 0x3FFu;
            vif1_regs.tops = vif1_regs.base & 0x3FFu;
            vif1_regs.stat &= ~(1u << 7); // clear DBF
            continue;
        }
        else if (opcode == VIF_BASE)
        {
            // BASE only updates the base register. TOPS changes on OFFSET/MSCAL.
            vif1_regs.base = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            // ITOP VIFcode writes pending ITOPS; VU XITOP observes it after MSCAL/MSCNT.
            vif1_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif1_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MSKPATH3)
        {
            // VIF command docs: MSKPATH3 uses IMMEDIATE bit 15.
            const bool wasMasked = m_path3Masked;
            const bool masked = (imm & 0x8000u) != 0u;
            if (wasMasked && !masked)
            {
                // PATH3 resumes; it runs until masked again (see the window above).
                m_path3Masked = false;
                m_path3Window = true;
                m_path3WindowReleased = false;
                m_path3WindowBudget = 0;
            }
            else if (!wasMasked && masked)
            {
                // The mask takes effect at the end of the GIF packet in progress.
                if (m_path3Window)
                    releasePath3Window();
                m_path3Window = false;
                m_path3Masked = true;
            }
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif1_regs.mark = imm;
            vif1_regs.stat |= (1u << 6); // MRK
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_MSCAL || opcode == VIF_MSCALF)
        {
            uint32_t startPC = (uint32_t)imm * 8u;

            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if (m_vu1MscalCallback)
                m_vu1MscalCallback(startPC, runTop, runItop);
            continue;
        }
        else if (opcode == VIF_MSCNT)
        {
            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if (m_vu1MscntCallback)
                m_vu1MscntCallback(runTop, runItop);
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4 > sizeBytes)
                break;
            uint32_t maskValue = 0;
            std::memcpy(&maskValue, data + pos, sizeof(maskValue));
            vif1_regs.mask = maskValue;
            pos += 4;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.row, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.col, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            uint32_t destAddr = (uint32_t)imm * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            if (m_vu1Code && destAddr < PS2_VU1_CODE_SIZE && mpgBytes > 0)
            {
                uint32_t copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU1_CODE_SIZE)
                    copyBytes = PS2_VU1_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu1Code + destAddr, data + pos, copyBytes);
                    markVU1CodeModified();
                }
            }
            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if (opcode == VIF_DIRECT || opcode == VIF_DIRECTHL)
        {
            uint32_t qwCount = imm;
            if (qwCount == 0)
                qwCount = 65536;
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            const bool truncated = qwCount > availableQw;
            if (qwCount > availableQw)
                qwCount = availableQw;

            if (qwCount > 0)
            {
                const bool directHl = (opcode == VIF_DIRECTHL);
                submitGifPacket(GifPathId::Path2, data + pos, qwCount * 16, true, directHl);

                const uint32_t pendingImageQw = m_gifPathsKeepTagState ? 0u : pendingGifImageQwc(data + pos, qwCount * 16u);
                if (pendingImageQw != 0u)
                {
                    m_vif1PendingPath2ImageQwc = pendingImageQw;
                    m_vif1PendingPath2DirectHl = directHl;
                }
            }

            pos += qwCount * 16;
            if (truncated)
            {
                pos = sizeBytes;
                break;
            }
            continue;
        }
        else if ((opcode & 0x60) == 0x60)
        {
            const uint32_t totalBytes = unpackToVu(vif1_regs, m_vu1Data, 0x3FFu, true, data, sizeBytes, pos,
                                                   opcode, imm, num, traceWindow.first <= traceWindow.second);
            pos += totalBytes;

            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            continue;
        }
    }

    // A VIF1 stream that ends with PATH3 unmasked leaves PATH3 running to the end of its queue.
    if (m_path3Window && !m_path3Masked)
    {
        m_path3Window = false;
        flushMaskedPath3Packets();
    }
}
