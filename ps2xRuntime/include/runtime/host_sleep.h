#pragma once

#include <chrono>

namespace ps2x
{
    // Sleeps this long, give or take a tenth of a millisecond. On Windows, sleep_for and timed
    // condition variable waits end on a timer tick after a whole number of milliseconds: 1 to 2 ms
    // for a 0.2 ms wait. This sleeps on a high-resolution waitable timer instead (Windows 10 1803
    // and later; sleep_for elsewhere and on older systems).
    void sleepPrecisely(std::chrono::nanoseconds duration);
}
