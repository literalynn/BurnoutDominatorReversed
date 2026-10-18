#include "emulator/core/iop_memory.h"
#include "emulator/core/iop_spu2.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    using namespace ps2x::iop::detail;

    void require(bool value, const char *message)
    {
        if (!value)
            throw std::runtime_error(message);
    }

    constexpr uint32_t Base = Spu2::RegBase;
    constexpr uint32_t Core1 = 0x400u;

    void write(Spu2 &spu, uint32_t core, uint32_t offset, uint16_t value)
    {
        spu.write16(Base + core * Core1 + offset, value);
    }

    uint16_t read(Spu2 &spu, uint32_t core, uint32_t offset)
    {
        return spu.read16(Base + core * Core1 + offset);
    }

    void setAddress(Spu2 &spu, uint32_t core, uint32_t offset, uint32_t words)
    {
        write(spu, core, offset, static_cast<uint16_t>((words >> 16u) & 0xFu));
        write(spu, core, offset + 2u, static_cast<uint16_t>(words & 0xFFFFu));
    }

    // One ADPCM block of 8 words: header (shift, predictor), flags, 28 nibbles.
    void putBlock(Spu2 &spu, uint32_t wordAddress, uint8_t header, uint8_t flags, uint8_t nibble)
    {
        spu.setSoundRam(wordAddress, static_cast<uint16_t>(header | (static_cast<uint16_t>(flags) << 8u)));
        const uint8_t pair = static_cast<uint8_t>(nibble | (nibble << 4u));
        for (uint32_t i = 1u; i < 8u; ++i)
            spu.setSoundRam(wordAddress + i, static_cast<uint16_t>(pair | (pair << 8u)));
    }

    // Instant attack, no decay, full sustain, fast release.
    void setFastEnvelope(Spu2 &spu, uint32_t core, uint32_t voice)
    {
        const uint32_t base = voice * 0x10u;
        write(spu, core, base + 0x6u, 0x000Fu); // attack shift 0, decay 0, sustain level F
        write(spu, core, base + 0x8u, 0x0000u); // linear sustain, release shift 0 (two samples to silence)
    }

    void testAdpcmDecode()
    {
        // Shift 12: the sample is the signed nibble itself.
        std::array<uint16_t, 8> block{};
        block[0] = 0x000C; // shift 12, predictor 0
        for (int i = 0; i < 14; ++i)
        {
            const uint8_t low = static_cast<uint8_t>((2 * i) & 0xF);
            const uint8_t high = static_cast<uint8_t>((2 * i + 1) & 0xF);
            const uint8_t byte = static_cast<uint8_t>(low | (high << 4));
            const uint32_t index = 2u + static_cast<uint32_t>(i);
            if ((index & 1u) == 0u)
                block[index / 2u] = static_cast<uint16_t>((block[index / 2u] & 0xFF00u) | byte);
            else
                block[index / 2u] = static_cast<uint16_t>((block[index / 2u] & 0x00FFu) | (byte << 8u));
        }
        int32_t h1 = 0;
        int32_t h2 = 0;
        std::array<int16_t, 28> out{};
        Spu2::decodeAdpcm(block.data(), h1, h2, out.data());
        require(out[0] == 0 && out[1] == 1 && out[7] == 7, "positive nibbles decode to themselves at shift 12");
        require(out[8] == -8 && out[9] == -7 && out[15] == -1, "nibbles >= 8 are negative");
        require(out[16] == 0 && out[27] == 11 - 16, "sequence wraps with the 4-bit nibbles");
        require(h1 == out[27] && h2 == out[26], "history keeps the last two samples");

        // Predictor 1 (60/64) with history.
        std::array<uint16_t, 8> predicted{};
        predicted[0] = 0x001C; // shift 12, predictor 1
        predicted[1] = 0x0011; // samples 0 and 1 are nibble 1
        int32_t p1 = 64;
        int32_t p2 = 0;
        std::array<int16_t, 28> decoded{};
        Spu2::decodeAdpcm(predicted.data(), p1, p2, decoded.data());
        require(decoded[0] == 1 + ((64 * 60 + 32) >> 6), "predictor 1 adds 60/64 of the previous sample");
    }

    void testRegistersAndTransfers()
    {
        Spu2 spu;
        setAddress(spu, 0, Spu2::RegVoiceAddress + 12u * 3u, 0x12345u);       // voice 3 SSA
        setAddress(spu, 0, Spu2::RegVoiceAddress + 12u * 3u + 4u, 0x22222u);  // voice 3 LSAX
        require(read(spu, 0, Spu2::RegVoiceAddress + 12u * 3u) == 0x1, "SSA high nibble");
        require(read(spu, 0, Spu2::RegVoiceAddress + 12u * 3u + 2u) == 0x2345, "SSA low word");
        require(read(spu, 0, Spu2::RegVoiceAddress + 12u * 3u + 4u) == 0x2, "LSAX high nibble");
        require(read(spu, 1, Spu2::RegVoiceAddress + 12u * 3u + 2u) == 0, "cores have separate voice registers");

        // PIO through the data port.
        setAddress(spu, 0, Spu2::RegTsa, 0x4000u);
        write(spu, 0, Spu2::RegData, 0xABCD);
        write(spu, 0, Spu2::RegData, 0x1234);
        require(spu.soundRam(0x4000) == 0xABCD && spu.soundRam(0x4001) == 0x1234, "PIO writes go to TSA and advance it");
        require(read(spu, 0, Spu2::RegTsa + 2u) == 0x4002, "TSA reads back its advanced value");
        setAddress(spu, 0, Spu2::RegTsa, 0x4000u);
        require(read(spu, 0, Spu2::RegData) == 0xABCD && read(spu, 0, Spu2::RegData) == 0x1234, "PIO reads");

        // DMA through IOP memory: the real CHCR path with block control.
        IopMemory memory;
        std::vector<uint8_t> payload(256);
        for (size_t i = 0; i < payload.size(); ++i)
            payload[i] = static_cast<uint8_t>(i * 3u + 1u);
        require(memory.writeRam(0x00130000u, payload.data(), payload.size()), "stage the DMA source");
        memory.write16(Spu2::RegBase + Spu2::RegTsa, 0x0000);
        memory.write16(Spu2::RegBase + Spu2::RegTsa + 2u, 0x8000);
        memory.write32(0x1F8010C0u, 0x00130000u);     // MADR
        memory.write32(0x1F8010C4u, (4u << 16u) | 16u); // 4 blocks of 16 words
        memory.write16(Spu2::RegBase + Spu2::RegAttr, Spu2::AttrEnable | 0x0020u); // DMA write mode
        require(memory.read16(Spu2::RegBase + Spu2::RegStatx) == 0u, "STATX is zero when idle");
        memory.write32(0x1F8010C8u, 0x01000201u);      // start, RAM -> SPU
        require(memory.read16(Spu2::RegBase + Spu2::RegStatx) == Spu2::StatxBusy, "STATX shows busy during the DMA");
        for (uint32_t i = 0; i < 128u; ++i)
        {
            const uint16_t expected = static_cast<uint16_t>(payload[i * 2u] | (payload[i * 2u + 1u] << 8u));
            require(memory.spu().soundRam(0x8000u + i) == expected, "DMA data lands in sound RAM at TSA");
        }
        require(memory.takeDmaStart().has_value(), "the DMA completion interrupt is scheduled");
        memory.spu().dmaCompleted(0);
        require(memory.read16(Spu2::RegBase + Spu2::RegStatx) == Spu2::StatxDone, "STATX shows done after the DMA");
        memory.write16(Spu2::RegBase + Spu2::RegAttr, Spu2::AttrEnable); // transfer mode off
        require(memory.read16(Spu2::RegBase + Spu2::RegStatx) == 0u, "clearing the transfer mode clears STATX");

        // DMA read back into IOP RAM.
        memory.write16(Spu2::RegBase + Spu2::RegTsa + 2u, 0x8000);
        memory.write32(0x1F8010C0u, 0x00140000u);
        memory.write32(0x1F8010C4u, (1u << 16u) | 16u);
        memory.write32(0x1F8010C8u, 0x01000200u); // start, SPU -> RAM
        std::array<uint8_t, 64> readBack{};
        require(memory.readRam(0x00140000u, readBack.data(), readBack.size()), "read the DMA destination");
        require(std::memcmp(readBack.data(), payload.data(), 64) == 0, "SPU -> RAM DMA returns the same bytes");
    }

    void testVoiceLoopAndIrq()
    {
        Spu2 spu;
        int irqs = 0;
        spu.raiseIrq = [&irqs]
        { ++irqs; };
        constexpr uint32_t Start = 0x4000u;
        putBlock(spu, Start + 0u, 0x0C, 0x04, 0x1); // loop start
        putBlock(spu, Start + 8u, 0x0C, 0x00, 0x1);
        putBlock(spu, Start + 16u, 0x0C, 0x00, 0x1);
        putBlock(spu, Start + 24u, 0x0C, 0x03, 0x1); // loop end + repeat
        write(spu, 0, Spu2::RegAttr, Spu2::AttrEnable | Spu2::AttrIrqEnable);
        setAddress(spu, 0, Spu2::RegIrqa, Start + 16u);
        setAddress(spu, 0, Spu2::RegVoiceAddress, Start);
        write(spu, 0, 0x04, 0x1000);
        setFastEnvelope(spu, 0, 0);
        write(spu, 0, Spu2::RegKon, 0x0001);
        require(spu.activeVoices() == 1u, "KON starts the voice");

        spu.advanceTo(40u * Spu2::CyclesPerSample);
        require(irqs == 0, "no IRQ before block 2 is fetched (it is reached after 56 samples)");
        spu.advanceTo(70u * Spu2::CyclesPerSample);
        require(irqs == 1, "IRQ when the voice fetches the block holding IRQA");
        spu.advanceTo(175u * Spu2::CyclesPerSample);
        require(irqs == 2, "the loop repeats and raises the IRQ again 112 samples later");
        require(spu.activeVoices() == 1u, "a looping voice keeps playing");
        require((read(spu, 0, Spu2::RegEndx) & 1u) != 0u, "ENDX records the loop end");
        require((spu.read16(Base + Spu2::RegSpdifIrqInfo) & 4u) != 0u, "SPDIF IRQ info flags core 0");
        write(spu, 0, Spu2::RegAttr, Spu2::AttrEnable); // disabling the IRQ acknowledges it
        require((spu.read16(Base + Spu2::RegSpdifIrqInfo) & 4u) == 0u, "disabling IRQ clears the flag");
    }

    void testIrqOnTransfer()
    {
        Spu2 spu;
        int irqs = 0;
        spu.raiseIrq = [&irqs]
        { ++irqs; };
        write(spu, 1, Spu2::RegAttr, Spu2::AttrEnable | Spu2::AttrIrqEnable);
        setAddress(spu, 1, Spu2::RegIrqa, 0x2010u);
        setAddress(spu, 1, Spu2::RegTsa, 0x2000u);
        std::array<uint8_t, 16> small{};
        spu.dmaWrite(1, small.data(), static_cast<uint32_t>(small.size())); // words 0x2000..0x2007
        require(irqs == 0, "a transfer that does not reach IRQA is silent");
        std::array<uint8_t, 64> large{};
        spu.dmaWrite(1, large.data(), static_cast<uint32_t>(large.size())); // words 0x2008..0x2027
        require(irqs == 1, "a transfer covering IRQA raises the IRQ");
    }

    void testMixerOutputClock()
    {
        // With the core enabled and no voice playing, the mixer still writes its output buffers
        // every sample: an IRQA inside one of them is a periodic clock (the RWA audio engine's).
        Spu2 spu;
        int irqs = 0;
        spu.raiseIrq = [&irqs]
        { ++irqs; };
        write(spu, 0, Spu2::RegAttr, Spu2::AttrEnable | Spu2::AttrIrqEnable);
        setAddress(spu, 0, Spu2::RegIrqa, 0x500u); // word 0x100 of the core 0 voice 1 output buffer
        require(spu.dueCycle() != Spu2::kNever, "an enabled core keeps the SPU2 clock running");
        spu.advanceTo(200u * Spu2::CyclesPerSample);
        require(irqs == 0, "no IRQ before the output position reaches IRQA");
        spu.advanceTo(260u * Spu2::CyclesPerSample);
        require(irqs == 1, "IRQ when the mixer writes the IRQA address");
        write(spu, 0, Spu2::RegAttr, Spu2::AttrEnable | Spu2::AttrIrqEnable); // keeps IRQ enabled
        spu.advanceTo(770u * Spu2::CyclesPerSample);
        require(irqs == 2, "the output buffer wraps every 512 samples");
        write(spu, 0, Spu2::RegAttr, 0u);
        require(spu.dueCycle() == Spu2::kNever, "a disabled core with no voice needs no clock");
    }

    void testVoiceStopsAtLoopEnd()
    {
        Spu2 spu;
        constexpr uint32_t Start = 0x800u;
        putBlock(spu, Start, 0x0C, 0x01, 0x2); // loop end, no repeat
        write(spu, 1, Spu2::RegAttr, Spu2::AttrEnable);
        setAddress(spu, 1, Spu2::RegVoiceAddress + 12u * 5u, Start);
        write(spu, 1, 5u * 0x10u + 0x4u, 0x1000);
        setFastEnvelope(spu, 1, 5);
        write(spu, 1, Spu2::RegKon, 0x0020);
        require(spu.activeVoices() == 1u, "voice 5 of core 1 is playing");
        spu.advanceTo(60u * Spu2::CyclesPerSample);
        require(spu.activeVoices() == 0u, "a voice reaching a loop end without repeat stops");
        require((read(spu, 1, Spu2::RegEndx) & 0x20u) != 0u, "ENDX bit 5 is set");
        require(read(spu, 1, 5u * 0x10u + 0xAu) == 0, "ENVX is zero once stopped");
    }

    // Core 0 voices to its dry mix, core 0 into core 1, core 1 to the output, all at full scale.
    void routeCore0ToOutput(Spu2 &spu)
    {
        write(spu, 0, Spu2::RegAttr, Spu2::AttrEnable);
        write(spu, 1, Spu2::RegAttr, Spu2::AttrEnable);
        write(spu, 0, Spu2::RegMmix, 0x0C00);       // voices to dry left and right
        write(spu, 1, Spu2::RegMmix, 0x000C);       // external input (core 0) to dry
        spu.write16(Base + 0x760u, 0x3FFF);         // core 0 master volume
        spu.write16(Base + 0x762u, 0x3FFF);
        spu.write16(Base + 0x788u, 0x3FFF);         // core 1 master volume
        spu.write16(Base + 0x78Au, 0x3FFF);
        spu.write16(Base + 0x794u, 0x7FFF);         // core 1 external input volume
        spu.write16(Base + 0x796u, 0x7FFF);
    }

    void testAudioOutputAndRelease()
    {
        Spu2 spu;
        constexpr uint32_t Start = 0x4000u;
        putBlock(spu, Start, 0x08, 0x04, 0x4);
        putBlock(spu, Start + 8u, 0x08, 0x03, 0x4); // loops forever
        routeCore0ToOutput(spu);
        write(spu, 0, Spu2::RegVmixl, 0x0001);
        write(spu, 0, Spu2::RegVmixr, 0x0001);
        setAddress(spu, 0, Spu2::RegVoiceAddress, Start);
        write(spu, 0, 0x00, 0x3FFF);
        write(spu, 0, 0x02, 0x1FFF);
        write(spu, 0, 0x04, 0x1000);
        setFastEnvelope(spu, 0, 0);
        write(spu, 0, Spu2::RegKon, 0x0001);
        spu.advanceTo(200u * Spu2::CyclesPerSample);

        std::vector<int16_t> frames(2u * 400u);
        const size_t count = spu.readAudio(frames.data(), 400u);
        require(count == 201u, "one frame at cycle 0 and one per elapsed sample period");
        require(frames[2u * 150u] > 0, "left channel carries the voice");
        require(frames[2u * 150u + 1u] > 0 && frames[2u * 150u + 1u] < frames[2u * 150u], "right channel is quieter (half volume)");

        write(spu, 0, Spu2::RegKoff, 0x0001);
        spu.advanceTo(600u * Spu2::CyclesPerSample);
        require(spu.activeVoices() == 0u, "KOFF releases the voice to silence and stops it");
    }

    // A one-shot sample whose loop address the driver points at a silent block: at the
    // sample's end the voice jumps there instead of repeating the sample.
    void testWrittenLoopAddressHolds()
    {
        Spu2 spu;
        constexpr uint32_t Start = 0x4000u;
        constexpr uint32_t Silence = 0x2800u;
        putBlock(spu, Start, 0x08, 0x04, 0x4);      // loop start flag in the data
        putBlock(spu, Start + 8u, 0x08, 0x03, 0x4); // loop end + repeat
        putBlock(spu, Silence, 0x0C, 0x07, 0x0);     // silent block looping on itself
        routeCore0ToOutput(spu);
        write(spu, 0, Spu2::RegVmixl, 0x0001);
        write(spu, 0, Spu2::RegVmixr, 0x0001);
        setAddress(spu, 0, Spu2::RegVoiceAddress, Start);
        setAddress(spu, 0, Spu2::RegVoiceAddress + 4u, Silence); // written before the key on
        write(spu, 0, 0x00, 0x3FFF);
        write(spu, 0, 0x02, 0x3FFF);
        write(spu, 0, 0x04, 0x1000);
        setFastEnvelope(spu, 0, 0);
        write(spu, 0, Spu2::RegKon, 0x0001);
        require(read(spu, 0, Spu2::RegVoiceAddress + 6u) == Silence, "key on keeps the written loop address");
        // KON clears the written state: the loop-start flag of the first block takes over.
        spu.advanceTo(2u * Spu2::CyclesPerSample);
        require(read(spu, 0, Spu2::RegVoiceAddress + 6u) == Start, "a loop-start flag after key on sets the loop address");

        // The same with the loop address written after the key on: it holds against the flag.
        write(spu, 0, Spu2::RegKon, 0x0001);
        setAddress(spu, 0, Spu2::RegVoiceAddress + 4u, Silence);
        spu.advanceTo(200u * Spu2::CyclesPerSample);
        std::vector<int16_t> frames(2u * 400u);
        spu.readAudio(frames.data(), 400u);
        require(spu.voiceNextAddress(0, 0) == Silence + 8u, "after the sample the voice loops on the silent block");
        spu.advanceTo(400u * Spu2::CyclesPerSample);
        const size_t count = spu.readAudio(frames.data(), 400u);
        bool silent = count > 0u;
        for (size_t i = 0; i < count * 2u; ++i)
            silent = silent && frames[i] == 0;
        require(silent, "the silent loop plays silence");
    }

    void testVolumeSweeps()
    {
        Spu2::Volume up;
        up.level = 0;
        up.set(0x8000u | (8u << 2u) | 3u); // linear increase, shift 8, step +4: +32 per sample
        for (int i = 0; i < 10; ++i)
            up.step();
        require(up.level == 320, "a linear increase adds its step every sample");
        for (int i = 0; i < 2000; ++i)
            up.step();
        require(up.level == 0x7FFF, "an increase saturates at full scale");

        Spu2::Volume down;
        down.set(0x3FFF);
        require(down.level == 0x7FFE, "a fixed volume is twice the 15-bit value");
        down.set(0x8000u | 0x2000u | (8u << 2u) | 0u); // linear decrease, shift 8, step -8: -64
        down.step();
        require(down.level == 0x7FFE - 64, "a linear decrease starts from the current level");
        for (int i = 0; i < 1000; ++i)
            down.step();
        require(down.level == 0, "a decrease stops at silence");

        Spu2::Volume slow;
        slow.level = 0;
        slow.set(0x8000u | (13u << 2u) | 0u); // shift 13: one step of +7 every 4 samples
        for (int i = 0; i < 8; ++i)
            slow.step();
        require(slow.level == 14, "shifts above 11 step every 2^(shift - 11) samples");

        // A voice playing at a sweeping volume reads back its current level (VOLXL).
        Spu2 spu;
        write(spu, 0, 0x00, 0x8000u | (8u << 2u) | 3u);
        spu.write16(Base + Spu2::RegAttr, Spu2::AttrEnable);
        spu.advanceTo(10u * Spu2::CyclesPerSample);
        require(read(spu, 0, 0x0C) == 11u * 32u, "VOLXL reads the current level (one step per sample, from cycle 0)");
    }

    void testCoreRouting()
    {
        Spu2 spu;
        constexpr uint32_t Start = 0x4000u;
        putBlock(spu, Start, 0x08, 0x04, 0x4);
        putBlock(spu, Start + 8u, 0x08, 0x03, 0x4);
        routeCore0ToOutput(spu);
        write(spu, 0, Spu2::RegVmixl, 0x0001);
        write(spu, 0, Spu2::RegVmixr, 0x0001);
        setAddress(spu, 0, Spu2::RegVoiceAddress, Start);
        write(spu, 0, 0x00, 0x3FFF);
        write(spu, 0, 0x02, 0x3FFF);
        write(spu, 0, 0x04, 0x1000);
        setFastEnvelope(spu, 0, 0);
        write(spu, 0, Spu2::RegKon, 0x0001);
        spu.advanceTo(100u * Spu2::CyclesPerSample);
        std::vector<int16_t> frames(2u * 200u);
        size_t count = spu.readAudio(frames.data(), 200u);
        require(count > 60u && frames[2u * 60u] > 0, "core 0 is heard through core 1");
        require(static_cast<int16_t>(spu.soundRam(0x800u + 60u)) > 0, "core 0's output is written at 0x800");

        write(spu, 1, Spu2::RegMmix, 0x0000); // core 1 no longer takes its external input
        spu.advanceTo(200u * Spu2::CyclesPerSample);
        count = spu.readAudio(frames.data(), 200u);
        require(count > 50u && frames[2u * 50u] == 0 && frames[2u * 50u + 1u] == 0, "MMIX gates the external input");
    }
}

int main()
{
    try
    {
        testAdpcmDecode();
        testRegistersAndTransfers();
        testVoiceLoopAndIrq();
        testIrqOnTransfer();
        testMixerOutputClock();
        testVoiceStopsAtLoopEnd();
        testAudioOutputAndRelease();
        testWrittenLoopAddressHolds();
        testVolumeSweeps();
        testCoreRouting();
        std::cout << "spu2 tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "spu2 test failed: " << error.what() << '\n';
        return 1;
    }
}
