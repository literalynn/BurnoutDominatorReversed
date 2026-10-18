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

    // Fast-forward: while this says yes (a loading screen, for instance) and the frames leave
    // most of each VBlank period idle, the scheduler stops waiting for the host clock, so the
    // emulated time runs as fast as the host allows. Checked once per VBlank on the EE thread;
    // deterministic runs ignore it.
    using FastForwardCheck = bool (*)();
    void setFastForwardCheck(FastForwardCheck check);
    FastForwardCheck fastForwardCheck();

    // While paused (the settings menu is open) the game thread stops between guest time
    // slices, so the EE, the IOP and the sound stand still.
    void setEmulationPaused(bool paused);
    bool emulationPaused();
}
