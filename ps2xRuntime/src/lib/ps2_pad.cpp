#include "runtime/ps2_pad.h"
#include "ps2_host_backend.h"
#include "runtime/gamepad_rumble.h"
#include "runtime/host_settings.h"
#include "runtime/settings_menu.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#if defined(PS2X_HAS_SDL2)
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <iostream>
#endif

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;

    // Sticks in -1..1 and the D-pad, face, shoulder and stick buttons as PAD_* bits.
    struct GamepadInput
    {
        uint16_t pressed = 0u;
        float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
    };

#if defined(PS2X_HAS_SDL2)
    SDL_GameController *g_controller = nullptr;

    // SDL2 game controllers: Xbox, PlayStation 3/4/5, Switch and the SDL_GameControllerDB
    // pads, over USB or Bluetooth, with or without Steam. Opened and pumped by the thread
    // that reads the pad.
    bool readGamepad(GamepadInput &input)
    {
        static const bool ready = []
        {
            SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
            // Without SDL video nothing pumps window messages on this thread: SDL handles
            // device and raw input messages on its own thread, and Xbox pads are polled
            // through XInput instead of raw input.
            SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
            SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
            SDL_SetMainReady();
            if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0)
            {
                std::cerr << "[pad] SDL game controllers unavailable: " << SDL_GetError() << std::endl;
                return false;
            }
            if (!ps2x::hostSettings().gamepadMappings.empty())
                SDL_GameControllerAddMappingsFromFile(ps2x::hostSettings().gamepadMappings.c_str());
            return true;
        }();
        SDL_GameController *&controller = g_controller;
        if (!ready)
            return false;
        // SDL 3, also through sdl2-compat, polls udev for hotplug only in the event pump. Nothing
        // reads the queue: the window is GLFW's.
        SDL_PumpEvents();
        SDL_GameControllerUpdate();
        SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
        if (controller && !SDL_GameControllerGetAttached(controller))
        {
            std::cerr << "[pad] gamepad disconnected" << std::endl;
            SDL_GameControllerClose(controller);
            controller = nullptr;
        }
        for (int index = 0; !controller && index < SDL_NumJoysticks(); ++index)
            if (SDL_IsGameController(index) && (controller = SDL_GameControllerOpen(index)))
                std::cerr << "[pad] gamepad: " << SDL_GameControllerName(controller) << std::endl;
        if (!controller)
            return false;

        static constexpr std::pair<SDL_GameControllerButton, uint16_t> kButtons[] = {
            {SDL_CONTROLLER_BUTTON_DPAD_UP, PAD_UP}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, PAD_DOWN},
            {SDL_CONTROLLER_BUTTON_DPAD_LEFT, PAD_LEFT}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, PAD_RIGHT},
            {SDL_CONTROLLER_BUTTON_A, PAD_CROSS}, {SDL_CONTROLLER_BUTTON_B, PAD_CIRCLE},
            {SDL_CONTROLLER_BUTTON_X, PAD_SQUARE}, {SDL_CONTROLLER_BUTTON_Y, PAD_TRIANGLE},
            {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, PAD_L1}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, PAD_R1},
            {SDL_CONTROLLER_BUTTON_START, PAD_START}, {SDL_CONTROLLER_BUTTON_BACK, PAD_SELECT},
            {SDL_CONTROLLER_BUTTON_LEFTSTICK, PAD_L3}, {SDL_CONTROLLER_BUTTON_RIGHTSTICK, PAD_R3}};
        for (const auto &[button, mask] : kButtons)
            if (SDL_GameControllerGetButton(controller, button))
                input.pressed |= mask;
        constexpr Sint16 kTriggerPressed = 8000;
        if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > kTriggerPressed)
            input.pressed |= PAD_L2;
        if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > kTriggerPressed)
            input.pressed |= PAD_R2;
        auto axis = [&](SDL_GameControllerAxis which)
        { return std::max(-1.0f, SDL_GameControllerGetAxis(controller, which) / 32767.0f); };
        input.lx = axis(SDL_CONTROLLER_AXIS_LEFTX);
        input.ly = axis(SDL_CONTROLLER_AXIS_LEFTY);
        input.rx = axis(SDL_CONTROLLER_AXIS_RIGHTX);
        input.ry = axis(SDL_CONTROLLER_AXIS_RIGHTY);
        static bool reported = false;
        if (!reported && input.pressed != 0u)
        {
            reported = true;
            std::cerr << "[pad] gamepad buttons received" << std::endl;
        }
        return true;
    }
#else
    // Without SDL2: raylib's gamepads (GLFW and its built-in SDL_GameControllerDB subset).
    bool readGamepad(GamepadInput &input)
    {
        int gamepad = -1;
        for (int index = 0; index < 4 && gamepad < 0; ++index)
            if (IsGamepadAvailable(index))
                gamepad = index;
        if (gamepad < 0)
            return false;
        static constexpr std::pair<int, uint16_t> kButtons[] = {
            {GAMEPAD_BUTTON_LEFT_FACE_UP, PAD_UP}, {GAMEPAD_BUTTON_LEFT_FACE_DOWN, PAD_DOWN},
            {GAMEPAD_BUTTON_LEFT_FACE_LEFT, PAD_LEFT}, {GAMEPAD_BUTTON_LEFT_FACE_RIGHT, PAD_RIGHT},
            {GAMEPAD_BUTTON_RIGHT_FACE_DOWN, PAD_CROSS}, {GAMEPAD_BUTTON_RIGHT_FACE_RIGHT, PAD_CIRCLE},
            {GAMEPAD_BUTTON_RIGHT_FACE_LEFT, PAD_SQUARE}, {GAMEPAD_BUTTON_RIGHT_FACE_UP, PAD_TRIANGLE},
            {GAMEPAD_BUTTON_LEFT_TRIGGER_1, PAD_L1}, {GAMEPAD_BUTTON_RIGHT_TRIGGER_1, PAD_R1},
            {GAMEPAD_BUTTON_LEFT_TRIGGER_2, PAD_L2}, {GAMEPAD_BUTTON_RIGHT_TRIGGER_2, PAD_R2},
            {GAMEPAD_BUTTON_MIDDLE_RIGHT, PAD_START}, {GAMEPAD_BUTTON_MIDDLE_LEFT, PAD_SELECT},
            {GAMEPAD_BUTTON_LEFT_THUMB, PAD_L3}, {GAMEPAD_BUTTON_RIGHT_THUMB, PAD_R3}};
        for (const auto &[button, mask] : kButtons)
            if (IsGamepadButtonDown(gamepad, button))
                input.pressed |= mask;
        input.lx = GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_X);
        input.ly = GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_Y);
        input.rx = GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_RIGHT_X);
        input.ry = GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_RIGHT_Y);
        return true;
    }
#endif
}

bool PSPadBackend::readState(int /*port*/, int /*slot*/, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;
    data[2] = 0xFF;
    data[3] = 0xFF;
    data[4] = data[5] = data[6] = data[7] = kPadStickCenter;

    uint16_t btns = 0xFFFFu;
    auto clearBit = [&btns](uint16_t mask)
    { btns &= ~mask; };

    // The first connected gamepad and the keyboard both drive the DualShock 2, at any time.
    GamepadInput pad;
    if (readGamepad(pad))
    {
        clearBit(pad.pressed);

        // Radial dead zone, then the remaining range spread over 0..255.
        const float deadzone = ps2x::hostSettings().stickDeadzone;
        auto stick = [&](float x, float y, uint8_t &outX, uint8_t &outY)
        {
            const float length = std::sqrt(x * x + y * y);
            const float scale = length <= deadzone ? 0.0f
                                                   : std::min(1.0f, (length - deadzone) / (1.0f - deadzone)) / length;
            auto toByte = [](float v)
            { return static_cast<uint8_t>(std::clamp(std::lround(128.0f + v * 127.5f), 0l, 255l)); };
            outX = toByte(x * scale);
            outY = toByte(y * scale);
        };
        stick(pad.lx, pad.ly, data[6], data[7]);
        stick(pad.rx, pad.ry, data[4], data[5]);
    }

    // Keyboard and mouse, as bound in settings.ini (padButtonNames() order); not while the
    // settings menu has them.
    if (!ps2x::settingsMenuCapturesInput())
    {
        static constexpr uint16_t kMasks[16] = {PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_CROSS, PAD_CIRCLE,
                                                PAD_SQUARE, PAD_TRIANGLE, PAD_L1, PAD_R1, PAD_L2, PAD_R2,
                                                PAD_START, PAD_SELECT, PAD_L3, PAD_R3};
        const bool alt = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
        // A wheel notch holds its binding for three polls, then leaves three released.
        static int wheelHeld = 0, wheelGap = 0, wheelDirection = 0;
        if (wheelHeld > 0)
            --wheelHeld;
        else if (wheelGap > 0)
            --wheelGap;
        else if ((wheelDirection = ps2x::takeMouseWheelNotch()) != 0)
        {
            wheelHeld = 2;
            wheelGap = 3;
        }
        const int wheelInput = wheelHeld > 0 || (wheelGap == 3 && wheelDirection != 0)
                                   ? (wheelDirection > 0 ? ps2x::kMouseWheelUp : ps2x::kMouseWheelDown)
                                   : 0;
        const auto &keys = ps2x::hostSettings().keys;
        for (size_t button = 0; button < keys.size(); ++button)
            for (const int input : keys[button])
            {
                const bool down = input == ps2x::kMouseWheelUp || input == ps2x::kMouseWheelDown
                                      ? input == wheelInput
                                  : input >= ps2x::kMouseInput ? IsMouseButtonDown(input - ps2x::kMouseInput)
                                  : input == KEY_ENTER       ? IsKeyDown(input) && !alt // Alt+Enter: full screen
                                  : input == KEY_ESCAPE      ? IsKeyDown(input) && !ps2x::escapeAsksToQuit()
                                  : input > 0                ? IsKeyDown(input)
                                                             : false;
                if (down)
                    clearBit(kMasks[button]);
            }
    }

    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    return true;
}

void ps2x::setGamepadRumble(bool small, uint8_t large)
{
#if defined(PS2X_HAS_SDL2)
    if (!g_controller || !ps2x::hostSettings().vibration)
        return;
    // The game sets the motors when they change; a long duration keeps them running until then.
    const Uint16 low = static_cast<Uint16>(large * 257u);
    const Uint16 high = small ? 0xFFFFu : 0u;
    SDL_GameControllerRumble(g_controller, low, high, low || high ? 10000u : 0u);
#else
    (void)small;
    (void)large;
#endif
}
