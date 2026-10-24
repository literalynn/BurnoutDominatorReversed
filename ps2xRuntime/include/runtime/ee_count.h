#pragma once

#include <cstdint>

struct GuestThread;

namespace ps2x
{
    // keep_speed's COP0 Count (EeScheduler.cpp) goes on from the value the running thread
    // last read, whatever its offset to the EE clock: after a state load, or when
    // keep_speed is turned on again.
    void continueGuestCount(const GuestThread *running, uint64_t eeCycle);
}
