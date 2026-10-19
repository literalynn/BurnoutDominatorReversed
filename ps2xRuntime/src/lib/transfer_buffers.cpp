#include "runtime/transfer_buffers.h"

#include <algorithm>
#include <mutex>

namespace
{
    // DMA copies (a VIF1 display list is about 1 MiB) are built on the EE thread
    // and freed on the GS worker. Above 512 KiB the heap maps fresh pages for each
    // one, which the copy then faults in, and unmaps them on free: reused buffers
    // avoid both. Sixteen cover the transfers of the frames the worker is behind.
    class TransferBuffers
    {
    public:
        // The largest buffer: a display list then fits without growing, which
        // would copy it and map new pages again.
        std::vector<uint8_t> take(size_t bytes)
        {
            if (bytes < kSmallest)
                return {};
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_free.empty())
                return {};
            const auto largest = std::max_element(m_free.begin(), m_free.end(),
                [](const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) { return a.capacity() < b.capacity(); });
            std::vector<uint8_t> buffer = std::move(*largest);
            *largest = std::move(m_free.back());
            m_free.pop_back();
            buffer.clear();
            return buffer;
        }

        void give(std::vector<uint8_t> &&buffer)
        {
            // Small buffers come from the heap's own pages.
            if (buffer.capacity() < kSmallest)
                return;
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_free.size() < 16u)
                m_free.push_back(std::move(buffer));
        }

    private:
        static constexpr size_t kSmallest = 256u * 1024u;
        std::mutex m_mutex;
        std::vector<std::vector<uint8_t>> m_free;
    };

    TransferBuffers &transferBuffers()
    {
        static TransferBuffers buffers;
        return buffers;
    }
}

std::vector<uint8_t> ps2x::takeTransferBuffer(size_t bytes)
{
    return transferBuffers().take(bytes);
}

void ps2x::giveTransferBuffer(std::vector<uint8_t> &&buffer)
{
    transferBuffers().give(std::move(buffer));
}
