#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

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
// A short notice in the bottom right corner in the menu's style, with an orange bar filled to
// progress (0..1, 0 = none). Window thread.
void drawCornerNotice(const std::string &text, float progress);

// Art and sounds a game can lend the menu so that it looks and sounds like its own menus. Anything
// left empty is drawn as without it (a system font, stripes and arrows of its own) or stays silent.
// Images are RGBA, 8 bits a channel, rows from the top.
struct MenuImage
{
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
};
struct MenuFont
{
    MenuImage atlas;   // white glyphs, their coverage in alpha
    float size = 0.0f; // text drawn at size s scales the atlas by s / size
    struct Glyph
    {
        float x, y, width, height; // the box in the atlas
        float left, top;           // from the pen, on the baseline, to the box's top left
        float advance;
    };
    std::vector<Glyph> glyphs;
    std::map<uint32_t, uint32_t> codes;                     // Latin-1 character to glyph
    std::map<std::pair<uint32_t, uint32_t>, float> kerning; // a pair of glyphs: change to the advance
};
struct MenuSound
{
    int sampleRate = 0;
    std::vector<int16_t> samples; // mono
};
struct MenuArt
{
    MenuFont font;
    MenuImage stripes;                 // hazard stripes that repeat across the image's width
    MenuImage arrow, arrowSelected;    // a chevron pointing right
    MenuImage stepEmpty, stepFull;     // one step of the volume bar
    MenuImage playStation[3], xbox[3]; // the pad's cross, circle and square (A, B and X)
    MenuSound move, left, right, select, back;
};
// Before the window loop starts, or on the window thread.
void setMenuArt(MenuArt art);

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
