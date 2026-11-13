#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ps2x
{
    // Reads the disc image ahead of the game, on a thread of its own. The game thread reads the
    // disc and waits for every read (the guest sees each read complete at once); after each read,
    // the bytes that follow are read here, so the next read of a stream (music, the track being
    // driven, a file being loaded) finds them in memory instead of waiting for the drive. A seek
    // costs about 10 ms on a hard disk. The bytes are the same either way.
    class DiscReadAhead
    {
    public:
        // Opens the file a second time; valid() is false when that fails.
        DiscReadAhead(const std::filesystem::path &path, uint64_t size);
        ~DiscReadAhead();
        DiscReadAhead(const DiscReadAhead &) = delete;
        DiscReadAhead &operator=(const DiscReadAhead &) = delete;

        bool valid() const { return m_file != nullptr; }
        // read and readAfter: one caller at a time (PS2IopHostAdapter's file lock).
        // Copies [offset, offset + size) when every byte of it was read ahead, waiting for bytes
        // still on their way. False otherwise: the caller reads them itself.
        bool read(uint64_t offset, void *destination, size_t size);
        // Queues the bytes after a read the game made.
        void readAfter(uint64_t offset, size_t size);

        uint64_t reads() const;
        uint64_t hits() const;

    private:
        struct Chunk
        {
            enum class State
            {
                Queued,
                Loading,
                Ready,
                Failed
            };
            State state = State::Queued;
            std::vector<uint8_t> bytes;
            uint64_t lastUse = 0u;
        };

        void run();
        void evict();

        std::FILE *m_file = nullptr;
        uint64_t m_size = 0u;
        mutable std::mutex m_mutex;
        std::condition_variable m_work;
        std::condition_variable m_done;
        std::unordered_map<uint64_t, Chunk> m_chunks;
        std::deque<uint64_t> m_queue;
        uint64_t m_clock = 0u;
        uint64_t m_reads = 0u;
        uint64_t m_hits = 0u;
        bool m_stop = false;
        std::thread m_thread;
    };
}
