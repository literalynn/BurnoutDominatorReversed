#pragma once

#include "iop_cpu.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps2x::iop::detail
{
    class IopMemory;

    enum class IopThreadState : uint8_t
    {
        Dormant,
        Ready,
        Running,
        Sleep,
        Delay,
        Semaphore,
        EventFlag,
        Suspended,
        Dead,
    };

    struct IopThread
    {
        int id = 0;
        IopThreadState state = IopThreadState::Dormant;
        IopCpuState cpu;
        uint32_t entry = 0;
        uint32_t stackBase = 0;
        uint32_t stackSize = 0;
        uint32_t priority = 0x40;
        uint32_t initialPriority = 0x40;
        uint32_t option = 0;
        uint32_t attr = 0;
        uint64_t wakeCycle = 0;
        int waitId = 0;
        uint32_t waitBits = 0;
        uint32_t waitMode = 0;
        uint32_t waitResultAddress = 0;
        int wakeupCount = 0;
    };

    class IopKernel
    {
    public:
        explicit IopKernel(IopMemory &memory) noexcept;

        void reset();

        [[nodiscard]] bool dispatchThreadImport(uint16_t ordinal, IopCpuState &cpu, uint64_t currentCycle);
        [[nodiscard]] bool dispatchSemaphoreImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] bool dispatchEventImport(uint16_t ordinal, IopCpuState &cpu);

        [[nodiscard]] int createInternalEventFlag(uint32_t attr, uint32_t option, uint32_t bits);
        [[nodiscard]] bool setInternalEventFlag(int id, uint32_t bits);

        void sleepCurrent(IopCpuState &cpu);
        void delayCurrentUntil(uint64_t wakeCycle, IopCpuState &cpu);

        [[nodiscard]] IopThread *beginNextReady(uint64_t currentCycle);
        [[nodiscard]] uint64_t nextWakeCycle(uint64_t fallback) const;
        void endTimeslice(IopThread &thread, uint32_t returnSentinel);
        void cleanupDeadThreads();
        [[nodiscard]] IopThread *createInternalCall(uint32_t entry, uint32_t gp, uint32_t returnAddress);
        void removeInternalCall(int id);
        [[nodiscard]] IopThread *findInternalCall(int id);
        void terminateThreadsInRange(uint32_t base, uint32_t size);

        [[nodiscard]] size_t threadCount() const noexcept { return m_threads.size(); }

        // One line per thread, semaphore and event flag, for diagnostics. `symbolize`
        // turns a guest address into text such as "RWA.IRX+0x1234".
        void describe(std::vector<std::string> &lines,
                      const std::function<std::string(uint32_t)> &symbolize) const;

        // Diagnostics: keep the last distinct thread/semaphore/event-flag operations
        // (shown by describe()). `symbolize` turns the caller's address into text.
        void enableLog(std::function<std::string(uint32_t)> symbolize)
        {
            m_symbolize = std::move(symbolize);
            m_logEnabled = true;
        }

    private:
        [[nodiscard]] bool dispatchThreadImportImpl(uint16_t ordinal, IopCpuState &cpu, uint64_t currentCycle);
        [[nodiscard]] bool dispatchSemaphoreImportImpl(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] bool dispatchEventImportImpl(uint16_t ordinal, IopCpuState &cpu);

        struct LogArgs
        {
            uint32_t a0 = 0;
            uint32_t a1 = 0;
            uint32_t a2 = 0;
            uint32_t ra = 0;
            int thread = 0;
        };
        [[nodiscard]] LogArgs beginLog(const IopCpuState &cpu) const;
        void endLog(const char *library, uint16_t ordinal, const IopCpuState &cpu, const LogArgs &args);

        struct Semaphore
        {
            int id = 0;
            uint32_t attr = 0;
            uint32_t option = 0;
            int current = 0;
            int maximum = 1;
        };

        struct EventFlag
        {
            int id = 0;
            uint32_t bits = 0;
            uint32_t attr = 0;
            uint32_t option = 0;
        };

        [[nodiscard]] bool referThreadStatus(int id, uint32_t outputAddress);
        void wakeOneSemaphore(int id);
        [[nodiscard]] static bool eventSatisfied(const EventFlag &event, uint32_t bits, uint32_t mode);
        void wakeEventWaiters(EventFlag &event);

        IopMemory &m_memory;
        std::map<int, IopThread> m_threads;
        std::vector<uint32_t> m_freeCallStacks;
        bool m_deadThreadPending = false; // a thread entered the Dead state since the last cleanup
        std::map<int, Semaphore> m_semaphores;
        std::map<int, EventFlag> m_eventFlags;
        uint32_t m_nextThreadId = 1;
        uint32_t m_nextSemaphoreId = 1;
        uint32_t m_nextEventFlagId = 1;
        IopThread *m_currentThread = nullptr;

        bool m_logEnabled = false;
        std::function<std::string(uint32_t)> m_symbolize;
        struct LogEntry
        {
            uint64_t firstSequence = 0;
            uint64_t lastSequence = 0;
            uint64_t count = 0;
            std::string text;
        };
        std::vector<LogEntry> m_log;
        std::unordered_map<std::string, size_t> m_logIndex;
        uint64_t m_logSequence = 0;
    };
}
