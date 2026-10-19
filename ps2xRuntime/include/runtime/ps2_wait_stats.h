#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

// Wall time the game thread spends blocked, by cause, for the [status] line.
// Waits on other threads (presentation, GS workers) are not counted.
namespace ps2_wait_stats
{
    enum class Wait : size_t
    {
        EeIdle,   // no EE thread ready: waiting for the next event
        EePacing, // an event is due in EE cycles but not yet in host time
        GsDrain,  // waiting for the GS raster workers
        GsLock,   // GS backend held by another thread (presentation)
        GsWorker, // the GS worker thread is still on earlier frames
        Count
    };

    inline std::array<std::atomic<uint64_t>, static_cast<size_t>(Wait::Count)> g_waitNs{};
    inline thread_local bool t_gameThread = false;

    class Scope
    {
    public:
        explicit Scope(Wait wait) noexcept : m_wait(wait), m_counted(t_gameThread)
        {
            if (m_counted)
                m_start = std::chrono::steady_clock::now();
        }
        ~Scope()
        {
            if (!m_counted)
                return;
            const auto elapsed = std::chrono::steady_clock::now() - m_start;
            g_waitNs[static_cast<size_t>(m_wait)].fetch_add(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
                std::memory_order_relaxed);
        }
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;

    private:
        Wait m_wait;
        bool m_counted;
        std::chrono::steady_clock::time_point m_start{};
    };
}
