#pragma once

#include <cstdint>

namespace ps2x
{
// The settings menu over the game window, opened and closed with F1: sound volume, picture
// shape, frames-per-second counter, window, gamepad and loading options, and the keyboard and
// mouse controls. Every change is saved to settings.ini at once and the game is paused while
// the menu is open. Runs on the window thread.

// Reads the keyboard and mouse; call it once per window loop pass, after input was polled.
void updateSettingsMenu();
bool settingsMenuOpen();
// Whether the keyboard and mouse belong to the menu: it is open, or a key used to close it is
// still held. Safe from any thread.
bool settingsMenuCapturesInput();
// Draws the FPS counter and the menu over the frame. framesShown counts the game frames the
// window has shown so far.
void drawSettingsOverlay(uint64_t framesShown);
}
