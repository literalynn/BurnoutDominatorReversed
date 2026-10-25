#include "iop_cpu.h"
#include "iop_memory.h"

#include <limits>

namespace ps2x::iop::detail
{
    IopCpuCore::IopCpuCore(IopMemory &memory) noexcept
        : m_memory(memory)
    {
    }

    void IopCpuCore::raiseException(IopCpuState &cpu, uint32_t code, uint32_t faultPc, bool delaySlot, std::optional<uint32_t> badAddress) const
    {
        uint32_t cause = cpu.cop0[13] & ~0x7Cu;
        cause |= (code & 0x1Fu) << 2u;
        if (delaySlot)
        {
            cause |= 0x80000000u;
            cpu.cop0[14] = faultPc - 4u;
        }
        else
        {
            cause &= ~0x80000000u;
            cpu.cop0[14] = faultPc;
        }
        cpu.cop0[13] = cause;
        if (badAddress)
            cpu.cop0[8] = *badAddress;
        const uint32_t status = cpu.cop0[12];
        cpu.cop0[12] = (status & ~0x3Fu) | ((status << 2u) & 0x3Fu);
        cpu.pc = (status & (1u << 22u)) ? 0xBFC00180u : 0x80000080u;
        cpu.branchPending = false;
        cpu.pendingLoad = false;
        cpu.exception = true;
    }
}
