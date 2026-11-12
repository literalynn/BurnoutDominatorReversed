#pragma once

#include <cstdint>
#include <functional>

namespace ps2x
{
// The settings menu over the game window, in the style of Burnout's own menus: sound volume,
// picture shape, frames-per-second counter, window, gamepad and loading options, the keyboard
// and mouse controls, and Quit game. Every change is saved to settings.ini at once and the game
// is paused while the menu is open. Runs on the window thread.
//
// F1 opens and closes it, and so does the gamepad's Guide (Xbox, PS) button or holding Select
// for most of a second. Arrows or the D-pad and the left stick move, Left/Right, L1 and R1
// change a value, Enter or Cross (A) selects, Escape or Circle (B) goes back; the mouse points
// and clicks. The hints at the bottom follow the device last used.

// Reads the keyboard, mouse and gamepad; call it once per window loop pass, after input was
// polled. It also passes the mouse wheel on to the pad (wheel bindings) while the menu is closed.
void updateSettingsMenu();
// Wheel notches waiting to press their binding: +1 up, -1 down. Taken by the pad.
int takeMouseWheelNotch();
// The size of the picture the game shows, for the Resolution row.
void setPictureSize(uint32_t width, uint32_t height);
bool settingsMenuOpen();
// Whether the keyboard, mouse and gamepad buttons belong to the menu: it is open, or a key or
// button used to close it is still held. Safe from any thread.
bool settingsMenuCapturesInput();
// Draws the FPS counter and the menu over the frame. framesShown counts the game frames the
// window has shown so far.
void drawSettingsOverlay(uint64_t framesShown);

// The shape of the picture the game draws (width / height), which the window keeps. A game that
// chooses it itself registers a probe, called on the window thread (Burnout asks the console once,
// at boot, so its shape only follows the aspect setting from its next start). Without a probe,
// or when it returns 0, the setting decides.
void setGameAspectProbe(std::function<float()> probe);
float gameAspect(); // 0 when no probe tells
float pictureAspect();

// Quitting asks first: the menu's Quit game, the window's close button (requestQuitConfirmation)
// and Escape while the game says Escape should not reach it (a race: the runner calls
// setEscapeAsksToQuit); elsewhere Escape is the game's back button. The game is paused while the
// question is shown. After yes, quitConfirmed() turns true once the memory card has been quiet for
// a moment of game time: a save in progress runs to its end first, with a notice on screen.
void setEscapeAsksToQuit(bool asks);
bool escapeAsksToQuit();
void requestQuitConfirmation();
bool quitConfirmed();
}
