#pragma once

#include <cstdint>

namespace ps2x
{
    // VBlanks a second for a game run faster than the console, 0 for the console's 60 (NTSC) or
    // 50 (PAL). Set when the game sets its frame rate (Burnout: src/burnout_main.cpp, after the
    // boot); save states record it and set it back.
    void setVBlankRate(uint32_t perSecond);
    uint32_t vblankRate();
}
