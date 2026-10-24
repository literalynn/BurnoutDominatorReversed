// Save states: the whole emulated machine (EE, VU, GS, IOP and the runtime's
// HLE kernel) written to one file and restored at a scheduler quiet point.
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ee_count.h"
#include "runtime/gs_worker.h"
#include "runtime/ps2_ipu.h"
#include "ps2x/iop/iop_subsystem.h"
#include "ps2x/state_archive.h"
#include "Kernel/Syscalls/Helpers/State.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace ps2_stubs
{
    void serializeCdState(ps2x::StateArchive &archive);
    void serializeMemoryCardState(ps2x::StateArchive &archive);
    void serializeSifState(ps2x::StateArchive &archive);
}

namespace
{
    constexpr char kStateMagic[8] = {'B', 'D', 'R', 'S', 'T', 'A', 'T', 'E'};
    constexpr uint32_t kStateVersion = 3u; // 3: SPU2 voices with volume sweeps and interpolation history

    void serializePendingTransfers(ps2x::StateArchive &archive, std::vector<PS2Memory::PendingTransfer> &transfers)
    {
        archive.sequence(transfers, [](ps2x::StateArchive &a, PS2Memory::PendingTransfer &transfer)
        {
            a.value(transfer.fromScratchpad);
            a.value(transfer.srcAddr);
            a.value(transfer.qwc);
            a.vector(transfer.chainData);
        });
    }

    // The runtime's HLE kernel globals (Syscalls/Helpers/State.h).
    void serializeSyscallState(ps2x::StateArchive &archive)
    {
        archive.tag("SYSC");
        archive.podMap(g_rpc_servers);
        archive.podMap(g_rpc_clients);
        archive.value(g_rpc_initialized);
        archive.value(g_rpc_next_id);
        archive.value(g_rpc_packet_index);
        archive.value(g_rpc_server_index);
        archive.value(g_rpc_active_queue);
        archive.value(g_bootmode_initialized);
        archive.value(g_bootmode_pool_offset);
        archive.podMap(g_bootmode_addresses);
        archive.value(g_tls_index);
        archive.value(g_osd_config_initialized);
        archive.value(g_osd_config_raw);
        archive.value(g_osd_config2_raw);
        uint32_t reservedBase = g_runtime_reserved_base.load(std::memory_order_relaxed);
        archive.value(reservedBase);
        g_runtime_reserved_base.store(reservedBase, std::memory_order_relaxed);
        archive.value(g_ee_kernel_heap_end);
        archive.map(g_sif_modules_by_id, [](ps2x::StateArchive &a, SifModuleRecord &record)
        {
            a.value(record.id);
            a.string(record.path);
            a.string(record.pathKey);
            a.value(record.refCount);
            a.value(record.loaded);
        });
        archive.podMap(g_sif_module_id_by_path);
        archive.value(g_next_sif_module_id);
    }
}

void PS2Memory::serialize(ps2x::StateArchive &archive)
{
    archive.tag("EMEM");
    archive.raw(m_rdram, PS2_RAM_SIZE);
    archive.raw(m_scratchpad, PS2_SCRATCHPAD_SIZE);
    archive.raw(iop_ram, 2u * 1024u * 1024u);
    archive.raw(m_gsVRAM, PS2_GS_VRAM_SIZE);
    archive.raw(m_vu0Code, PS2_VU0_CODE_SIZE);
    archive.raw(m_vu0Data, PS2_VU0_DATA_SIZE);
    archive.raw(m_vu1Code, PS2_VU1_CODE_SIZE);
    archive.raw(m_vu1Data, PS2_VU1_DATA_SIZE);
    archive.value(m_seenGifCopy);
    archive.podMap(m_ioRegisters);
    // GSRegisters holds two lock-free atomics; their bytes are their values.
    archive.raw(&gs_regs, sizeof(gs_regs));
    archive.value(vif0_regs);
    archive.value(vif1_regs);
    archive.value(dma_regs);
    archive.vector(m_tlbEntries);
    archive.value(m_path3Masked);
    archive.value(m_vif1PendingPath2ImageQwc);
    archive.value(m_vif1PendingPath2DirectHl);
    if (!archive.loading() && m_path3FifoHead != 0u && !m_path3MaskedFifo.empty())
    {
        auto &first = m_path3MaskedFifo.front();
        first.erase(first.begin(), first.begin() + static_cast<std::ptrdiff_t>(m_path3FifoHead));
    }
    m_path3FifoHead = 0u;
    archive.sequence(m_path3MaskedFifo, [](ps2x::StateArchive &a, std::vector<uint8_t> &packet) { a.vector(packet); });
    archive.value(m_path3Window);
    archive.value(m_path3WindowReleased);
    archive.value(m_path3WindowBudget);
    serializePendingTransfers(archive, m_pendingGifTransfers);
    serializePendingTransfers(archive, m_pendingVif0Transfers);
    serializePendingTransfers(archive, m_pendingVif1Transfers);
    {
        std::lock_guard<std::mutex> lock(m_completedDmacMutex);
        archive.vector(m_completedDmacCauses);
    }
    archive.value(m_eeTimers);
    m_ipu->serialize(archive);
    if (archive.loading())
    {
        // Decoded and compiled VU programs are caches keyed by these generations.
        markVU0CodeModified();
        markVU1CodeModified();
    }
}

void VU1Interpreter::serialize(ps2x::StateArchive &archive)
{
    archive.tag("VUNT");
    archive.value(m_state);
    archive.value(m_flagPipeline);
    archive.value(m_fdiv);
    archive.value(m_efu);
    archive.value(m_storePipeline);
    archive.value(m_vfWritePipeline);
    archive.value(m_viWritePipeline);
    archive.value(m_accWritePipeline);
    archive.value(m_activeFlags);
    archive.value(m_activeStores);
    archive.value(m_activeVfWrites);
    archive.value(m_activeViWrites);
    archive.value(m_activeAccWrites);
    archive.value(m_xgkick);
    archive.value(m_vfReady);
    archive.value(m_viReady);
    archive.value(m_accReady);
    archive.value(m_vfLatestWrite);
    archive.value(m_viLatestWrite);
    archive.value(m_accLatestWrite);
    archive.value(m_cycle);
    archive.value(m_nextWriteSequence);
    archive.value(m_efuResourceReady);
    archive.value(m_workingClip);
    archive.value(m_currentUpperInstruction);
    archive.value(m_viBranchBackupValue);
    archive.value(m_viBranchBackupReg);
    archive.value(m_viBranchBackupValid);
    archive.value(m_stopRequested);
    archive.value(m_pendingHaltD);
    archive.value(m_pendingHaltT);
    if (archive.loading())
        m_decodedCodeCacheValid = false;
}

void GS::serialize(ps2x::StateArchive &archive)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    archive.tag("GSFE");
    archive.value(m_ctx);
    archive.value(m_prim);
    archive.value(m_primRegister);
    archive.value(m_prmodeRegister);
    archive.value(m_curR);
    archive.value(m_curG);
    archive.value(m_curB);
    archive.value(m_curA);
    archive.value(m_curQ);
    archive.value(m_curS);
    archive.value(m_curT);
    archive.value(m_curU);
    archive.value(m_curV);
    archive.value(m_curFog);
    archive.value(m_fogR);
    archive.value(m_fogG);
    archive.value(m_fogB);
    archive.value(m_prmodecont);
    archive.value(m_pabe);
    archive.value(m_scanmsk);
    archive.value(m_dimx);
    archive.value(m_dthe);
    archive.value(m_colclamp);
    archive.value(m_texa);
    archive.value(m_texclut);
    archive.value(m_bitbltbuf);
    archive.value(m_trxpos);
    archive.value(m_trxreg);
    archive.value(m_trxdir);
    archive.value(m_vtxQueue);
    archive.value(m_vtxCount);
    archive.value(m_vtxIndex);
    std::lock_guard<std::mutex> backendLock(m_backendLifetimeMutex);
    if (!m_backend)
        throw std::runtime_error("save states need a GS backend");
    m_backend->SerializeState(archive);
}

void GifArbiter::serialize(ps2x::StateArchive &archive)
{
    archive.tag("GIFA");
    archive.sequence(m_queue, [](ps2x::StateArchive &a, GifArbiterPacket &packet)
    {
        a.value(packet.pathId);
        a.value(packet.path2DirectHl);
        a.value(packet.path3Image);
        a.vector(packet.data);
    });
}

bool EeScheduler::stateQuiet(std::string &reason) const
{
    if (m_insideInterrupt || m_guestExecuting.load(std::memory_order_acquire))
    {
        reason = "guest code is running";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_eventMutex);
        if (!m_pendingInvocations.empty())
        {
            reason = "a guest callback is queued";
            return false;
        }
    }
    for (const auto &[id, thread] : m_threads)
    {
        if (!thread.invocations.empty() || thread.resumeCompletion || thread.wait.completion)
        {
            reason = "EE thread " + std::to_string(id) + " is inside a callback or an HLE wait";
            return false;
        }
    }
    return true;
}

void EeScheduler::serialize(ps2x::StateArchive &archive)
{
    archive.tag("EESC");
    for (auto &queue : m_readyQueues)
        archive.sequence(queue, [](ps2x::StateArchive &a, int &id) { a.value(id); });
    archive.map(m_threads, [](ps2x::StateArchive &a, GuestThread &thread)
    {
        a.value(thread.id);
        a.value(thread.context);
        a.value(thread.entry);
        a.value(thread.stack);
        a.value(thread.stackSize);
        a.value(thread.gp);
        a.value(thread.attr);
        a.value(thread.option);
        a.value(thread.arg);
        a.value(thread.initialPriority);
        a.value(thread.currentPriority);
        a.value(thread.status);
        a.value(thread.suspendCount);
        a.value(thread.wakeupCount);
        a.value(thread.ownsStack);
        a.value(thread.tlsBase);
        a.value(thread.wait.reason);
        uint32_t kind = static_cast<uint32_t>(thread.wait.payload.index());
        a.value(kind);
        if (a.loading())
        {
            switch (kind)
            {
            case 0: thread.wait.payload = std::monostate{}; break;
            case 1: thread.wait.payload = EeSemaphoreWait{}; break;
            case 2: thread.wait.payload = EeEventFlagWait{}; break;
            case 3: thread.wait.payload = EeVSyncWait{}; break;
            case 4: thread.wait.payload = EeExternalWait{}; break;
            default: throw std::runtime_error("save state has an unknown EE wait kind");
            }
        }
        std::visit([&a](auto &payload)
        {
            if constexpr (!std::is_same_v<std::decay_t<decltype(payload)>, std::monostate>)
                a.value(payload);
        }, thread.wait.payload);
    });
    const auto serializeWaiters = [](ps2x::StateArchive &a, std::deque<int> &waiters)
    {
        a.sequence(waiters, [](ps2x::StateArchive &b, int &id) { b.value(id); });
    };
    archive.map(m_semaphores, [&](ps2x::StateArchive &a, EeSemaphore &sema)
    {
        a.value(sema.id);
        a.value(sema.count);
        a.value(sema.maxCount);
        a.value(sema.initCount);
        a.value(sema.attr);
        a.value(sema.option);
        serializeWaiters(a, sema.waiters);
    });
    archive.map(m_eventFlags, [&](ps2x::StateArchive &a, EeEventFlag &flag)
    {
        a.value(flag.id);
        a.value(flag.attr);
        a.value(flag.option);
        a.value(flag.initBits);
        a.value(flag.bits);
        serializeWaiters(a, flag.waiters);
    });
    archive.podMap(m_alarms);
    archive.podMap(m_intcHandlers);
    archive.podMap(m_dmacHandlers);
    archive.value(m_nextThreadId);
    archive.value(m_nextInvocationThreadId);
    archive.value(m_nextSemaphoreId);
    archive.value(m_nextEventFlagId);
    archive.value(m_nextAlarmId);
    archive.value(m_nextIntcHandlerId);
    archive.value(m_nextDmacHandlerId);
    archive.value(m_intcHeadOrder);
    archive.value(m_intcTailOrder);
    archive.value(m_dmacHeadOrder);
    archive.value(m_dmacTailOrder);
    archive.value(m_enabledIntcMask);
    archive.value(m_enabledDmacMask);
    archive.value(m_currentThreadId);
    archive.value(m_rescheduleRequested);
    archive.value(m_timeSliceExpired);
    archive.value(m_pendingEeTimerInterrupts);
    archive.value(m_eeCycle);
    archive.value(m_deviceCycleDebt);
    archive.value(m_sliceEndCycle);
    {
        std::lock_guard<std::mutex> lock(m_eventMutex);
        archive.sequence(m_events, [](ps2x::StateArchive &a, EeEvent &event) { a.value(event); });
        // Host deadlines (real-time pacing) are kept relative to now.
        const auto now = std::chrono::steady_clock::now();
        archive.sequence(m_deadlines, [now](ps2x::StateArchive &a, ScheduledEvent &scheduled)
        {
            a.value(scheduled.deadlineCycle);
            int64_t remainingNs = std::chrono::duration_cast<std::chrono::nanoseconds>(scheduled.hostDeadline - now).count();
            a.value(remainingNs);
            // A state saved by a deterministic run (no pacing) can hold deadlines minutes ahead of
            // real time: loaded in a paced run, the game would wait for them.
            if (a.loading())
                remainingNs = std::clamp<int64_t>(remainingNs, 0, 1'000'000'000);
            scheduled.hostDeadline = now + std::chrono::nanoseconds(remainingNs);
            a.value(scheduled.event);
            a.value(scheduled.sequence);
        });
    }
    archive.value(m_eventSequence);
    archive.value(m_invocationSequence);
    archive.value(m_vsyncTick);
    // Set once by SetGsCrt at boot: PAL VBlanks every 20 ms instead of 16.7 ms.
    bool pal = m_palVideoTiming.load(std::memory_order_relaxed);
    if (!archive.loading() || archive.version() >= 2u)
        archive.value(pal);
    else
        pal = true; // version 1 lacked this flag; only Burnout Dominator PAL wrote it
    m_palVideoTiming.store(pal, std::memory_order_relaxed);
    archive.value(m_vsyncFlagAddress);
    archive.value(m_vsyncTickAddress);
    archive.value(m_gsVSyncCallback);
    archive.value(m_gsVSyncCallbackGp);
    archive.value(m_gsVSyncCallbackSp);
    archive.podMap(m_invocationStackTops);
    archive.vector(m_freeInvocationStackTops);
    if (archive.loading())
    {
        m_cachedThreadId = 0;
        m_cachedThread = nullptr;
        m_runtime.memory().gs().vsyncTick.store(m_vsyncTick, std::memory_order_release);
        ps2x::continueGuestCount(currentThread(), m_eeCycle);
        updateNextDeadline();
        publishSnapshot();
    }
}

void PS2Runtime::serializeState(ps2x::StateArchive &archive)
{
    // The worker owns VIF1, VU1 and the GS: it must have run everything queued.
    if (m_gsWorker)
        m_gsWorker->sync();
    archive.tag("BDRS");
    m_memory.serialize(archive);
    m_gifArbiter.serialize(archive);
    m_gs.serialize(archive);
    m_vu0.serialize(archive);
    m_vu1.serialize(archive);

    archive.tag("RTIM");
    archive.value(m_cpuContext);
    {
        std::lock_guard<std::mutex> lock(m_eeKernelStateMutex);
        archive.map(m_eeExitHandlers, [](ps2x::StateArchive &a, std::vector<EeExitHandlerRegistration> &handlers) { a.vector(handlers); });
        archive.podMap(m_eeSyscallOverrides);
        std::vector<uint32_t> mirrors(m_eeSyscallMirrorAddresses.begin(), m_eeSyscallMirrorAddresses.end());
        archive.vector(mirrors);
        m_eeSyscallMirrorAddresses = {mirrors.begin(), mirrors.end()};
    }
    {
        std::lock_guard<std::mutex> lock(m_guestHeapMutex);
        archive.vector(m_guestHeapBlocks);
        archive.value(m_guestHeapBase);
        archive.value(m_guestHeapEnd);
        archive.value(m_guestHeapLimit);
        archive.value(m_guestHeapSuggestedBase);
        archive.value(m_guestHeapConfigured);
    }
    {
        std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
        archive.value(m_asyncCallbackStackFloor);
        archive.value(m_asyncCallbackStackTop);
    }
    archive.sequence(m_loadedModules, [](ps2x::StateArchive &a, LoadedModule &module)
    {
        a.string(module.name);
        a.value(module.baseAddress);
        a.value(module.size);
        a.value(module.active);
    });

    m_eeScheduler->serialize(archive);
    m_iopSubsystem->serialize(archive);
    serializeSyscallState(archive);
    ps2_stubs::serializeCdState(archive);
    ps2_stubs::serializeMemoryCardState(archive);
    ps2_stubs::serializeSifState(archive);
    archive.tag("END.");
}

void PS2Runtime::requestSaveState(const std::filesystem::path &file)
{
    std::lock_guard<std::mutex> lock(m_stateRequestMutex);
    m_stateRequest = StateRequest{false, file, 0};
    m_stateRequestPending.store(true, std::memory_order_release);
}

void PS2Runtime::requestLoadState(const std::filesystem::path &file)
{
    std::lock_guard<std::mutex> lock(m_stateRequestMutex);
    m_stateRequest = StateRequest{true, file, 0};
    m_stateRequestPending.store(true, std::memory_order_release);
}

void PS2Runtime::serviceStateRequest()
{
    StateRequest request;
    {
        std::lock_guard<std::mutex> lock(m_stateRequestMutex);
        if (!m_stateRequest)
        {
            m_stateRequestPending.store(false, std::memory_order_release);
            return;
        }
        std::string reason;
        bool quiet = m_eeScheduler->stateQuiet(reason);
        if (quiet && !m_iopSubsystem->idle())
        {
            quiet = false;
            reason = "IOP code is running";
        }
        if (quiet && m_memory.m_ipu->busy())
        {
            quiet = false;
            reason = "the IPU is decoding";
        }
        if (quiet && !m_vfs.descriptors().empty())
        {
            quiet = false;
            reason = "a host file is open";
        }
        if (!quiet)
        {
            // Busy machines reach a quiet point within a few dispatches; say why when one does not.
            if (++m_stateRequest->attempts == 200000u)
                std::cout << "[state] waiting for a quiet point: " << reason << std::endl;
            return;
        }
        request = std::move(*m_stateRequest);
        m_stateRequest.reset();
        m_stateRequestPending.store(false, std::memory_order_release);
    }

    const uint64_t vsync = m_eeScheduler->currentVSyncTick();
    if (!request.load)
    {
        try
        {
            ps2x::StateArchive archive = ps2x::StateArchive::writer();
            serializeState(archive);
            std::error_code ec;
            if (request.file.has_parent_path())
                std::filesystem::create_directories(request.file.parent_path(), ec);
            const std::filesystem::path temporary = request.file.string() + ".tmp";
            {
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                const uint64_t size = archive.data().size();
                out.write(kStateMagic, sizeof(kStateMagic));
                out.write(reinterpret_cast<const char *>(&kStateVersion), sizeof(kStateVersion));
                out.write(reinterpret_cast<const char *>(&size), sizeof(size));
                out.write(reinterpret_cast<const char *>(archive.data().data()), static_cast<std::streamsize>(size));
                if (!out)
                    throw std::runtime_error("cannot write " + temporary.string());
            }
            std::filesystem::rename(temporary, request.file);
            std::cout << "[state] saved " << request.file.string() << " at vsync " << vsync << " ("
                      << archive.data().size() / (1024u * 1024u) << " MiB)" << std::endl;
        }
        catch (const std::exception &error)
        {
            std::cout << "[state] save failed: " << error.what() << std::endl;
        }
        return;
    }

    std::vector<uint8_t> payload;
    uint32_t version = 0u;
    try
    {
        std::ifstream in(request.file, std::ios::binary);
        char magic[sizeof(kStateMagic)] = {};
        uint64_t size = 0u;
        in.read(magic, sizeof(magic));
        in.read(reinterpret_cast<char *>(&version), sizeof(version));
        in.read(reinterpret_cast<char *>(&size), sizeof(size));
        if (!in || std::memcmp(magic, kStateMagic, sizeof(magic)) != 0)
            throw std::runtime_error(request.file.string() + " is not a save state");
        if (version < 1u || version > kStateVersion)
            throw std::runtime_error("save state version " + std::to_string(version) + " is not supported");
        payload.resize(static_cast<size_t>(size));
        in.read(reinterpret_cast<char *>(payload.data()), static_cast<std::streamsize>(size));
        if (!in)
            throw std::runtime_error("save state is truncated");
    }
    catch (const std::exception &error)
    {
        // Nothing was changed yet: the game keeps running.
        std::cout << "[state] load failed: " << error.what() << std::endl;
        return;
    }
    try
    {
        ps2x::StateArchive archive = ps2x::StateArchive::reader(std::move(payload), version);
        serializeState(archive);
        if (!archive.atEnd())
            throw std::runtime_error("save state has trailing data");
        std::cout << "[state] loaded " << request.file.string() << " (vsync " << m_eeScheduler->currentVSyncTick()
                  << ")" << std::endl;
    }
    catch (const std::exception &error)
    {
        // A partial restore leaves an inconsistent machine: stop rather than run it.
        std::cout << "[state] load failed midway, stopping: " << error.what() << std::endl;
        requestStop();
    }
}
