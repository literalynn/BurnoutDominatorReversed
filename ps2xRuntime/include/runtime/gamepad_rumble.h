#pragma once

#include <cstdint>

namespace ps2x
{
// Vibrates the host gamepad like a DualShock 2: the small motor (on or off) as the high
// frequency rumble, the large one (0-255) as the low frequency rumble. settings.ini
// vibration turns it off. Call it from the thread that reads the pad.
void setGamepadRumble(bool small, uint8_t large);
}
