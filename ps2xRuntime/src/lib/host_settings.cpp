#include "runtime/host_settings.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace ps2x
{
    HostSettings &hostSettings()
    {
        static HostSettings settings;
        return settings;
    }

    namespace
    {
        std::string trim(const std::string &text)
        {
            const size_t begin = text.find_first_not_of(" \t\r");
            if (begin == std::string::npos)
                return {};
            return text.substr(begin, text.find_last_not_of(" \t\r") - begin + 1);
        }

        bool parseBool(const std::string &value, bool &out)
        {
            std::string lower = value;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (lower == "1" || lower == "true" || lower == "yes" || lower == "on")
                return out = true, true;
            if (lower == "0" || lower == "false" || lower == "no" || lower == "off")
                return out = false, true;
            return false;
        }

        bool parseInt(const std::string &value, int low, int high, int &out)
        {
            try
            {
                size_t used = 0;
                const int parsed = std::stoi(value, &used);
                if (used != value.size() || parsed < low || parsed > high)
                    return false;
                out = parsed;
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        bool apply(HostSettings &s, const std::string &key, const std::string &value)
        {
            if (key == "window_width")
                return parseInt(value, 320, 16384, s.windowWidth);
            if (key == "window_height")
                return parseInt(value, 240, 16384, s.windowHeight);
            if (key == "fullscreen")
                return parseBool(value, s.fullscreen);
            if (key == "vsync")
                return parseBool(value, s.vsync);
            if (key == "fps_limit")
                return parseInt(value, 0, 10000, s.fpsLimit);
            if (key == "aspect")
                return (value == "4:3" || value == "16:9" || value == "stretch") && (s.aspect = value, true);
            if (key == "smooth_scaling")
                return parseBool(value, s.smoothScaling);
            if (key == "super_sampling")
            {
                int n = 0;
                return parseInt(value, 1, 16, n) && (n & (n - 1)) == 0 && (s.superSampling = n, true);
            }
            if (key == "progressive_scan")
                return parseBool(value, s.progressiveScan);
            if (key == "gs_thread")
                return parseBool(value, s.gsThread);
            if (key == "stick_deadzone")
            {
                int percent = 0;
                return parseInt(value, 0, 90, percent) && (s.stickDeadzone = float(percent) / 100.0f, true);
            }
            if (key == "gamepad_mappings")
                return s.gamepadMappings = value, true;
            return false;
        }
    }

    bool loadHostSettings(const std::filesystem::path &path, std::string &errors)
    {
        std::ifstream file(path);
        if (!file)
            return false;
        HostSettings &settings = hostSettings();
        std::string line;
        for (int number = 1; std::getline(file, line); ++number)
        {
            line = trim(line.substr(0, line.find('#')));
            if (line.empty())
                continue;
            const size_t equals = line.find('=');
            if (equals == std::string::npos ||
                !apply(settings, trim(line.substr(0, equals)), trim(line.substr(equals + 1))))
                errors += path.filename().string() + ":" + std::to_string(number) + ": ignored '" + line + "'\n";
        }
        return true;
    }

    bool writeHostSettings(const std::filesystem::path &path)
    {
        const HostSettings &s = hostSettings();
        std::ofstream file(path);
        file << "# Burnout Dominator Reversed settings. Lines are \"key = value\"; '#' starts a comment.\n"
                "\n"
                "# Window size in pixels at start-up (the window can be resized). F11 or Alt+Enter toggles full screen.\n"
                "window_width = " << s.windowWidth << "\n"
                "window_height = " << s.windowHeight << "\n"
                "fullscreen = " << (s.fullscreen ? "true" : "false") << "\n"
                "\n"
                "# vsync = true waits for the monitor refresh. fps_limit caps the display loop (0 = no cap).\n"
                "# The game itself runs at 50 frames per second (PAL) or 60 (choose 60 Hz at start-up).\n"
                "vsync = " << (s.vsync ? "true" : "false") << "\n"
                "fps_limit = " << s.fpsLimit << "\n"
                "\n"
                "# Picture shape: 4:3, 16:9 (turn on widescreen in the game's options too) or stretch.\n"
                "aspect = " << s.aspect << "\n"
                "# Smooth (bilinear) enlargement instead of sharp pixels.\n"
                "smooth_scaling = " << (s.smoothScaling ? "true" : "false") << "\n"
                "\n"
                "# Rendering quality. super_sampling renders 1, 2, 4, 8 or 16 samples per pixel: smoother edges,\n"
                "# more GPU work. progressive_scan shows whole frames instead of interlaced fields (no line combing).\n"
                "super_sampling = " << s.superSampling << "\n"
                "progressive_scan = " << (s.progressiveScan ? "true" : "false") << "\n"
                "\n"
                "# gs_thread runs the PS2 graphics units (VIF1, VU1, GS) on a second CPU core.\n"
                "gs_thread = " << (s.gsThread ? "true" : "false") << "\n"
                "\n"
                "# Gamepads: any Xbox, PlayStation 4/5 or SDL-compatible pad works next to the keyboard.\n"
                "# stick_deadzone is a percentage. gamepad_mappings may name an extra gamecontrollerdb.txt file.\n"
                "stick_deadzone = " << int(s.stickDeadzone * 100.0f + 0.5f) << "\n"
                "gamepad_mappings = " << s.gamepadMappings << "\n";
        return static_cast<bool>(file);
    }
}
