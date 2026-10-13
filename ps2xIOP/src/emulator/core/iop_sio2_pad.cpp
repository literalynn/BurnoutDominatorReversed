#include "iop_sio2_pad.h"

namespace ps2x::iop::detail
{
    namespace
    {
        // Pressure bytes 9-20 of a 0x79 poll, as bits of the digital button word.
        constexpr std::array<uint8_t, 12> kPressureButtons{
            5u,  // right
            7u,  // left
            4u,  // up
            6u,  // down
            12u, // triangle
            13u, // circle
            14u, // cross
            15u, // square
            10u, // L1
            11u, // R1
            8u,  // L2
            9u,  // R2
        };
        constexpr uint32_t kAnalogResponse = 0x0000003Fu;
        constexpr uint32_t kPressureResponse = 0x0003FFFFu;
    }

    void DualShock2::reset() noexcept
    {
        *this = DualShock2{};
    }

    void DualShock2::beginPacket(const ControllerPadState &input) noexcept
    {
        m_input = input;
        m_command = 0u;
        m_position = 1u;
        m_configAtStart = m_config;
    }

    uint8_t DualShock2::pressure(uint32_t buttonBit) const noexcept
    {
        return (m_input.buttons & (1u << buttonBit)) == 0u ? 0xFFu : 0x00u;
    }

    uint8_t DualShock2::poll(uint32_t position, uint8_t command, bool driveMotors) noexcept
    {
        switch (position)
        {
        case 3u:
        case 4u:
            if (driveMotors)
                m_motors[position - 3u] = command;
            return static_cast<uint8_t>(m_input.buttons >> ((position - 3u) * 8u));
        case 5u:
            return m_input.rightX;
        case 6u:
            return m_input.rightY;
        case 7u:
            return m_input.leftX;
        case 8u:
            return m_input.leftY;
        default:
            if (position >= 9u && position < 9u + kPressureButtons.size())
                return pressure(kPressureButtons[position - 9u]);
            return 0x00u;
        }
    }

    uint8_t DualShock2::exchange(uint8_t command) noexcept
    {
        const uint32_t position = m_position++;
        if (position == 1u)
        {
            m_command = command;
            return m_config ? ConfigMode : m_mode;
        }
        if (position == 2u)
            return 0x5Au;

        uint8_t reply = 0x00u;
        switch (m_command)
        {
        case 0x40u:
            reply = position == 5u ? 0x02u : position == 8u ? 0x5Au : 0x00u;
            break;
        case 0x41u: // which bytes a poll returns
            if (m_mode != DigitalMode)
                reply = position <= 4u ? 0xFFu : position == 5u ? 0x03u : position == 8u ? 0x5Au : 0x00u;
            break;
        case 0x42u:
            reply = poll(position, command, true);
            break;
        case 0x43u: // outside configuration it also reports the buttons
            if (!m_configAtStart)
                reply = poll(position, command, false);
            if (position == 3u)
                m_config = command != 0u;
            break;
        case 0x44u: // digital/analog switch and lock
            if (position == 3u)
                m_mode = command != 0u ? AnalogMode : DigitalMode;
            else if (position == 4u)
                m_analogLocked = command == 0x03u;
            break;
        case 0x45u: // model, LED state
            switch (position)
            {
            case 3u:
                reply = 0x03u; // DualShock 2
                break;
            case 4u:
                reply = 0x02u;
                break;
            case 5u:
                reply = m_mode != DigitalMode ? 0x01u : 0x00u;
                break;
            case 6u:
                reply = 0x02u;
                break;
            case 7u:
                reply = 0x01u;
                break;
            default:
                break;
            }
            break;
        case 0x46u:
            if (position == 3u)
                m_stage = command != 0u;
            else if (position == 5u)
                reply = 0x01u;
            else if (position == 6u)
                reply = m_stage ? 0x01u : 0x02u;
            else if (position == 7u)
                reply = m_stage ? 0x01u : 0x00u;
            else if (position == 8u)
                reply = m_stage ? 0x14u : 0x0Au;
            break;
        case 0x47u:
            reply = position == 5u ? 0x02u : position == 7u ? 0x01u : 0x00u;
            break;
        case 0x4Cu:
            if (position == 3u)
                m_stage = command != 0u;
            else if (position == 6u)
                reply = m_stage ? 0x07u : 0x04u;
            break;
        case 0x4Du: // motor mapping: reply with the previous one
            reply = 0xFFu;
            if (position == 3u || position == 4u)
            {
                reply = m_motorMap[position - 3u];
                m_motorMap[position - 3u] = command;
            }
            break;
        case 0x4Fu: // which bytes a poll returns
            if (position == 3u)
                m_responseMask = command;
            else if (position == 4u)
                m_responseMask |= static_cast<uint32_t>(command) << 8u;
            else if (position == 5u)
            {
                m_responseMask |= static_cast<uint32_t>(command) << 16u;
                m_mode = m_responseMask == kPressureResponse ? PressureMode
                         : m_responseMask == kAnalogResponse  ? AnalogMode
                                                              : DigitalMode;
            }
            else if (position == 8u)
                reply = 0x5Au;
            break;
        default:
            break;
        }
        return reply;
    }
}
