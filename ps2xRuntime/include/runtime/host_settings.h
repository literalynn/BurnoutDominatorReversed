#pragma once

#include <array>
#include <filesystem>
#include <string>

namespace ps2x
{
    // Player-facing display and input settings, read from a settings.ini file
    // ("key = value" lines, '#' comments) before the runtime starts.
    struct HostSettings
    {
        int windowWidth = 1280;
        int windowHeight = 960;
        bool fullscreen = false;   // borderless window on the current monitor
        bool vsync = true;         // presentation waits for the monitor refresh
        int fpsLimit = 0;          // presentation loop limit, 0 = none (monitor refresh with vsync)
        std::string aspect = "4:3"; // 4:3, 16:9 (in-game widescreen) or stretch
        bool smoothScaling = true; // bilinear filtering when the image is enlarged
        int superSampling = 4;     // paraLLEl-GS SSAA: 1, 2, 4, 8 or 16
        bool progressiveScan = true; // whole frames instead of interlaced fields (no line combing)
        bool highResolution = true;  // 2x the PS2 resolution from 4 or 8 samples a pixel, 4x from 16
        bool gsThread = true;        // VIF1, VU1 and GS submission on a second thread
        bool fastLoading = true;     // run faster than real time while the game is silent and idle
        float stickDeadzone = 0.12f;
        bool vibration = true;       // the game's pad vibration on the host gamepad
        std::string gamepadMappings; // extra SDL_GameControllerDB mappings file
        int volume = 100;            // sound volume in percent
        std::string refreshRate = "60"; // "60" or "50": the answer to the game's boot question; "ask" shows it
        bool showFps = false;        // frames per second in a corner of the window
        // Keyboard and mouse controls: up to three inputs for each DualShock 2 button, in the
        // order of padButtonNames(). An input is a raylib key code, kMouseInput + a raylib
        // mouse button, or a wheel direction; 0 is none.
        std::array<std::array<int, 3>, 16> keys = defaultKeys();

        static std::array<std::array<int, 3>, 16> defaultKeys();
    };

    constexpr int kMouseInput = 1000;
    constexpr int kMouseWheelUp = kMouseInput + 10;   // a notch presses the button briefly
    constexpr int kMouseWheelDown = kMouseInput + 11;

    // The DualShock 2 buttons keys can be bound to: up, down, left, right, cross, circle,
    // square, triangle, L1, R1, L2, R2, start, select, L3, R3.
    const std::array<const char *, 16> &padButtonNames();
    // "X", "LEFT_SHIFT", "MOUSE_LEFT"... and back; an unknown name is -1, "NONE" is 0.
    std::string inputName(int input);
    int inputFromName(const std::string &name);

    HostSettings &hostSettings();
    // The file the settings were loaded from (or written to first), for saving changes.
    const std::filesystem::path &hostSettingsFile();

    // Applies the keys of an existing file; errors lists malformed or unknown lines.
    bool loadHostSettings(const std::filesystem::path &path, std::string &errors);
    // Writes the current settings with explanations, for a first launch.
    bool writeHostSettings(const std::filesystem::path &path);
}
