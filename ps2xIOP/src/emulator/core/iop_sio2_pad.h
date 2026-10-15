#pragma once

#include "ps2x/iop/iop_host.h"

#include <array>
#include <cstdint>

namespace ps2x
{
    class StateArchive;
}

namespace ps2x::iop::detail
{
    // DualShock 2 on a SIO2 port. The SIO2 answers the address byte (0x01) of
    // a packet itself; every following byte goes through exchange(). Command
    // replies follow the controller protocol (0x40-0x4F): mode byte, 0x5A,
    // then button, analog and pressure data or configuration constants.
    class DualShock2
    {
    public:
        static constexpr uint8_t DigitalMode = 0x41u;
        static constexpr uint8_t AnalogMode = 0x73u;
        static constexpr uint8_t PressureMode = 0x79u;
        static constexpr uint8_t ConfigMode = 0xF3u;

        void reset() noexcept;
        void serialize(ps2x::StateArchive &archive);
        void beginPacket(const ControllerPadState &input) noexcept;
        [[nodiscard]] uint8_t exchange(uint8_t command) noexcept;

        [[nodiscard]] uint8_t mode() const noexcept { return m_mode; }
        [[nodiscard]] bool inConfig() const noexcept { return m_config; }
        [[nodiscard]] std::array<uint8_t, 2> motors() const noexcept { return m_motors; }

    private:
        [[nodiscard]] uint8_t poll(uint32_t position, uint8_t command, bool driveMotors) noexcept;
        [[nodiscard]] uint8_t pressure(uint32_t buttonBit) const noexcept;

        ControllerPadState m_input{};
        uint8_t m_mode = DigitalMode;
        bool m_config = false;
        bool m_configAtStart = false;
        bool m_analogLocked = false;
        bool m_stage = false;
        uint8_t m_command = 0u;
        uint32_t m_position = 0u;
        uint32_t m_responseMask = 0u;
        std::array<uint8_t, 2> m_motorMap{0xFFu, 0xFFu};
        std::array<uint8_t, 2> m_motors{};
    };
}
