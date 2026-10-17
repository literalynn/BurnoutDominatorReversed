#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

// Runs VIF1, VU1 and the GIF/GS submission on a thread of their own, in the order the
// game thread queues them. The game thread copies DMA data when it queues a job, so the
// EE can reuse its buffers at once, and waits (sync) only where it observes state the
// worker owns: VU1 memory, save states, shutdown. Presentations mark frame boundaries:
// the game thread may queue at most kFramesAhead of them before it blocks.
class GsWorker
{
public:
    using Job = std::function<void()>;
    static constexpr uint32_t kFramesAhead = 2u;

    ~GsWorker();
    void start();
    // Runs every queued job, then joins the thread.
    void stop();
    bool active() const { return m_thread.joinable(); }
    bool onWorkerThread() const { return std::this_thread::get_id() == m_thread.get_id(); }

    void enqueue(Job job);
    void enqueuePresent(Job job);
    // Returns once every job queued so far has run. A no-op on the worker thread.
    void sync();
    // Presentations queued and not yet finished, the running one included.
    uint32_t queuedPresents() const { return m_queuedPresents.load(std::memory_order_relaxed); }

private:
    void run();

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_pushed;
    std::condition_variable m_drained;
    std::deque<std::pair<Job, bool>> m_queue; // bool: a presentation
    std::atomic<uint32_t> m_queuedPresents{0u};
    bool m_busy = false;
    bool m_stop = false;
};
