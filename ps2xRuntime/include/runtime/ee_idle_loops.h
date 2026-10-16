#pragma once

#include <cstdint>

struct R5900Context;

namespace ps2x
{
    // A game can declare a polling loop that only reads memory, entered at pc.
    // While Idle says the loop is waiting, each pass through pc advances the EE
    // clock to the next scheduled event (at most one device batch) instead of
    // the few cycles of one dispatch: the loop still runs and rereads memory,
    // it just stops costing host time between events.
    using IdleCheck = bool (*)(const uint8_t *rdram, const R5900Context &context);

    void registerIdleLoop(uint32_t pc, IdleCheck idle);
    IdleCheck idleLoopAt(uint32_t pc);
}
