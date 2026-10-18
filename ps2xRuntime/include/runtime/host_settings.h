#pragma once

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
        bool gsThread = true;        // VIF1, VU1 and GS submission on a second thread
        bool fastLoading = true;     // run faster than real time while the game is silent and idle
        float stickDeadzone = 0.12f;
        std::string gamepadMappings; // extra SDL_GameControllerDB mappings file
    };

    HostSettings &hostSettings();

    // Applies the keys of an existing file; errors lists malformed or unknown lines.
    bool loadHostSettings(const std::filesystem::path &path, std::string &errors);
    // Writes the current settings with explanations, for a first launch.
    bool writeHostSettings(const std::filesystem::path &path);
}
