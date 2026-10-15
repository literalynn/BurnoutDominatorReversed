#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ps2x
{
    class StateArchive;
}

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopGuestExecutor;
    class IopMemory;

    class IopIoman
    {
    public:
        explicit IopIoman(IopMemory &memory) noexcept;

        void reset();
        void serialize(ps2x::StateArchive &archive);
        [[nodiscard]] bool dispatchImport(uint16_t ordinal, IopCpuState &cpu, IopGuestExecutor &executor);

    private:
        struct Device
        {
            uint32_t address = 0u;
            uint32_t gp = 0u;
            std::string name;
        };

        IopMemory &m_memory;
        std::vector<Device> m_devices;
    };
}
