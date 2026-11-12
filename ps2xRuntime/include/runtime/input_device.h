#pragma once

#include <cstdint>

namespace ps2x
{
// The kind of controller the player uses, for button prompts (settings.ini button_prompts).
// Gamepads other than PlayStation ones count as Xbox: SDL gives every pad the Xbox layout.
enum class InputDevice : uint8_t
{
    Keyboard,
    Xbox,
    PlayStation,
};

// The pad reader reports the device the player pressed something on (a bound key or mouse
// button, a gamepad button, trigger or stick); before any input, the connected gamepad counts.
void noteInputDevice(InputDevice device);
// The prompts to show: button_prompts, or with "auto" the last device used (the keyboard first).
InputDevice buttonPromptDevice();
}
