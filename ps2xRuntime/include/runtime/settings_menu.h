#pragma once

#include <cstdint>

namespace ps2x
{
// The settings menu over the game window, opened and closed with F1: sound volume, picture
// shape, frames-per-second counter, window, gamepad and loading options, and the keyboard and
// mouse controls. Every change is saved to settings.ini at once and the game is paused while
// the menu is open. Runs on the window thread.

// Reads the keyboard and mouse; call it once per window loop pass, after input was polled.
// It also passes the mouse wheel on to the pad (wheel bindings) while the menu is closed.
void updateSettingsMenu();
// Wheel notches waiting to press their binding: +1 up, -1 down. Taken by the pad.
int takeMouseWheelNotch();
bool settingsMenuOpen();
// Whether the keyboard and mouse belong to the menu: it is open, or a key used to close it is
// still held. Safe from any thread.
bool settingsMenuCapturesInput();
// Draws the FPS counter and the menu over the frame. framesShown counts the game frames the
// window has shown so far.
void drawSettingsOverlay(uint64_t framesShown);

// Quitting asks first. The window's close button opens the question (requestQuitConfirmation);
// so does Escape while the game says Escape should not reach it (a race: the runner calls
// setEscapeAsksToQuit), and elsewhere Escape is the game's back button. The game is paused
// while the question is shown; quitConfirmed() turns true once it is answered yes.
void setEscapeAsksToQuit(bool asks);
bool escapeAsksToQuit();
void requestQuitConfirmation();
bool quitConfirmed();
}
