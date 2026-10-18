#pragma once

#include <cstdint>

namespace ps2x
{
// Buttons the runner presses for the player (to answer a question the game asks at boot):
// DualShock 2 button bits held on controller port 0 from one VSync tick to before another, on
// top of the real pad.
void injectPadPress(uint16_t buttons, uint64_t beginVsync, uint64_t endVsync);
uint16_t injectedPadButtons(uint64_t vsync);

// Until this VSync tick the window keeps showing the frame it has (so a question answered for
// the player does not flash on screen).
void holdDisplayUntil(uint64_t vsync);
bool displayHeld(uint64_t vsync);
}
