#include "iop_rpc.h"

#include "../core/iop_cpu.h"
#include "../core/iop_kernel.h"
#include "../core/iop_memory.h"
#include "ps2x/iop/iop_host.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <sstream>
#include <vector>

namespace ps2x::iop::detail
{
    IopRpcBridge::IopRpcBridge(IopHost &host, IopMemory &memory, IopKernel &kernel) noexcept
        : m_host(host), m_memory(memory), m_kernel(kernel)
    {
    }

    namespace
    {
        constexpr uint32_t kSystemCmdBit = 0x80000000u;
        constexpr uint32_t kBuiltinSystemCmdCount = 32u;
        constexpr uint32_t kCmdHandlerEntrySize = 8u;     // SifCmdHandlerData_t {handler, harg}
        constexpr uint32_t kSysCmdHandlerEntrySize = 12u; // SifCmdSysHandlerData_t {handler, harg, unknown08}
        constexpr uint32_t kCmdHeaderSize = 16u;
        constexpr uint32_t kCmdPacketMaxSize = 112u;
    }

    void IopRpcBridge::reset()
    {
        m_servers.clear();
        m_userCmdTable = 0u;
        m_userCmdCount = 0u;
        m_systemCmdTable = 0u;
        m_systemCmdCount = 0u;
        m_systemCmdHandlers.clear();
        m_cmdHandlerGp.clear();
        m_reportedCmdIds.clear();
        m_cmdReceiveBuffer = 0u;
        m_pendingDmaCallback.reset();
        m_nextDmaId = 1u;
        m_sifInitialized = false;
    }

    bool IopRpcBridge::findCmdHandler(uint32_t cid, CmdHandler &handler) const
    {
        uint32_t table = m_userCmdTable;
        uint32_t count = m_userCmdCount;
        uint32_t stride = kCmdHandlerEntrySize;
        uint32_t index = cid;
        if ((cid & kSystemCmdBit) != 0u)
        {
            index = cid & ~kSystemCmdBit;
            if (m_systemCmdTable == 0u)
            {
                const auto entry = m_systemCmdHandlers.find(index);
                if (entry == m_systemCmdHandlers.end())
                    return false;
                handler = entry->second;
                return true;
            }
            table = m_systemCmdTable;
            count = m_systemCmdCount;
            stride = kSysCmdHandlerEntrySize;
        }
        if (table == 0u || index >= count)
            return false;
        const uint32_t entry = table + index * stride;
        handler.function = m_memory.read32(entry);
        handler.argument = m_memory.read32(entry + 4u);
        return true;
    }

    bool IopRpcBridge::storeCmdHandler(uint32_t cid, const CmdHandler &handler)
    {
        uint32_t table = m_userCmdTable;
        uint32_t count = m_userCmdCount;
        uint32_t stride = kCmdHandlerEntrySize;
        uint32_t index = cid;
        if ((cid & kSystemCmdBit) != 0u)
        {
            index = cid & ~kSystemCmdBit;
            if (m_systemCmdTable == 0u)
            {
                if (index >= kBuiltinSystemCmdCount)
                    return false;
                m_systemCmdHandlers[index] = handler;
                return true;
            }
            table = m_systemCmdTable;
            count = m_systemCmdCount;
            stride = kSysCmdHandlerEntrySize;
        }
        // sifcmd indexes the table without a bound check; a table that is
        // missing or too small would be overwritten past its end.
        if (table == 0u || index >= count)
            return false;
        const uint32_t entry = table + index * stride;
        m_memory.write32(entry, handler.function);
        m_memory.write32(entry + 4u, handler.argument);
        return true;
    }

    void IopRpcBridge::reportCmdOnce(uint32_t cid, const char *what)
    {
        if (!m_reportedCmdIds.insert(cid).second)
            return;
        std::ostringstream out;
        out << "[IOP] SIF command 0x" << std::hex << cid << ": " << what;
        m_host.log(LogLevel::Warning, out.str());
    }

    bool IopRpcBridge::deliverSifCommand(const void *packet, size_t packetSize, IopGuestExecutor &executor)
    {
        if (!packet || packetSize < kCmdHeaderSize || packetSize > kCmdPacketMaxSize)
            return false;

        uint32_t cid = 0u;
        std::memcpy(&cid, static_cast<const uint8_t *>(packet) + 8u, sizeof(cid));
        CmdHandler handler{};
        if (!findCmdHandler(cid, handler) || handler.function == 0u)
        {
            // sifcmd drops a command without a handler.
            reportCmdOnce(cid, "no IOP handler, command dropped");
            return false;
        }

        // sifcmd's single receive buffer: the packet stays valid until the next command.
        if (m_cmdReceiveBuffer == 0u)
            m_cmdReceiveBuffer = m_memory.allocate(kCmdPacketMaxSize, 16u);
        if (m_cmdReceiveBuffer == 0u || !m_memory.writeRam(m_cmdReceiveBuffer, packet, packetSize))
            return false;

        const auto gp = m_cmdHandlerGp.find(cid);
        (void)executor.executeGuestFunction(handler.function,
                                            m_cmdReceiveBuffer,
                                            handler.argument,
                                            0u,
                                            0u,
                                            gp != m_cmdHandlerGp.end() ? gp->second : 0u);
        return true;
    }

    uint32_t IopRpcBridge::setDma(uint32_t descriptorAddress, uint32_t descriptorCount)
    {
        constexpr uint32_t kDescriptorSize = 16u;
        constexpr uint32_t kMaxDescriptors = 32u;
        if (descriptorAddress == 0u || descriptorCount == 0u || descriptorCount > kMaxDescriptors)
        {
            return 0u;
        }

        struct PendingTransfer
        {
            uint32_t source = 0u;
            uint32_t destination = 0u;
            uint32_t size = 0u;
        };

        std::array<uint32_t, kMaxDescriptors * 4u> descriptorWords{};
        const size_t descriptorBytes = static_cast<size_t>(descriptorCount) * kDescriptorSize;
        if (!m_memory.readRam(descriptorAddress, descriptorWords.data(), descriptorBytes))
        {
            return 0u;
        }

        std::array<PendingTransfer, kMaxDescriptors> pending{};
        uint32_t pendingCount = 0u;
        uint32_t largestTransfer = 0u;
        for (uint32_t i = 0u; i < descriptorCount; ++i)
        {
            const uint32_t source = descriptorWords[i * 4u + 0u];
            const uint32_t destination = descriptorWords[i * 4u + 1u];
            const int32_t signedSize = static_cast<int32_t>(descriptorWords[i * 4u + 2u]);
            if (signedSize <= 0)
                continue;

            const uint32_t size = static_cast<uint32_t>(signedSize);
            if (!m_memory.ownsRamRange(source, size))
            {
                return 0u;
            }
            pending[pendingCount++] = {source, destination, size};
            largestTransfer = std::max(largestTransfer, size);
        }

        // IOP-side sceSifSetDma sends IOP RAM to the EE. Validate all EE
        // destinations before committing any write so a bad chain cannot
        // partially update guest memory, but maybe we could skip this check if we trust the EE-side SIF driver to validate the chain ?!
        // TODO check later
        std::vector<uint8_t> scratch(largestTransfer);
        for (uint32_t i = 0u; i < pendingCount; ++i)
        {
            const PendingTransfer &transfer = pending[i];
            if (!m_host.readGuest(transfer.destination, scratch.data(), transfer.size))
            {
                return 0u;
            }
        }

        for (uint32_t i = 0u; i < pendingCount; ++i)
        {
            const PendingTransfer &transfer = pending[i];
            if (!m_memory.readRam(transfer.source, scratch.data(), transfer.size) || !m_host.writeGuest(transfer.destination, scratch.data(), transfer.size))
            {
                return 0u;
            }
        }

        const uint32_t dmaId = m_nextDmaId++;
        if (m_nextDmaId == 0u || m_nextDmaId > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
        {
            m_nextDmaId = 1u;
        }
        return dmaId;
    }

    bool IopRpcBridge::dispatchSifManImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        switch (ordinal)
        {
        case 4: // sceSifDma2Init
        case 5: // sceSifInit
            m_sifInitialized = true;
            setV0(0u);
            return true;
        case 7: // sceSifSetDma(dmat, count)
            setV0(setDma(cpu.gpr[4], cpu.gpr[5]));
            return true;
        case 32: // sceSifSetDmaIntr(dmat, count, func, data): func(data) runs once the transfer is done
        {
            const uint32_t dmaId = setDma(cpu.gpr[4], cpu.gpr[5]);
            if (dmaId != 0u && cpu.gpr[6] != 0u)
                m_pendingDmaCallback = DmaCallback{cpu.gpr[6], cpu.gpr[7], cpu.gpr[28]};
            setV0(dmaId);
            return true;
        }
        case 8: // sceSifDmaStat
            setV0(0xFFFFFFFFu);
            return true;
        case 29: // sceSifCheckInit
            setV0(m_sifInitialized ? 1u : 0u);
            return true;
        default:
            setV0(0u);
            return true;
        }
    }

    std::optional<IopRpcBridge::DmaCallback> IopRpcBridge::takeDmaCallback() noexcept
    {
        std::optional<DmaCallback> callback = m_pendingDmaCallback;
        m_pendingDmaCallback.reset();
        return callback;
    }

    bool IopRpcBridge::dispatchSifCmdImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        switch (ordinal)
        {
        case 4: // InitCmd
        case 5:
        case 6:
        case 7:
        case 14: // InitRpc
        case 15:
        case 16:
            setV0(0);
            return true;
        case 8: // sceSifSetCmdBuffer(SifCmdHandlerData_t *table, int count)
        case 9: // sceSifSetSysCmdBuffer(SifCmdHandlerData_t *table, int count)
        {
            uint32_t &table = ordinal == 8 ? m_userCmdTable : m_systemCmdTable;
            uint32_t &count = ordinal == 8 ? m_userCmdCount : m_systemCmdCount;
            const uint32_t previous = table;
            table = cpu.gpr[4];
            count = cpu.gpr[5];
            setV0(previous);
            return true;
        }
        case 10: // sceSifAddCmdHandler(cid, handler, harg)
        {
            const uint32_t cid = cpu.gpr[4];
            if (storeCmdHandler(cid, CmdHandler{cpu.gpr[5], cpu.gpr[6]}))
                m_cmdHandlerGp[cid] = cpu.gpr[28];
            else
                reportCmdOnce(cid, "sceSifAddCmdHandler without a handler table entry, handler not stored");
            setV0(0);
            return true;
        }
        case 11: // sceSifRemoveCmdHandler(cid)
        {
            const uint32_t cid = cpu.gpr[4];
            (void)storeCmdHandler(cid, CmdHandler{});
            m_cmdHandlerGp.erase(cid);
            setV0(0);
            return true;
        }
        case 12: // sceSifSendCmd
        case 13: // isceSifSendCmd
        {
            constexpr uint32_t kHeaderSize = 16u;
            constexpr uint32_t kMaxPacketSize = 112u;
            const uint32_t commandId = cpu.gpr[4];
            const uint32_t packetAddress = cpu.gpr[5];
            const uint32_t packetSize = cpu.gpr[6];
            const uint32_t extraSource = cpu.gpr[7];
            const uint32_t stackPointer = cpu.gpr[29];
            const uint32_t extraDestination = m_memory.read32(stackPointer + 16u);
            const int32_t signedExtraSize = static_cast<int32_t>(m_memory.read32(stackPointer + 20u));

            if (packetAddress == 0u || packetSize < kHeaderSize || packetSize > kMaxPacketSize ||
                !m_memory.ownsRamRange(packetAddress, packetSize))
            {
                setV0(0u);
                return true;
            }

            std::array<uint8_t, kMaxPacketSize> packet{};
            if (!m_memory.readRam(packetAddress, packet.data(), packetSize))
            {
                setV0(0u);
                return true;
            }

            uint32_t extraSize = 0u;
            if (signedExtraSize > 0)
            {
                extraSize = static_cast<uint32_t>(signedExtraSize);
                if (extraSource == 0u || extraDestination == 0u ||
                    !m_memory.ownsRamRange(extraSource, extraSize) ||
                    !m_host.writeGuest(extraDestination, m_memory.ram().data() + IopMemory::physicalAddress(extraSource), extraSize))
                {
                    setV0(0u);
                    return true;
                }
            }

            const uint32_t sizeWord = packetSize | (extraSize << 8u);
            std::memcpy(packet.data() + 0u, &sizeWord, sizeof(sizeWord));
            std::memcpy(packet.data() + 4u, &extraDestination, sizeof(extraDestination));
            std::memcpy(packet.data() + 8u, &commandId, sizeof(commandId));

            if (!m_host.sendSifCommand(commandId, packet.data(), packetSize))
            {
                // A command without an EE handler is still a completed DMA on  real hardware. Only malformed packets fail above.
            }

            const uint32_t dmaId = m_nextDmaId++;
            if (m_nextDmaId == 0u || m_nextDmaId > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
                m_nextDmaId = 1u;
            setV0(dmaId);
            return true;
        }
        case 17: // sceSifRegisterRpc
        {
            RpcServer server;
            server.serverData = cpu.gpr[4];
            server.sid = cpu.gpr[5];
            server.function = cpu.gpr[6];
            server.gp = cpu.gpr[28];
            server.buffer = cpu.gpr[7];
            const uint32_t stackPointer = cpu.gpr[29];
            server.callback = m_memory.read32(stackPointer + 16u);
            server.callbackBuffer = m_memory.read32(stackPointer + 20u);
            server.queue = m_memory.read32(stackPointer + 24u);
            m_servers[server.sid] = server;
            if (server.serverData != 0u)
            {
                m_memory.write32(server.serverData + 0x20u, server.sid);
                m_memory.write32(server.serverData + 0x28u, server.function);
                m_memory.write32(server.serverData + 0x2Cu, server.buffer);
            }
            setV0(server.serverData);
            return true;
        }
        case 18:
            setV0(0);
            return true;
        case 19: // SetRpcQueue
            setV0(cpu.gpr[4]);
            return true;
        case 20:
        case 21:
            setV0(0);
            return true;
        case 22: // RpcLoop
            m_kernel.sleepCurrent(cpu);
            setV0(0);
            return true;
        case 23:
            setV0(0);
            return true;
        case 24: // RemoveRpc
        {
            const uint32_t serverData = cpu.gpr[4];
            for (auto server = m_servers.begin(); server != m_servers.end(); ++server)
            {
                if (server->second.serverData == serverData)
                {
                    m_servers.erase(server);
                    break;
                }
            }
            setV0(0);
            return true;
        }
        case 25:
        case 26:
        case 27:
        case 28:
        case 29:
            setV0(0);
            return true;
        default:
            return false;
        }
    }

    RpcResult IopRpcBridge::handleRpc(const RpcRequest &request, IopGuestExecutor &executor)
    {
        RpcResult result{};
        const auto serverIt = m_servers.find(request.sid);
        if (serverIt == m_servers.end() || serverIt->second.function == 0u)
            return result;

        RpcServer &server = serverIt->second;
        if (request.send.size != 0u && server.buffer != 0u)
        {
            const uint32_t copySize = std::min<uint32_t>(request.send.size, IopMemory::RamSize - std::min(server.buffer, IopMemory::RamSize));
            if (copySize != 0u)
            {
                std::vector<uint8_t> payload(copySize);
                if (m_host.readGuest(request.send.address, payload.data(), payload.size()))
                    (void)m_memory.writeRam(server.buffer, payload.data(), payload.size());
            }
        }

        uint32_t returnPointer = executor.executeGuestFunction(server.function,
                                                               request.function,
                                                               server.buffer,
                                                               request.send.size,
                                                               0u,
                                                               server.gp);
        if (returnPointer == 0u)
            returnPointer = server.buffer;
        if (request.receive.address != 0u && request.receive.size != 0u && returnPointer != 0u)
        {
            const uint32_t physical = IopMemory::physicalAddress(returnPointer);
            if (physical < IopMemory::RamSize)
            {
                const uint32_t copySize = std::min<uint32_t>(request.receive.size, IopMemory::RamSize - physical);
                (void)m_host.writeGuest(request.receive.address, m_memory.ram().data() + physical, copySize);
                if (copySize < request.receive.size)
                    (void)m_host.zeroGuest(request.receive.address + copySize, request.receive.size - copySize);
            }
        }

        result.handled = true;
        result.resultAddress = request.receive.address;
        result.serverDispatchPolicy = ServerDispatchPolicy::Suppress;
        result.signalNowaitCompletion = true;
        result.signalCompletion = true;
        return result;
    }

    void IopRpcBridge::onSifTransfer(const SifTransfer &transfer)
    {
        // The EE SIF transport owns the actual directional memory movement.
        // Services still receive both phases through IopSubsystem, but mirroring
        // IOP bytes through an equal-numbered EE address would alias two distinct
        // PS2 address spaces and can overwrite live game data.
        (void)transfer;
    }

    void IopRpcBridge::removeServersInRange(uint32_t base, uint32_t size)
    {
        for (auto server = m_servers.begin(); server != m_servers.end();)
        {
            const uint32_t function = IopMemory::physicalAddress(server->second.function);
            if (function >= base && function < base + size)
                server = m_servers.erase(server);
            else
                ++server;
        }
    }

    bool IopRpcBridge::hasServer(uint32_t sid) const noexcept
    {
        const auto server = m_servers.find(sid);
        return server != m_servers.end() && server->second.function != 0u;
    }
}
