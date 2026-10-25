#include "ps2_vu1_exec.inl"
#include "ps2_vu1_program.h"
#include "runtime/ps2_vu1.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "ps2_vu1_detail.h"
#include "ps2_vu1_capture.h"

#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <string>
#include <vector>
#include <ps2_log.h>
#if defined(_MSC_VER) && defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace
{
    void recordUncompiledBlock(const char *path, unsigned unit, const uint8_t *code)
    {
        static std::mutex mutex;
        const std::lock_guard lock(mutex);
        static std::ofstream output(path);
        static std::set<std::pair<unsigned, std::array<uint64_t, 4>>> recorded;
        if (!output)
            return;
        if (recorded.empty())
            output << "VU-BLOCKS 1\n";
        std::array<uint64_t, 4> words;
        std::memcpy(words.data(), code, sizeof(words));
        if (!recorded.emplace(unit, words).second)
            return;
        output << std::dec << unit;
        for (const auto word : words)
            output << ' ' << std::hex << word;
        output << '\n';
        output.flush();
    }

}

VU1Interpreter::VU1Interpreter(Unit unit)
    : m_unit(unit)
{
    const char *execution = std::getenv("PS2_VU_EXECUTION");
    m_useCompiledExecution = execution == nullptr || std::strcmp(execution, "interpreter") != 0;
    reset();
}

void VU1Interpreter::resetScheduler()
{
    // With nothing in flight the pipeline entries are read only through their active
    // bits, and a latest-write sequence only against a queued write's, which is newer
    // as the count goes on: clearing them at every VU0 call changes nothing.
    const bool idle = !m_fdiv.valid && !m_efu[0].valid && !m_efu[1].valid && !m_xgkick.active &&
                      (m_activeFlags | m_activeStores | m_activeVfWrites | m_activeViWrites | m_activeAccWrites) == 0u;
    if (!idle)
    {
        m_flagPipeline = {};
        m_fdiv = {};
        m_efu = {};
        m_storePipeline = {};
        m_vfWritePipeline = {};
        m_viWritePipeline = {};
        m_accWritePipeline = {};
    }
    m_activeFlags = m_activeStores = m_activeVfWrites = m_activeViWrites = m_activeAccWrites = 0u;
    // Reset the PATH1 transfer state but not its 64 KiB packet buffer: every
    // byte is copied in before it is read or submitted, and clearing it here
    // ran at every MSCAL (about 1 GB/s of memset in DQ8's open world).
    m_xgkick.sourceAddress = 0;
    m_xgkick.totalBytes = 0;
    m_xgkick.copiedBytes = 0;
    m_xgkick.currentTagEnd = 0;
    m_xgkick.cycleCredit = 0;
    m_xgkick.issueCycle = 0;
    m_xgkick.active = false;
    m_xgkick.currentTagEop = false;
    m_vfReady = {};
    m_viReady = {};
    m_accReady = {};
    if (!idle)
    {
        m_vfLatestWrite = {};
        m_viLatestWrite = {};
        m_accLatestWrite = {};
        m_nextWriteSequence = 0;
    }
    m_efuResourceReady = 0;
    m_workingClip = m_state.clip;
    m_viBranchBackupValue = 0;
    m_viBranchBackupReg = 0;
    m_viBranchBackupValid = false;
    m_stopRequested = false;
    m_pendingHaltD = false;
    m_pendingHaltT = false;
}

void VU1Interpreter::reset()
{
    std::memset(&m_state, 0, sizeof(m_state));
    m_state.vf[0][3] = 1.0f;
    m_state.q = 1.0f;
    m_state.r = 0x3F800000u;
    m_cycle = 0;
    resetScheduler();
    // VU0 resets for every micro call; VU1 only at startup, and in tests.
    if (m_unit == Unit::VU1)
        ps2_vu_program::forgetRoutines();
}

void VU1Interpreter::progressXgkick()
{
    if (!m_xgkick.active || !m_activeVuData || m_activeVuDataSize == 0u)
        return;

    ++m_xgkick.cycleCredit;
    while (m_xgkick.active && m_xgkick.cycleCredit >= 2u)
    {
        m_xgkick.cycleCredit -= 2u;
        if (m_xgkick.copiedBytes > XgkickPipeline::kBufferSize - 16u)
        {
            reportReservedInstruction(false, 0xFFFFFFFBu);
            m_xgkick.active = false;
            return;
        }

        const uint32_t qwordOffset = m_xgkick.copiedBytes;
        // Within a tag's data, every qword the credit allows goes in one copy;
        // the state ends as one qword per pass would leave it.
        uint32_t qwords = 1u;
        if (m_xgkick.currentTagEnd != 0u)
            qwords = std::min(std::min(1u + m_xgkick.cycleCredit / 2u, (m_xgkick.currentTagEnd - qwordOffset) / 16u),
                              m_activeVuDataSize / 16u);
        m_xgkick.cycleCredit -= 2u * (qwords - 1u);
        const uint32_t bytes = qwords * 16u;
        const uint32_t source = (m_xgkick.sourceAddress + qwordOffset) % m_activeVuDataSize;
        if (source + bytes <= m_activeVuDataSize && qwords == 1u)
            std::memcpy(m_xgkick.packet.data() + qwordOffset, m_activeVuData + source, 16u);
        else
        {
            const uint32_t firstBytes = std::min(bytes, m_activeVuDataSize - source);
            std::memcpy(m_xgkick.packet.data() + qwordOffset, m_activeVuData + source, firstBytes);
            if (firstBytes < bytes)
                std::memcpy(m_xgkick.packet.data() + qwordOffset + firstBytes,
                            m_activeVuData, bytes - firstBytes);
        }
        m_xgkick.copiedBytes += bytes;

        if (m_xgkick.currentTagEnd == 0u)
        {
            uint64_t tagLo = 0;
            std::memcpy(&tagLo, m_xgkick.packet.data() + qwordOffset, sizeof(tagLo));
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            // BDR_VU_BADKICK_DIR=dir: VU1 data memory and state at the first kick
            // whose first tag has only NLOOP/EOP (no PRE, PRIM, FLG, NREG) but a
            // non-zero REGS word, and at the last kick before it.
            static const char *badKickDir = std::getenv("BDR_VU_BADKICK_DIR");
            if (badKickDir && m_unit == Unit::VU1 && qwordOffset == 0u)
            {
                static std::vector<uint8_t> lastGood;
                static VU1State lastGoodState{};
                static uint32_t lastGoodSource = 0;
                static bool done = false;
                uint64_t tagHi = 0;
                std::memcpy(&tagHi, m_xgkick.packet.data() + 8u, sizeof(tagHi));
                // Also a PACKED packet whose data runs past qword 1006 of VU1 memory,
                // where Burnout's microcode keeps its GIF tag template.
                const uint32_t start = m_xgkick.sourceAddress / 16u;
                const bool overflows = format == 0u && start < 1006u && start + 1u + nloop * nreg > 1006u;
                const bool bad = ((tagLo >> 32) == 0u && tagHi != 0u && nloop != 0u) || overflows;
                const auto dump = [&](const char *name, const uint8_t *bytes, size_t size, const VU1State &state, uint32_t sourceAddress) {
                    const std::string stem = std::string(badKickDir) + "/" + name;
                    if (FILE *file = std::fopen((stem + ".data").c_str(), "wb"))
                    {
                        std::fwrite(bytes, 1, size, file);
                        std::fclose(file);
                    }
                    if (FILE *file = std::fopen((stem + ".txt").c_str(), "w"))
                    {
                        std::fprintf(file, "source=0x%x pc=0x%x top=0x%x itop=0x%x\n", sourceAddress, state.pc, state.top, state.itop);
                        for (int i = 0; i < 16; ++i)
                            std::fprintf(file, "vi%d=0x%x\n", i, static_cast<uint32_t>(state.vi[i]) & 0xFFFFu);
                        for (int i = 0; i < 32; ++i)
                        {
                            uint32_t words[4];
                            std::memcpy(words, state.vf[i], sizeof(words));
                            std::fprintf(file, "vf%d=%08x %08x %08x %08x\n", i, words[0], words[1], words[2], words[3]);
                        }
                        std::fclose(file);
                    }
                };
                if (!done && bad)
                {
                    done = true;
                    std::error_code ignored;
                    std::filesystem::create_directories(badKickDir, ignored);
                    std::printf("[vu1] bad XGKICK (%s) at pc=0x%x vsync=%llu, dumped to %s\n", overflows ? "overflow" : "tag", m_state.pc,
                                m_activeMemory ? static_cast<unsigned long long>(m_activeMemory->gs().vsyncTick.load()) : 0ull, badKickDir);
                    dump("bad", m_activeVuData, m_activeVuDataSize, m_state, m_xgkick.sourceAddress);
                    if (m_activeMemory && m_activeMemory->getVU1Code())
                        if (FILE *file = std::fopen((std::string(badKickDir) + "/bad.code").c_str(), "wb"))
                        {
                            std::fwrite(m_activeMemory->getVU1Code(), 1, PS2_VU1_CODE_SIZE, file);
                            std::fclose(file);
                        }
                    if (m_activeMemory && m_activeMemory->getRDRAM())
                        if (FILE *file = std::fopen((std::string(badKickDir) + "/bad.rdram").c_str(), "wb"))
                        {
                            std::fwrite(m_activeMemory->getRDRAM(), 1, PS2_RAM_SIZE, file);
                            std::fclose(file);
                        }
                    if (!lastGood.empty())
                        dump("good", lastGood.data(), lastGood.size(), lastGoodState, lastGoodSource);
                    std::fprintf(stderr, "[vu-badkick] dumped to %s\n", badKickDir);
                }
                else if (!done)
                {
                    lastGood.assign(m_activeVuData, m_activeVuData + m_activeVuDataSize);
                    lastGoodState = m_state;
                    lastGoodSource = m_xgkick.sourceAddress;
                }
            }

            uint64_t tagBytes = 16u;
            if (format == 0u)
                tagBytes += static_cast<uint64_t>(nloop) * nreg * 16u;
            else if (format == 1u)
                tagBytes += ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u;
            else // IMAGE, and FLG 3, which the GS manual defines as working like IMAGE
                tagBytes += static_cast<uint64_t>(nloop) * 16u;

            if (tagBytes > XgkickPipeline::kBufferSize - qwordOffset)
            {
                reportReservedInstruction(false, 0xFFFFFFFBu);
                m_xgkick.active = false;
                return;
            }
            m_xgkick.currentTagEnd = qwordOffset + static_cast<uint32_t>(tagBytes);
            m_xgkick.currentTagEop = ((tagLo >> 15) & 1u) != 0u;
            if (m_xgkick.currentTagEop)
                m_xgkick.totalBytes = m_xgkick.currentTagEnd;
        }

        if (m_xgkick.copiedBytes >= m_xgkick.currentTagEnd)
        {
            if (m_xgkick.currentTagEop)
                finishXgkick();
            else
            {
                // The next transferred qword is another GIFtag.
                m_xgkick.currentTagEnd = 0u;
                m_xgkick.currentTagEop = false;
            }
        }
    }
}

void VU1Interpreter::finishXgkick()
{
    if (!m_xgkick.active)
        return;

    if (m_activeMemory)
        m_activeMemory->submitGifPacket(GifPathId::Path1, m_xgkick.packet.data(), m_xgkick.totalBytes);
    else if (m_activeGs)
        m_activeGs->processGIFPacket(m_xgkick.packet.data(), m_xgkick.totalBytes, 1u);
    m_xgkick.active = false;
}

void VU1Interpreter::startXgkick(uint32_t qwordAddress)
{
    if (m_unit != Unit::VU1 || !m_activeVuData || m_activeVuDataSize < 16u)
        return;

    const uint32_t sourceAddress = (qwordAddress * 16u) % m_activeVuDataSize;
    // Every submitted byte is copied before use; clearing the packet buffer
    // on each kick adds 64 KiB of unrelated writes.
    m_xgkick.totalBytes = 0;
    m_xgkick.copiedBytes = 0;
    m_xgkick.currentTagEnd = 0;
    m_xgkick.currentTagEop = false;
    m_xgkick.active = true;
    m_xgkick.sourceAddress = sourceAddress;
    m_xgkick.cycleCredit = 1u; // XGKICK's issue cycle counts toward PATH1.
    m_xgkick.issueCycle = m_cycle;
}

bool VU1Interpreter::pipelinesPending() const
{
    if (m_fdiv.valid || m_xgkick.active)
        return true;
    for (const ScalarPipelineEntry &entry : m_efu)
        if (entry.valid)
            return true;
    return (m_activeFlags | m_activeStores | m_activeVfWrites | m_activeViWrites | m_activeAccWrites) != 0u;
}

void VU1Interpreter::flushPipelines()
{
    while (pipelinesPending())
    {
        // Once PATH1 is all that is left, nothing else touches VU memory or the
        // registers: the cycles the current GIF tag still needs pass at once,
        // which copies the same qwords and ends on the same cycle as one at a time.
        if (m_xgkick.active && m_activeVuData && !m_fdiv.valid && !m_efu[0].valid && !m_efu[1].valid &&
            (m_activeFlags | m_activeStores | m_activeVfWrites | m_activeViWrites | m_activeAccWrites) == 0u)
        {
            const uint32_t qwords =
                m_xgkick.currentTagEnd == 0u ? 1u : (m_xgkick.currentTagEnd - m_xgkick.copiedBytes) / 16u;
            const uint32_t cycles = 2u * qwords > m_xgkick.cycleCredit ? 2u * qwords - m_xgkick.cycleCredit : 1u;
            m_cycle += cycles;
            m_state.cycles = m_cycle;
            m_xgkick.cycleCredit += cycles - 1u; // progressXgkick adds the last cycle's
            progressXgkick();
            continue;
        }
        advanceOneCycle();
    }
}

uint64_t VU1Interpreter::calculatePairReadyCycle(const DecodedInstructionPair &decoded) const
{
    return calculatePairReadyCycleInline(decoded);
}

void VU1Interpreter::markPairWrites(const DecodedInstructionPair &decoded)
{
    markPairWritesInline(decoded);
}

VU1Interpreter::DecodedInstructionPair VU1Interpreter::decodeInstructionPair(const uint8_t *vuCode, uint32_t pc) const
{
    uint32_t lower, upper;
    std::memcpy(&lower, vuCode + pc, sizeof(lower));
    std::memcpy(&upper, vuCode + pc + sizeof(lower), sizeof(upper));
    return decodeInstructionWords(lower, upper, m_unit);
}

VU1Interpreter::DecodedInstructionPair VU1Interpreter::getDecodedInstructionPairForPc(
    const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory, uint32_t pc)
{
    if ((pc & 7u) != 0u)
        return decodeInstructionPair(vuCode, pc);

    const bool trackedVu1Code = memory != nullptr &&
                                ((m_unit == Unit::VU1 && vuCode == memory->getVU1Code()) ||
                                 (m_unit == Unit::VU0 && vuCode == memory->getVU0Code()));
    if (!trackedVu1Code)
        return decodeInstructionPair(vuCode, pc);

    const uint64_t generation = m_unit == Unit::VU1 ? memory->getVU1CodeGeneration() : memory->getVU0CodeGeneration();
    if (!m_decodedCodeCacheValid ||
        m_cachedVuCode != vuCode ||
        m_cachedMemory != memory ||
        m_cachedCodeSize != codeSize ||
        m_cachedCodeGeneration != generation)
    {
        rebuildDecodedCodeCache(vuCode, codeSize, memory, generation);
    }
    const uint32_t pairIndex = pc / 8u;
    if (pairIndex >= kMaxDecodedPairs)
        return decodeInstructionPair(vuCode, pc);
    return m_decodedCodeCache[pairIndex];
}

void VU1Interpreter::reportReservedInstruction(bool upper, uint32_t instruction)
{
    RUNTIME_ERROR(
        "[VU" << (m_unit == Unit::VU1 ? "1" : "0")
              << " reserved " << (upper ? "upper" : "lower")
              << "] cycle=" << m_cycle
              << " pc=0x" << std::hex << m_state.pc
              << " instruction=0x" << instruction
              << std::dec << '\n');
    m_stopRequested = true;
}

void VU1Interpreter::execute(uint8_t *vuCode, uint32_t codeSize,
                             uint8_t *vuData, uint32_t dataSize,
                             GS &gs, PS2Memory *memory,
                             uint32_t startPC, uint32_t top, uint32_t itop,
                             uint32_t maxCycles)
{
    resetScheduler();
    m_state.pc = startPC & microAddressMask();
    m_state.ebit = false;
    m_state.haltAfterDelaySlot = false;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    m_state.top = top;
    m_state.itop = itop;
    m_state.branchPending = false;
    m_state.branchTarget = 0;
    m_state.branchDelay = 0;
    m_state.vf[0][0] = 0.0f;
    m_state.vf[0][1] = 0.0f;
    m_state.vf[0][2] = 0.0f;
    m_state.vf[0][3] = 1.0f;
    static const char *captureDirectory = std::getenv("PS2_VU_CAPTURE_DIR");
    if (captureDirectory && *captureDirectory && memory) {
        // execute() has an empty pipeline; resumed states need a fuller snapshot.
        const uint64_t generation = m_unit == Unit::VU1
            ? memory->getVU1CodeGeneration() : memory->getVU0CodeGeneration();
        ps2_vu_detail::captureProgramStart(captureDirectory, m_unit == Unit::VU1 ? 1u : 0u,
                            vuCode, codeSize, generation, vuData, dataSize, m_state);
    }
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
}

void VU1Interpreter::resume(uint8_t *vuCode, uint32_t codeSize,
                            uint8_t *vuData, uint32_t dataSize,
                            GS &gs, PS2Memory *memory,
                            uint32_t top, uint32_t itop, uint32_t maxCycles)
{
    m_state.top = top;
    m_state.itop = itop;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    static const char *captureDirectory = std::getenv("PS2_VU_CAPTURE_DIR");
    // A drained continuation has no hidden writes or branch forwarding to serialize.
    if (captureDirectory && *captureDirectory && memory && !pipelinesPending() &&
        !m_viBranchBackupValid && !m_state.branchPending && !m_stopRequested)
        ps2_vu_detail::captureProgramStart(captureDirectory, m_unit == Unit::VU1 ? 1u : 0u,
            vuCode, codeSize, m_unit == Unit::VU1 ? memory->getVU1CodeGeneration() : memory->getVU0CodeGeneration(),
            vuData, dataSize, m_state);
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
}

#ifdef PS2X_VU_AOT_EXTERN_INCLUDE
#include PS2X_VU_AOT_EXTERN_INCLUDE
#endif

VU1Interpreter::CompiledBlock VU1Interpreter::findCompiledBlock(
    const uint8_t *code, uint32_t size, Unit unit)
{
#ifdef PS2X_VU_AOT_INCLUDE
    struct Entry {
        Unit unit;
        std::array<uint64_t, 32> words;
        CompiledBlock run;
        uint32_t bytes;
    };
    static const Entry entries[] = {
#include PS2X_VU_AOT_INCLUDE
    };
    if (size < 8u)
        return nullptr;
    static const bool pairsOnly = [] {
        const char *mode = std::getenv("PS2_VU_EXECUTION");
        return mode && std::strcmp(mode, "pairs") == 0;
    }();
    uint64_t first;
    std::memcpy(&first, code, sizeof(first));
    const auto *entry = std::lower_bound(std::begin(entries), std::end(entries), first,
        [](const Entry &entry, uint64_t word) { return entry.words[0] < word; });
    CompiledBlock pair = nullptr;
    CompiledBlock block = nullptr;
    uint32_t longest = 0u;
    for (; entry != std::end(entries) && entry->words[0] == first; ++entry) {
        if (entry->unit != unit)
            continue;
        if (entry->bytes == 8u)
            pair = entry->run;
        else if (!pairsOnly && entry->bytes > longest && size >= entry->bytes &&
                 std::memcmp(code, entry->words.data(), entry->bytes) == 0)
        {
            block = entry->run;
            longest = entry->bytes;
        }
    }
    return block ? block : pair;
#endif
    return nullptr;
}

void VU1Interpreter::run(uint8_t *vuCode, uint32_t codeSize,
                         uint8_t *vuData, uint32_t dataSize,
                         GS &gs, PS2Memory *memory, uint32_t maxCycles)
{
    static const bool profileExecution = std::getenv("PS2_VU_PROFILE_EXECUTION") != nullptr;
    static const char *blockProfile = std::getenv("PS2_VU_AOT_PROFILE");
    const uint64_t pairsBefore = m_compiledPairsExecuted + m_interpretedPairsExecuted;
    m_activeVuData = vuData;
    m_activeVuDataSize = dataSize;
    m_activeGs = &gs;
    m_activeMemory = memory;

    // VU rounding, toward zero, and on x86 denormal operands read as zero
    // (PS2_VU_HOST_DAZ). The game thread already runs that way (EeScheduler::run), so a
    // VU0 call there leaves MXCSR alone. With MSVC on x86-64
    // floating point is SSE only (long double is double): MXCSR is set inline, without
    // the C library's x87 round trip. Elsewhere long double is x87, which the exact paths use.
#if defined(_MSC_VER) && defined(_M_X64)
    constexpr unsigned vuCsrMask = _MM_ROUND_MASK | (PS2_VU_HOST_DAZ ? _MM_DENORMALS_ZERO_MASK : 0u);
    constexpr unsigned vuCsr = _MM_ROUND_TOWARD_ZERO | (PS2_VU_HOST_DAZ ? _MM_DENORMALS_ZERO_ON : 0u);
    const unsigned previousCsr = _mm_getcsr();
    const bool useVuRounding = (previousCsr & vuCsrMask) != vuCsr;
    if (useVuRounding)
        _mm_setcsr((previousCsr & ~vuCsrMask) | vuCsr);
#else
    const int previousRoundingMode = std::fegetround();
    const bool changeRounding = previousRoundingMode != FE_TOWARDZERO;
    const bool useVuRounding = changeRounding && std::fesetround(FE_TOWARDZERO) == 0;
#if PS2_VU_HOST_DAZ
    const unsigned previousDaz = _MM_GET_DENORMALS_ZERO_MODE();
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
#endif
    const uint64_t budgetEnd = m_cycle + maxCycles;
    bool programEnded = false;
    const bool trackedCode = memory != nullptr && codeSize <= kMaxDecodedPairs * 8u &&
        vuCode == (m_unit == Unit::VU1 ? memory->getVU1Code() : memory->getVU0Code());
    if (trackedCode && (!m_decodedCodeCacheValid || m_cachedVuCode != vuCode ||
                        m_cachedMemory != memory || m_cachedCodeSize != codeSize))
        rebuildDecodedCodeCache(vuCode, codeSize, memory,
            m_unit == Unit::VU1 ? memory->getVU1CodeGeneration() : memory->getVU0CodeGeneration());
    commitReadyPipelines();
    // A drained VU at the entry of a compiled program runs it whole.
    if (trackedCode && m_useCompiledExecution &&
        budgetEnd - m_cycle >= ps2_vu_program::kMinimumBudget && ps2_vu_program::Access::drained(*this))
    {
        const uint64_t generation = m_unit == Unit::VU1 ? memory->getVU1CodeGeneration()
                                                        : memory->getVU0CodeGeneration();
        if (const auto routine = ps2_vu_program::find(vuCode, codeSize, m_state.pc, generation, m_unit))
            programEnded = ps2_vu_program::run(*this, vuCode, codeSize, budgetEnd, routine);
        else
            ps2_vu_program::recordMissing(vuCode, codeSize, m_state.pc);
    }
    while (!programEnded && m_cycle < budgetEnd && !m_stopRequested)
    {
        // advanceOneCycle already committed this boundary, including stalls.
        if (m_state.pc + 8u > codeSize)
            break;

        DecodedInstructionPair uncached;
        const DecodedInstructionPair *pair = nullptr;
        if (trackedCode && (m_state.pc & 7u) == 0u)
        {
            const uint64_t generation = m_unit == Unit::VU1
                ? memory->getVU1CodeGeneration() : memory->getVU0CodeGeneration();
            if (generation != m_cachedCodeGeneration)
                rebuildDecodedCodeCache(vuCode, codeSize, memory, generation);
            pair = &m_decodedCodeCache[m_state.pc / 8u];
        }
        else
        {
            uncached = getDecodedInstructionPairForPc(vuCode, codeSize, memory, m_state.pc);
            pair = &uncached;
        }
        const DecodedInstructionPair &decoded = *pair;
        const auto compiled = trackedCode && (m_state.pc & 7u) == 0u && m_useCompiledExecution
            ? m_compiledCodeCache[m_state.pc / 8u] : nullptr;
        if (compiled)
            programEnded = compiled(*this, budgetEnd);
        else
        {
            if (blockProfile && m_state.pc + 32ull <= codeSize)
                recordUncompiledBlock(blockProfile, m_unit == Unit::VU1 ? 1u : 0u,
                                      vuCode + m_state.pc);
            programEnded = runDecodedPair<false>(decoded, budgetEnd, codeSize);
        }
        if (programEnded)
            break;
    }

    if (programEnded)
    {
        flushPipelines();
        m_state.ebit = false;
        m_state.haltAfterDelaySlot = false;
        m_pendingHaltD = false;
        m_pendingHaltT = false;
    }
    m_state.cycles = m_cycle;
#if defined(_MSC_VER) && defined(_M_X64)
    if (useVuRounding) // the exception flags raised meanwhile stay, as with fesetround
        _mm_setcsr((_mm_getcsr() & ~vuCsrMask) | (previousCsr & vuCsrMask));
#else
    if (useVuRounding && previousRoundingMode != -1)
        std::fesetround(previousRoundingMode);
#if PS2_VU_HOST_DAZ
    _MM_SET_DENORMALS_ZERO_MODE(previousDaz);
#endif
#endif
    const uint64_t pairsAfter = m_compiledPairsExecuted + m_interpretedPairsExecuted;
    if (ps2_vu_detail::profileRegions)
        ps2_vu_detail::regionCounters.total[m_unit == Unit::VU1 ? 1u : 0u] += pairsAfter - pairsBefore;
    if (profileExecution && (pairsBefore >> 24u) != (pairsAfter >> 24u))
        std::fprintf(stderr, "[VU%u execution] native=%llu fallback=%llu (%.1f%% native)\n",
                     m_unit == Unit::VU1 ? 1u : 0u,
                     static_cast<unsigned long long>(m_compiledPairsExecuted),
                     static_cast<unsigned long long>(m_interpretedPairsExecuted),
                     100.0 * m_compiledPairsExecuted / pairsAfter);
}
