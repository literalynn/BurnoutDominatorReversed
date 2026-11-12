#pragma once

#include <cstdint>

namespace ps2x
{
// Vibrates the host gamepad like a DualShock 2: the small motor (on or off) as the high
// frequency rumble, the large one (0-255) as the low frequency rumble. settings.ini
// vibration turns it off. Call it from the thread that reads the pad.
void setGamepadRumble(bool small, uint8_t large);

// The host gamepad as the settings menu reads it, from any thread (the game's own reads stop
// while the menu pauses it). Bit i of buttons is padButtonNames()[i] (host_settings.h), bit
// kGuideButton the Xbox or PS button where the pad reports it; the left stick is in -1..1.
constexpr int kGuideButton = 16;
struct HostGamepad
{
    bool connected = false;
    bool playStation = false; // the pad has PlayStation symbols, else Xbox letters
    uint32_t buttons = 0u;
    float lx = 0.0f, ly = 0.0f;
};
HostGamepad readHostGamepad();
}
