#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ps2x
{
    // Byte buffers reused for DMA and GIF copies (ps2_memory.cpp): above 512 KiB the
    // heap maps fresh pages for every new one, which the copy then faults in, and
    // unmaps them on free. Returns the largest free buffer, emptied, for a copy of
    // `bytes` (unknown: SIZE_MAX); a plain empty vector below 256 KiB.
    std::vector<uint8_t> takeTransferBuffer(size_t bytes = SIZE_MAX);
    // Keeps a buffer of 256 KiB or more for later; smaller ones are just freed.
    void giveTransferBuffer(std::vector<uint8_t> &&buffer);

    inline std::vector<uint8_t> copyToTransferBuffer(const uint8_t *begin, const uint8_t *end)
    {
        std::vector<uint8_t> buffer = takeTransferBuffer(static_cast<size_t>(end - begin));
        buffer.assign(begin, end);
        return buffer;
    }
}
