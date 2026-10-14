#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>

namespace GSMem
{
    // Bit i of a page's mask is set while cache i holds a copy of that page
    // taken after the page's last write; every VRAM write clears the mask.
    inline std::array<std::atomic<uint64_t>, 512> g_pageCopyHolders{};

    inline void NotePageWrite(uint32_t byteAddress) noexcept
    {
        std::atomic<uint64_t> &holders = g_pageCopyHolders[(byteAddress >> 13u) & 511u];
        if (holders.load(std::memory_order_relaxed) != 0u)
            holders.store(0u, std::memory_order_release);
    }

    class TexturePageCache
    {
    public:
        static constexpr uint32_t kPageSize = 8192u;

        TexturePageCache() : m_slots(std::make_unique<Slot[]>(kSlots)), m_bit(AcquireBit()) {}
        ~TexturePageCache() { ReleaseBit(m_bit); }
        TexturePageCache(const TexturePageCache &) = delete;
        TexturePageCache &operator=(const TexturePageCache &) = delete;

        void Invalidate() noexcept
        {
            m_currentBase = UINT32_MAX;
            m_currentBytes = nullptr;
            for (uint32_t i = 0; i < kSlots; ++i)
                m_slots[i].pageBase = UINT32_MAX;
        }

        // byteAddress is the wrapped, swizzled VRAM address. The returned
        // pointer is valid only until the next miss or invalidation. As in the
        // GS page buffer, the current page keeps the bytes it had when it was
        // loaded until another page is read or TEXFLUSH; a page that is loaded
        // again reuses an earlier copy only if no write touched it since.
        const uint8_t* Resolve(const uint8_t* vram, uint32_t byteAddress) noexcept
        {
            const uint32_t pageBase = byteAddress & ~(kPageSize - 1u);
            if (m_currentBase != pageBase || m_currentVram != vram)
            {
                Slot &slot = m_slots[(pageBase / kPageSize) & (kSlots - 1u)];
                std::atomic<uint64_t> &holders = g_pageCopyHolders[(pageBase / kPageSize) & 511u];
                if (slot.pageBase != pageBase || slot.vram != vram ||
                    (holders.load(std::memory_order_acquire) & m_bit) == 0u)
                {
                    // Marked before copying, so a write during the copy clears the mark.
                    holders.fetch_or(m_bit, std::memory_order_acq_rel);
                    std::memcpy(slot.bytes.data(), vram + pageBase, kPageSize);
                    slot.pageBase = pageBase;
                    slot.vram = vram;
                }
                m_currentBase = pageBase;
                m_currentVram = vram;
                m_currentBytes = slot.bytes.data();
            }
            return m_currentBytes + (byteAddress & (kPageSize - 1u));
        }

    private:
        static constexpr uint32_t kSlots = 64u;

        struct Slot
        {
            alignas(64) std::array<uint8_t, kPageSize> bytes{};
            uint32_t pageBase = UINT32_MAX;
            const uint8_t *vram = nullptr;
        };

        // Up to 64 live caches get a bit; others (bit 0) copy on every page change.
        static uint64_t AcquireBit() noexcept
        {
            std::lock_guard<std::mutex> lock(BitMutex());
            for (uint32_t i = 0; i < 64u; ++i)
                if ((UsedBits() & (1ull << i)) == 0u)
                {
                    UsedBits() |= 1ull << i;
                    for (auto &holders : g_pageCopyHolders)
                        holders.fetch_and(~(1ull << i), std::memory_order_relaxed);
                    return 1ull << i;
                }
            return 0u;
        }

        static void ReleaseBit(uint64_t bit) noexcept
        {
            std::lock_guard<std::mutex> lock(BitMutex());
            UsedBits() &= ~bit;
        }

        static std::mutex &BitMutex() noexcept
        {
            static std::mutex mutex;
            return mutex;
        }

        static uint64_t &UsedBits() noexcept
        {
            static uint64_t bits = 0u;
            return bits;
        }

        std::unique_ptr<Slot[]> m_slots;
        uint64_t m_bit;
        uint32_t m_currentBase = UINT32_MAX;
        const uint8_t *m_currentVram = nullptr;
        const uint8_t *m_currentBytes = nullptr;
    };
}
