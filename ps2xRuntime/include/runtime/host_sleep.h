#pragma once

#include <chrono>

namespace ps2x
{
    // Sleeps this long, and ends 0.2 to 0.5 ms late on Windows. There, sleep_for and timed
    // condition variable waits end on a timer tick after a whole number of milliseconds: 1 to 2 ms
    // for a 0.2 ms wait. This sleeps on a high-resolution waitable timer instead (Windows 10 1803
    // and later; sleep_for elsewhere and on older systems).
    void sleepPrecisely(std::chrono::nanoseconds duration);

    // Waits for the desktop compositor's next frame, which follows the monitor (DwmFlush on
    // Windows). False where there is none to wait for.
    bool waitForComposition();
}
