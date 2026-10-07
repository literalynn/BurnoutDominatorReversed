#include "iop_spu2.h"

#include <algorithm>
#include <cstring>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr int kCoefficients[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
        constexpr size_t kAudioCapacityFrames = Spu2::SampleRate; // one second

        // Fixed volume: 15-bit signed, 0x3FFF is full scale. The sweep modes (bit 15) are not
        // modelled; such voices play at full scale.
        int32_t volumeOf(uint16_t reg) noexcept
        {
            if ((reg & 0x8000u) != 0u)
                return 0x3FFF;
            return static_cast<int16_t>(static_cast<uint16_t>(reg << 1u)) >> 1;
        }

        uint32_t withHigh(uint32_t current, uint16_t value) noexcept
        {
            return (current & 0xFFFFu) | (static_cast<uint32_t>(value & 0xFu) << 16u);
        }

        uint32_t withLow(uint32_t current, uint16_t value) noexcept
        {
            return (current & 0xF0000u) | value;
        }
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
        if (offset >= 0x760u)
            return m_raw[offset >> 1];

        const int core = offset >= 0x400u ? 1 : 0;
        const uint32_t c = offset & 0x3FFu;
        Core &state = m_cores[static_cast<size_t>(core)];

        if (c < 0x180u)
        {
            const Voice &voice = state.voices[c / 0x10u];
            if ((c & 0xFu) == 0xAu)
                return static_cast<uint16_t>(voice.level);
            return m_raw[offset >> 1];
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
        if (offset >= 0x760u)
            return; // master volumes and SPDIF: kept in the backing store

        const int core = offset >= 0x400u ? 1 : 0;
        const uint32_t c = offset & 0x3FFu;
        Core &state = m_cores[static_cast<size_t>(core)];

        if (c < 0x180u)
        {
            Voice &voice = state.voices[c / 0x10u];
            switch (c & 0xFu)
            {
            case 0x0: voice.volumeLeft = value; break;
            case 0x2: voice.volumeRight = value; break;
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
            case 4: voice.loopAddress = withHigh(voice.loopAddress, value); break;
            case 6: voice.loopAddress = withLow(voice.loopAddress, value); break;
            case 8: voice.nextAddress = withHigh(voice.nextAddress, value); break;
            case 10: voice.nextAddress = withLow(voice.nextAddress, value); break;
            default: break;
            }
            return;
        }

        switch (c)
        {
        case RegAttr:
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
        voice.nextAddress = voice.startAddress;
        voice.loopAddress = voice.startAddress;
        voice.position = 0u;
        voice.needBlock = true;
        voice.blockFlags = 0u;
        voice.history1 = 0;
        voice.history2 = 0;
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
            sample = std::clamp(sample, -32768, 32767);
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
        decodeAdpcm(block, voice.history1, voice.history2, voice.samples.data());
        voice.blockFlags = static_cast<uint8_t>(block[0] >> 8u);
        if ((voice.blockFlags & 4u) != 0u)
            voice.loopAddress = address;
        if ((voice.blockFlags & 1u) != 0u)
            state.endx |= 1u << voiceIndex;
        testIrqRange(core, address, 8u);
        voice.nextAddress = (address + 8u) & RamMask;
    }

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
        int32_t amount = step * (1 << std::max(0, 11 - shift));
        if (exponential && increase && voice.level > 0x6000)
            cycles *= 4u;
        if (exponential && !increase)
            amount = (amount * voice.level) >> 15;
        if (++voice.counter < cycles)
            return;
        voice.counter = 0u;
        voice.level = std::clamp(voice.level + amount, 0, 0x7FFF);

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
        int32_t outLeft = 0;
        int32_t outRight = 0;
        for (int core = 0; core < CoreCount; ++core)
        {
            Core &state = m_cores[static_cast<size_t>(core)];
            const uint32_t mixLeft = maskOf(core, RegVmixl);
            const uint32_t mixRight = maskOf(core, RegVmixr);
            int32_t accLeft = 0;
            int32_t accRight = 0;
            int32_t voice1Output = 0;
            int32_t voice3Output = 0;
            for (int index = 0; index < VoiceCount; ++index)
            {
                Voice &voice = state.voices[static_cast<size_t>(index)];
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

                const uint32_t sampleIndex = std::min<uint32_t>(voice.position >> 12u, 27u);
                const int32_t current = voice.samples[sampleIndex];
                const int32_t next = sampleIndex + 1u < 28u ? voice.samples[sampleIndex + 1u] : current;
                const int32_t fraction = static_cast<int32_t>(voice.position & 0xFFFu);
                int32_t sample = current + (((next - current) * fraction) >> 12);

                stepEnvelope(voice);
                if (voice.phase == Phase::Off)
                {
                    stopVoice(core, index);
                    continue;
                }
                sample = (sample * voice.level) >> 15;
                if (index == 1)
                    voice1Output = (sample * volumeOf(voice.volumeLeft)) >> 14;
                else if (index == 3)
                    voice3Output = (sample * volumeOf(voice.volumeLeft)) >> 14;
                if ((mixLeft & (1u << index)) != 0u)
                    accLeft += (sample * volumeOf(voice.volumeLeft)) >> 14;
                if ((mixRight & (1u << index)) != 0u)
                    accRight += (sample * volumeOf(voice.volumeRight)) >> 14;

                voice.position += std::min<uint32_t>(voice.pitch, 0x3FFFu);
                while ((voice.position >> 12u) >= 28u)
                {
                    voice.position -= 28u << 12u;
                    fetchBlock(core, index);
                    if (!voice.active)
                        break;
                }
            }
            if ((state.attr & AttrEnable) != 0u)
            {
                const uint32_t base = (0x760u + static_cast<uint32_t>(core) * 0x28u) >> 1;
                const int32_t coreLeft = (accLeft * volumeOf(m_raw[base])) >> 14;
                const int32_t coreRight = (accRight * volumeOf(m_raw[base + 1u])) >> 14;
                outLeft += coreLeft;
                outRight += coreRight;

                // The mixer streams its results into sound RAM every sample (the memory
                // output buffers, 0x200 words each). Software uses them as a clock by
                // putting IRQA inside one of them. All offsets are in 16-bit words.
                const uint32_t position = state.outPosition;
                const uint32_t voiceBase = core == 0 ? 0x400u : 0xC00u;
                const uint32_t mixBase = core == 0 ? 0x1000u : 0x1800u;
                memoryOutWrite(voiceBase + position, static_cast<int16_t>(std::clamp(voice1Output, -32768, 32767)));
                memoryOutWrite(voiceBase + 0x200u + position, static_cast<int16_t>(std::clamp(voice3Output, -32768, 32767)));
                memoryOutWrite(mixBase + position, static_cast<int16_t>(std::clamp(coreLeft, -32768, 32767)));
                memoryOutWrite(mixBase + 0x200u + position, static_cast<int16_t>(std::clamp(coreRight, -32768, 32767)));
                memoryOutWrite(mixBase + 0x400u + position, 0);
                memoryOutWrite(mixBase + 0x600u + position, 0);
                state.outPosition = (position + 1u) & 0x1FFu;
            }
        }

        const int16_t left = static_cast<int16_t>(std::clamp(outLeft, -32768, 32767));
        const int16_t right = static_cast<int16_t>(std::clamp(outRight, -32768, 32767));
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
