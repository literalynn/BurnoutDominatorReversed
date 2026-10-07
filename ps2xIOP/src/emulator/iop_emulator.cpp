#include "iop_emulator.h"
#include "imports/iop_cdvd.h"
#include "core/iop_cpu.h"
#include "imports/iop_heaplib.h"
#include "imports/iop_imports.h"
#include "imports/iop_intrman.h"
#include "imports/iop_ioman.h"
#include "core/iop_kernel.h"
#include "imports/iop_loadcore.h"
#include "core/iop_memory.h"
#include "services/iop_module_loader.h"
#include "services/iop_rpc.h"
#include "imports/iop_stdio.h"
#include "imports/iop_sysclib.h"
#include "imports/iop_sysmem.h"
#include "imports/iop_timrman.h"
#include "imports/iop_vblank.h"
#include "iop_emulator_const.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <utility>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kRamSize = IopMemory::RamSize;
        constexpr uint32_t kKernelHeapBase = IopMemory::HeapBase;
        constexpr uint32_t kKernelHeapLimit = IopMemory::HeapLimit;
        constexpr uint32_t kCallStackBase = kKernelHeapLimit;
        constexpr uint32_t kCallStackLimit = 0x001FFF00u;
        constexpr uint32_t kCallStackSize = 0x2000u;
        constexpr uint32_t kCallStackCapacity = (kCallStackLimit - kCallStackBase) / kCallStackSize;
        constexpr uint64_t kCdvdCompletionCycles = 128u;
        constexpr uint64_t kSifDmaCompletionCycles = 128u;

        uint32_t physicalAddress(uint32_t address)
        {
            return IopMemory::physicalAddress(address);
        }

        int32_t sign16(uint32_t value)
        {
            return static_cast<int16_t>(value & 0xFFFFu);
        }

        bool iequals(std::string_view lhs, std::string_view rhs)
        {
            if (lhs.size() != rhs.size())
                return false;
            for (size_t i = 0; i < lhs.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
                    std::tolower(static_cast<unsigned char>(rhs[i])))
                    return false;
            }
            return true;
        }

    }

    class IopEmulator::Impl final : public IopGuestExecutor
    {
    public:
        using CpuState = IopCpuState;

        struct Module
        {
            int id = 0;
            std::string path;
            std::string name;
            std::string irxName; // internal module name from the IRX header
            uint32_t base = 0;
            uint32_t size = 0;
            uint32_t entry = 0;
            uint32_t gp = 0;
            bool resident = false;
        };

        struct GuestCallback
        {
            uint32_t function = 0;
            uint32_t gp = 0;
        };

        struct ScheduledGuestCallback
        {
            uint32_t function = 0u;
            uint32_t gp = 0u;
            uint32_t argument = 0u;
            bool alarm = false;
        };

        explicit Impl(IopHost &hostRef)
            : host(hostRef),
              sysmem(host, memory),
              kernel(memory),
              cdvd(host, memory, kernel),
              vblank(kernel),
              rpc(host, memory, kernel),
              sysclib(memory),
              stdio(host, memory),
              heaplib(memory),
              intrman(memory),
              timrman(),
              ioman(memory),
              cpuCore(memory),
              imports(memory),
              loadcore(memory, imports)
        {
            reset();
            // The SPU2 asserts IOP interrupt 9 when a voice or a transfer reaches its IRQ address.
            memory.setSio2IrqCallback([this] { pendingDmaInterrupts[17] = totalCycles + 64u; });
            memory.spu().raiseIrq = [this]
            {
                pendingDmaInterrupts[kSpu2Irq] = totalCycles;
            };
            static const bool traceHardware = []
            {
                const char *env = std::getenv("BDR_TRACE_IOPHW");
                return env && env[0] != '\0' && env[0] != '0';
            }();
            if (traceHardware)
                memory.setHardwareWriteHook([this](uint32_t address, uint32_t value)
                                            { traceHardwareWrite(address, value); });
            static const bool kernelLog = []
            {
                const char *env = std::getenv("BDR_STATUS_DETAIL");
                return env && env[0] != '\0' && env[0] != '0';
            }();
            if (kernelLog)
                kernel.enableLog([this](uint32_t address)
                                 { return symbolize(address); });
        }

        std::string symbolize(uint32_t address) const
        {
            std::ostringstream out;
            for (const auto &[id, module] : modules)
            {
                if (address >= module.base && address < module.base + module.size)
                {
                    out << (module.name.empty() ? module.path : module.name) << "+0x" << std::hex << address - module.base;
                    return out.str();
                }
            }
            out << "0x" << std::hex << address;
            return out.str();
        }

        void reset()
        {
            memory.reset();
            kernel.reset();
            modules.clear();
            imports.reset();
            rpc.reset();
            cdvd.reset();
            intrman.reset();
            timrman.reset();
            ioman.reset();
            pendingDmaInterrupts.clear();
            pendingGuestCallbacks.clear();
            nextModuleId = 1;
            moduleCursor = kModuleLoadBase;
            totalCycles = 0;
            totalInstructions = 0;
            eeCycleCarry = 0;
            activeCpu = nullptr;
            lastError.clear();
            servicingDmaInterrupts = false;
            servicingGuestCallbacks = false;
            callDepth = 0u;
            reportedMissingImports.clear();
            secrMcCommandHandler = {};
            secrMcDevIdHandler = {};
            checkKelfPathCallback = {};
        }

        uint8_t read8(uint32_t address) const
        {
            return memory.read8(address);
        }

        uint16_t read16(uint32_t address) const
        {
            return memory.read16(address);
        }

        uint32_t read32(uint32_t address) const
        {
            return memory.read32(address);
        }

        // BDR_TRACE_IOPHW=1: bounded log of IOP writes to the DMA and SPU2 register ranges.
        void traceHardwareWrite(uint32_t address, uint32_t value)
        {
            const uint32_t phys = address & 0x1FFFFFFFu;
            const bool dma = phys >= 0x1F8010C0u && phys < 0x1F801100u;
            const bool dma2 = phys >= 0x1F801500u && phys < 0x1F801570u;
            // SPU2 core registers without the per-voice parameters and the PIO data port.
            const uint32_t coreOffset = phys & 0x3FFu;
            const bool spuCore = (phys >= 0x1F900000u && phys < 0x1F900800u) && coreOffset >= 0x180u;
            const bool dataPort = coreOffset == 0x1ACu || coreOffset == 0x1ADu || coreOffset == 0x1AEu;
            const bool spu = spuCore && !dataPort;
            if (!(dma || dma2 || spu) || traceHardwareWrites >= 6000u)
                return;
            ++traceHardwareWrites;
            std::ostringstream out;
            const uint32_t pc = activeCpu ? activeCpu->pc : 0u;
            std::string where = "?";
            for (const auto &[id, module] : modules)
            {
                if (pc >= module.base && pc < module.base + module.size)
                {
                    std::ostringstream w;
                    w << module.name << "+0x" << std::hex << pc - module.base;
                    where = w.str();
                    break;
                }
            }
            out << "[iop-hw] cyc=" << totalCycles << " pc=" << where << " 0x" << std::hex << phys
                << " = 0x" << value;
            log(LogLevel::Info, out.str());
        }

        void write8(uint32_t address, uint8_t value)
        {
            memory.write8(address, value);
            schedulePendingDma();
        }

        void write16(uint32_t address, uint16_t value)
        {
            memory.write16(address, value);
            schedulePendingDma();
        }

        void write32(uint32_t address, uint32_t value)
        {
            memory.write32(address, value);
            schedulePendingDma();
        }

        void schedulePendingDma()
        {
            if (const auto dma = memory.takeDmaStart())
            {
                pendingDmaInterrupts[dma->irq] = totalCycles + dma->delayCycles;
                if (traceSpu && traceSpuDmas++ < 64u)
                {
                    const bool core1 = dma->irq == 0x28;
                    const uint32_t channel = core1 ? 0x1F801500u : 0x1F8010C0u;
                    std::ostringstream out;
                    out << "[IOP SPU trace] start irq=0x" << std::hex << dma->irq
                        << " pc=0x" << (activeCpu ? activeCpu->pc : 0u)
                        << " gp=0x" << (activeCpu ? activeCpu->gpr[28] : 0u)
                        << " madr=0x" << memory.read32(channel)
                        << " bcr=0x" << memory.read32(channel + 4u)
                        << " chcr=0x" << memory.read32(channel + 8u)
                        << " attr=0x" << memory.read16(0x1F90019Au + (core1 ? 0x400u : 0u))
                        << " statx=0x" << memory.read16(0x1F900344u + (core1 ? 0x400u : 0u))
                        << std::dec << " due=" << totalCycles + dma->delayCycles;
                    log(LogLevel::Info, out.str());
                }
            }
        }

        bool readRam(uint32_t address, void *destination, size_t size) const
        {
            return memory.readRam(address, destination, size);
        }

        bool writeRam(uint32_t address, const void *source, size_t size)
        {
            return memory.writeRam(address, source, size);
        }

        bool zeroRam(uint32_t address, size_t size)
        {
            return memory.zeroRam(address, size);
        }

        bool isHardwareAddress(uint32_t phys) const
        {
            return memory.isHardwareAddress(phys);
        }

        uint32_t allocate(uint32_t size, uint32_t alignment = 16u, std::optional<uint32_t> fixed = std::nullopt)
        {
            return memory.allocate(size, alignment, fixed);
        }

        bool freeAllocation(uint32_t address)
        {
            return memory.freeAllocation(address);
        }

        void log(LogLevel level, std::string_view text)
        {
            host.log(level, text);
        }

        bool checkInterrupt(CpuState &cpu)
        {
            const uint32_t status = cpu.cop0[12];
            if ((status & 1u) == 0u)
                return false;
            if ((status & 0x2u) != 0u)
                return false;
            const bool pending = memory.interruptControl() != 0u && (memory.interruptStatus() & memory.interruptMask()) != 0u;
            if (!pending)
                return false;
            cpu.cop0[13] |= 0x400u;
            cpuCore.raiseException(cpu, 0u, cpu.pc, false);
            return true;
        }

        enum class ImportDisposition
        {
            Handled,
            JumpToGuest,
            Missing,
        };

        // Built-in libraries without an export fallback leave v0 unchanged on an
        // unknown ordinal; report each such import once so it is not silent.
        ImportDisposition missingBuiltinImport(const IopImportCall &call, const CpuState &cpu)
        {
            if (reportedMissingImports.emplace(asciiLower(call.library), call.ordinal).second)
            {
                std::ostringstream out;
                out << "[IOP] unhandled built-in import " << call.library << ':' << call.ordinal
                    << " version=0x" << std::hex << call.version << " pc=0x" << cpu.pc
                    << " (v0 left unchanged)";
                log(LogLevel::Warning, out.str());
            }
            return ImportDisposition::Missing;
        }

        static std::string asciiLower(std::string_view text)
        {
            std::string lower(text);
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c)
                           { return static_cast<char>(std::tolower(c)); });
            return lower;
        }

        ImportDisposition dispatchImport(const IopImportCall &call, CpuState &cpu)
        {
            const uint32_t a0 = cpu.gpr[4];
            if (traceSpu && iequals(call.library, "libsd") &&
                (call.ordinal == 17u || call.ordinal == 19u || call.ordinal == 28u) &&
                traceSpuImports++ < 96u)
            {
                std::ostringstream out;
                out << "[IOP SPU trace] libsd:" << call.ordinal
                    << " pc=0x" << std::hex << cpu.pc << " gp=0x" << cpu.gpr[28]
                    << " a0=0x" << cpu.gpr[4] << " a1=0x" << cpu.gpr[5]
                    << " a2=0x" << cpu.gpr[6] << " a3=0x" << cpu.gpr[7]
                    << " stackarg=0x" << memory.read32(cpu.gpr[29] + 16u)
                    << " irqctl=0x" << memory.interruptControl();
                log(LogLevel::Info, out.str());
            }
            auto setV0 = [&](uint32_t value)
            {
                cpu.gpr[2] = value;
            };

            if (iequals(call.library, "sysmem") && sysmem.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "cdvdman") && cdvd.dispatchImport(call.ordinal, cpu))
            {
                if (const auto callback = cdvd.takeCompletionCallback())
                {
                    pendingGuestCallbacks.emplace(
                        totalCycles + kCdvdCompletionCycles,
                        ScheduledGuestCallback{
                            callback->address,
                            callback->gp,
                            callback->reason,
                        });
                }
                return ImportDisposition::Handled;
            }

            if (iequals(call.library, "loadcore") && loadcore.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "thbase") || iequals(call.library, "threadman"))
            {
                if (call.ordinal >= 35u && call.ordinal <= 38u)
                {
                    if (call.ordinal == 35u || call.ordinal == 36u) // SetAlarm / iSetAlarm
                    {
                        const uint32_t clock = cpu.gpr[4];
                        if (!memory.ownsRamRange(clock, 8u) || cpu.gpr[5] == 0u)
                            setV0(-1);
                        else
                        {
                            const uint64_t delay = uint64_t{memory.read32(clock)} |
                                                   (uint64_t{memory.read32(clock + 4u)} << 32u);
                            pendingGuestCallbacks.emplace(totalCycles + std::max<uint64_t>(delay, 1u),
                                ScheduledGuestCallback{cpu.gpr[5], cpu.gpr[28], cpu.gpr[6], true});
                            setV0(0);
                        }
                    }
                    else // CancelAlarm / iCancelAlarm identify callback and argument
                    {
                        bool removed = false;
                        for (auto alarm = pendingGuestCallbacks.begin(); alarm != pendingGuestCallbacks.end();)
                        {
                            if (alarm->second.alarm && alarm->second.function == cpu.gpr[4] &&
                                alarm->second.argument == cpu.gpr[5])
                            {
                                alarm = pendingGuestCallbacks.erase(alarm);
                                removed = true;
                            }
                            else
                                ++alarm;
                        }
                        setV0(removed ? 0 : -1);
                    }
                    return ImportDisposition::Handled;
                }
                return kernel.dispatchThreadImport(call.ordinal, cpu, totalCycles)
                           ? ImportDisposition::Handled
                           : missingBuiltinImport(call, cpu);
            }
            if (iequals(call.library, "thsemap"))
            {
                return kernel.dispatchSemaphoreImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : missingBuiltinImport(call, cpu);
            }
            if (iequals(call.library, "thevent"))
            {
                return kernel.dispatchEventImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : missingBuiltinImport(call, cpu);
            }
            if (iequals(call.library, "sifcmd"))
            {
                return rpc.dispatchSifCmdImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : missingBuiltinImport(call, cpu);
            }
            if (iequals(call.library, "intrman") && intrman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "secrman"))
            {
                switch (call.ordinal)
                {
                case 4: // SecrSetMcCommandHandler
                    secrMcCommandHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                case 5: // SecrSetMcDevIDHandler
                    secrMcDevIdHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                default:
                    break;
                }
            }
            if (iequals(call.library, "modload") && call.ordinal == 13u)
            {
                checkKelfPathCallback = {a0, cpu.gpr[28]};
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "ioman") && ioman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sifman"))
            {
                if (!rpc.dispatchSifManImport(call.ordinal, cpu))
                    return missingBuiltinImport(call, cpu);
                if (const auto callback = rpc.takeDmaCallback())
                {
                    // The SIF DMA interrupt runs the sceSifSetDmaIntr handler
                    // after the transfer, not inside the call.
                    pendingGuestCallbacks.emplace(
                        totalCycles + kSifDmaCompletionCycles,
                        ScheduledGuestCallback{callback->function, callback->gp, callback->argument});
                }
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "vblank") && vblank.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "timrman") && timrman.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "dmacman"))
            {
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "stdio") && stdio.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sysclib"))
            {
                return sysclib.dispatchImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : missingBuiltinImport(call, cpu);
            }
            if (iequals(call.library, "heaplib") && heaplib.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            const uint32_t target = imports.resolve(call.library, call.ordinal, call.version);
            if (target != 0u)
            {
                cpu.pc = target;
                cpu.branchPending = false;
                return ImportDisposition::JumpToGuest;
            }

            std::ostringstream out;
            out << "[IOP] unhandled import " << call.library << ':' << call.ordinal
                << " version=0x" << std::hex << call.version << " pc=0x" << cpu.pc;
            log(LogLevel::Warning, out.str());
            setV0(0);
            return ImportDisposition::Missing;
        }

        bool step(CpuState &cpu)
        {
            if (cpu.stopped)
                return false;
            if (cpu.pc == kThreadReturnSentinel || cpu.pc == kCallReturnSentinel)
            {
                cpu.stopped = true;
                return false;
            }
            if (physicalAddress(cpu.pc) >= kRamSize)
            {
                std::ostringstream out;
                out << "[IOP] execution outside RAM pc=0x" << std::hex << cpu.pc;
                log(LogLevel::Error, out.str());
                cpu.stopped = true;
                return false;
            }
            if (checkInterrupt(cpu))
                return true;

            if (!tracedPcs.empty())
            {
                for (auto pending = tracedReturns.begin(); pending != tracedReturns.end(); ++pending)
                {
                    if (pending->returnAddress == cpu.pc && pending->stackPointer == cpu.gpr[29])
                    {
                        std::ostringstream out;
                        out << "[iop-ret] cyc=" << totalCycles << " " << pending->name << " v0=0x" << std::hex << cpu.gpr[2];
                        log(LogLevel::Info, out.str());
                        tracedReturns.erase(pending);
                        break;
                    }
                }
                if (tracedPcs.count(cpu.pc) != 0u && traceCallCount < 4000u)
                {
                    ++traceCallCount;
                    std::ostringstream out;
                    out << "[iop-call] cyc=" << totalCycles << " " << symbolize(cpu.pc) << " a0=0x" << std::hex << cpu.gpr[4]
                        << " a1=0x" << cpu.gpr[5] << " a2=0x" << cpu.gpr[6] << " a3=0x" << cpu.gpr[7] << " sp0=0x"
                        << memory.read32(cpu.gpr[29] + 16u) << " ra=" << symbolize(cpu.gpr[31]);
                    log(LogLevel::Info, out.str());
                    if (tracedReturns.size() < 256u)
                        tracedReturns.push_back({cpu.gpr[31], cpu.gpr[29], symbolize(cpu.pc)});
                }
            }

            if (const auto import = imports.decode(cpu.pc))
            {
                const ImportDisposition disposition = dispatchImport(*import, cpu);
                ++totalInstructions;
                ++totalCycles;
                if (disposition == ImportDisposition::JumpToGuest)
                    return true;
                cpu.pc = cpu.gpr[31];
                cpu.branchPending = false;
                return !cpu.stopped;
            }

            const bool running = cpuCore.executeInstruction(cpu);
            schedulePendingDma();
            ++totalInstructions;
            ++totalCycles;
            return running;
        }

        uint32_t runCpu(CpuState &cpu, uint32_t instructionBudget)
        {
            CpuState *previous = activeCpu;
            activeCpu = &cpu;
            const uint64_t start = totalInstructions;
            while (!cpu.stopped && !cpu.yielded && totalInstructions - start < instructionBudget)
            {
                if (profileIop && (totalInstructions & 0x3Fu) == 0u)
                    ++pcHistogram[cpu.pc & ~0xFu];
                if (!step(cpu))
                    break;
                if (totalCycles >= memory.spu().dueCycle())
                    memory.spu().advanceTo(totalCycles);
                if (!servicingDmaInterrupts && !pendingDmaInterrupts.empty())
                    servicePendingDmaInterrupts();
                if (!servicingGuestCallbacks && !pendingGuestCallbacks.empty())
                    servicePendingGuestCallbacks();
            }
            activeCpu = previous;
            return static_cast<uint32_t>(totalInstructions - start);
        }

        uint32_t callFunction(uint32_t address,
                              uint32_t a0,
                              uint32_t a1,
                              uint32_t a2,
                              uint32_t a3,
                              uint32_t gp,
                              uint32_t budget = kMaxCallInstructions)
        {
            struct CallDepthGuard
            {
                uint32_t &depth;
                ~CallDepthGuard() { --depth; }
            };

            const uint32_t depth = callDepth++;
            const CallDepthGuard depthGuard{callDepth};
            CpuState cpu{};
            cpu.pc = address;
            cpu.gpr[4] = a0;
            cpu.gpr[5] = a1;
            cpu.gpr[6] = a2;
            cpu.gpr[7] = a3;
            cpu.gpr[28] = gp;
            if (depth < kCallStackCapacity)
            {
                const uint32_t stackTop = kCallStackLimit - depth * kCallStackSize;
                cpu.gpr[29] = stackTop - 32u;
            }
            else if (activeCpu && activeCpu->gpr[29] > kCallStackBase + kStackGuardBytes)
            {
                // Extremely deep re-entrancy borrows unused space below the
                // suspended caller's live frame. Stack growth remains away
                // from the caller, so its saved registers stay intact.
                cpu.gpr[29] = (activeCpu->gpr[29] - kStackGuardBytes) & ~15u;
            }
            else
            {
                cpu.gpr[29] = kCallStackBase - 32u;
            }
            cpu.gpr[31] = kCallReturnSentinel;
            runCpu(cpu, budget);
            return cpu.gpr[2];
        }

        uint32_t executeGuestFunction(uint32_t address,
                                      uint32_t a0,
                                      uint32_t a1,
                                      uint32_t a2,
                                      uint32_t a3,
                                      uint32_t gp) override
        {
            return callFunction(address, a0, a1, a2, a3, gp);
        }

        uint32_t executeRpcFunction(uint32_t address, uint32_t a0, uint32_t a1,
                                    uint32_t a2, uint32_t a3, uint32_t gp) override
        {
            if (activeCpu != nullptr)
                throw std::runtime_error("IOP RPC reentry while executing guest code");
            IopThread *thread = kernel.createInternalCall(address, gp, kThreadReturnSentinel);
            if (!thread)
                throw std::runtime_error("IOP RPC call stack allocation failed");
            const int id = thread->id;
            struct CallGuard
            {
                IopKernel &kernel;
                int id;
                ~CallGuard() { kernel.removeInternalCall(id); }
            } guard{kernel, id};
            thread->cpu.gpr[4] = a0;
            thread->cpu.gpr[5] = a1;
            thread->cpu.gpr[6] = a2;
            thread->cpu.gpr[7] = a3;
            const uint64_t firstInstruction = totalInstructions;
            const uint64_t firstCycle = totalCycles;
            for (;;)
            {
                thread = kernel.findInternalCall(id);
                if (!thread)
                    throw std::runtime_error("IOP RPC server deleted its execution thread");
                if (thread->state == IopThreadState::Dormant)
                    break;
                if (totalInstructions - firstInstruction >= kMaxCallInstructions ||
                    totalCycles - firstCycle >= kIopClockHz * 2u)
                    throw std::runtime_error("IOP RPC server exhausted its execution budget");
                runCycles(kDefaultSlice);
            }
            if (thread->cpu.pc != kThreadReturnSentinel)
                throw std::runtime_error("IOP RPC server did not return within its execution budget");
            return thread->cpu.gpr[2];
        }

        uint32_t executeGuestFunctionWithBudget(uint32_t address,
                                                uint32_t a0,
                                                uint32_t a1,
                                                uint32_t a2,
                                                uint32_t a3,
                                                uint32_t gp,
                                                uint32_t instructionBudget) override
        {
            return callFunction(address, a0, a1, a2, a3, gp, instructionBudget);
        }

        // Not that good to use exception handling for control flow but will do for now
        void servicePendingDmaInterrupts()
        {
            if (servicingDmaInterrupts || pendingDmaInterrupts.empty())
                return;

            servicingDmaInterrupts = true;

            std::vector<int> completed;
            for (auto it = pendingDmaInterrupts.begin(); it != pendingDmaInterrupts.end();)
            {
                if (it->second > totalCycles)
                {
                    ++it;
                    continue;
                }
                completed.push_back(it->first);
                it = pendingDmaInterrupts.erase(it);
            }
            try
            {
                for (const int irq : completed)
                {
                    if (irq == kSpu0DmaIrq)
                        memory.spu().dmaCompleted(0);
                    else if (irq == kSpu1DmaIrq)
                        memory.spu().dmaCompleted(1);
                    const bool dispatched = intrman.dispatchInterrupt(irq, *this);
                    if (traceSpu && traceSpuIrqs++ < 64u)
                    {
                        std::ostringstream out;
                        out << "[IOP SPU trace] complete irq=0x" << std::hex << irq
                            << " dispatched=" << dispatched
                            << " pc=0x" << (activeCpu ? activeCpu->pc : 0u)
                            << " gp=0x" << (activeCpu ? activeCpu->gpr[28] : 0u)
                            << " irqctl=0x" << memory.interruptControl()
                            << std::dec << " cycle=" << totalCycles;
                        log(LogLevel::Info, out.str());
                    }
                }
            }
            catch (...)
            {
                servicingDmaInterrupts = false;
                throw;
            }
            servicingDmaInterrupts = false;
        }

        void servicePendingGuestCallbacks()
        {
            if (servicingGuestCallbacks || pendingGuestCallbacks.empty())
                return;

            std::vector<ScheduledGuestCallback> callbacks;
            for (auto it = pendingGuestCallbacks.begin(); it != pendingGuestCallbacks.end();)
            {
                if (it->first > totalCycles)
                    break;
                callbacks.push_back(it->second);
                it = pendingGuestCallbacks.erase(it);
            }
            if (callbacks.empty())
                return;

            servicingGuestCallbacks = true;
            try
            {
                for (const ScheduledGuestCallback &callback : callbacks)
                {
                    if (callback.function != 0u)
                    {
                        const uint32_t interval = callFunction(callback.function,
                                           callback.argument,
                                           0u,
                                           0u,
                                           0u,
                                           callback.gp,
                                           100000u);
                        if (callback.alarm && interval != 0u)
                            pendingGuestCallbacks.emplace(totalCycles + interval, callback);
                    }
                }
            }
            catch (...)
            {
                servicingGuestCallbacks = false;
                throw;
            }
            servicingGuestCallbacks = false;
        }

        void runCycles(uint64_t cycles) noexcept
        {
            try
            {
                const uint64_t target = totalCycles + cycles;
                while (totalCycles < target)
                {
                    if (totalCycles >= memory.spu().dueCycle())
                        memory.spu().advanceTo(totalCycles);
                    servicePendingDmaInterrupts();
                    servicePendingGuestCallbacks();
                    timrman.serviceDue(totalCycles, *this);
                    IopThread *next = kernel.beginNextReady(totalCycles);
                    if (!next)
                    {
                        uint64_t nextWake = kernel.nextWakeCycle(target);
                        for (const auto &[irq, completionCycle] : pendingDmaInterrupts)
                            nextWake = std::min(nextWake, completionCycle);
                        if (!pendingGuestCallbacks.empty())
                            nextWake = std::min(nextWake, pendingGuestCallbacks.begin()->first);
                        nextWake = std::min(nextWake, memory.spu().dueCycle());
                        nextWake = timrman.nextEventCycle(nextWake);
                        totalCycles = std::max(totalCycles + 1u, std::min(target, nextWake));
                        continue;
                    }
                    const uint64_t before = totalCycles;
                    runCpu(next->cpu, static_cast<uint32_t>(std::min<uint64_t>(kDefaultSlice, target - totalCycles)));
                    kernel.endTimeslice(*next, kThreadReturnSentinel);
                    if (totalCycles == before)
                        ++totalCycles;
                }
            }
            catch (...)
            {
                // Runtime scheduling must never throw through EeScheduler::accountCycles().
            }
        }

        ModuleLoadResult loadImage(std::string path, std::span<const uint8_t> image, const void *arguments, uint32_t argumentSize)
        {
            ModuleLoadResult result{true, -1, -1};
            const IopImageLoadResult loaded = IopModuleLoader::load(image, memory, moduleCursor);
            moduleCursor = loaded.nextModuleCursor;
            if (!loaded)
            {
                if (loaded.error == IopImageLoadError::InvalidElf)
                    log(LogLevel::Error, "[IOP] rejected invalid/non-MIPS IRX ELF");
                else if (loaded.error == IopImageLoadError::ArenaExhausted)
                    log(LogLevel::Error, "[IOP] module arena exhausted");
                return result;
            }
            if (!loaded.relocationsComplete)
                log(LogLevel::Warning, "[IOP] one or more IRX relocations were unsupported");

            Module module;
            module.id = nextModuleId++;
            module.path = std::move(path);
            const size_t slash = module.path.find_last_of("/\\:");
            module.name = slash == std::string::npos ? module.path : module.path.substr(slash + 1u);
            module.irxName = loaded.name;
            module.base = loaded.base;
            module.size = loaded.size;
            module.entry = loaded.entry;
            module.gp = loaded.gp;

            uint32_t args = 0u;
            if (arguments && argumentSize)
            {
                args = allocate(argumentSize + 1u, 16u);
                if (args)
                {
                    writeRam(args, arguments, argumentSize);
                    write8(args + argumentSize, 0u);
                }
            }
            const uint32_t startResult = callFunction(module.entry, argumentSize, args, 0u, 0u, module.gp);
            if (args)
                freeAllocation(args);
            module.resident = startResult == 0u || startResult == 2u;
            result.moduleId = module.id;
            result.startResult = static_cast<int32_t>(startResult);
            modules[module.id] = std::move(module);

            std::ostringstream out;
            out << "[IOP] loaded IRX id=" << result.moduleId
                << " entry=0x" << std::hex << modules[result.moduleId].entry
                << " base=0x" << modules[result.moduleId].base
                << " start=" << std::dec << result.startResult;
            log(LogLevel::Info, out.str());
            return result;
        }

        ModuleLoadResult loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readWholeHostFile(host, path, image))
            {
                log(LogLevel::Warning, std::string("[IOP] failed to open IRX '") + std::string(path) + "'");
                return {true, -1, -1};
            }
            return loadImage(std::string(path), image, arguments, argumentSize);
        }

        ModuleLoadResult loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readElfFromGuest(host, guestAddress, image))
                return {true, -1, -1};
            std::ostringstream tag;
            tag << "buffer@0x" << std::hex << guestAddress;
            return loadImage(tag.str(), image, arguments, argumentSize);
        }

        int32_t findModuleByName(std::string_view name) const noexcept
        {
            if (name.empty())
                return -1;
            for (const auto &[id, module] : modules)
            {
                if (module.resident && module.irxName == name)
                    return id;
            }
            return -1;
        }

        bool stopModule(int32_t moduleId, int32_t *result)
        {
            auto it = modules.find(moduleId);
            if (it == modules.end())
                return false;
            // A removable IRX normally exposes a stop entry through module metadata. We do not guess it; terminate owned execution and release the image cleanly.
            kernel.terminateThreadsInRange(it->second.base, it->second.size);
            rpc.removeServersInRange(it->second.base, it->second.size);
            imports.eraseRange(it->second.base, it->second.size);
            modules.erase(it);
            kernel.cleanupDeadThreads();
            if (result)
                *result = 0;
            return true;
        }

        IopHost &host;
        IopMemory memory;
        IopSysmem sysmem;
        IopKernel kernel;
        IopCdvd cdvd;
        IopVblank vblank;
        IopRpcBridge rpc;
        IopSysclib sysclib;
        IopStdio stdio;
        IopHeaplib heaplib;
        IopIntrman intrman;
        IopTimrman timrman;
        IopIoman ioman;
        IopCpuCore cpuCore;
        IopImportRegistry imports;
        IopLoadcore loadcore;
        std::map<int, Module> modules;
        std::map<int, uint64_t> pendingDmaInterrupts;
        std::multimap<uint64_t, ScheduledGuestCallback> pendingGuestCallbacks;
        uint32_t nextModuleId = 1;
        uint32_t moduleCursor = kModuleLoadBase;
        uint64_t totalCycles = 0;
        uint64_t totalInstructions = 0;
        uint64_t eeCycleCarry = 0;
        CpuState *activeCpu = nullptr;
        std::string lastError;
        bool servicingDmaInterrupts = false;
        bool servicingGuestCallbacks = false;
        uint32_t callDepth = 0u;
        // Opt-in bounded diagnostics; do not alter guest transfer semantics.
        bool traceSpu = []
        {
            const char *value = std::getenv("PS2X_IOP_TRACE_SPU");
            return value && value[0] != '\0' && value[0] != '0';
        }();
        static constexpr int kSpu2Irq = 9;
        static constexpr int kSpu0DmaIrq = 0x24;
        static constexpr int kSpu1DmaIrq = 0x28;
        uint32_t traceHardwareWrites = 0u;
        // BDR_TRACE_IOPCALLS=0xADDR,...: log each time the IOP executes one of these absolute
        // addresses (function entry points), with a0-a3, the first stack argument and ra.
        std::set<uint32_t> tracedPcs = []
        {
            std::set<uint32_t> result;
            if (const char *env = std::getenv("BDR_TRACE_IOPCALLS"))
            {
                std::stringstream list(env);
                for (std::string item; std::getline(list, item, ',');)
                    if (!item.empty())
                        result.insert(static_cast<uint32_t>(std::stoul(item, nullptr, 16)));
            }
            return result;
        }();
        uint32_t traceCallCount = 0u;
        // BDR_IOP_PROFILE=1: where the IOP spends its instructions (sampled every 64th), in 16-byte buckets.
        const bool profileIop = []
        {
            const char *value = std::getenv("BDR_IOP_PROFILE");
            return value && value[0] != '\0' && value[0] != '0';
        }();
        std::unordered_map<uint32_t, uint32_t> pcHistogram;
        struct PendingReturn
        {
            uint32_t returnAddress = 0u;
            uint32_t stackPointer = 0u;
            std::string name;
        };
        std::vector<PendingReturn> tracedReturns;
        uint32_t traceSpuImports = 0u;
        uint32_t traceSpuDmas = 0u;
        uint32_t traceSpuIrqs = 0u;
        std::set<std::pair<std::string, uint16_t>> reportedMissingImports;
        GuestCallback secrMcCommandHandler;
        GuestCallback secrMcDevIdHandler;
        GuestCallback checkKelfPathCallback;
    };

    IopEmulator::IopEmulator(IopHost &host)
        : m_impl(std::make_unique<Impl>(host))
    {
    }

    IopEmulator::~IopEmulator() = default;

    void IopEmulator::reset()
    {
        m_impl->reset();
    }

    ModuleLoadResult IopEmulator::loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
    {
        return m_impl->loadModule(path, arguments, argumentSize);
    }

    ModuleLoadResult IopEmulator::loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
    {
        return m_impl->loadModuleBuffer(guestAddress, arguments, argumentSize);
    }

    bool IopEmulator::stopModule(int32_t moduleId, int32_t *result)
    {
        return m_impl->stopModule(moduleId, result);
    }

    int32_t IopEmulator::findModuleByName(std::string_view name) const noexcept
    {
        return m_impl->findModuleByName(name);
    }

    void IopEmulator::runEeCycles(uint64_t eeCycles) noexcept
    {
        const uint64_t total = m_impl->eeCycleCarry + eeCycles;
        const uint64_t iopCycles = total / 8u;
        m_impl->eeCycleCarry = total % 8u;
        if (iopCycles)
            m_impl->runCycles(iopCycles);
    }

    RpcResult IopEmulator::handleRpc(const RpcRequest &request)
    {
        return m_impl->rpc.handleRpc(request, *m_impl);
    }

    bool IopEmulator::deliverSifCommand(const void *packet, size_t packetSize)
    {
        return m_impl->rpc.deliverSifCommand(packet, packetSize, *m_impl);
    }

    bool IopEmulator::hasRpcServer(uint32_t sid) const noexcept
    {
        return m_impl->rpc.hasServer(sid);
    }

    void IopEmulator::onSifTransfer(const SifTransfer &transfer)
    {
        m_impl->rpc.onSifTransfer(transfer);
    }

    uint32_t IopEmulator::allocateMemory(uint32_t size, uint32_t alignment)
    {
        return m_impl->memory.allocate(size, alignment);
    }

    bool IopEmulator::freeMemory(uint32_t address)
    {
        return m_impl->memory.freeAllocation(address);
    }

    bool IopEmulator::readMemory(uint32_t address, void *destination, size_t size) const
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.readRam(address, destination, size);
    }

    bool IopEmulator::writeMemory(uint32_t address, const void *source, size_t size)
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.writeRam(address, source, size);
    }

    bool IopEmulator::zeroMemory(uint32_t address, size_t size)
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.zeroRam(address, size);
    }

    bool IopEmulator::isMemoryRange(uint32_t address, size_t size) const
    {
        const bool physicalSegment = address < IopMemory::RamSize;
        const bool cachedSegment = address >= 0x80000000u && address < 0x80200000u;
        const bool uncachedSegment = address >= 0xA0000000u && address < 0xA0200000u;
        if (!physicalSegment && !cachedSegment && !uncachedSegment)
            return false;
        const uint32_t physical = IopMemory::physicalAddress(address);
        return physical <= IopMemory::RamSize && size <= IopMemory::RamSize - physical;
    }

    uint64_t IopEmulator::cycles() const noexcept
    {
        return m_impl->totalCycles;
    }

    uint64_t IopEmulator::instructions() const noexcept
    {
        return m_impl->totalInstructions;
    }

    uint32_t IopEmulator::loadedModuleCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->modules.size());
    }

    uint32_t IopEmulator::threadCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->kernel.threadCount());
    }

    void IopEmulator::describeKernel(std::vector<std::string> &lines) const
    {
        const auto &modules = m_impl->modules;
        if (m_impl->profileIop)
        {
            std::vector<std::pair<uint32_t, uint32_t>> ranked(m_impl->pcHistogram.begin(), m_impl->pcHistogram.end());
            std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b)
                      { return a.second > b.second; });
            uint64_t total = 0u;
            for (const auto &entry : ranked)
                total += entry.second;
            for (size_t i = 0; i < ranked.size() && i < 24u; ++i)
            {
                std::ostringstream out;
                out << "iop-prof " << std::dec << ranked[i].second * 100u / std::max<uint64_t>(total, 1u) << "% "
                    << m_impl->symbolize(ranked[i].first);
                lines.push_back(out.str());
            }
        }
        m_impl->kernel.describe(lines, [&modules](uint32_t address)
                                {
            std::ostringstream out;
            for (const auto &[id, module] : modules)
            {
                if (address >= module.base && address < module.base + module.size)
                {
                    out << (module.name.empty() ? module.path : module.name) << "+0x" << std::hex << address - module.base
                        << " (0x" << address << ')';
                    return out.str();
                }
            }
            out << "0x" << std::hex << address;
            return out.str(); });
    }

    uint32_t IopEmulator::rpcServerCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->rpc.serverCount());
    }

}
