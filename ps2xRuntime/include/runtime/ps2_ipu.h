#pragma once

#include <cstdint>
#include <functional>
#include <memory>

// Image Processing Unit of the EE: the MPEG-2 macroblock decoder and colour space
// converter driven by Sony's libmpeg and libipu. Registers at 0x10002000-0x1000203F,
// FIFO ports 0x10007000 (output) and 0x10007010 (input), DMA channel 3 (from the IPU)
// and channel 4 (to the IPU).
//
// Commands run synchronously as far as their input allows. A command that runs out of
// input stays busy and resumes from its start when the EE or DMA channel 4 supplies
// more data, which is when libmpeg's "no data" callback has refilled the stream.
class Ps2Ipu
{
public:
    struct Bus
    {
        // EE memory as the DMA sees it: bit 31 of the address selects the scratchpad.
        std::function<bool(uint32_t address, void *destination, uint32_t size)> readMemory;
        std::function<bool(uint32_t address, const void *source, uint32_t size)> writeMemory;
        // DMA channel register slot (CHCR, MADR, QWC, TADR, ASR0, ASR1).
        std::function<uint32_t &(uint32_t address)> registerRef;
        // Clears STR of the channel and raises its DMAC interrupt cause.
        std::function<void(uint32_t channelBase, uint32_t cause)> completeChannel;
    };

    static constexpr uint32_t kRegisterBase = 0x10002000u;
    static constexpr uint32_t kRegisterEnd = 0x10002040u;
    static constexpr uint32_t kOutFifo = 0x10007000u;
    static constexpr uint32_t kInFifo = 0x10007010u;
    static constexpr uint32_t kFromIpuChannel = 0x1000B000u; // DMA channel 3
    static constexpr uint32_t kToIpuChannel = 0x1000B400u;   // DMA channel 4

    explicit Ps2Ipu(Bus bus);
    ~Ps2Ipu();
    Ps2Ipu(const Ps2Ipu &) = delete;
    Ps2Ipu &operator=(const Ps2Ipu &) = delete;

    void reset();

    [[nodiscard]] uint32_t read32(uint32_t address);
    [[nodiscard]] uint64_t read64(uint32_t address);
    void write32(uint32_t address, uint32_t value);

    void readOutFifo(uint8_t qword[16]);
    void writeInFifo(const uint8_t qword[16]);

    // CHCR of DMA channel 3 or 4 was written with STR set.
    void onDmaStart(uint32_t channelBase);

    [[nodiscard]] bool busy() const;

private:
    struct State;
    std::unique_ptr<State> m_state;
};
