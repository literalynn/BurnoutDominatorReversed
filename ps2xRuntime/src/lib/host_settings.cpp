#include "runtime/host_settings.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
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
        std::filesystem::path g_settingsFile;

        // raylib's key codes (GLFW's) with the names settings.ini uses.
        struct NamedInput
        {
            int code;
            const char *name;
        };
        constexpr NamedInput kNamedInputs[] = {
            {32, "SPACE"}, {39, "APOSTROPHE"}, {44, "COMMA"}, {45, "MINUS"}, {46, "PERIOD"}, {47, "SLASH"},
            {59, "SEMICOLON"}, {61, "EQUAL"}, {91, "LEFT_BRACKET"}, {92, "BACKSLASH"}, {93, "RIGHT_BRACKET"},
            {96, "GRAVE"}, {256, "ESCAPE"}, {257, "ENTER"}, {258, "TAB"}, {259, "BACKSPACE"}, {260, "INSERT"},
            {261, "DELETE"}, {262, "RIGHT"}, {263, "LEFT"}, {264, "DOWN"}, {265, "UP"}, {266, "PAGE_UP"},
            {267, "PAGE_DOWN"}, {268, "HOME"}, {269, "END"}, {280, "CAPS_LOCK"}, {281, "SCROLL_LOCK"},
            {282, "NUM_LOCK"}, {283, "PRINT_SCREEN"}, {284, "PAUSE"}, {320, "KP_0"}, {321, "KP_1"},
            {322, "KP_2"}, {323, "KP_3"}, {324, "KP_4"}, {325, "KP_5"}, {326, "KP_6"}, {327, "KP_7"},
            {328, "KP_8"}, {329, "KP_9"}, {330, "KP_DECIMAL"}, {331, "KP_DIVIDE"}, {332, "KP_MULTIPLY"},
            {333, "KP_SUBTRACT"}, {334, "KP_ADD"}, {335, "KP_ENTER"}, {336, "KP_EQUAL"}, {340, "LEFT_SHIFT"},
            {341, "LEFT_CONTROL"}, {342, "LEFT_ALT"}, {343, "LEFT_SUPER"}, {344, "RIGHT_SHIFT"},
            {345, "RIGHT_CONTROL"}, {346, "RIGHT_ALT"}, {347, "RIGHT_SUPER"}, {348, "MENU"},
            {kMouseInput + 0, "MOUSE_LEFT"}, {kMouseInput + 1, "MOUSE_RIGHT"}, {kMouseInput + 2, "MOUSE_MIDDLE"},
            {kMouseInput + 3, "MOUSE_SIDE"}, {kMouseInput + 4, "MOUSE_EXTRA"}, {kMouseInput + 5, "MOUSE_FORWARD"},
            {kMouseInput + 6, "MOUSE_BACK"}, {kMouseWheelUp, "MOUSE_WHEEL_UP"}, {kMouseWheelDown, "MOUSE_WHEEL_DOWN"},
        };

        std::string keysValue(const std::array<int, 3> &inputs)
        {
            std::string value;
            for (const int input : inputs)
                if (input != 0)
                    value += (value.empty() ? "" : " ") + inputName(input);
            return value.empty() ? "NONE" : value;
        }

        bool parseKeys(const std::string &value, std::array<int, 3> &out)
        {
            std::array<int, 3> inputs{};
            size_t count = 0;
            std::string name;
            std::istringstream words(value);
            while (words >> name)
            {
                const int input = inputFromName(name);
                if (input < 0 || count == inputs.size())
                    return false;
                if (input != 0)
                    inputs[count++] = input;
            }
            out = inputs;
            return true;
        }
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
            if (key == "high_resolution")
                return parseBool(value, s.highResolution);
            if (key == "gs_thread")
                return parseBool(value, s.gsThread);
            if (key == "fast_loading")
                return parseBool(value, s.fastLoading);
            if (key == "vibration")
                return parseBool(value, s.vibration);
            if (key == "stick_deadzone")
            {
                int percent = 0;
                return parseInt(value, 0, 90, percent) && (s.stickDeadzone = float(percent) / 100.0f, true);
            }
            if (key == "gamepad_mappings")
                return s.gamepadMappings = value, true;
            if (key == "volume")
                return parseInt(value, 0, 100, s.volume);
            if (key == "refresh_rate")
                return (value == "60" || value == "50" || value == "ask") && (s.refreshRate = value, true);
            if (key == "show_fps")
                return parseBool(value, s.showFps);
            for (size_t i = 0; i < s.keys.size(); ++i)
                if (key == std::string("key_") + padButtonNames()[i])
                    return parseKeys(value, s.keys[i]);
            return false;
        }
    }

    std::array<std::array<int, 3>, 16> HostSettings::defaultKeys()
    {
        // The mouse moves through the menus: wheel up and down, left click selects (cross), right
        // click goes back (triangle).
        return {{{265, 'W', kMouseWheelUp}, {264, 'S', kMouseWheelDown}, {263, 'A'}, {262, 'D'},
                 {'X', 32, kMouseInput + 0}, {'C', 321}, {'Z', 320}, {'V', 256, kMouseInput + 1},
                 {'Q'}, {'E'}, {340}, {344}, {257}, {258}, {}, {}}};
    }

    const std::array<const char *, 16> &padButtonNames()
    {
        static const std::array<const char *, 16> names = {"up", "down", "left", "right", "cross", "circle",
                                                           "square", "triangle", "l1", "r1", "l2", "r2",
                                                           "start", "select", "l3", "r3"};
        return names;
    }

    std::string inputName(int input)
    {
        if (input == 0)
            return "NONE";
        if ((input >= 'A' && input <= 'Z') || (input >= '0' && input <= '9'))
            return std::string(1, char(input));
        if (input >= 290 && input <= 301)
            return "F" + std::to_string(input - 289);
        for (const NamedInput &named : kNamedInputs)
            if (named.code == input)
                return named.name;
        return "KEY_" + std::to_string(input);
    }

    int inputFromName(const std::string &name)
    {
        std::string upper = name;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return char(std::toupper(c)); });
        if (upper == "NONE")
            return 0;
        if (upper.size() == 1 && ((upper[0] >= 'A' && upper[0] <= 'Z') || (upper[0] >= '0' && upper[0] <= '9')))
            return upper[0];
        if (upper.size() >= 2 && upper.size() <= 3 && upper[0] == 'F' && std::isdigit(static_cast<unsigned char>(upper[1])))
        {
            const int number = std::atoi(upper.c_str() + 1);
            if (number >= 1 && number <= 12)
                return 289 + number;
        }
        for (const NamedInput &named : kNamedInputs)
            if (upper == named.name)
                return named.code;
        if (upper.rfind("KEY_", 0) == 0 && upper.size() > 4 && std::isdigit(static_cast<unsigned char>(upper[4])))
            return std::atoi(upper.c_str() + 4);
        return -1;
    }

    const std::filesystem::path &hostSettingsFile()
    {
        return g_settingsFile;
    }

    bool loadHostSettings(const std::filesystem::path &path, std::string &errors)
    {
        g_settingsFile = path;
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
        if (g_settingsFile.empty())
            g_settingsFile = path;
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
                "# high_resolution shows twice the PS2 resolution (1280x1024), built from the super samples:\n"
                "# it needs super_sampling 4 or more (8 and 16 also smooth the edges). F1 changes both.\n"
                "high_resolution = " << (s.highResolution ? "true" : "false") << "\n"
                "\n"
                "# gs_thread runs the PS2 graphics units (VIF1, VU1, GS) on a second CPU core.\n"
                "gs_thread = " << (s.gsThread ? "true" : "false") << "\n"
                "# fast_loading runs loading screens (silent, mostly idle) faster than real time.\n"
                "fast_loading = " << (s.fastLoading ? "true" : "false") << "\n"
                "\n"
                "# Gamepads: any Xbox, PlayStation 4/5 or SDL-compatible pad works next to the keyboard.\n"
                "# stick_deadzone is a percentage. gamepad_mappings may name an extra gamecontrollerdb.txt file.\n"
                "stick_deadzone = " << int(s.stickDeadzone * 100.0f + 0.5f) << "\n"
                "gamepad_mappings = " << s.gamepadMappings << "\n"
                "# vibration passes the game's pad vibration on to the gamepad.\n"
                "vibration = " << (s.vibration ? "true" : "false") << "\n"
                "\n"
                "# Sound volume in percent, and a frames-per-second counter in the window corner.\n"
                "volume = " << s.volume << "\n"
                "show_fps = " << (s.showFps ? "true" : "false") << "\n"
                "# The game asks 50 or 60 Hz at start-up: refresh_rate 60 or 50 answers for you, ask shows it.\n"
                "refresh_rate = " << s.refreshRate << "\n"
                "\n"
                "# Keyboard and mouse controls, three inputs at most per button: letters, digits, F1-F12,\n"
                "# SPACE, ENTER, ESCAPE, TAB, UP, LEFT_SHIFT, KP_0, MOUSE_LEFT, MOUSE_RIGHT, MOUSE_WHEEL_UP...\n"
                "# or NONE.\n"
                "# F1 opens the settings menu, which also changes them.\n";
        for (size_t i = 0; i < s.keys.size(); ++i)
            file << "key_" << padButtonNames()[i] << " = " << keysValue(s.keys[i]) << '\n';
        return static_cast<bool>(file);
    }
}
