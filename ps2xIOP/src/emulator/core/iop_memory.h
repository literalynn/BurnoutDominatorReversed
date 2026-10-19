#pragma once

#include "iop_sio2_pad.h"
#include "iop_spu2.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <deque>
#include <utility>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps2x
{
    class StateArchive;
}

namespace ps2x::iop::detail
{
    class IopMemory
    {
    public:
        static constexpr uint32_t RamSize = 2u * 1024u * 1024u;
        static constexpr uint32_t ScratchBase = 0x1F800000u;
        static constexpr uint32_t ScratchSize = 0x400u;
        static constexpr uint32_t HardwareBase = 0x1F801000u;
        static constexpr uint32_t HardwareEnd = 0x1F900000u;
        static constexpr uint32_t Spu2Base = 0x1F900000u;
        static constexpr uint32_t Spu2End = 0x1FA00000u;
        static constexpr uint32_t SifBase = 0x1D000000u;
        static constexpr uint32_t SifEnd = 0x1D001000u;
        static constexpr uint32_t HeapBase = 0x00120000u;
        static constexpr uint32_t HeapLimit = 0x001F0000u;

        struct Allocation
        {
            uint32_t address = 0;
            uint32_t size = 0;
        };

        struct DmaStart
        {
            int irq = 0;
            uint64_t delayCycles = 0;
        };

        IopMemory();

        void reset();
        void serialize(ps2x::StateArchive &archive);

        // Diagnostics: called with (address, merged 32-bit value) for every write to the
        // hardware register ranges. Survives reset().
        void setHardwareWriteHook(std::function<void(uint32_t, uint32_t)> hook) { m_hardwareWriteHook = std::move(hook); }

        // RAM inline (every instruction fetch comes here), the rest out of line.
        [[nodiscard]] uint8_t read8(uint32_t address) const
        {
            const uint32_t phys = physicalAddress(address);
            return phys < RamSize ? m_ram[phys] : readSlow8(address);
        }
        [[nodiscard]] uint16_t read16(uint32_t address) const
        {
            const uint32_t phys = physicalAddress(address);
            if (phys + 1u < RamSize)
            {
                uint16_t value;
                std::memcpy(&value, m_ram.data() + phys, sizeof(value));
                return value;
            }
            return readSlow16(address);
        }
        [[nodiscard]] uint32_t read32(uint32_t address) const
        {
            const uint32_t phys = physicalAddress(address);
            if ((phys & 3u) == 0u && phys + 3u < RamSize)
            {
                uint32_t value;
                std::memcpy(&value, m_ram.data() + phys, sizeof(value));
                return value;
            }
            return readSlow32(address);
        }
        void write8(uint32_t address, uint8_t value);
        void write16(uint32_t address, uint16_t value);
        void write32(uint32_t address, uint32_t value);

        [[nodiscard]] bool readRam(uint32_t address, void *destination, size_t size) const;
        [[nodiscard]] bool writeRam(uint32_t address, const void *source, size_t size);
        [[nodiscard]] bool zeroRam(uint32_t address, size_t size);
        [[nodiscard]] bool ownsRamRange(uint32_t address, size_t size) const;
        // First byte of the range that no allocation or write has claimed, or `address + size` when all are owned.
        [[nodiscard]] uint32_t firstUnownedRam(uint32_t address, size_t size) const;
        [[nodiscard]] bool isHardwareAddress(uint32_t address) const;
        [[nodiscard]] std::string readString(uint32_t address, size_t limit = 1024u) const;

        [[nodiscard]] uint32_t allocate(uint32_t size, uint32_t alignment = 16u, std::optional<uint32_t> fixed = std::nullopt);
        [[nodiscard]] bool freeAllocation(uint32_t address);
        [[nodiscard]] uint32_t maxFreeMemory() const;
        [[nodiscard]] std::optional<Allocation> allocationContaining(uint32_t address) const;

        [[nodiscard]] uint32_t interruptStatus() const noexcept { return m_interruptStatus; }
        [[nodiscard]] uint32_t interruptMask() const noexcept { return m_interruptMask; }
        [[nodiscard]] uint32_t interruptControl() const noexcept { return m_interruptControl; }
        void setInterruptStatus(uint32_t value) noexcept { m_interruptStatus = value; }
        void setInterruptMask(uint32_t value) noexcept { m_interruptMask = value; }
        void setInterruptControl(uint32_t value) noexcept { m_interruptControl = value & 1u; }

        [[nodiscard]] bool dmaStartPending() const noexcept { return m_dmaStart.has_value(); }
        [[nodiscard]] std::optional<DmaStart> takeDmaStart() noexcept;
        [[nodiscard]] Spu2 &spu() const noexcept { return m_spu; }
        void setSio2IrqCallback(std::function<void()> callback) { m_sio2Irq = std::move(callback); }
        // Reads the controller on a SIO2 port; false leaves the port empty.
        // Told when the motors of the pad in a port change speed (small on/off, large 0-255).
        void setMotorSink(std::function<void(uint32_t, bool, uint8_t)> sink)
        {
            m_motorSink = std::move(sink);
        }
        void setControllerSource(std::function<bool(uint32_t, ControllerPadState &)> source)
        {
            m_controllerSource = std::move(source);
        }
        // Called after each SIO2 command with its port, sent and received bytes.
        void setSio2TraceHook(std::function<void(uint32_t, const std::vector<uint8_t> &, const std::vector<uint8_t> &)> hook)
        {
            m_sio2Trace = std::move(hook);
        }
        [[nodiscard]] const DualShock2 &controller(uint32_t port) const noexcept { return m_pads[port & 1u]; }
        [[nodiscard]] std::span<const uint8_t> ram() const noexcept { return m_ram; }

        [[nodiscard]] static constexpr uint32_t physicalAddress(uint32_t address) noexcept { return address & 0x1FFFFFFFu; }

    private:
        [[nodiscard]] uint32_t readHardware32(uint32_t address) const;
        void writeHardware32(uint32_t address, uint32_t value);
        void markOwned(uint32_t address, size_t size);

        [[nodiscard]] uint8_t readSlow8(uint32_t address) const;
        [[nodiscard]] uint16_t readSlow16(uint32_t address) const;
        [[nodiscard]] uint32_t readSlow32(uint32_t address) const;

        std::vector<uint8_t> m_ram;
        std::vector<uint8_t> m_owned;
        std::vector<uint8_t> m_scratch;
        std::unordered_map<uint32_t, uint32_t> m_hardware;
        std::vector<Allocation> m_allocations;
        uint32_t m_heapCursor = HeapBase;
        uint32_t m_interruptStatus = 0;
        uint32_t m_interruptMask = 0;
        uint32_t m_interruptControl = 1;
        std::optional<DmaStart> m_dmaStart;
        std::function<void(uint32_t, uint32_t)> m_hardwareWriteHook;
        // Reads of the SPU2 data port advance its transfer address, hence mutable.
        mutable Spu2 m_spu;
        // SIO2 PIO transport. A DualShock 2 answers pad packets (first byte 0x01)
        // on ports the controller source reports; any other device (memory
        // card, multitap) is absent: its bytes read 0xFF and RECV1 flags the port.
        mutable std::deque<uint8_t> m_sio2Output;
        uint32_t m_sio2CommandIndex = 0u;
        uint32_t m_sio2InputCount = 0u;
        uint32_t m_sio2Ports = 0u;
        uint32_t m_sio2Missing = 0u;
        bool m_sio2PadPacket = false;
        std::array<DualShock2, 2> m_pads{};
        std::function<bool(uint32_t, ControllerPadState &)> m_controllerSource;
        std::function<void(uint32_t, bool, uint8_t)> m_motorSink;
        std::array<DualShock2::MotorSpeeds, 2> m_reportedMotors{};
        std::function<void(uint32_t, const std::vector<uint8_t> &, const std::vector<uint8_t> &)> m_sio2Trace;
        std::vector<uint8_t> m_sio2Sent;
        std::vector<uint8_t> m_sio2Received;
        std::function<void()> m_sio2Irq;
    };
}
