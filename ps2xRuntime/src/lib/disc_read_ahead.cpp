#include "runtime/disc_read_ahead.h"

#include "ThreadNaming.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace ps2x
{
    namespace
    {
        constexpr uint64_t kChunk = 64u * 1024u;
        constexpr uint64_t kAheadChunks = 8u; // 512 KiB after each read, more after a larger read
        constexpr size_t kCapacity = 256u;    // 16 MiB kept
        constexpr uint64_t kRunChunks = 16u;  // read at most 1 MiB at once

        bool seek(std::FILE *file, uint64_t offset)
        {
#if defined(_WIN32)
            return offset <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) &&
                   _fseeki64(file, static_cast<int64_t>(offset), SEEK_SET) == 0;
#else
            return offset <= static_cast<uint64_t>(std::numeric_limits<off_t>::max()) &&
                   fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
        }
    }

    DiscReadAhead::DiscReadAhead(const std::filesystem::path &path, uint64_t size) : m_size(size)
    {
#if defined(_WIN32)
        m_file = ::_wfopen(path.c_str(), L"rb");
#else
        m_file = std::fopen(path.string().c_str(), "rb");
#endif
        if (!m_file)
            return;
        std::setvbuf(m_file, nullptr, _IONBF, 0);
        m_thread = std::thread([this] { run(); });
    }

    DiscReadAhead::~DiscReadAhead()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_work.notify_all();
        if (m_thread.joinable())
            m_thread.join();
        if (m_file)
            std::fclose(m_file);
        if (const char *trace = std::getenv("BDR_TRACE_CDVD"); trace && trace[0] != '\0' && trace[0] != '0')
            std::fprintf(stderr, "[disc] read ahead: %llu of %llu reads served from it\n",
                         static_cast<unsigned long long>(m_hits), static_cast<unsigned long long>(m_reads));
    }

    uint64_t DiscReadAhead::reads() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_reads;
    }

    uint64_t DiscReadAhead::hits() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_hits;
    }

    bool DiscReadAhead::read(uint64_t offset, void *destination, size_t size)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        ++m_reads;
        if (!m_file || size == 0u || offset >= m_size || size > m_size - offset)
            return false;
        const uint64_t first = offset / kChunk;
        const uint64_t last = (offset + size - 1u) / kChunk;
        std::vector<Chunk *> chunks;
        for (uint64_t index = first; index <= last; ++index)
        {
            const auto found = m_chunks.find(index);
            if (found == m_chunks.end() || found->second.state == Chunk::State::Failed)
                return false;
            chunks.push_back(&found->second); // only readAfter removes chunks, never during this call
        }
        for (uint64_t index = last + 1u; index-- > first;)
        {
            // Needed now: read next, in order, so that they are read at once.
            if (const auto queued = std::find(m_queue.begin(), m_queue.end(), index); queued != m_queue.end())
            {
                m_queue.erase(queued);
                m_queue.push_front(index);
            }
        }
        m_done.wait(lock, [&]
                    { return std::all_of(chunks.begin(), chunks.end(), [](const Chunk *chunk)
                                         { return chunk->state == Chunk::State::Ready || chunk->state == Chunk::State::Failed; }); });
        if (std::any_of(chunks.begin(), chunks.end(), [](const Chunk *chunk)
                        { return chunk->state == Chunk::State::Failed; }))
            return false;

        auto *out = static_cast<uint8_t *>(destination);
        for (uint64_t index = first; index <= last; ++index)
        {
            Chunk &chunk = *chunks[index - first];
            const uint64_t start = index * kChunk;
            const uint64_t from = std::max(offset, start);
            const uint64_t to = std::min(offset + size, start + chunk.bytes.size());
            std::memcpy(out + (from - offset), chunk.bytes.data() + (from - start), static_cast<size_t>(to - from));
            chunk.lastUse = ++m_clock;
        }
        ++m_hits;
        return true;
    }

    void DiscReadAhead::readAfter(uint64_t offset, size_t size)
    {
        if (!m_file || offset >= m_size || size >= m_size - offset)
            return;
        const uint64_t first = (offset + size) / kChunk;
        const uint64_t ahead = std::min<uint64_t>(std::max<uint64_t>(kAheadChunks, (size + kChunk - 1u) / kChunk), kCapacity / 4u);
        const uint64_t last = std::min((m_size - 1u) / kChunk, first + ahead - 1u);
        bool queued = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (uint64_t index = first; index <= last; ++index)
            {
                const auto [chunk, added] = m_chunks.try_emplace(index);
                chunk->second.lastUse = ++m_clock;
                if (added)
                {
                    m_queue.push_back(index);
                    queued = true;
                }
            }
            evict();
        }
        if (queued)
            m_work.notify_one();
    }

    void DiscReadAhead::evict()
    {
        while (m_chunks.size() > kCapacity)
        {
            auto oldest = m_chunks.end();
            for (auto it = m_chunks.begin(); it != m_chunks.end(); ++it)
            {
                const bool done = it->second.state == Chunk::State::Ready || it->second.state == Chunk::State::Failed;
                if (done && (oldest == m_chunks.end() || it->second.lastUse < oldest->second.lastUse))
                    oldest = it;
            }
            if (oldest == m_chunks.end())
                return; // all on their way
            m_chunks.erase(oldest);
        }
    }

    void DiscReadAhead::run()
    {
        ThreadNaming::SetCurrentThreadName("DiscReadAhead");
        std::vector<uint8_t> buffer;
        std::unique_lock<std::mutex> lock(m_mutex);
        for (;;)
        {
            m_work.wait(lock, [this] { return m_stop || !m_queue.empty(); });
            if (m_stop)
                return;
            // Consecutive queued chunks are read at once.
            const uint64_t first = m_queue.front();
            uint64_t count = 0u;
            while (!m_queue.empty() && m_queue.front() == first + count && count < kRunChunks)
            {
                m_queue.pop_front();
                m_chunks.at(first + count).state = Chunk::State::Loading;
                ++count;
            }
            lock.unlock();
            const uint64_t start = first * kChunk;
            const size_t bytes = static_cast<size_t>(std::min(count * kChunk, m_size - start));
            buffer.resize(bytes);
            const bool ok = seek(m_file, start) && std::fread(buffer.data(), 1u, bytes, m_file) == bytes;
            if (!ok)
                std::clearerr(m_file);
            lock.lock();
            for (uint64_t i = 0u; i < count; ++i)
            {
                Chunk &chunk = m_chunks.at(first + i); // not evicted while loading
                if (ok)
                {
                    const size_t from = static_cast<size_t>(i * kChunk);
                    chunk.bytes.assign(buffer.begin() + from, buffer.begin() + std::min<size_t>(bytes, from + kChunk));
                    chunk.state = Chunk::State::Ready;
                }
                else
                    chunk.state = Chunk::State::Failed;
            }
            m_done.notify_all();
        }
    }
}
