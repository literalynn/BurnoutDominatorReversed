#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace ps2x::iop::detail
{
    // SPU2 sound processor: 2 MiB of sound RAM, two cores of 24 ADPCM voices, the register
    // file at 0x1F900000 and the transfer paths (TSA + PIO data port, DMA). Time is driven by the
    // IOP cycle count: one output sample every CyclesPerSample cycles (48 kHz).
    //
    // Modelled: ADPCM decode, ADSR envelopes, pitch, key on/off, loop flags and ENDX, the IRQ
    // raised when a voice fetches (or a transfer touches) the address in IRQA, STATX/ATTR/SPDIF
    // IRQ status, volumes and the dry mix to a stereo output ring. Not modelled yet: reverb,
    // noise, pitch modulation, ADMA streaming and volume sweeps.
    class Spu2
    {
    public:
        static constexpr uint32_t RegBase = 0x1F900000u;
        static constexpr uint32_t RegEnd = 0x1F900800u;
        static constexpr uint32_t RamWords = 0x100000u; // 16-bit words
        static constexpr uint32_t RamMask = RamWords - 1u;
        static constexpr uint64_t CyclesPerSample = 768u; // 36.864 MHz IOP clock / 48 kHz
        static constexpr int CoreCount = 2;
        static constexpr int VoiceCount = 24;
        static constexpr uint32_t SampleRate = 48000u;

        // Register offsets inside a core block (core 1 is at +0x400).
        static constexpr uint32_t RegPmon = 0x180u;
        static constexpr uint32_t RegNon = 0x184u;
        static constexpr uint32_t RegVmixl = 0x188u;
        static constexpr uint32_t RegVmixel = 0x18Cu;
        static constexpr uint32_t RegVmixr = 0x190u;
        static constexpr uint32_t RegVmixer = 0x194u;
        static constexpr uint32_t RegMmix = 0x198u;
        static constexpr uint32_t RegAttr = 0x19Au;
        static constexpr uint32_t RegIrqa = 0x19Cu; // high word, low word at +2
        static constexpr uint32_t RegKon = 0x1A0u;
        static constexpr uint32_t RegKoff = 0x1A4u;
        static constexpr uint32_t RegTsa = 0x1A8u;
        static constexpr uint32_t RegData = 0x1ACu;
        static constexpr uint32_t RegAdmas = 0x1B0u;
        static constexpr uint32_t RegVoiceAddress = 0x1C0u; // 12 bytes per voice
        static constexpr uint32_t RegEndx = 0x340u;
        static constexpr uint32_t RegStatx = 0x344u;
        static constexpr uint32_t RegSpdifIrqInfo = 0x7C2u; // absolute offset

        static constexpr uint16_t AttrEnable = 0x8000u;
        static constexpr uint16_t AttrIrqEnable = 0x0040u;
        static constexpr uint16_t AttrTransferMask = 0x0030u; // 1 = PIO write, 2 = DMA write, 3 = DMA read
        // STATX: 0 when idle, StatxBusy while a transfer runs, StatxDone once it finished and
        // until the transfer mode is cleared in ATTR (this is the protocol libsd polls).
        static constexpr uint16_t StatxDone = 0x0080u;
        static constexpr uint16_t StatxBusy = 0x0400u;

        Spu2();

        void reset();

        [[nodiscard]] static constexpr bool owns(uint32_t physical) noexcept
        {
            return physical >= RegBase && physical < RegEnd;
        }

        [[nodiscard]] uint16_t read16(uint32_t physical);
        void write16(uint32_t physical, uint16_t value);

        // IOP RAM <-> SPU RAM at the core's TSA (which advances past the transfer).
        void dmaWrite(int core, const uint8_t *source, uint32_t bytes);
        void dmaRead(int core, uint8_t *destination, uint32_t bytes);
        void dmaStarted(int core);
        void dmaCompleted(int core);

        // Run emulated time up to the given IOP cycle.
        void advanceTo(uint64_t cycle);
        // First cycle at which advanceTo() has something to do (UINT64_MAX when no voice plays).
        [[nodiscard]] uint64_t dueCycle() const noexcept
        {
            return (m_activeVoices != 0u || m_enabledCores != 0u) ? m_nextSampleCycle : kNever;
        }

        // Called when a core asserts its IRQ line (once per assertion); the owner raises IOP IRQ 9.
        std::function<void()> raiseIrq;

        // Interleaved stereo int16 at SampleRate; returns the number of frames written. The
        // ring drops its oldest frames when the consumer is slower than the emulation.
        size_t readAudio(int16_t *destination, size_t frames);
        [[nodiscard]] uint64_t producedFrames() const noexcept { return m_producedFrames; }

        // Introspection for tests and diagnostics.
        [[nodiscard]] uint16_t soundRam(uint32_t wordAddress) const noexcept { return m_ram[wordAddress & RamMask]; }
        void setSoundRam(uint32_t wordAddress, uint16_t value) noexcept { m_ram[wordAddress & RamMask] = value; }
        [[nodiscard]] uint32_t activeVoices() const noexcept { return m_activeVoices; }
        [[nodiscard]] uint32_t voiceNextAddress(int core, int voice) const noexcept;

        // Decode one 16-byte ADPCM block (8 words) into 28 samples; history is updated.
        static void decodeAdpcm(const uint16_t *block, int32_t &history1, int32_t &history2, int16_t *samples) noexcept;

        static constexpr uint64_t kNever = ~uint64_t{0};

    private:
        enum class Phase : uint8_t
        {
            Off,
            Attack,
            Decay,
            Sustain,
            Release,
        };

        struct Voice
        {
            uint16_t volumeLeft = 0;
            uint16_t volumeRight = 0;
            uint16_t pitch = 0;
            uint16_t adsr1 = 0;
            uint16_t adsr2 = 0;
            uint32_t startAddress = 0;
            uint32_t loopAddress = 0;
            uint32_t nextAddress = 0;

            bool active = false;
            bool needBlock = false;
            Phase phase = Phase::Off;
            int32_t level = 0;
            uint32_t counter = 0;
            uint32_t position = 0; // 12 fractional bits
            uint8_t blockFlags = 0;
            int32_t history1 = 0;
            int32_t history2 = 0;
            std::array<int16_t, 28> samples{};
        };

        struct Core
        {
            uint16_t attr = 0;
            uint32_t irqAddress = 0;
            uint32_t transferAddress = 0;
            uint32_t endx = 0;
            bool dmaBusy = false;
            bool dmaDone = false; // transfer finished while the ATTR transfer mode is still set
            uint32_t outPosition = 0; // write position inside the memory output buffers (0..0x1FF)
            std::array<Voice, VoiceCount> voices;
        };

        void tickSample();
        void keyOn(int core, int voice);
        void keyOff(int core, int voice);
        void stopVoice(int core, int voice);
        void fetchBlock(int core, int voice);
        void stepEnvelope(Voice &voice) const;
        void testIrqRange(int core, uint32_t firstWord, uint32_t words);
        void testIrqAllCores(uint32_t wordAddress);
        void memoryOutWrite(uint32_t wordAddress, int16_t value);
        void assertIrq(int core);
        void refreshActiveCount() noexcept;
        [[nodiscard]] uint32_t maskOf(int core, uint32_t offset) const noexcept;

        std::vector<uint16_t> m_ram;
        std::array<uint16_t, 0x400> m_raw{}; // register backing store, indexed by byte offset / 2
        std::array<Core, CoreCount> m_cores;
        uint16_t m_spdifIrqInfo = 0;
        uint32_t m_activeVoices = 0;
        uint32_t m_enabledCores = 0; // bit per core whose ATTR enable bit is set
        uint64_t m_nextSampleCycle = 0;

        mutable std::mutex m_audioMutex;
        std::vector<int16_t> m_audio; // interleaved ring
        size_t m_audioHead = 0;       // next frame to read
        size_t m_audioCount = 0;      // frames stored
        uint64_t m_producedFrames = 0;
    };
}
