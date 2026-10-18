#ifndef PS2_GIF_ARBITER_H
#define PS2_GIF_ARBITER_H

#include <cstdint>
#include <functional>
#include <vector>

namespace ps2x
{
    class StateArchive;
}

enum class GifPathId : uint8_t
{
    Path1 = 1,
    Path2 = 2,
    Path3 = 3,
};

struct GifArbiterPacket
{
    GifPathId pathId;
    bool path2DirectHl = false;
    bool path3Image = false;
    std::vector<uint8_t> data;
};

class GifArbiter
{
public:
    using ProcessPacketFn = std::function<void(GifPathId, const uint8_t *, uint32_t)>;

    GifArbiter() = default;
    explicit GifArbiter(ProcessPacketFn processFn);
    explicit GifArbiter(std::function<void(const uint8_t *, uint32_t)> processFn)
        : GifArbiter([fn = std::move(processFn)](GifPathId, const uint8_t *data, uint32_t size) { fn(data, size); }) {}

    void setProcessPacketFn(ProcessPacketFn fn) { m_processFn = std::move(fn); }

    void submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl = false);
    // Hands a packet straight to the GS, without a copy: for one that would be drained
    // at once with nothing queued to order it against.
    void process(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes)
    {
        if (m_processFn)
            m_processFn(pathId, data, sizeBytes);
    }

    void drain();
    bool empty() const { return m_queue.empty(); }
    void serialize(ps2x::StateArchive &archive);

private:
    ProcessPacketFn m_processFn;
    std::vector<GifArbiterPacket> m_queue;

    static bool isImagePacket(const uint8_t *data, uint32_t sizeBytes);
    static uint8_t pathPriority(GifPathId id);
};

#endif
