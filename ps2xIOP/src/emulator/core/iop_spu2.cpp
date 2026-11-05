#include "iop_spu2.h"
#include "ps2x/state_archive.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr int kCoefficients[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
        constexpr size_t kAudioCapacityFrames = Spu2::SampleRate; // one second

        // The voices' 4-point Gaussian interpolation weights, the PS1 SPU's table, which the
        // SPU2 and PCSX2 share (psx-spx, "4-Point Gaussian Interpolation").
        constexpr int16_t kGauss[512] = {
            -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001,
            -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001,
            0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001,
            0x0001, 0x0001, 0x0001, 0x0002, 0x0002, 0x0002, 0x0003, 0x0003,
            0x0003, 0x0004, 0x0004, 0x0005, 0x0005, 0x0006, 0x0007, 0x0007,
            0x0008, 0x0009, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E,
            0x000F, 0x0010, 0x0011, 0x0012, 0x0013, 0x0015, 0x0016, 0x0018,
            0x0019, 0x001B, 0x001C, 0x001E, 0x0020, 0x0021, 0x0023, 0x0025,
            0x0027, 0x0029, 0x002C, 0x002E, 0x0030, 0x0033, 0x0035, 0x0038,
            0x003A, 0x003D, 0x0040, 0x0043, 0x0046, 0x0049, 0x004D, 0x0050,
            0x0054, 0x0057, 0x005B, 0x005F, 0x0063, 0x0067, 0x006B, 0x006F,
            0x0074, 0x0078, 0x007D, 0x0082, 0x0087, 0x008C, 0x0091, 0x0096,
            0x009C, 0x00A1, 0x00A7, 0x00AD, 0x00B3, 0x00BA, 0x00C0, 0x00C7,
            0x00CD, 0x00D4, 0x00DB, 0x00E3, 0x00EA, 0x00F2, 0x00FA, 0x0101,
            0x010A, 0x0112, 0x011B, 0x0123, 0x012C, 0x0135, 0x013F, 0x0148,
            0x0152, 0x015C, 0x0166, 0x0171, 0x017B, 0x0186, 0x0191, 0x019C,
            0x01A8, 0x01B4, 0x01C0, 0x01CC, 0x01D9, 0x01E5, 0x01F2, 0x0200,
            0x020D, 0x021B, 0x0229, 0x0237, 0x0246, 0x0255, 0x0264, 0x0273,
            0x0283, 0x0293, 0x02A3, 0x02B4, 0x02C4, 0x02D6, 0x02E7, 0x02F9,
            0x030B, 0x031D, 0x0330, 0x0343, 0x0356, 0x036A, 0x037E, 0x0392,
            0x03A7, 0x03BC, 0x03D1, 0x03E7, 0x03FC, 0x0413, 0x042A, 0x0441,
            0x0458, 0x0470, 0x0488, 0x04A0, 0x04B9, 0x04D2, 0x04EC, 0x0506,
            0x0520, 0x053B, 0x0556, 0x0572, 0x058E, 0x05AA, 0x05C7, 0x05E4,
            0x0601, 0x061F, 0x063E, 0x065C, 0x067C, 0x069B, 0x06BB, 0x06DC,
            0x06FD, 0x071E, 0x0740, 0x0762, 0x0784, 0x07A7, 0x07CB, 0x07EF,
            0x0813, 0x0838, 0x085D, 0x0883, 0x08A9, 0x08D0, 0x08F7, 0x091E,
            0x0946, 0x096F, 0x0998, 0x09C1, 0x09EB, 0x0A16, 0x0A40, 0x0A6C,
            0x0A98, 0x0AC4, 0x0AF1, 0x0B1E, 0x0B4C, 0x0B7A, 0x0BA9, 0x0BD8,
            0x0C07, 0x0C38, 0x0C68, 0x0C99, 0x0CCB, 0x0CFD, 0x0D30, 0x0D63,
            0x0D97, 0x0DCB, 0x0E00, 0x0E35, 0x0E6B, 0x0EA1, 0x0ED7, 0x0F0F,
            0x0F46, 0x0F7F, 0x0FB7, 0x0FF1, 0x102A, 0x1065, 0x109F, 0x10DB,
            0x1116, 0x1153, 0x118F, 0x11CD, 0x120B, 0x1249, 0x1288, 0x12C7,
            0x1307, 0x1347, 0x1388, 0x13C9, 0x140B, 0x144D, 0x1490, 0x14D4,
            0x1517, 0x155C, 0x15A0, 0x15E6, 0x162C, 0x1672, 0x16B9, 0x1700,
            0x1747, 0x1790, 0x17D8, 0x1821, 0x186B, 0x18B5, 0x1900, 0x194B,
            0x1996, 0x19E2, 0x1A2E, 0x1A7B, 0x1AC8, 0x1B16, 0x1B64, 0x1BB3,
            0x1C02, 0x1C51, 0x1CA1, 0x1CF1, 0x1D42, 0x1D93, 0x1DE5, 0x1E37,
            0x1E89, 0x1EDC, 0x1F2F, 0x1F82, 0x1FD6, 0x202A, 0x207F, 0x20D4,
            0x2129, 0x217F, 0x21D5, 0x222C, 0x2282, 0x22DA, 0x2331, 0x2389,
            0x23E1, 0x2439, 0x2492, 0x24EB, 0x2545, 0x259E, 0x25F8, 0x2653,
            0x26AD, 0x2708, 0x2763, 0x27BE, 0x281A, 0x2876, 0x28D2, 0x292E,
            0x298B, 0x29E7, 0x2A44, 0x2AA1, 0x2AFF, 0x2B5C, 0x2BBA, 0x2C18,
            0x2C76, 0x2CD4, 0x2D33, 0x2D91, 0x2DF0, 0x2E4F, 0x2EAE, 0x2F0D,
            0x2F6C, 0x2FCC, 0x302B, 0x308B, 0x30EA, 0x314A, 0x31AA, 0x3209,
            0x3269, 0x32C9, 0x3329, 0x3389, 0x33E9, 0x3449, 0x34A9, 0x3509,
            0x3569, 0x35C9, 0x3629, 0x3689, 0x36E8, 0x3748, 0x37A8, 0x3807,
            0x3867, 0x38C6, 0x3926, 0x3985, 0x39E4, 0x3A43, 0x3AA2, 0x3B00,
            0x3B5F, 0x3BBD, 0x3C1B, 0x3C79, 0x3CD7, 0x3D35, 0x3D92, 0x3DEF,
            0x3E4C, 0x3EA9, 0x3F05, 0x3F62, 0x3FBD, 0x4019, 0x4074, 0x40D0,
            0x412A, 0x4185, 0x41DF, 0x4239, 0x4292, 0x42EB, 0x4344, 0x439C,
            0x43F4, 0x444C, 0x44A3, 0x44FA, 0x4550, 0x45A6, 0x45FC, 0x4651,
            0x46A6, 0x46FA, 0x474E, 0x47A1, 0x47F4, 0x4846, 0x4898, 0x48E9,
            0x493A, 0x498A, 0x49D9, 0x4A29, 0x4A77, 0x4AC5, 0x4B13, 0x4B5F,
            0x4BAC, 0x4BF7, 0x4C42, 0x4C8D, 0x4CD7, 0x4D20, 0x4D68, 0x4DB0,
            0x4DF7, 0x4E3E, 0x4E84, 0x4EC9, 0x4F0E, 0x4F52, 0x4F95, 0x4FD7,
            0x5019, 0x505A, 0x509A, 0x50DA, 0x5118, 0x5156, 0x5194, 0x51D0,
            0x520C, 0x5247, 0x5281, 0x52BA, 0x52F3, 0x532A, 0x5361, 0x5397,
            0x53CC, 0x5401, 0x5434, 0x5467, 0x5499, 0x54CA, 0x54FA, 0x5529,
            0x5558, 0x5585, 0x55B2, 0x55DE, 0x5609, 0x5632, 0x565B, 0x5684,
            0x56AB, 0x56D1, 0x56F6, 0x571B, 0x573E, 0x5761, 0x5782, 0x57A3,
            0x57C3, 0x57E2, 0x57FF, 0x581C, 0x5838, 0x5853, 0x586D, 0x5886,
            0x589E, 0x58B5, 0x58CB, 0x58E0, 0x58F4, 0x5907, 0x5919, 0x592A,
            0x593A, 0x5949, 0x5958, 0x5965, 0x5971, 0x597C, 0x5986, 0x598F,
            0x5997, 0x599E, 0x59A4, 0x59A9, 0x59AD, 0x59B0, 0x59B2, 0x59B3,
        };

        // Each set of four weights sums to 0x7F7F..0x7F81, as psx-spx notes: under 0x8000, so an
        // interpolated sample stays within 16 bits.
        constexpr bool gaussWeightsSumBelowOne() noexcept
        {
            for (int i = 0; i < 256; ++i)
            {
                const int sum = kGauss[i] + kGauss[0xFF - i] + kGauss[0x100 + i] + kGauss[0x1FF - i];
                if (sum < 0x7F7F || sum > 0x7F81)
                    return false;
            }
            return true;
        }
        static_assert(gaussWeightsSumBelowOne());

        uint32_t withHigh(uint32_t current, uint16_t value) noexcept
        {
            return (current & 0xFFFFu) | (static_cast<uint32_t>(value & 0xFu) << 16u);
        }

        uint32_t withLow(uint32_t current, uint16_t value) noexcept
        {
            return (current & 0xF0000u) | value;
        }

        // std::clamp returns a reference: MSVC kept the bounds in memory and selected an address.
        int32_t clamp16(int32_t value) noexcept
        {
            return value < -32768 ? -32768 : (value > 32767 ? 32767 : value);
        }

        // Save states of format version 2 hold the cores in this layout.
        enum class PhaseV2 : uint8_t
        {
            Off,
        };

        struct VoiceV2
        {
            uint16_t volumeLeft;
            uint16_t volumeRight;
            uint16_t pitch;
            uint16_t adsr1;
            uint16_t adsr2;
            uint32_t startAddress;
            uint32_t loopAddress;
            uint32_t nextAddress;
            bool active;
            bool needBlock;
            PhaseV2 phase;
            int32_t level;
            uint32_t counter;
            uint32_t position;
            uint8_t blockFlags;
            int32_t history1;
            int32_t history2;
            std::array<int16_t, 28> samples;
        };

        struct CoreV2
        {
            uint16_t attr;
            uint32_t irqAddress;
            uint32_t transferAddress;
            uint32_t endx;
            bool dmaBusy;
            bool dmaDone;
            uint32_t outPosition;
            std::array<VoiceV2, Spu2::VoiceCount> voices;
        };
    }

    void Spu2::Volume::set(uint16_t value) noexcept
    {
        reg = value;
        counter = 0u;
        if ((value & 0x8000u) == 0u)
            level = static_cast<int16_t>(static_cast<uint16_t>(value << 1u));
    }

    // PCSX2's V_VolumeSlide::Update.
    void Spu2::Volume::sweep() noexcept
    {
        const bool exponential = (reg & 0x4000u) != 0u;
        const bool decrease = (reg & 0x2000u) != 0u;
        const bool negative = (reg & 0x1000u) != 0u;
        const int shift = (reg >> 2u) & 0x1F;
        const int stepBits = reg & 3;
        int32_t stepSize = 7 - stepBits;
        if (decrease)
            stepSize = ~stepSize;
        uint32_t counterInc = 0x8000u >> std::max(0, shift - 11);
        int32_t levelInc = stepSize * (1 << std::max(0, 11 - shift));
        if (exponential)
        {
            if (!decrease && level > 0x6000)
                counterInc >>= 2u;
            if (decrease)
                levelInc = static_cast<int16_t>((levelInc * level) >> 15);
        }
        if (stepBits != 3 && shift != 0x1F)
            counterInc = std::max<uint32_t>(1u, counterInc);
        counter += counterInc;
        if (!(exponential && decrease))
            levelInc = negative ? -levelInc : levelInc;
        if (counter < 0x8000u)
            return;
        counter = 0u;
        if (!decrease)
        {
            level = std::clamp(level + levelInc, -32768, 32767);
            return;
        }
        int32_t low = negative ? -32768 : 0;
        int32_t high = negative ? 0 : 32767;
        if (exponential)
        {
            low = 0;
            high = 32767;
        }
        level = std::clamp(level + levelInc, low, high);
    }

    Spu2::Spu2()
        : m_ram(RamWords, 0u), m_audio(kAudioCapacityFrames * 2u, 0)
    {
        reset();
    }

    void Spu2::reset()
    {
        std::fill(m_ram.begin(), m_ram.end(), uint16_t{0});
        m_raw.fill(0u);
        for (Core &core : m_cores)
            core = Core{};
        m_reverb = {};
        m_spdifIrqInfo = 0u;
        m_activeVoices = 0u;
        m_enabledCores = 0u;
        m_nextSampleCycle = 0u;
        std::lock_guard<std::mutex> lock(m_audioMutex);
        m_audioHead = 0u;
        m_audioCount = 0u;
        m_producedFrames = 0u;
    }

    uint32_t Spu2::maskOf(int core, uint32_t offset) const noexcept
    {
        const uint32_t base = static_cast<uint32_t>(core) * 0x400u + offset;
        return static_cast<uint32_t>(m_raw[base >> 1]) | (static_cast<uint32_t>(m_raw[(base + 2u) >> 1] & 0xFFu) << 16u);
    }

    uint32_t Spu2::voiceNextAddress(int core, int voice) const noexcept
    {
        return m_cores[static_cast<size_t>(core)].voices[static_cast<size_t>(voice)].nextAddress;
    }

    uint16_t Spu2::read16(uint32_t physical)
    {
        const uint32_t offset = (physical - RegBase) & 0x7FEu;
        if (offset == RegSpdifIrqInfo)
            return m_spdifIrqInfo;
        if (offset >= RegMvol)
        {
            for (int core = 0; core < CoreCount; ++core)
            {
                const uint32_t mvolx = RegMvolx + static_cast<uint32_t>(core) * 0x28u;
                if (offset == mvolx || offset == mvolx + 2u)
                    return static_cast<uint16_t>(m_cores[static_cast<size_t>(core)].master[(offset - mvolx) / 2u].level);
            }
            return m_raw[offset >> 1];
        }

        const int core = offset >= 0x400u ? 1 : 0;
        const uint32_t c = offset & 0x3FFu;
        Core &state = m_cores[static_cast<size_t>(core)];

        if (c < 0x180u)
        {
            const Voice &voice = state.voices[c / 0x10u];
            switch (c & 0xFu)
            {
            case 0xA: return static_cast<uint16_t>(voice.level);
            case 0xC: return static_cast<uint16_t>(voice.volume[0].level);
            case 0xE: return static_cast<uint16_t>(voice.volume[1].level);
            default: return m_raw[offset >> 1];
            }
        }
        if (c >= RegVoiceAddress && c < 0x2E0u)
        {
            const uint32_t relative = c - RegVoiceAddress;
            const Voice &voice = state.voices[relative / 12u];
            switch (relative % 12u)
            {
            case 0: return static_cast<uint16_t>((voice.startAddress >> 16u) & 0xFu);
            case 2: return static_cast<uint16_t>(voice.startAddress & 0xFFFFu);
            case 4: return static_cast<uint16_t>((voice.loopAddress >> 16u) & 0xFu);
            case 6: return static_cast<uint16_t>(voice.loopAddress & 0xFFFFu);
            case 8: return static_cast<uint16_t>((voice.nextAddress >> 16u) & 0xFu);
            case 10: return static_cast<uint16_t>(voice.nextAddress & 0xFFFFu);
            default: return m_raw[offset >> 1];
            }
        }

        switch (c)
        {
        case RegEndx:
            return static_cast<uint16_t>(state.endx & 0xFFFFu);
        case RegEndx + 2u:
            return static_cast<uint16_t>((state.endx >> 16u) & 0xFFu);
        case RegStatx:
            if (state.dmaBusy)
                return StatxBusy;
            return (state.dmaDone && (state.attr & AttrTransferMask) != 0u) ? StatxDone : uint16_t{0};
        case RegTsa:
            return static_cast<uint16_t>((state.transferAddress >> 16u) & 0xFu);
        case RegTsa + 2u:
            return static_cast<uint16_t>(state.transferAddress & 0xFFFFu);
        case RegData:
        {
            const uint16_t value = m_ram[state.transferAddress & RamMask];
            state.transferAddress = (state.transferAddress + 1u) & RamMask;
            return value;
        }
        default:
            return m_raw[offset >> 1];
        }
    }

    void Spu2::write16(uint32_t physical, uint16_t value)
    {
        const uint32_t offset = (physical - RegBase) & 0x7FEu;
        m_raw[offset >> 1] = value;
        if (offset >= RegMvol)
        {
            for (int core = 0; core < CoreCount; ++core)
            {
                const uint32_t mvol = RegMvol + static_cast<uint32_t>(core) * 0x28u;
                if (offset == mvol || offset == mvol + 2u)
                    m_cores[static_cast<size_t>(core)].master[(offset - mvol) / 2u].set(value);
            }
            return; // other volumes and SPDIF: kept in the backing store
        }

        const int core = offset >= 0x400u ? 1 : 0;
        const uint32_t c = offset & 0x3FFu;
        Core &state = m_cores[static_cast<size_t>(core)];

        if (c < 0x180u)
        {
            Voice &voice = state.voices[c / 0x10u];
            switch (c & 0xFu)
            {
            case 0x0: voice.volume[0].set(value); break;
            case 0x2: voice.volume[1].set(value); break;
            case 0x4: voice.pitch = value; break;
            case 0x6: voice.adsr1 = value; break;
            case 0x8: voice.adsr2 = value; break;
            default: break;
            }
            return;
        }
        if (c >= RegVoiceAddress && c < 0x2E0u)
        {
            const uint32_t relative = c - RegVoiceAddress;
            Voice &voice = state.voices[relative / 12u];
            switch (relative % 12u)
            {
            case 0: voice.startAddress = withHigh(voice.startAddress, value); break;
            case 2: voice.startAddress = withLow(voice.startAddress, value); break;
            // A written loop address holds against loop-start flags until the next key on.
            case 4: voice.loopAddress = withHigh(voice.loopAddress, value); voice.customLoop = true; break;
            case 6: voice.loopAddress = withLow(voice.loopAddress, value); voice.customLoop = true; break;
            case 8: voice.nextAddress = withHigh(voice.nextAddress, value); break;
            case 10: voice.nextAddress = withLow(voice.nextAddress, value); break;
            default: break;
            }
            return;
        }

        switch (c)
        {
        case RegAttr:
            if ((value & AttrEffectEnable) != 0u && (state.attr & AttrEffectEnable) == 0u)
            {
                static int reported = 0;
                const uint32_t base = (offset & 0x400u) >> 1;
                if (reported++ < 4)
                    std::fprintf(stderr, "[spu2] core %d reverb on, work area 0x%x-0x%xffff\n", core,
                                 ((m_raw[base + (RegEsa >> 1)] & 0xFu) << 16u) | m_raw[base + ((RegEsa + 2u) >> 1)],
                                 m_raw[base + (RegEea >> 1)] & 0xFu);
            }
            state.attr = value;
            if ((value & AttrEnable) != 0u)
                m_enabledCores |= 1u << core;
            else
                m_enabledCores &= ~(1u << core);
            if ((value & AttrTransferMask) == 0u)
                state.dmaDone = false;
            if ((value & AttrIrqEnable) == 0u)
                m_spdifIrqInfo = static_cast<uint16_t>(m_spdifIrqInfo & ~(4u << core));
            break;
        case RegIrqa:
            state.irqAddress = withHigh(state.irqAddress, value);
            break;
        case RegEsa:
        case RegEsa + 2u:
            // Writing ESA also moves the buffer position to it (psx-spx).
            m_reverb[static_cast<size_t>(core)].position = 0u;
            break;
        case RegIrqa + 2u:
            state.irqAddress = withLow(state.irqAddress, value);
            break;
        case RegKon:
            for (int bit = 0; bit < 16; ++bit)
                if ((value & (1u << bit)) != 0u)
                    keyOn(core, bit);
            break;
        case RegKon + 2u:
            for (int bit = 0; bit < 8; ++bit)
                if ((value & (1u << bit)) != 0u)
                    keyOn(core, 16 + bit);
            break;
        case RegKoff:
            for (int bit = 0; bit < 16; ++bit)
                if ((value & (1u << bit)) != 0u)
                    keyOff(core, bit);
            break;
        case RegKoff + 2u:
            for (int bit = 0; bit < 8; ++bit)
                if ((value & (1u << bit)) != 0u)
                    keyOff(core, 16 + bit);
            break;
        case RegAdmas:
            if (value != 0u)
            {
                static int reported = 0;
                if (reported++ < 4)
                    std::fprintf(stderr, "[spu2] core %d ADMA streaming requested (ADMAS=0x%x), not modelled\n", core, value);
            }
            break;
        case RegTsa:
            state.transferAddress = withHigh(state.transferAddress, value);
            break;
        case RegTsa + 2u:
            state.transferAddress = withLow(state.transferAddress, value);
            break;
        case RegData:
            m_ram[state.transferAddress & RamMask] = value;
            testIrqRange(core, state.transferAddress, 1u);
            state.transferAddress = (state.transferAddress + 1u) & RamMask;
            break;
        default:
            break;
        }
    }

    void Spu2::dmaWrite(int core, const uint8_t *source, uint32_t bytes)
    {
        Core &state = m_cores[static_cast<size_t>(core)];
        const uint32_t words = bytes / 2u;
        uint32_t address = state.transferAddress & RamMask;
        for (uint32_t i = 0u; i < words; ++i)
        {
            uint16_t value = 0u;
            std::memcpy(&value, source + static_cast<size_t>(i) * 2u, sizeof(value));
            m_ram[(address + i) & RamMask] = value;
        }
        testIrqRange(core, address, words);
        state.transferAddress = (address + words) & RamMask;
    }

    void Spu2::dmaRead(int core, uint8_t *destination, uint32_t bytes)
    {
        Core &state = m_cores[static_cast<size_t>(core)];
        const uint32_t words = bytes / 2u;
        const uint32_t address = state.transferAddress & RamMask;
        for (uint32_t i = 0u; i < words; ++i)
        {
            const uint16_t value = m_ram[(address + i) & RamMask];
            std::memcpy(destination + static_cast<size_t>(i) * 2u, &value, sizeof(value));
        }
        state.transferAddress = (address + words) & RamMask;
    }

    void Spu2::dmaStarted(int core)
    {
        Core &state = m_cores[static_cast<size_t>(core)];
        state.dmaBusy = true;
        state.dmaDone = false;
    }

    void Spu2::dmaCompleted(int core)
    {
        Core &state = m_cores[static_cast<size_t>(core)];
        state.dmaBusy = false;
        state.dmaDone = true;
    }

    void Spu2::testIrqRange(int core, uint32_t firstWord, uint32_t words)
    {
        const Core &state = m_cores[static_cast<size_t>(core)];
        if ((state.attr & AttrIrqEnable) == 0u)
            return;
        const uint32_t delta = (state.irqAddress - firstWord) & RamMask;
        if (delta < words)
            assertIrq(core);
    }

    void Spu2::testIrqAllCores(uint32_t wordAddress)
    {
        for (int core = 0; core < CoreCount; ++core)
        {
            const Core &state = m_cores[static_cast<size_t>(core)];
            if ((state.attr & AttrIrqEnable) != 0u && state.irqAddress == (wordAddress & RamMask))
                assertIrq(core);
        }
    }

    void Spu2::memoryOutWrite(uint32_t wordAddress, int16_t value)
    {
        m_ram[wordAddress & RamMask] = static_cast<uint16_t>(value);
        testIrqAllCores(wordAddress);
    }

    void Spu2::assertIrq(int core)
    {
        m_spdifIrqInfo = static_cast<uint16_t>(m_spdifIrqInfo | (4u << core));
        if (raiseIrq)
            raiseIrq();
    }

    void Spu2::refreshActiveCount() noexcept
    {
        uint32_t count = 0u;
        for (const Core &core : m_cores)
            for (const Voice &voice : core.voices)
                if (voice.active)
                    ++count;
        m_activeVoices = count;
    }

    void Spu2::keyOn(int core, int voiceIndex)
    {
        Core &state = m_cores[static_cast<size_t>(core)];
        Voice &voice = state.voices[static_cast<size_t>(voiceIndex)];
        // The loop address is left as it is (PCSX2's V_Voice::Start): drivers point one-shot
        // samples at a silent loop by writing it, before or after the key on.
        voice.nextAddress = voice.startAddress;
        voice.customLoop = false;
        voice.position = 0u;
        voice.needBlock = true;
        voice.blockFlags = 0u;
        voice.history1 = 0;
        voice.history2 = 0;
        voice.samples.fill(0);
        voice.phase = Phase::Attack;
        voice.level = 0;
        voice.counter = 0u;
        voice.active = true;
        state.endx &= ~(1u << voiceIndex);
        refreshActiveCount();
    }

    void Spu2::keyOff(int core, int voiceIndex)
    {
        Voice &voice = m_cores[static_cast<size_t>(core)].voices[static_cast<size_t>(voiceIndex)];
        if (voice.active && voice.phase != Phase::Off)
        {
            voice.phase = Phase::Release;
            voice.counter = 0u;
        }
    }

    void Spu2::stopVoice(int core, int voiceIndex)
    {
        Voice &voice = m_cores[static_cast<size_t>(core)].voices[static_cast<size_t>(voiceIndex)];
        voice.active = false;
        voice.phase = Phase::Off;
        voice.level = 0;
        refreshActiveCount();
    }

    void Spu2::decodeAdpcm(const uint16_t *block, int32_t &history1, int32_t &history2, int16_t *samples) noexcept
    {
        const uint8_t header = static_cast<uint8_t>(block[0] & 0xFFu);
        int shift = header & 0xF;
        if (shift > 12)
            shift = 9;
        const int predictor = std::min(static_cast<int>((header >> 4) & 0xF), 4);
        const int coefficient0 = kCoefficients[predictor][0];
        const int coefficient1 = kCoefficients[predictor][1];
        for (int i = 0; i < 28; ++i)
        {
            const uint32_t byteIndex = 2u + static_cast<uint32_t>(i) / 2u;
            const uint16_t word = block[byteIndex / 2u];
            const uint8_t byte = (byteIndex & 1u) != 0u ? static_cast<uint8_t>(word >> 8u) : static_cast<uint8_t>(word & 0xFFu);
            const int nibble = (i & 1) != 0 ? (byte >> 4) & 0xF : byte & 0xF;
            int32_t sample = static_cast<int16_t>(static_cast<uint16_t>(nibble << 12)) >> shift;
            sample += (history1 * coefficient0 + history2 * coefficient1 + 32) >> 6;
            sample = clamp16(sample);
            samples[i] = static_cast<int16_t>(sample);
            history2 = history1;
            history1 = sample;
        }
    }

    void Spu2::fetchBlock(int core, int voiceIndex)
    {
        Core &state = m_cores[static_cast<size_t>(core)];
        Voice &voice = state.voices[static_cast<size_t>(voiceIndex)];
        if ((voice.blockFlags & 1u) != 0u)
        {
            // The previous block ended the loop: repeat from the loop address or stop.
            if ((voice.blockFlags & 2u) != 0u)
            {
                voice.nextAddress = voice.loopAddress;
            }
            else
            {
                stopVoice(core, voiceIndex);
                return;
            }
        }

        const uint32_t address = voice.nextAddress & RamMask;
        uint16_t block[8];
        for (uint32_t i = 0u; i < 8u; ++i)
            block[i] = m_ram[(address + i) & RamMask];
        // Keep the previous block's last three samples for the interpolation.
        std::copy(voice.samples.end() - 3, voice.samples.end(), voice.samples.begin());
        decodeAdpcm(block, voice.history1, voice.history2, voice.samples.data() + 3);
        voice.blockFlags = static_cast<uint8_t>(block[0] >> 8u);
        if ((voice.blockFlags & 4u) != 0u && !voice.customLoop)
            voice.loopAddress = address;
        if ((voice.blockFlags & 1u) != 0u)
            state.endx |= 1u << voiceIndex;
        testIrqRange(core, address, 8u);
        voice.nextAddress = (address + 8u) & RamMask;
    }

    // Inline in tickSample, its one caller, for every voice at every sample.
#if defined(_MSC_VER)
    __forceinline
#else
    inline __attribute__((always_inline))
#endif
    void Spu2::stepEnvelope(Voice &voice) const
    {
        bool increase = false;
        bool exponential = false;
        int shift = 0;
        int step = 0;
        switch (voice.phase)
        {
        case Phase::Attack:
            exponential = (voice.adsr1 & 0x8000u) != 0u;
            shift = (voice.adsr1 >> 10) & 0x1F;
            step = 7 - ((voice.adsr1 >> 8) & 3);
            increase = true;
            break;
        case Phase::Decay:
            exponential = true;
            shift = (voice.adsr1 >> 4) & 0xF;
            step = -8;
            break;
        case Phase::Sustain:
            exponential = (voice.adsr2 & 0x8000u) != 0u;
            increase = (voice.adsr2 & 0x4000u) == 0u;
            shift = (voice.adsr2 >> 8) & 0x1F;
            step = (voice.adsr2 >> 6) & 3;
            step = increase ? 7 - step : -8 + step;
            break;
        case Phase::Release:
            exponential = (voice.adsr2 & 0x20u) != 0u;
            shift = voice.adsr2 & 0x1F;
            step = -8;
            break;
        case Phase::Off:
            return;
        }

        uint32_t cycles = 1u << std::max(0, shift - 11);
        if (exponential && increase && voice.level > 0x6000)
            cycles *= 4u;
        if (++voice.counter < cycles)
            return;
        voice.counter = 0u;
        int32_t amount = step * (1 << std::max(0, 11 - shift));
        if (exponential && !increase)
            amount = (amount * voice.level) >> 15;
        // Not std::clamp, which returns a reference: MSVC put the bounds on the stack.
        const int32_t level = voice.level + amount;
        voice.level = level < 0 ? 0 : (level > 0x7FFF ? 0x7FFF : level);

        switch (voice.phase)
        {
        case Phase::Attack:
            if (voice.level >= 0x7FFF)
            {
                voice.level = 0x7FFF;
                voice.phase = Phase::Decay;
                // A sustain level at full scale needs no decay.
                if (std::min(((voice.adsr1 & 0xF) + 1) * 0x800, 0x7FFF) >= voice.level)
                    voice.phase = Phase::Sustain;
            }
            break;
        case Phase::Decay:
        {
            const int32_t sustainLevel = std::min(((voice.adsr1 & 0xF) + 1) * 0x800, 0x7FFF);
            if (voice.level <= sustainLevel)
                voice.phase = Phase::Sustain;
            break;
        }
        case Phase::Release:
            if (voice.level == 0)
                voice.phase = Phase::Off;
            break;
        default:
            break;
        }
    }

    void Spu2::tickSample()
    {
        // Core 0's output enters core 1 as its external input; core 1's output is the SPU2's.
        int32_t external[2] = {0, 0};
        int32_t output[2] = {0, 0};
        for (int core = 0; core < CoreCount; ++core)
        {
            Core &state = m_cores[static_cast<size_t>(core)];
            const uint32_t dryMask[2] = {maskOf(core, RegVmixl), maskOf(core, RegVmixr)};
            const uint32_t wetMask[2] = {maskOf(core, RegVmixel), maskOf(core, RegVmixer)};
            int32_t voices[2] = {0, 0};
            int32_t wet[2] = {0, 0};
            int32_t voice1Output = 0;
            int32_t voice3Output = 0;
            for (int index = 0; index < VoiceCount; ++index)
            {
                Voice &voice = state.voices[static_cast<size_t>(index)];
                voice.volume[0].step();
                voice.volume[1].step();
                if (!voice.active)
                    continue;
                if (voice.needBlock)
                {
                    fetchBlock(core, index);
                    voice.needBlock = false;
                    voice.position = 0u;
                    if (!voice.active)
                        continue;
                }

                // In a race more than half of the voices play at zero volume on both sides: they
                // add nothing to the mix, so their sample is neither computed nor mixed. Voices 1
                // and 3 also feed the memory output buffers.
                const bool heard = voice.volume[0].level != 0 || voice.volume[1].level != 0 || index == 1 || index == 3;
                int32_t sample = 0;
                if (heard)
                {
                    // The hardware's Gaussian interpolation over the current sample and the three
                    // before it, weighted by bits 4 to 11 of the pitch counter.
                    const uint32_t current = std::min<uint32_t>(voice.position >> 12u, 27u);
                    const int16_t *taps = voice.samples.data() + current;
                    const uint32_t i = (voice.position >> 4u) & 0xFFu;
                    sample = ((kGauss[0xFFu - i] * taps[0]) >> 15) + ((kGauss[0x1FFu - i] * taps[1]) >> 15) +
                             ((kGauss[0x100u + i] * taps[2]) >> 15) + ((kGauss[i] * taps[3]) >> 15);
                }

                stepEnvelope(voice);
                if (voice.phase == Phase::Off)
                {
                    stopVoice(core, index);
                    continue;
                }
                if (heard)
                {
                    sample = (sample * voice.level) >> 15;
                    if (index == 1)
                        voice1Output = sample;
                    else if (index == 3)
                        voice3Output = sample;
                    for (int channel = 0; channel < 2; ++channel)
                    {
                        const int32_t value = (sample * voice.volume[static_cast<size_t>(channel)].level) >> 15;
                        if ((dryMask[channel] & (1u << index)) != 0u)
                            voices[channel] += value;
                        if ((wetMask[channel] & (1u << index)) != 0u)
                            wet[channel] += value;
                    }
                }

                voice.position += std::min<uint32_t>(voice.pitch, 0x3FFFu);
                while ((voice.position >> 12u) >= 28u)
                {
                    voice.position -= 28u << 12u;
                    fetchBlock(core, index);
                    if (!voice.active)
                        break;
                }
            }

            state.master[0].step();
            state.master[1].step();
            if ((state.attr & AttrEnable) == 0u)
                continue;

            // MMIX gates (core 0 has no external input): bits 11/10 voices, 3/2 external input
            // to the dry mix; bits 9/8 voices, 1/0 external input to the reverb. The sound data
            // input (ADMA) is not modelled.
            const uint32_t mmix = m_raw[(static_cast<uint32_t>(core) * 0x400u + RegMmix) >> 1] & (core == 0 ? 0xFF0u : 0xFFFu);
            const int32_t dry[2] = {((mmix & 0x800u) ? voices[0] : 0) + ((mmix & 0x008u) ? external[0] : 0),
                                    ((mmix & 0x400u) ? voices[1] : 0) + ((mmix & 0x004u) ? external[1] : 0)};
            const int32_t wetInput[2] = {((mmix & 0x200u) ? wet[0] : 0) + ((mmix & 0x002u) ? external[0] : 0),
                                         ((mmix & 0x100u) ? wet[1] : 0) + ((mmix & 0x001u) ? external[1] : 0)};
            int32_t effect[2] = {0, 0};
            reverbSample(core, wetInput, effect);
            const uint32_t evol = (RegEvol + static_cast<uint32_t>(core) * 0x28u) >> 1;
            const int32_t dryLeft = clamp16(dry[0]);
            const int32_t dryRight = clamp16(dry[1]);
            const int32_t mixLeft = clamp16(dry[0] + ((effect[0] * static_cast<int16_t>(m_raw[evol])) >> 15));
            const int32_t mixRight = clamp16(dry[1] + ((effect[1] * static_cast<int16_t>(m_raw[evol + 1u])) >> 15));
            const int32_t outLeft = (mixLeft * state.master[0].level) >> 15;
            const int32_t outRight = (mixRight * state.master[1].level) >> 15;

            // The mixer streams its results into sound RAM every sample (the memory output
            // buffers, 0x200 words each). Software uses them as a clock by putting IRQA inside
            // one of them. All offsets are in 16-bit words.
            const uint32_t position = state.outPosition;
            const uint32_t voiceBase = core == 0 ? 0x400u : 0xC00u;
            const uint32_t mixBase = core == 0 ? 0x1000u : 0x1800u;
            memoryOutWrite(voiceBase + position, static_cast<int16_t>(voice1Output));
            memoryOutWrite(voiceBase + 0x200u + position, static_cast<int16_t>(voice3Output));
            memoryOutWrite(mixBase + position, static_cast<int16_t>(dryLeft));
            memoryOutWrite(mixBase + 0x200u + position, static_cast<int16_t>(dryRight));
            memoryOutWrite(mixBase + 0x400u + position, static_cast<int16_t>(clamp16(wet[0])));
            memoryOutWrite(mixBase + 0x600u + position, static_cast<int16_t>(clamp16(wet[1])));
            if (core == 0)
            {
                memoryOutWrite(0x800u + position, static_cast<int16_t>(clamp16(outLeft)));
                memoryOutWrite(0xA00u + position, static_cast<int16_t>(clamp16(outRight)));
                // Core 1's BVOL: the external input volume, a plain signed level.
                const uint32_t bvol = (RegBvol + 0x28u) >> 1;
                external[0] = (clamp16(outLeft) * static_cast<int16_t>(m_raw[bvol])) >> 15;
                external[1] = (clamp16(outRight) * static_cast<int16_t>(m_raw[bvol + 1u])) >> 15;
            }
            else
            {
                output[0] = outLeft;
                output[1] = outRight;
            }
            state.outPosition = (position + 1u) & 0x1FFu;
        }

        const int16_t left = static_cast<int16_t>(clamp16(output[0]));
        const int16_t right = static_cast<int16_t>(clamp16(output[1]));
        m_silentFrames = (left == 0 && right == 0) ? m_silentFrames + 1u : 0u;
        std::lock_guard<std::mutex> lock(m_audioMutex);
        if (m_audioCount == kAudioCapacityFrames)
        {
            m_audioHead = (m_audioHead + 1u) % kAudioCapacityFrames;
            --m_audioCount;
        }
        const size_t slot = (m_audioHead + m_audioCount) % kAudioCapacityFrames;
        m_audio[slot * 2u] = left;
        m_audio[slot * 2u + 1u] = right;
        ++m_audioCount;
        ++m_producedFrames;
    }

    void Spu2::reverbSample(int core, const int32_t (&input)[2], int32_t (&output)[2])
    {
        // The resampling filter (psx-spx): odd taps are zero but the centre one. Its gain is one
        // going down (>> 15) and two coming up (>> 14), half of each side's output being zeros.
        static constexpr int32_t kFir[39] = {-1, 0, 2, 0, -10, 0, 35, 0, -103, 0, 266, 0, -616, 0, 1332, 0, -2960, 0, 10246, 16384,
                                             10246, 0, -2960, 0, 1332, 0, -616, 0, 266, 0, -103, 0, 35, 0, -10, 0, 2, 0, -1};
        Reverb &reverb = m_reverb[static_cast<size_t>(core)];
        const uint32_t slot = reverb.slot;
        const uint32_t first = (slot + 64u - 38u) & 63u; // the 39 samples that end at this one
        const auto fir = [&](const std::array<int16_t, 128> &history) {
            const int16_t *samples = history.data() + first;
            int32_t sum = samples[19] * kFir[19];
            for (int tap = 0; tap < 39; tap += 2)
                sum += samples[tap] * kFir[tap];
            return sum;
        };
        const bool right = reverb.right;
        reverb.slot = (slot + 1u) & 63u;
        reverb.right = !right;

        const uint32_t base = static_cast<uint32_t>(core) * 0x400u;
        const auto pair = [&](uint32_t offset) {
            return ((static_cast<uint32_t>(m_raw[(base + offset) >> 1]) & 0xFu) << 16u) | m_raw[(base + offset + 2u) >> 1];
        };
        const uint32_t start = pair(RegEsa);
        const uint32_t end = ((static_cast<uint32_t>(m_raw[(base + RegEea) >> 1]) & 0xFu) << 16u) | 0xFFFFu;
        const uint32_t volumes = (RegReverbVolumes + static_cast<uint32_t>(core) * 0x28u) >> 1;
        const auto volume = [&](uint32_t index) { return static_cast<int32_t>(static_cast<int16_t>(m_raw[volumes + index])); };
        const uint32_t evol = (RegEvol + static_cast<uint32_t>(core) * 0x28u) >> 1;
        const bool writes = (m_cores[static_cast<size_t>(core)].attr & AttrEffectEnable) != 0u;
        if (start >= end || (!writes && m_raw[evol] == 0u && m_raw[evol + 1u] == 0u))
        {
            // No work area, or a reverb that neither writes nor is heard: only the clock moves.
            if (right)
                ++reverb.position;
            return;
        }
        const uint32_t size = end - start + 1u;
        if (reverb.position >= size)
            reverb.position = 0u;
        const uint32_t position = reverb.position;
        if (right)
            reverb.position = position + 1u;

        for (size_t side = 0; side < 2u; ++side)
        {
            const int16_t value = static_cast<int16_t>(clamp16(input[side]));
            reverb.input[side][slot] = value;
            reverb.input[side][slot + 64u] = value;
        }

        // A word of the work area, offset words after the buffer position, wrapped in ESA..EEA.
        // While the reverb writes, its 14 accesses a sample can raise the IRQ (as in PCSX2).
        const int64_t length = size;
        const auto at = [&](int64_t offset) {
            int64_t x = static_cast<int64_t>(position) + offset;
            if (x >= length)
                x = x - length < length ? x - length : x % length;
            else if (x < 0)
                x = x + length >= 0 ? x + length : (x % length + length) % length;
            const uint32_t word = (start + static_cast<uint32_t>(x)) & RamMask;
            if (writes)
                testIrqAllCores(word);
            return word;
        };
        const auto ram = [&](uint32_t word) { return static_cast<int32_t>(static_cast<int16_t>(m_ram[word])); };
        const auto mul = [](int32_t a, int32_t b) { return static_cast<int32_t>((static_cast<int64_t>(a) * b) >> 15); };

        const uint32_t side = right ? 4u : 0u; // right-side address registers follow the left ones
        const int64_t sameDestination = pair(0x2ECu + side);
        const int64_t diffDestination = pair(0x30Cu + side);
        const int64_t apf1Destination = pair(0x32Cu + side);
        const int64_t apf2Destination = pair(0x334u + side);
        const uint32_t sameSource = at(pair(0x304u + side));
        const uint32_t samePrevious = at(sameDestination - 1);
        const uint32_t diffSource = at(pair(0x328u - side)); // the other side's
        const uint32_t diffPrevious = at(diffDestination - 1);
        const uint32_t comb[4] = {at(pair(0x2F4u + side)), at(pair(0x2FCu + side)), at(pair(0x314u + side)), at(pair(0x31Cu + side))};
        const uint32_t apf1Source = at(apf1Destination - pair(0x2E4u));
        const uint32_t apf2Source = at(apf2Destination - pair(0x2E8u));
        const uint32_t sameWord = at(sameDestination);
        const uint32_t diffWord = at(diffDestination);
        const uint32_t apf1Word = at(apf1Destination);
        const uint32_t apf2Word = at(apf2Destination);

        const int32_t in = mul(volume(right ? 9u : 8u), clamp16(fir(reverb.input[right ? 1u : 0u]) >> 15));
        const int32_t iir = volume(0u);
        const int32_t wall = volume(5u);
        // Same side, then the other side's reflection.
        const int32_t same = mul(iir, in + mul(wall, ram(sameSource)) - ram(samePrevious)) + ram(samePrevious);
        const int32_t diff = mul(iir, in + mul(wall, ram(diffSource)) - ram(diffPrevious)) + ram(diffPrevious);
        // Early echo: four combs.
        int32_t out = mul(volume(1u), ram(comb[0])) + mul(volume(2u), ram(comb[1])) + mul(volume(3u), ram(comb[2])) +
                      mul(volume(4u), ram(comb[3]));
        // Late reverb: two all-pass filters.
        const int32_t apf1 = out - mul(volume(6u), ram(apf1Source));
        out = ram(apf1Source) + mul(volume(6u), apf1);
        const int32_t apf2 = out - mul(volume(7u), ram(apf2Source));
        out = ram(apf2Source) + mul(volume(7u), apf2);
        // ATTR bit 7 clear: the work area is read, never written.
        if (writes)
        {
            m_ram[sameWord] = static_cast<uint16_t>(clamp16(same));
            m_ram[diffWord] = static_cast<uint16_t>(clamp16(diff));
            m_ram[apf1Word] = static_cast<uint16_t>(clamp16(apf1));
            m_ram[apf2Word] = static_cast<uint16_t>(clamp16(apf2));
        }

        const int16_t result = static_cast<int16_t>(clamp16(out));
        const size_t heard = right ? 1u : 0u;
        reverb.output[heard][slot] = result;
        reverb.output[heard][slot + 64u] = result;
        reverb.output[1u - heard][slot] = 0;
        reverb.output[1u - heard][slot + 64u] = 0;
        output[0] = clamp16(fir(reverb.output[0]) >> 14);
        output[1] = clamp16(fir(reverb.output[1]) >> 14);
    }

    void Spu2::advanceTo(uint64_t cycle)
    {
        while ((m_activeVoices != 0u || m_enabledCores != 0u) && m_nextSampleCycle <= cycle)
        {
            tickSample();
            m_nextSampleCycle += CyclesPerSample;
        }
        if (m_activeVoices == 0u && m_enabledCores == 0u && m_nextSampleCycle <= cycle)
        {
            // The SPU2 is off: keep the sample phase without generating frames.
            m_nextSampleCycle += ((cycle - m_nextSampleCycle) / CyclesPerSample + 1u) * CyclesPerSample;
        }
    }

    size_t Spu2::readAudio(int16_t *destination, size_t frames)
    {
        std::lock_guard<std::mutex> lock(m_audioMutex);
        const size_t count = std::min(frames, m_audioCount);
        for (size_t i = 0u; i < count; ++i)
        {
            const size_t slot = (m_audioHead + i) % kAudioCapacityFrames;
            destination[i * 2u] = m_audio[slot * 2u];
            destination[i * 2u + 1u] = m_audio[slot * 2u + 1u];
        }
        m_audioHead = (m_audioHead + count) % kAudioCapacityFrames;
        m_audioCount -= count;
        return count;
    }
}

namespace ps2x::iop::detail
{
    void Spu2::loadVersion2(StateArchive &archive)
    {
        std::array<CoreV2, CoreCount> old{};
        archive.value(old);
        for (int core = 0; core < CoreCount; ++core)
        {
            const CoreV2 &from = old[static_cast<size_t>(core)];
            Core &to = m_cores[static_cast<size_t>(core)];
            to = Core{};
            to.attr = from.attr;
            to.irqAddress = from.irqAddress;
            to.transferAddress = from.transferAddress;
            to.endx = from.endx;
            to.dmaBusy = from.dmaBusy;
            to.dmaDone = from.dmaDone;
            to.outPosition = from.outPosition;
            const uint32_t mvol = (RegMvol + static_cast<uint32_t>(core) * 0x28u) >> 1;
            for (size_t channel = 0; channel < 2u; ++channel)
            {
                to.master[channel].set(m_raw[mvol + channel]);
                if ((m_raw[mvol + channel] & 0x8000u) != 0u)
                    to.master[channel].level = 0x7FFF; // version 2 played sweeps at full scale
            }
            for (int index = 0; index < VoiceCount; ++index)
            {
                const VoiceV2 &v = from.voices[static_cast<size_t>(index)];
                Voice &voice = to.voices[static_cast<size_t>(index)];
                for (size_t channel = 0; channel < 2u; ++channel)
                {
                    const uint16_t reg = channel == 0u ? v.volumeLeft : v.volumeRight;
                    voice.volume[channel].set(reg);
                    if ((reg & 0x8000u) != 0u)
                        voice.volume[channel].level = (reg & 0x2000u) != 0u ? 0 : 0x7FFF;
                }
                voice.pitch = v.pitch;
                voice.adsr1 = v.adsr1;
                voice.adsr2 = v.adsr2;
                voice.startAddress = v.startAddress;
                voice.loopAddress = v.loopAddress;
                voice.nextAddress = v.nextAddress;
                voice.active = v.active;
                voice.needBlock = v.needBlock;
                voice.phase = static_cast<Phase>(v.phase);
                voice.level = v.level;
                voice.counter = v.counter;
                voice.position = v.position;
                voice.blockFlags = v.blockFlags;
                voice.history1 = v.history1;
                voice.history2 = v.history2;
                std::copy(v.samples.begin(), v.samples.end(), voice.samples.begin() + 3);
                // Version 2 overwrote the loop address at key on; the register keeps what the
                // driver wrote, a silent loop for one-shot samples.
                const uint32_t lsa = core * 0x200u + ((RegVoiceAddress + static_cast<uint32_t>(index) * 12u + 4u) >> 1);
                const uint32_t written = (static_cast<uint32_t>(m_raw[lsa] & 0xFu) << 16u) | m_raw[lsa + 1u];
                if (written != voice.startAddress)
                {
                    voice.loopAddress = written;
                    voice.customLoop = true;
                }
            }
        }
    }

    void Spu2::serialize(StateArchive &archive)
    {
        archive.tag("SPU2");
        archive.vector(m_ram);
        archive.value(m_raw);
        // Version 0 is an in-memory archive of this build's layout.
        if (archive.loading() && archive.version() != 0u && archive.version() < 3u)
            loadVersion2(archive);
        else
            archive.value(m_cores);
        // The reverb's state is saved since format version 5; earlier states start it empty.
        if (!archive.loading() || archive.version() == 0u || archive.version() >= 5u)
            archive.value(m_reverb);
        else
            m_reverb = {};
        archive.value(m_spdifIrqInfo);
        archive.value(m_activeVoices);
        archive.value(m_enabledCores);
        archive.value(m_nextSampleCycle);
        archive.value(m_producedFrames);
        if (archive.loading())
        {
            // Samples already mixed are host output, not machine state.
            std::lock_guard<std::mutex> lock(m_audioMutex);
            m_audioHead = 0;
            m_audioCount = 0;
        }
    }
}
