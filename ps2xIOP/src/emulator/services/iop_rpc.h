#pragma once

#include "ps2x/iop/iop_types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <unordered_map>

namespace ps2x::iop
{
    class IopHost;
}

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopKernel;
    class IopMemory;

    class IopGuestExecutor
    {
    public:
        virtual ~IopGuestExecutor() = default;

        [[nodiscard]] virtual uint32_t executeGuestFunction(uint32_t address,
                                                            uint32_t a0,
                                                            uint32_t a1,
                                                            uint32_t a2,
                                                            uint32_t a3,
                                                            uint32_t gp) = 0;
        [[nodiscard]] virtual uint32_t executeGuestFunctionWithBudget(uint32_t address,
                                                                      uint32_t a0,
                                                                      uint32_t a1,
                                                                      uint32_t a2,
                                                                      uint32_t a3,
                                                                      uint32_t gp,
                                                                      uint32_t instructionBudget)
        {
            return executeGuestFunction(address, a0, a1, a2, a3, gp);
        }
    };

    class IopRpcBridge
    {
    public:
        // sceSifSetDmaIntr completion handler: function(argument), run once the
        // transfer to the EE is done.
        struct DmaCallback
        {
            uint32_t function = 0;
            uint32_t argument = 0;
            uint32_t gp = 0;
        };

        IopRpcBridge(IopHost &host, IopMemory &memory, IopKernel &kernel) noexcept;

        void reset();
        [[nodiscard]] bool dispatchSifManImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] std::optional<DmaCallback> takeDmaCallback() noexcept;
        [[nodiscard]] bool dispatchSifCmdImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] RpcResult handleRpc(const RpcRequest &request, IopGuestExecutor &executor);
        // Runs the IOP handler registered for an EE-to-IOP SIF command, as the
        // IOP sifcmd interrupt handler does once the packet has arrived.
        [[nodiscard]] bool deliverSifCommand(const void *packet, size_t packetSize, IopGuestExecutor &executor);
        void onSifTransfer(const SifTransfer &transfer);
        void removeServersInRange(uint32_t base, uint32_t size);

        [[nodiscard]] bool hasServer(uint32_t sid) const noexcept;
        [[nodiscard]] size_t serverCount() const noexcept { return m_servers.size(); }

    private:
        struct RpcServer
        {
            uint32_t sid = 0;
            uint32_t serverData = 0;
            uint32_t function = 0;
            uint32_t gp = 0;
            uint32_t buffer = 0;
            uint32_t callback = 0;
            uint32_t callbackBuffer = 0;
            uint32_t queue = 0;
        };

        struct CmdHandler
        {
            uint32_t function = 0;
            uint32_t argument = 0;
        };

        [[nodiscard]] uint32_t setDma(uint32_t descriptorAddress, uint32_t descriptorCount);
        [[nodiscard]] bool findCmdHandler(uint32_t cid, CmdHandler &handler) const;
        [[nodiscard]] bool storeCmdHandler(uint32_t cid, const CmdHandler &handler);
        void reportCmdOnce(uint32_t cid, const char *what);

        IopHost &m_host;
        IopMemory &m_memory;
        IopKernel &m_kernel;
        std::unordered_map<uint32_t, RpcServer> m_servers;
        // sifcmd handler tables: user commands live in the guest table given to
        // sceSifSetCmdBuffer, system commands in sceSifSetSysCmdBuffer's table or
        // in sifcmd's own 32 entries (m_systemCmdHandlers).
        uint32_t m_userCmdTable = 0u;
        uint32_t m_userCmdCount = 0u;
        uint32_t m_systemCmdTable = 0u;
        uint32_t m_systemCmdCount = 0u;
        std::unordered_map<uint32_t, CmdHandler> m_systemCmdHandlers;
        std::unordered_map<uint32_t, uint32_t> m_cmdHandlerGp;
        std::set<uint32_t> m_reportedCmdIds;
        uint32_t m_cmdReceiveBuffer = 0u;
        std::optional<DmaCallback> m_pendingDmaCallback;
        uint32_t m_nextDmaId = 1u;
        bool m_sifInitialized = false;
    };
}
