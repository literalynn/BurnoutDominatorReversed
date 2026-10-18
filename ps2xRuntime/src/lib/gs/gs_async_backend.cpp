#include "runtime/gs/gs_parallel_backend.h"

#include "ThreadNaming.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace
{
    // Runs a backend's GIF processing on a thread of its own. Calls without a result (GIF
    // packets, register writes, image data, flushes) are copied into a queue and return at
    // once; the others wait until the queue has drained and then call the backend on the
    // caller's thread. paraLLEl-GS spends about a third of the GS worker's time decoding GIF
    // packets and recording GPU work, which now overlaps with the worker's VIF1 and VU1 work.
    class AsyncBackend final : public GSRasterBackend
    {
    public:
        explicit AsyncBackend(std::unique_ptr<GSRasterBackend> inner) : m_inner(std::move(inner))
        {
            m_thread = std::thread([this] { run(); });
        }

        ~AsyncBackend() override
        {
            sync();
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stop = true;
            }
            m_pushed.notify_all();
            m_thread.join();
        }

        void Initialize(uint8_t *vram, uint32_t vramSize) override
        {
            sync();
            m_inner->Initialize(vram, vramSize);
        }

        void Reset() override
        {
            sync();
            m_inner->Reset();
        }

        bool UsesRawCommands() const override { return m_inner->UsesRawCommands(); }
        void ProcessGIF(uint32_t path, const uint8_t *data, uint32_t sizeBytes) override
        {
            push(Kind::Gif, path, 0u, data, sizeBytes);
        }
        void WriteRegisterRaw(uint8_t addr, uint64_t value) override { push(Kind::Register, addr, value, nullptr, 0u); }

        void Submit(const GSPrimitiveBatch &batch) override
        {
            sync();
            m_inner->Submit(batch);
        }
        void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override
        {
            sync();
            m_inner->LoadClut(tex0, texclut);
        }
        void BeginTransfer(const GSTransferCommand &command) override
        {
            sync();
            m_inner->BeginTransfer(command);
        }
        void UploadImage(const uint8_t *data, uint32_t sizeBytes) override { push(Kind::Image, 0u, 0u, data, sizeBytes); }

        void Flush() override { push(Kind::Flush, 0u, 0u, nullptr, 0u); }
        void TextureFlush() override { push(Kind::TextureFlush, 0u, 0u, nullptr, 0u); }
        void Sync(GSSyncReason reason) override
        {
            sync();
            m_inner->Sync(reason);
        }
        PresentationFrame Present(const GSPresentationRequest &request) override
        {
            sync();
            return m_inner->Present(request);
        }

        bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override
        {
            sync();
            return m_inner->ClearFramebuffer(context, rgba);
        }
        uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override
        {
            sync();
            return m_inner->ConsumeLocalToHostBytes(dst, maxBytes);
        }
        uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override
        {
            sync();
            return m_inner->ReadVram(psm, base, bw, x, y);
        }
        void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override
        {
            sync();
            m_inner->WriteVram(psm, base, bw, x, y, value);
        }
        void SnapshotVram(std::vector<uint8_t> &out) const override
        {
            sync();
            m_inner->SnapshotVram(out);
        }
        GSTransferSnapshot GetTransferSnapshot() const override
        {
            sync();
            return m_inner->GetTransferSnapshot();
        }
        void SerializeState(ps2x::StateArchive &archive) override
        {
            sync();
            m_inner->SerializeState(archive);
        }

    private:
        enum class Kind : uint8_t
        {
            Gif,
            Register,
            Image,
            Flush,
            TextureFlush,
        };

        struct Job
        {
            Kind kind;
            uint32_t argument;
            uint64_t value;
            std::vector<uint8_t> data;
        };

        // More queued data than this makes the producer wait: a frame's GIF data is a few MiB.
        static constexpr size_t kMaxQueuedBytes = 64u << 20;

        void push(Kind kind, uint32_t argument, uint64_t value, const uint8_t *data, uint32_t size)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_queuedBytes > kMaxQueuedBytes)
            {
                if (m_sleeping)
                {
                    m_sleeping = false;
                    m_pushed.notify_one();
                }
                m_drained.wait(lock, [this] { return m_queuedBytes <= kMaxQueuedBytes / 2u; });
            }
            std::vector<uint8_t> buffer;
            if (size != 0u)
            {
                // Buffers come back from finished jobs, so steady state allocates nothing.
                if (!m_spare.empty())
                {
                    buffer = std::move(m_spare.back());
                    m_spare.pop_back();
                }
                buffer.assign(data, data + size);
                m_queuedBytes += size;
            }
            m_queue.push_back(Job{kind, argument, value, std::move(buffer)});
            // Waking the thread costs a system call, and it empties the queue faster than it
            // fills: wake it for a batch of packets, at a flush, or when someone waits (sync).
            const bool wake = m_sleeping && (kind == Kind::Flush || m_queue.size() >= 32u ||
                                             m_queuedBytes >= (256u << 10));
            if (wake)
                m_sleeping = false;
            lock.unlock();
            if (wake)
                m_pushed.notify_one();
        }

        void sync() const
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_sleeping && !m_queue.empty())
            {
                m_sleeping = false;
                m_pushed.notify_one();
            }
            m_drained.wait(lock, [this] { return m_queue.empty() && !m_busy; });
        }

        void run()
        {
            ThreadNaming::SetCurrentThreadName("GsSubmit");
            std::deque<Job> batch;
            std::unique_lock<std::mutex> lock(m_mutex);
            for (;;)
            {
                while (m_queue.empty() && !m_stop)
                {
                    m_sleeping = true;
                    m_pushed.wait(lock);
                }
                if (m_queue.empty())
                    return;
                // Everything queued so far in one go, so that the lock is taken once per batch.
                batch.swap(m_queue);
                m_busy = true;
                lock.unlock();
                size_t bytes = 0u;
                for (Job &job : batch)
                {
                    switch (job.kind)
                    {
                    case Kind::Gif:
                        m_inner->ProcessGIF(job.argument, job.data.data(), static_cast<uint32_t>(job.data.size()));
                        break;
                    case Kind::Register:
                        m_inner->WriteRegisterRaw(static_cast<uint8_t>(job.argument), job.value);
                        break;
                    case Kind::Image:
                        m_inner->UploadImage(job.data.data(), static_cast<uint32_t>(job.data.size()));
                        break;
                    case Kind::Flush:
                        m_inner->Flush();
                        break;
                    case Kind::TextureFlush:
                        m_inner->TextureFlush();
                        break;
                    }
                    bytes += job.data.size();
                }
                lock.lock();
                m_busy = false;
                m_queuedBytes -= bytes;
                for (Job &job : batch)
                    if (!job.data.empty() && m_spare.size() < 256u)
                        m_spare.push_back(std::move(job.data));
                batch.clear();
                m_drained.notify_all();
            }
        }

        std::unique_ptr<GSRasterBackend> m_inner;
        mutable std::mutex m_mutex;
        mutable std::condition_variable m_pushed;
        mutable std::condition_variable m_drained;
        std::deque<Job> m_queue;
        std::vector<std::vector<uint8_t>> m_spare;
        size_t m_queuedBytes = 0u;
        bool m_busy = false;
        mutable bool m_sleeping = false;
        bool m_stop = false;
        std::thread m_thread;
    };
}

std::unique_ptr<GSRasterBackend> CreateAsyncGSBackend(std::unique_ptr<GSRasterBackend> inner)
{
    return std::make_unique<AsyncBackend>(std::move(inner));
}
