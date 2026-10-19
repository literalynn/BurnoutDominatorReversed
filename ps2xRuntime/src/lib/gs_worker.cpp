#include "runtime/gs_worker.h"

#include "ThreadNaming.h"
#include "runtime/ps2_wait_stats.h"

GsWorker::~GsWorker()
{
    stop();
}

void GsWorker::start()
{
    if (m_thread.joinable())
        return;
    m_stop = false;
    m_thread = std::thread([this] { run(); });
}

void GsWorker::stop()
{
    if (!m_thread.joinable())
        return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_pushed.notify_all();
    m_thread.join();
}

void GsWorker::enqueue(Job job)
{
    if (!m_thread.joinable() || onWorkerThread())
    {
        job();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.emplace_back(std::move(job), false);
    }
    m_pushed.notify_one();
}

void GsWorker::enqueuePresent(Job job)
{
    if (!m_thread.joinable() || onWorkerThread())
    {
        job();
        return;
    }
    {
        ps2_wait_stats::Scope wait(ps2_wait_stats::Wait::GsWorker);
        std::unique_lock<std::mutex> lock(m_mutex);
        m_drained.wait(lock, [this] { return m_queuedPresents < kFramesAhead || m_stop; });
        m_queue.emplace_back(std::move(job), true);
        ++m_queuedPresents;
    }
    m_pushed.notify_one();
}

void GsWorker::sync()
{
    if (!m_thread.joinable() || onWorkerThread())
        return;
    ps2_wait_stats::Scope wait(ps2_wait_stats::Wait::GsWorker);
    std::unique_lock<std::mutex> lock(m_mutex);
    m_drained.wait(lock, [this] { return (m_queue.empty() && !m_busy) || m_stop; });
}

void GsWorker::run()
{
    ThreadNaming::SetCurrentThreadName("GsWorker");
    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;)
    {
        m_pushed.wait(lock, [this] { return !m_queue.empty() || m_stop; });
        if (m_queue.empty())
            return; // stop requested and nothing left
        auto [job, present] = std::move(m_queue.front());
        m_queue.pop_front();
        m_busy = true;
        lock.unlock();
        job();
        lock.lock();
        m_busy = false;
        if (present)
            --m_queuedPresents;
        m_drained.notify_all();
    }
}
