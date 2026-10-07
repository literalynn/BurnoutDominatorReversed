#include "iop_memory.h"

#include <algorithm>
#include <cstring>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kDmaSpu0Chcr = 0x1F8010C8u;
        constexpr uint32_t kDmaSpu1Chcr = 0x1F801508u;
        constexpr uint32_t kDmaStart = 1u << 24u;
        constexpr int kDmaSpu0Irq = 0x24;
        constexpr int kDmaSpu1Irq = 0x28;

        uint32_t alignUp(uint32_t value, uint32_t alignment)
        {
            return (value + alignment - 1u) & ~(alignment - 1u);
        }
    }

    IopMemory::IopMemory()
        : m_ram(RamSize), m_owned(RamSize), m_scratch(ScratchSize)
    {
        reset();
    }

    void IopMemory::reset()
    {
        std::fill(m_ram.begin(), m_ram.end(), uint8_t{0});
        std::fill(m_owned.begin(), m_owned.end(), uint8_t{0});
        std::fill(m_scratch.begin(), m_scratch.end(), uint8_t{0});
        m_hardware.clear();
        m_allocations.clear();
        m_heapCursor = HeapBase;
        m_interruptStatus = 0;
        m_interruptMask = 0;
        m_interruptControl = 1;
        m_dmaStart.reset();
        m_spu.reset();
    }

    uint32_t IopMemory::physicalAddress(uint32_t address) noexcept
    {
        return address & 0x1FFFFFFFu;
    }

    uint8_t IopMemory::read8(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys < RamSize)
            return m_ram[phys];
        if (phys >= ScratchBase && phys < ScratchBase + ScratchSize)
            return m_scratch[phys - ScratchBase];
        if (Spu2::owns(phys))
        {
            const uint16_t halfword = m_spu.read16(phys & ~1u);
            return static_cast<uint8_t>((phys & 1u) != 0u ? halfword >> 8u : halfword);
        }
        const uint32_t value = readHardware32(phys & ~3u);
        return static_cast<uint8_t>(value >> ((phys & 3u) * 8u));
    }

    uint16_t IopMemory::read16(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys + 1u < RamSize)
        {
            uint16_t value;
            std::memcpy(&value, m_ram.data() + phys, sizeof(value));
            return value;
        }
        if (Spu2::owns(phys) && (phys & 1u) == 0u)
            return m_spu.read16(phys);
        return static_cast<uint16_t>(read8(address) | (static_cast<uint16_t>(read8(address + 1u)) << 8u));
    }

    uint32_t IopMemory::read32(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if ((phys & 3u) == 0u && phys + 3u < RamSize)
        {
            uint32_t value;
            std::memcpy(&value, m_ram.data() + phys, sizeof(value));
            return value;
        }
        if ((phys & 3u) == 0u && phys >= ScratchBase && phys + 3u < ScratchBase + ScratchSize)
        {
            uint32_t value;
            std::memcpy(&value, m_scratch.data() + (phys - ScratchBase), sizeof(value));
            return value;
        }
        if ((phys & 3u) == 0u && Spu2::owns(phys))
            return static_cast<uint32_t>(m_spu.read16(phys)) | (static_cast<uint32_t>(m_spu.read16(phys + 2u)) << 16u);
        if ((phys & 3u) == 0u && isHardwareAddress(phys))
            return readHardware32(phys);

        return static_cast<uint32_t>(read8(address)) |
               (static_cast<uint32_t>(read8(address + 1u)) << 8u) |
               (static_cast<uint32_t>(read8(address + 2u)) << 16u) |
               (static_cast<uint32_t>(read8(address + 3u)) << 24u);
    }

    void IopMemory::write8(uint32_t address, uint8_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys < RamSize)
        {
            m_ram[phys] = value;
            markOwned(phys, sizeof(value));
            return;
        }
        if (phys >= ScratchBase && phys < ScratchBase + ScratchSize)
        {
            m_scratch[phys - ScratchBase] = value;
            return;
        }
        if (Spu2::owns(phys))
        {
            // Byte access to a 16-bit register: merge into one halfword write.
            const uint32_t even = phys & ~1u;
            uint16_t halfword = m_spu.read16(even);
            const uint32_t shift = (phys & 1u) * 8u;
            halfword = static_cast<uint16_t>((halfword & ~(0xFFu << shift)) | (static_cast<uint32_t>(value) << shift));
            write16(even, halfword);
            return;
        }
        const uint32_t aligned = phys & ~3u;
        uint32_t current = readHardware32(aligned);
        const uint32_t shift = (phys & 3u) * 8u;
        current = (current & ~(0xFFu << shift)) | (static_cast<uint32_t>(value) << shift);
        writeHardware32(aligned, current);
    }

    void IopMemory::write16(uint32_t address, uint16_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys + 1u < RamSize)
        {
            std::memcpy(m_ram.data() + phys, &value, sizeof(value));
            markOwned(phys, sizeof(value));
            return;
        }
        if (Spu2::owns(phys) && (phys & 1u) == 0u)
        {
            // SPU2 registers are 16-bit and many have side effects: one access, one event.
            if (m_hardwareWriteHook)
                m_hardwareWriteHook(phys, value);
            m_spu.write16(phys, value);
            return;
        }
        write8(address, static_cast<uint8_t>(value));
        write8(address + 1u, static_cast<uint8_t>(value >> 8u));
    }

    void IopMemory::write32(uint32_t address, uint32_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if ((phys & 3u) == 0u && phys + 3u < RamSize)
        {
            std::memcpy(m_ram.data() + phys, &value, sizeof(value));
            markOwned(phys, sizeof(value));
            return;
        }
        if ((phys & 3u) == 0u && phys >= ScratchBase && phys + 3u < ScratchBase + ScratchSize)
        {
            std::memcpy(m_scratch.data() + (phys - ScratchBase), &value, sizeof(value));
            return;
        }
        if ((phys & 3u) == 0u && Spu2::owns(phys))
        {
            write16(phys, static_cast<uint16_t>(value));
            write16(phys + 2u, static_cast<uint16_t>(value >> 16u));
            return;
        }
        if ((phys & 3u) == 0u)
        {
            writeHardware32(phys, value);
            return;
        }
        write8(address, static_cast<uint8_t>(value));
        write8(address + 1u, static_cast<uint8_t>(value >> 8u));
        write8(address + 2u, static_cast<uint8_t>(value >> 16u));
        write8(address + 3u, static_cast<uint8_t>(value >> 24u));
    }

    bool IopMemory::readRam(uint32_t address, void *destination, size_t size) const
    {
        const uint32_t phys = physicalAddress(address);
        if ((!destination && size != 0u) || phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
            std::memcpy(destination, m_ram.data() + phys, size);
        return true;
    }

    bool IopMemory::writeRam(uint32_t address, const void *source, size_t size)
    {
        const uint32_t phys = physicalAddress(address);
        if ((!source && size != 0u) || phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
        {
            std::memcpy(m_ram.data() + phys, source, size);
            markOwned(phys, size);
        }
        return true;
    }

    bool IopMemory::zeroRam(uint32_t address, size_t size)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
        {
            std::memset(m_ram.data() + phys, 0, size);
            markOwned(phys, size);
        }
        return true;
    }

    bool IopMemory::ownsRamRange(uint32_t address, size_t size) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys > RamSize || size > RamSize - phys)
            return false;
        // memchr is vectorized: CD reads check up to megabytes at a time.
        return size == 0u || std::memchr(m_owned.data() + phys, 0, size) == nullptr;
    }

    uint32_t IopMemory::firstUnownedRam(uint32_t address, size_t size) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys > RamSize || size > RamSize - phys)
            return phys;
        const void *hole = size != 0u ? std::memchr(m_owned.data() + phys, 0, size) : nullptr;
        return hole ? static_cast<uint32_t>(static_cast<const uint8_t *>(hole) - m_owned.data())
                    : static_cast<uint32_t>(phys + size);
    }

    void IopMemory::markOwned(uint32_t address, size_t size)
    {
        if (address > RamSize || size > RamSize - address)
            return;
        std::fill(m_owned.begin() + address, m_owned.begin() + address + size, uint8_t{1});
    }

    bool IopMemory::isHardwareAddress(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        return (phys >= HardwareBase && phys < HardwareEnd) ||
               (phys >= Spu2Base && phys < Spu2End) ||
               (phys >= SifBase && phys < SifEnd);
    }

    uint32_t IopMemory::readHardware32(uint32_t address) const
    {
        const auto value = m_hardware.find(address);
        if (value != m_hardware.end())
            return value->second;
        switch (address)
        {
        case 0x1F801070u:
            return m_interruptStatus;
        case 0x1F801074u:
            return m_interruptMask;
        case 0x1F801078u:
            return m_interruptControl;
        default:
            return 0u;
        }
    }

    void IopMemory::writeHardware32(uint32_t address, uint32_t value)
    {
        if (m_hardwareWriteHook)
            m_hardwareWriteHook(address, value);
        switch (address)
        {
        case 0x1F801070u:
            m_interruptStatus &= value;
            return;
        case 0x1F801074u:
            m_interruptMask = value;
            return;
        case 0x1F801078u:
            m_interruptControl = value & 1u;
            return;
        default:
            break;
        }

        m_hardware[address] = value;
        if ((address != kDmaSpu0Chcr && address != kDmaSpu1Chcr) || (value & kDmaStart) == 0u)
            return;

        const bool secondCore = address == kDmaSpu1Chcr;
        m_hardware[address] = value & ~kDmaStart;

        // MADR, BCR and CHCR sit 8, 4 and 0 bytes before/at the CHCR address.
        const uint32_t memoryAddress = address - 2u * sizeof(uint32_t);
        const uint32_t blockControlAddress = address - sizeof(uint32_t);
        uint32_t madr = 0u;
        if (const auto current = m_hardware.find(memoryAddress); current != m_hardware.end())
            madr = current->second;
        uint32_t blockControl = 0u;
        if (const auto current = m_hardware.find(blockControlAddress); current != m_hardware.end())
            blockControl = current->second;
        const uint32_t wordsPerBlock = std::max<uint32_t>(blockControl & 0xFFFFu, 1u);
        const uint32_t blockCount = std::max<uint32_t>(blockControl >> 16u, 1u);
        const uint64_t transferWords = static_cast<uint64_t>(wordsPerBlock) * blockCount;

        // Move the data now; completion (status, interrupt) is signalled after a delay.
        const int core = secondCore ? 1 : 0;
        const uint64_t bytes = transferWords * sizeof(uint32_t);
        const uint32_t physicalMadr = physicalAddress(madr);
        if (physicalMadr < RamSize && bytes <= RamSize - physicalMadr)
        {
            if ((value & 1u) != 0u)
            {
                m_spu.dmaWrite(core, m_ram.data() + physicalMadr, static_cast<uint32_t>(bytes));
            }
            else
            {
                m_spu.dmaRead(core, m_ram.data() + physicalMadr, static_cast<uint32_t>(bytes));
                markOwned(physicalMadr, static_cast<size_t>(bytes));
            }
            m_hardware[memoryAddress] = madr + static_cast<uint32_t>(bytes);
        }
        m_spu.dmaStarted(core);
        m_dmaStart = DmaStart{
            secondCore ? kDmaSpu1Irq : kDmaSpu0Irq,
            std::max<uint64_t>(transferWords * 2u, 64u),
        };
    }

    std::optional<IopMemory::DmaStart> IopMemory::takeDmaStart() noexcept
    {
        std::optional<DmaStart> result = m_dmaStart;
        m_dmaStart.reset();
        return result;
    }

    uint32_t IopMemory::allocate(uint32_t size, uint32_t alignment, std::optional<uint32_t> fixed)
    {
        size = alignUp(std::max(size, 1u), 16u);
        alignment = std::max<uint32_t>(alignment, 4u);
        if (fixed)
        {
            const uint32_t address = *fixed;
            if (address < HeapBase || address + size > HeapLimit)
                return 0u;
            for (const auto &block : m_allocations)
                if (address < block.address + block.size && block.address < address + size)
                    return 0u;
            m_allocations.push_back({address, size});
            markOwned(address, size);
            return address;
        }

        uint32_t candidate = alignUp(m_heapCursor, alignment);
        for (;;)
        {
            bool overlap = false;
            for (const auto &block : m_allocations)
            {
                if (candidate < block.address + block.size && block.address < candidate + size)
                {
                    candidate = alignUp(block.address + block.size, alignment);
                    overlap = true;
                    break;
                }
            }
            if (!overlap)
                break;
        }
        if (candidate > HeapLimit || size > HeapLimit - candidate)
            return 0u;
        m_allocations.push_back({candidate, size});
        markOwned(candidate, size);
        m_heapCursor = std::max(m_heapCursor, candidate + size);
        return candidate;
    }

    bool IopMemory::freeAllocation(uint32_t address)
    {
        const auto block = std::find_if(m_allocations.begin(), m_allocations.end(),
                                        [&](const Allocation &candidate)
                                        { return candidate.address == address; });
        if (block == m_allocations.end())
            return false;
        std::fill(m_owned.begin() + block->address,
                  m_owned.begin() + block->address + block->size,
                  uint8_t{0});
        m_allocations.erase(block);
        return true;
    }

    uint32_t IopMemory::maxFreeMemory() const
    {
        return m_heapCursor < HeapLimit ? HeapLimit - m_heapCursor : 0u;
    }

    std::optional<IopMemory::Allocation> IopMemory::allocationContaining(uint32_t address) const
    {
        const auto block = std::find_if(m_allocations.begin(), m_allocations.end(),
                                        [&](const Allocation &candidate)
                                        {
                                            return address >= candidate.address &&
                                                   address < candidate.address + candidate.size;
                                        });
        if (block == m_allocations.end())
            return std::nullopt;
        return *block;
    }

    std::string IopMemory::readString(uint32_t address, size_t limit) const
    {
        std::string result;
        result.reserve(std::min<size_t>(limit, 64u));
        for (size_t i = 0; i < limit; ++i)
        {
            const char ch = static_cast<char>(read8(address + static_cast<uint32_t>(i)));
            if (ch == '\0')
                break;
            result.push_back(ch);
        }
        return result;
    }
}
