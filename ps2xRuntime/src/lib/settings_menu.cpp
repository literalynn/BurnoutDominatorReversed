#include "runtime/settings_menu.h"

#include "Kernel/Stubs/MemoryCard.h"
#include "ps2_host_backend.h"
#include "runtime/adaptive_resolution.h"
#include "runtime/ee_idle_loops.h"
#include "runtime/gamepad_rumble.h"
#include "runtime/host_settings.h"
#include "runtime/input_device.h"
#include "rlgl.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    enum class Page
    {
        Main,
        Controls,
    };

    enum MainRow
    {
        RowVolume,
        RowResolution,
        RowAntiAliasing,
        RowAdaptive,
        RowAspect,
        RowFps,
        RowFullscreen,
        RowSmooth,
        RowCarLights,
        RowVibration,
        RowFastLoading,
        RowVideoSkip,
        RowKeepSpeed,
        RowRefresh,
        RowDeadzone,
        RowPrompts,
        RowControls,
        RowClose,
        RowQuit,
        MainRowCount,
    };

    // Controls page: the 16 buttons, then these two.
    constexpr int kRowReset = 16;
    constexpr int kRowBack = 17;
    constexpr int kControlRowCount = 18;

    constexpr const char *kButtonLabels[16] = {"D-pad up", "D-pad down", "D-pad left", "D-pad right",
                                               "Cross", "Circle", "Square", "Triangle",
                                               "L1", "R1", "L2", "R2",
                                               "Start", "Select", "L3", "R3"};
    // Keys the window uses itself.
    constexpr int kReservedKeys[] = {KEY_F1, KEY_F5, KEY_F7, KEY_F9, KEY_F11};

    // ps2x::HostGamepad::buttons, in padButtonNames() order.
    constexpr uint32_t PadUp = 1u << 0, PadDown = 1u << 1, PadLeft = 1u << 2, PadRight = 1u << 3,
                       PadCross = 1u << 4, PadCircle = 1u << 5, PadSquare = 1u << 6, PadTriangle = 1u << 7,
                       PadL1 = 1u << 8, PadR1 = 1u << 9, PadStart = 1u << 12, PadSelect = 1u << 13,
                       PadGuide = 1u << ps2x::kGuideButton;

    // What one pass of keyboard or gamepad input asks of the menu.
    struct Input
    {
        bool up = false, down = false, left = false, right = false;
        bool activate = false, back = false, clear = false;
        bool toggle = false; // F1 or the Guide button
        bool close = false;  // Start or Select, while the menu is open
    };

    struct Menu
    {
        bool open = false;
        bool releasing = false; // closed, but a key or button used in the menu may still be held
        Page page = Page::Main;
        int row = 0;
        int column = 0;     // controls page: the first or second input of a button
        int capturing = -1; // controls page: the button waiting for an input
        bool mouseArmed = false;
        std::string notice;
        // Where the last draw put each row (and, on the controls page, each input cell).
        std::vector<Rectangle> rows;
        std::vector<Rectangle> cells;
        float valueLeft = 0.0f, valueCenter = 0.0f; // the left arrow and the value column
    };

    // The gamepad as the menu reads it: the left stick, L1 and R1 also move like the D-pad, which
    // repeats while held.
    struct PadState
    {
        uint32_t buttons = 0u;
        uint32_t held = 0u;
        Clock::time_point repeatAt[4];
        Clock::time_point selectSince; // Select pressed while the menu was closed
    };

    Menu g_menu;
    PadState g_pad;
    // The hints show the buttons of the device in use, as the game's prompts do (button_prompts).
    bool usingPad() { return ps2x::buttonPromptDevice() != ps2x::InputDevice::Keyboard; }
    bool playStationPad() { return ps2x::buttonPromptDevice() == ps2x::InputDevice::PlayStation; }
    std::atomic<bool> g_captures{false};
    std::atomic<int> g_wheelNotches{0};
    float g_wheel = 0.0f;
    std::atomic<bool> g_escapeAsksToQuit{false};
    bool g_quitQuestion = false;
    bool g_quitFromMenu = false;
    bool g_waitingForSave = false; // quitting once the memory card is quiet
    Clock::time_point g_waitStart;
    bool g_quitConfirmed = false;
    Rectangle g_quitButtons[2] = {}; // no, yes
    std::function<float()> g_gameAspect;

    // The game's art (setMenuArt), made into textures and sounds on the window thread.
    ps2x::MenuArt g_art;
    bool g_artChanged = false;
    enum Cue
    {
        CueLeft,
        CueRight,
        CueSelect,
        CueBack,
    };
    struct ArtTextures
    {
        Texture2D font{}, stripes{}, arrow{}, arrowSelected{}, stepEmpty{}, stepFull{};
        Texture2D playStation[3]{}, xbox[3]{};
        Sound cues[4]{};
    };

    Texture2D toTexture(const ps2x::MenuImage &image)
    {
        if (image.width <= 0 || image.height <= 0 || image.rgba.size() != size_t(image.width) * image.height * 4u)
            return Texture2D{};
        Image pixels{const_cast<uint8_t *>(image.rgba.data()), image.width, image.height, 1,
                     PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        Texture2D texture = LoadTextureFromImage(pixels);
        SetTextureFilter(texture, TEXTURE_FILTER_BILINEAR);
        return texture;
    }

    // The font's atlas four times larger with its edges sharpened, so that text stays crisp when
    // drawn far larger than the game drew it; mipmaps keep small text smooth.
    Texture2D fontTexture(const ps2x::MenuImage &atlas)
    {
        constexpr int kScale = 4;
        const int w = atlas.width, h = atlas.height;
        if (w <= 0 || h <= 0 || atlas.rgba.size() != size_t(w) * h * 4u)
            return Texture2D{};
        const auto alpha = [&](int x, int y)
        { return float(atlas.rgba[(size_t(std::clamp(y, 0, h - 1)) * w + size_t(std::clamp(x, 0, w - 1))) * 4u + 3u]); };
        ps2x::MenuImage big{w * kScale, h * kScale, std::vector<uint8_t>(size_t(w) * h * kScale * kScale * 4u, 255u)};
        for (int y = 0; y < big.height; ++y)
            for (int x = 0; x < big.width; ++x)
            {
                const float u = (float(x) + 0.5f) / kScale - 0.5f, v = (float(y) + 0.5f) / kScale - 0.5f;
                const int x0 = int(std::floor(u)), y0 = int(std::floor(v));
                const float fx = u - float(x0), fy = v - float(y0);
                const float a = (alpha(x0, y0) * (1 - fx) + alpha(x0 + 1, y0) * fx) * (1 - fy) +
                                (alpha(x0, y0 + 1) * (1 - fx) + alpha(x0 + 1, y0 + 1) * fx) * fy;
                const float t = std::clamp((a / 255.0f - 0.3f) / 0.4f, 0.0f, 1.0f);
                big.rgba[(size_t(y) * big.width + x) * 4u + 3u] = uint8_t(255.0f * t * t * (3.0f - 2.0f * t) + 0.5f);
            }
        Texture2D texture = toTexture(big);
        if (texture.id != 0)
        {
            GenTextureMipmaps(&texture);
            SetTextureFilter(texture, TEXTURE_FILTER_TRILINEAR);
        }
        return texture;
    }

    Sound toSound(const ps2x::MenuSound &sound)
    {
        if (sound.sampleRate <= 0 || sound.samples.empty() || !IsAudioDeviceReady())
            return Sound{};
        Wave wave{unsigned(sound.samples.size()), unsigned(sound.sampleRate), 16, 1,
                  const_cast<int16_t *>(sound.samples.data())};
        return LoadSoundFromWave(wave);
    }

    const ArtTextures &art()
    {
        static ArtTextures loaded;
        if (!g_artChanged)
            return loaded;
        g_artChanged = false;
        for (Texture2D *texture : {&loaded.font, &loaded.stripes, &loaded.arrow, &loaded.arrowSelected, &loaded.stepEmpty,
                                   &loaded.stepFull, &loaded.playStation[0], &loaded.playStation[1], &loaded.playStation[2],
                                   &loaded.xbox[0], &loaded.xbox[1], &loaded.xbox[2]})
            if (texture->id != 0)
                UnloadTexture(*texture);
        for (Sound &cue : loaded.cues)
            if (cue.frameCount != 0)
                UnloadSound(cue);
        loaded = ArtTextures{};
        loaded.font = g_art.font.glyphs.empty() ? Texture2D{} : fontTexture(g_art.font.atlas);
        loaded.stripes = toTexture(g_art.stripes);
        if (loaded.stripes.id != 0)
            SetTextureWrap(loaded.stripes, TEXTURE_WRAP_REPEAT);
        loaded.arrow = toTexture(g_art.arrow);
        loaded.arrowSelected = toTexture(g_art.arrowSelected);
        loaded.stepEmpty = toTexture(g_art.stepEmpty);
        loaded.stepFull = toTexture(g_art.stepFull);
        for (int i = 0; i < 3; ++i)
        {
            loaded.playStation[i] = toTexture(g_art.playStation[i]);
            loaded.xbox[i] = toTexture(g_art.xbox[i]);
        }
        const ps2x::MenuSound *sounds[] = {&g_art.left, &g_art.right, &g_art.select, &g_art.back};
        for (int i = 0; i < 4; ++i)
            loaded.cues[i] = toSound(*sounds[i]);
        return loaded;
    }

    void cue(Cue which)
    {
        const Sound &sound = art().cues[which];
        if (sound.frameCount == 0)
            return;
        SetSoundVolume(sound, 0.7f * float(std::clamp(ps2x::hostSettings().volume, 0, 100)) / 100.0f);
        PlaySound(sound);
    }

    const Color kOrange{255, 140, 16, 255}; // the selected row, as in the game's menus
    const Color kAmber{250, 178, 0, 255};   // hazard stripes
    const Color kGrey{150, 150, 156, 255};

    void save()
    {
        if (!ps2x::hostSettingsFile().empty())
            ps2x::writeHostSettings(ps2x::hostSettingsFile());
    }

    bool pressed(int key)
    {
        return IsKeyPressed(key) || IsKeyPressedRepeat(key);
    }

    bool menuKeyHeld()
    {
        for (const int key : {KEY_F1, KEY_ESCAPE, KEY_ENTER, KEY_KP_ENTER, KEY_SPACE, KEY_BACKSPACE, KEY_UP,
                              KEY_DOWN, KEY_LEFT, KEY_RIGHT})
            if (IsKeyDown(key))
                return true;
        for (int button = MOUSE_BUTTON_LEFT; button <= MOUSE_BUTTON_BACK; ++button)
            if (IsMouseButtonDown(button))
                return true;
        return g_pad.buttons != 0u;
    }

    void openMenu()
    {
        g_menu = Menu{};
        g_menu.open = true;
    }

    void closeMenu()
    {
        g_menu.open = false;
        g_menu.releasing = true;
        g_menu.capturing = -1;
        g_menu.notice.clear();
    }

    void askToQuit(bool fromMenu)
    {
        g_quitQuestion = true;
        g_quitFromMenu = fromMenu;
    }

    // Yes quits once the memory card is quiet (updateSettingsMenu); the game runs until then.
    void answerQuit(bool quit)
    {
        g_quitQuestion = false;
        if (quit)
        {
            closeMenu();
            g_waitingForSave = true;
            g_waitStart = Clock::now();
        }
        else if (!g_quitFromMenu)
            g_menu.releasing = true;
    }

    // A save writes its files some frames apart: the card counts as busy until the game has run
    // this long without changing it.
    constexpr Clock::duration kCardQuiet = std::chrono::milliseconds(1500);
    uint64_t g_cardChanges = 0u;
    Clock::duration g_cardQuietFor = kCardQuiet;
    Clock::time_point g_lastCardCheck;

    bool cardBusy(Clock::time_point now)
    {
        const ps2_stubs::MemoryCardActivity card = ps2_stubs::memoryCardActivity();
        if (card.writing || card.changes != g_cardChanges)
            g_cardQuietFor = Clock::duration::zero();
        else if (!ps2x::emulationPaused() && g_lastCardCheck != Clock::time_point{})
            g_cardQuietFor += now - g_lastCardCheck;
        g_cardChanges = card.changes;
        g_lastCardCheck = now;
        return g_cardQuietFor < kCardQuiet;
    }

    std::string onOff(bool value)
    {
        return value ? "On" : "Off";
    }

    std::string mainLabel(int row)
    {
        switch (row)
        {
        case RowVolume: return "Sound volume";
        case RowResolution: return "Resolution";
        case RowAntiAliasing: return "Anti-aliasing";
        case RowAdaptive: return "Adaptive resolution";
        case RowAspect: return "Picture shape";
        case RowFps: return "Show FPS";
        case RowFullscreen: return "Full screen (F11)";
        case RowSmooth: return "Smooth scaling";
        case RowCarLights: return "Car lights";
        case RowVibration: return "Gamepad vibration";
        case RowFastLoading: return "Fast loading";
        case RowVideoSkip: return "Skip videos (hold Start)";
        case RowKeepSpeed: return "Keep speed when slow (next race)";
        case RowRefresh: return "Frame rate (next menu page)";
        case RowDeadzone: return "Stick dead zone";
        case RowPrompts: return "Button prompts";
        case RowControls: return "Keyboard and mouse controls";
        case RowQuit: return "Quit game";
        default: return "Continue";
        }
    }

    // Resolutions: the PS2's at one sample a pixel, twice it from 4 (or, with supersampling,
    // 8 averaged in pairs), four times from 16.
    std::atomic<uint32_t> g_pictureWidth{0u}, g_pictureHeight{0u};

    int resolutionScale(const ps2x::HostSettings &s)
    {
        if (!s.highResolution || s.superSampling < 4)
            return 0;
        return s.superSampling == 16 ? 2 : 1;
    }

    std::string mainValue(int row)
    {
        const ps2x::HostSettings &s = ps2x::hostSettings();
        switch (row)
        {
        case RowVolume: return std::to_string(s.volume) + "%";
        case RowResolution:
        {
            static const char *kScales[] = {"1x (PS2)", "2x", "4x"};
            std::string label = kScales[resolutionScale(s)];
            if (g_pictureWidth.load() != 0u)
                label += " - " + std::to_string(g_pictureWidth.load()) + "x" + std::to_string(g_pictureHeight.load());
            return label;
        }
        case RowAntiAliasing:
        {
            const bool supersampling = resolutionScale(s) == 1 && s.superSampling >= 8;
            return supersampling ? (s.fxaa ? "Supersampling + FXAA" : "Supersampling") : (s.fxaa ? "FXAA" : "Off");
        }
        case RowAdaptive:
            return !s.adaptiveResolution ? "Off" : ps2x::g_adaptiveResolutionStep.load() ? "On - lowered now" : "On";
        case RowAspect: return s.aspect == "stretch" ? "Stretch" : s.aspect;
        case RowFps: return onOff(s.showFps);
        case RowFullscreen: return onOff(IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE));
        case RowSmooth: return onOff(s.smoothScaling);
        case RowCarLights: return s.carLights == 100 ? "Original" : std::to_string(s.carLights) + "%";
        case RowVibration: return onOff(s.vibration);
        case RowFastLoading: return onOff(s.fastLoading);
        case RowVideoSkip: return onOff(s.videoSkip);
        case RowKeepSpeed: return onOff(s.keepSpeed);
        case RowRefresh: return s.refreshRate + " Hz";
        case RowDeadzone: return std::to_string(int(s.stickDeadzone * 100.0f + 0.5f)) + "%";
        case RowPrompts:
            return s.buttonPrompts == "playstation" ? "PlayStation"
                   : s.buttonPrompts == "xbox"      ? "Xbox"
                   : s.buttonPrompts == "keyboard"  ? "Keyboard"
                                                    : "Auto";
        default: return "";
        }
    }

    // The line under the rows for the selected main-page row.
    std::string mainNote(int row)
    {
        if (row == RowQuit)
            return "Closes the game and goes back to the desktop";
        if (row == RowAdaptive)
            return "Lowers the resolution while the graphics card cannot keep up";
        if (row != RowAspect)
            return "Saved to settings.ini at once";
        const std::string &aspect = ps2x::hostSettings().aspect;
        if (aspect == "stretch")
            return "The picture fills the window";
        const float game = ps2x::gameAspect();
        const bool wide = aspect == "16:9";
        if (game > 0.0f && (game > 1.5f) != wide)
            return "The game changes to " + aspect + " at its next start and draws " + (wide ? "4:3" : "16:9") +
                   " until then";
        return wide ? "Real widescreen: the game draws a wider view" : "The picture of a 4:3 television";
    }

    // Changes a main-page value by one step; activate is Enter, the confirm button or a click.
    void changeMain(int row, int step, bool activate)
    {
        ps2x::HostSettings &s = ps2x::hostSettings();
        switch (row)
        {
        case RowVolume:
            s.volume = std::clamp(s.volume + 5 * (step == 0 ? 1 : step), 0, 100);
            break;
        case RowResolution:
        {
            static const int kSamples[] = {1, 4, 16};
            const int scale = (resolutionScale(s) + (step < 0 ? 2 : 1)) % 3;
            s.superSampling = kSamples[scale];
            s.highResolution = scale > 0;
            g_pictureWidth = 0u; // until the new size shows
            break;
        }
        case RowAntiAliasing:
        {
            // Off, FXAA, and at 2x supersampling (8 samples) without and with FXAA.
            const bool twice = resolutionScale(s) == 1;
            const int count = twice ? 4 : 2;
            const int current = (twice && s.superSampling >= 8 ? 2 : 0) + (s.fxaa ? 1 : 0);
            const int next = (current + (step < 0 ? count - 1 : 1)) % count;
            s.fxaa = (next & 1) != 0;
            if (twice)
                s.superSampling = next >= 2 ? 8 : 4;
            break;
        }

        case RowAdaptive:
            s.adaptiveResolution = !s.adaptiveResolution;
            break;

        case RowAspect:
        {
            static const char *kShapes[] = {"4:3", "16:9", "stretch"};
            int index = 0;
            while (index < 3 && s.aspect != kShapes[index])
                ++index;
            s.aspect = kShapes[(index % 3 + (step < 0 ? 2 : 1)) % 3];
            break;
        }
        case RowFps:
            s.showFps = !s.showFps;
            break;
        case RowFullscreen:
            ToggleBorderlessWindowed();
            s.fullscreen = IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE);
            break;
        case RowSmooth:
            s.smoothScaling = !s.smoothScaling;
            break;
        case RowCarLights:
        {
            static const int kLevels[] = {100, 150, 200};
            int index = 0;
            while (index < 3 && s.carLights != kLevels[index])
                ++index;
            s.carLights = kLevels[(index % 3 + (step < 0 ? 2 : 1)) % 3];
            break;
        }
        case RowVibration:
            s.vibration = !s.vibration;
            break;
        case RowFastLoading:
            s.fastLoading = !s.fastLoading;
            break;
        case RowVideoSkip:
            s.videoSkip = !s.videoSkip;
            break;
        case RowKeepSpeed:
            s.keepSpeed = !s.keepSpeed;
            break;
        case RowRefresh:
        {
            static const char *kRates[] = {"60", "120", "144", "165", "240", "50"};
            constexpr int kCount = static_cast<int>(std::size(kRates));
            int index = 0;
            while (index < kCount && s.refreshRate != kRates[index])
                ++index;
            s.refreshRate = kRates[(index % kCount + (step < 0 ? kCount - 1 : 1)) % kCount];
            break;
        }
        case RowDeadzone:
            s.stickDeadzone = std::clamp(s.stickDeadzone + 0.02f * float(step == 0 ? 1 : step), 0.0f, 0.5f);
            break;
        case RowPrompts:
        {
            static const char *kPrompts[] = {"auto", "playstation", "xbox", "keyboard"};
            int index = 0;
            while (index < 4 && s.buttonPrompts != kPrompts[index])
                ++index;
            s.buttonPrompts = kPrompts[(index % 4 + (step < 0 ? 3 : 1)) % 4];
            break;
        }
        case RowControls:
            if (activate || step > 0)
            {
                g_menu.page = Page::Controls;
                g_menu.row = 0;
                g_menu.column = 0;
            }
            return;
        case RowQuit:
            if (activate)
                askToQuit(true);
            return;
        default:
            if (activate)
                closeMenu();
            return;
        }
        cue(step < 0 ? CueLeft : CueRight);
        save();
    }

    void startCapture(int button, int column)
    {
        g_menu.capturing = button;
        g_menu.column = column;
        g_menu.mouseArmed = false; // the click that started it must be released first
        g_menu.notice.clear();
        while (GetKeyPressed() != 0)
        {
        }
    }

    void bind(int button, int column, int input)
    {
        auto &keys = ps2x::hostSettings().keys;
        // One input presses one button.
        for (auto &inputs : keys)
            for (int &bound : inputs)
                if (bound == input)
                    bound = 0;
        keys[button][column] = input;
        g_menu.capturing = -1;
        save();
    }

    void updateCapture()
    {
        if (IsKeyPressed(KEY_ESCAPE))
        {
            g_menu.capturing = -1;
            while (GetKeyPressed() != 0)
            {
            }
            return;
        }
        for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed())
        {
            if (key == KEY_ESCAPE)
                continue;
            if (std::find(std::begin(kReservedKeys), std::end(kReservedKeys), key) != std::end(kReservedKeys))
            {
                g_menu.notice = ps2x::inputName(key) + " is used by the window";
                continue;
            }
            bind(g_menu.capturing, g_menu.column, key);
            return;
        }
        if (const float wheel = GetMouseWheelMove(); wheel != 0.0f)
        {
            bind(g_menu.capturing, g_menu.column, wheel > 0.0f ? ps2x::kMouseWheelUp : ps2x::kMouseWheelDown);
            return;
        }
        bool anyDown = false;
        for (int button = MOUSE_BUTTON_LEFT; button <= MOUSE_BUTTON_BACK; ++button)
        {
            if (g_menu.mouseArmed && IsMouseButtonPressed(button))
            {
                bind(g_menu.capturing, g_menu.column, ps2x::kMouseInput + button);
                return;
            }
            anyDown = anyDown || IsMouseButtonDown(button);
        }
        g_menu.mouseArmed = g_menu.mouseArmed || !anyDown;
    }

    int rowUnderMouse()
    {
        const Vector2 mouse = GetMousePosition();
        for (size_t i = 0; i < g_menu.rows.size(); ++i)
            if (CheckCollisionPointRec(mouse, g_menu.rows[i]))
                return int(i);
        return -1;
    }

    bool mouseMoved()
    {
        return GetMouseDelta().x != 0.0f || GetMouseDelta().y != 0.0f;
    }

    void updateMain(const Input &in)
    {
        if (in.back)
        {
            closeMenu();
            return;
        }
        if (in.up)
            g_menu.row = (g_menu.row + MainRowCount - 1) % MainRowCount;
        if (in.down)
            g_menu.row = (g_menu.row + 1) % MainRowCount;
        if (in.left)
            changeMain(g_menu.row, -1, false);
        else if (in.right)
            changeMain(g_menu.row, 1, false);
        else if (in.activate)
            changeMain(g_menu.row, 0, true);
        const int hovered = rowUnderMouse();
        if (hovered >= 0 && mouseMoved())
            g_menu.row = hovered;
        const float x = GetMousePosition().x;
        if (hovered >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            changeMain(hovered, x > g_menu.valueLeft && x < g_menu.valueCenter ? -1 : 1, true); // the left arrow lowers
        else if (hovered >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
            changeMain(hovered, -1, false);
    }

    void updateControls(const Input &in)
    {
        if (g_menu.capturing >= 0)
        {
            if (in.back) // the gamepad's back button; Escape is read by updateCapture
                g_menu.capturing = -1;
            else
                updateCapture();
            return;
        }
        if (in.back)
        {
            g_menu.page = Page::Main;
            g_menu.row = RowControls;
            return;
        }
        if (in.up)
            g_menu.row = (g_menu.row + kControlRowCount - 1) % kControlRowCount;
        if (in.down)
            g_menu.row = (g_menu.row + 1) % kControlRowCount;
        if (in.left)
            g_menu.column = (g_menu.column + 2) % 3;
        if (in.right)
            g_menu.column = (g_menu.column + 1) % 3;
        if (in.activate && g_menu.row < 16)
            startCapture(g_menu.row, g_menu.column);
        else if (in.activate && g_menu.row == kRowReset)
        {
            ps2x::hostSettings().keys = ps2x::HostSettings::defaultKeys();
            cue(CueSelect);
            save();
        }
        else if (in.activate)
        {
            g_menu.page = Page::Main;
            g_menu.row = RowControls;
            return;
        }
        if (g_menu.row < 16 && in.clear)
        {
            ps2x::hostSettings().keys[g_menu.row][g_menu.column] = 0;
            cue(CueBack);
            save();
        }

        const Vector2 mouse = GetMousePosition();
        const bool moved = mouseMoved();
        for (size_t i = 0; i < g_menu.cells.size(); ++i)
            if (CheckCollisionPointRec(mouse, g_menu.cells[i]))
            {
                if (moved)
                {
                    g_menu.row = int(i / 3);
                    g_menu.column = int(i % 3);
                }
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                    startCapture(int(i / 3), int(i % 3));
                else if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
                {
                    ps2x::hostSettings().keys[i / 3][i % 3] = 0;
                    cue(CueBack);
                    save();
                }
                return;
            }
        const int hovered = rowUnderMouse();
        if (hovered >= 0 && moved)
            g_menu.row = hovered;
        if (hovered == kRowReset && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            ps2x::hostSettings().keys = ps2x::HostSettings::defaultKeys();
            cue(CueSelect);
            save();
        }
        else if (hovered == kRowBack && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            g_menu.page = Page::Main;
            g_menu.row = RowControls;
        }
    }

    void updateQuitQuestion(const Input &in)
    {
        const Vector2 mouse = GetMousePosition();
        const bool click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
        if (in.activate || IsKeyPressed(KEY_Y) || (click && CheckCollisionPointRec(mouse, g_quitButtons[1])))
            answerQuit(true);
        else if (in.back || in.close || IsKeyPressed(KEY_N) || (click && CheckCollisionPointRec(mouse, g_quitButtons[0])))
            answerQuit(false);
    }

    Input readKeys()
    {
        Input in;
        in.up = pressed(KEY_UP);
        in.down = pressed(KEY_DOWN);
        in.left = pressed(KEY_LEFT);
        in.right = pressed(KEY_RIGHT);
        in.activate = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_SPACE);
        in.back = IsKeyPressed(KEY_ESCAPE);
        in.clear = IsKeyPressed(KEY_BACKSPACE) || IsKeyPressed(KEY_DELETE);
        in.toggle = IsKeyPressed(KEY_F1);
        return in;
    }

    // Holding Select opens the menu, as F1 does: Burnout does not use Select (held through a race
    // it changes nothing), so the hold cannot clash with the game, and pads whose Guide button
    // Windows or Steam keep for themselves can still open it.
    constexpr Clock::duration kSelectHold = std::chrono::milliseconds(800);

    Input readPad(Clock::time_point now, bool &selectHeld)
    {
        ps2x::HostGamepad pad = ps2x::readHostGamepad();
        uint32_t held = pad.buttons;
        if (pad.ly < -0.5f)
            held |= PadUp;
        if (pad.ly > 0.5f)
            held |= PadDown;
        if (pad.lx < -0.5f || (held & PadL1))
            held |= PadLeft;
        if (pad.lx > 0.5f || (held & PadR1))
            held |= PadRight;
        const uint32_t down = held & ~g_pad.held;
        Input in;
        bool *directions[4] = {&in.up, &in.down, &in.left, &in.right};
        for (int i = 0; i < 4; ++i)
        {
            const uint32_t bit = 1u << i;
            if ((down & bit) || ((held & bit) && now >= g_pad.repeatAt[i]))
            {
                *directions[i] = true;
                g_pad.repeatAt[i] = now + std::chrono::milliseconds((down & bit) ? 400 : 80);
            }
        }
        in.activate = (down & PadCross) != 0u;
        in.back = (down & (PadCircle | PadTriangle)) != 0u; // triangle is the game's own back button
        in.clear = (down & PadSquare) != 0u;
        in.toggle = (down & PadGuide) != 0u;
        in.close = (down & (PadStart | PadSelect)) != 0u;
        if (!(held & PadSelect))
            g_pad.selectSince = {};
        else if (down & PadSelect)
            g_pad.selectSince = g_menu.open ? Clock::time_point{} : now;
        selectHeld = g_pad.selectSince != Clock::time_point{} && now - g_pad.selectSince >= kSelectHold;
        if (selectHeld)
            g_pad.selectSince = {};
        if (down != 0u)
            ps2x::noteInputDevice(pad.playStation ? ps2x::InputDevice::PlayStation : ps2x::InputDevice::Xbox);
        g_pad.buttons = pad.buttons;
        g_pad.held = held;
        return in;
    }

    // ---- Drawing, in the style of the game's own menus: hazard-stripe bars, white heavy italic
    // capitals, the selected row in orange, chevrons around values and the pad's button symbols.

    // A heavy italic from the system; no font is shipped. raylib's own font if none is found.
    const std::string &menuFontFile()
    {
        static const std::string file = []
        {
            const char *windows = std::getenv("WINDIR");
            const std::string fonts = std::string(windows ? windows : "C:/Windows") + "/Fonts/";
            for (const std::string &candidate : {fonts + "verdanaz.ttf", fonts + "seguibli.ttf", fonts + "arialbi.ttf",
                                                 std::string("/usr/share/fonts/truetype/dejavu/DejaVuSans-BoldOblique.ttf"),
                                                 std::string("/usr/share/fonts/TTF/DejaVuSans-BoldOblique.ttf"),
                                                 std::string("/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-BoldOblique.ttf"),
                                                 std::string("/usr/share/fonts/truetype/liberation/LiberationSans-BoldItalic.ttf"),
                                                 std::string("/System/Library/Fonts/Supplemental/Verdana Bold Italic.ttf"),
                                                 std::string("/Library/Fonts/Verdana Bold Italic.ttf")})
            {
                std::error_code error;
                if (std::filesystem::is_regular_file(candidate, error))
                    return candidate;
            }
            return std::string();
        }();
        return file;
    }

    // The font rasterized at each size drawn, for sharp text at any window size.
    Font menuFont(float size)
    {
        static std::map<int, Font> fonts;
        if (menuFontFile().empty())
            return GetFontDefault();
        const int pixels = std::max(8, int(size + 0.5f));
        auto found = fonts.find(pixels);
        if (found == fonts.end())
        {
            if (fonts.size() >= 12) // the window was resized: keep only new sizes
            {
                for (auto &entry : fonts)
                    UnloadFont(entry.second);
                fonts.clear();
            }
            Font font = LoadFontEx(menuFontFile().c_str(), pixels, nullptr, 0);
            SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
            found = fonts.emplace(pixels, font).first;
        }
        return found->second;
    }

    std::string capitals(std::string text)
    {
        for (char &c : text)
            c = char(std::toupper(static_cast<unsigned char>(c)));
        return text;
    }

    // A quad of texture between uv0 and uv1 (top left, bottom right) over the corners top left,
    // bottom left, bottom right, top right, as raylib draws its own.
    void texturedQuad(const Texture2D &texture, Vector2 uv0, Vector2 uv1, const std::array<Vector2, 4> &corner, Color color)
    {
        rlSetTexture(texture.id);
        rlBegin(RL_QUADS);
        rlColor4ub(color.r, color.g, color.b, color.a);
        rlNormal3f(0.0f, 0.0f, 1.0f);
        rlTexCoord2f(uv0.x, uv0.y);
        rlVertex2f(corner[0].x, corner[0].y);
        rlTexCoord2f(uv0.x, uv1.y);
        rlVertex2f(corner[1].x, corner[1].y);
        rlTexCoord2f(uv1.x, uv1.y);
        rlVertex2f(corner[2].x, corner[2].y);
        rlTexCoord2f(uv1.x, uv0.y);
        rlVertex2f(corner[3].x, corner[3].y);
        rlEnd();
        rlSetTexture(0);
    }

    // The game's font when it lent one (setMenuArt): its menus write in it, slanted.
    bool gameFont()
    {
        return art().font.id != 0;
    }

    // Calls draw(glyph, pen) for each character the game's font has; returns the line's advance.
    template <typename Draw>
    float layoutText(const std::string &text, float scale, Draw draw)
    {
        const ps2x::MenuFont &font = g_art.font;
        float pen = 0.0f;
        uint32_t previous = UINT32_MAX;
        for (const unsigned char c : text)
        {
            const auto code = font.codes.find(c);
            if (code == font.codes.end() || code->second >= font.glyphs.size())
                continue;
            if (const auto kern = font.kerning.find({previous, code->second}); kern != font.kerning.end())
                pen += kern->second * scale;
            draw(font.glyphs[code->second], pen);
            pen += font.glyphs[code->second].advance * scale;
            previous = code->second;
        }
        return pen;
    }

    constexpr float kSlant = 0.2f; // of the game's menu text

    void drawGameText(const std::string &text, Vector2 at, float size, Color color, float rotation)
    {
        const Texture2D &texture = art().font;
        const ps2x::MenuFont &font = g_art.font;
        const float scale = size / font.size, baseline = 0.84f * size; // capitals centred in size
        const float c = std::cos(rotation * DEG2RAD), s = std::sin(rotation * DEG2RAD);
        const float w = float(font.atlas.width), h = float(font.atlas.height);
        const auto point = [&](float x, float y) // x along the line, y down from at
        {
            x += (baseline - y) * kSlant;
            return Vector2{at.x + x * c - y * s, at.y + x * s + y * c};
        };
        layoutText(text, scale, [&](const ps2x::MenuFont::Glyph &g, float pen)
        {
            if (g.width <= 0.0f || g.height <= 0.0f)
                return;
            const float left = pen + g.left * scale, top = baseline - g.top * scale;
            const float right = left + g.width * scale, bottom = top + g.height * scale;
            texturedQuad(texture, {g.x / w, g.y / h}, {(g.x + g.width) / w, (g.y + g.height) / h},
                         {point(left, top), point(left, bottom), point(right, bottom), point(right, top)}, color);
        });
    }

    // The system font writes in capitals; the game's as given (callers ask for capitals where the
    // game uses them: titles and button hints).
    float textWidth(const std::string &text, float size)
    {
        if (gameFont())
            return layoutText(text, size / g_art.font.size, [](const ps2x::MenuFont::Glyph &, float) {});
        return MeasureTextEx(menuFont(size), capitals(text).c_str(), size, size / 20.0f).x;
    }

    // With a drop shadow, or a black outline for titles over the stripes.
    void drawText(const std::string &text, Vector2 at, float size, Color color, float rotation = 0.0f,
                  bool outline = false)
    {
        const bool game = gameFont();
        const Font font = game ? Font{} : menuFont(size);
        const std::string shown = game ? text : capitals(text);
        const auto draw = [&](Vector2 p, Color ink)
        {
            if (game)
                drawGameText(shown, p, size, ink, rotation);
            else
                DrawTextPro(font, shown.c_str(), p, {0.0f, 0.0f}, rotation, size, size / 20.0f, ink);
        };
        const float d = std::max(1.0f, size / 16.0f);
        const Color shade = Fade(BLACK, 0.9f * float(color.a) / 255.0f);
        if (outline)
            for (const Vector2 offset : {Vector2{-d, -d}, Vector2{d, -d}, Vector2{-d, d}, Vector2{d, d}})
                draw({at.x + offset.x, at.y + offset.y}, shade);
        draw({at.x + d, at.y + d}, shade);
        draw(at, color);
    }

    void triangle(Vector2 a, Vector2 b, Vector2 c, Color color)
    {
        // raylib skips triangles wound clockwise on screen
        if ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x) > 0.0f)
            std::swap(b, c);
        DrawTriangle(a, b, c, color);
    }

    void quad(Vector2 a, Vector2 b, Vector2 c, Vector2 d, Color color)
    {
        triangle(a, b, c, color);
        triangle(a, c, d, color);
    }

    // A bar of yellow and black stripes from origin, along a direction turned by angle degrees.
    void drawStripes(Vector2 origin, float length, float thickness, float angle)
    {
        const float c = std::cos(angle * DEG2RAD), s = std::sin(angle * DEG2RAD);
        const auto at = [&](float u, float v) { return Vector2{origin.x + u * c - v * s, origin.y + u * s + v * c}; };
        quad(at(0.0f, 0.0f), at(length, 0.0f), at(length, thickness), at(0.0f, thickness), Color{8, 8, 8, 235});
        const float inset = thickness * 0.1f;
        if (const Texture2D &stripes = art().stripes; stripes.id != 0) // the game's, repeated
        {
            const float repeat = (thickness - 2.0f * inset) * float(stripes.width) / float(stripes.height);
            texturedQuad(stripes, {0.0f, 0.0f}, {length / repeat, 1.0f},
                         {at(0.0f, inset), at(0.0f, thickness - inset), at(length, thickness - inset), at(length, inset)}, WHITE);
            return;
        }
        const float period = thickness * 0.95f, width = period * 0.48f;
        for (float u = 0.0f; u + period < length; u += period)
            quad(at(u + thickness * 0.8f, inset), at(u + thickness * 0.8f + width, inset),
                 at(u + width, thickness - inset), at(u, thickness - inset), kAmber);
    }

    struct Layout
    {
        float width = 0.0f, height = 0.0f;
        float left = 0.0f, span = 0.0f; // the content, centred in wide windows
        float unit = 0.0f;              // a hundredth of the height
    };

    Layout layout()
    {
        Layout l;
        l.width = float(GetScreenWidth());
        l.height = float(GetScreenHeight());
        l.span = std::min(l.width, l.height * 1.6f);
        l.left = (l.width - l.span) * 0.5f;
        l.unit = l.height / 100.0f;
        return l;
    }

    // The tilted bar across the top with the title on it, as the game's pause menu.
    void drawHeader(const Layout &l, const std::string &title, const std::string &subtitle)
    {
        const float thickness = 7.5f * l.unit;
        const float angle = -std::atan2(12.0f * l.unit, l.width) * RAD2DEG;
        const Vector2 origin{-0.05f * l.width, 10.5f * l.unit};
        drawStripes(origin, l.width * 1.15f, thickness, angle);
        const float c = std::cos(angle * DEG2RAD), s = std::sin(angle * DEG2RAD);
        const float size = 7.5f * l.unit, u = l.left + 0.05f * l.span - origin.x;
        drawText(capitals(title), {origin.x + u * c + 0.6f * size * s, origin.y + u * s - 0.6f * size * c}, size, WHITE,
                 angle, true);
        if (!subtitle.empty())
        {
            const float v = 0.45f * size;
            drawText(capitals(subtitle), {origin.x + u * c - v * s, origin.y + u * s + v * c}, 3.2f * l.unit, WHITE, angle,
                     true);
        }
    }

    // A thin light line that fades out at both ends, above and below the game's panels.
    void drawRule(const Layout &l, float y)
    {
        const int thick = std::max(2, int(0.35f * l.unit));
        const int half = int(l.width / 2.0f);
        const Color edge{235, 235, 240, 0}, middle{235, 235, 240, 230};
        DrawRectangleGradientH(0, int(y) - thick / 2, half, thick, edge, middle);
        DrawRectangleGradientH(half, int(y) - thick / 2, int(l.width) - half, thick, middle, edge);
    }

    void drawPanel(const Layout &l, float top, float bottom, float alpha = 0.72f)
    {
        DrawRectangle(0, int(top), int(l.width), int(bottom - top), Fade(BLACK, alpha));
        drawRule(l, top);
        drawRule(l, bottom);
    }

    enum class Glyph
    {
        Confirm,
        Back,
        Clear,
        Change,
    };

    struct Hint
    {
        Glyph glyph;
        std::string label;
    };

    const char *keyCaption(Glyph glyph)
    {
        switch (glyph)
        {
        case Glyph::Confirm: return "ENTER";
        case Glyph::Back: return "ESC";
        case Glyph::Clear: return "BACKSPACE";
        default: return "";
        }
    }

    // The width the glyph takes for button height 2 * r.
    float glyphWidth(Glyph glyph, float r)
    {
        if (usingPad())
            return 2.0f * r;
        return glyph == Glyph::Change ? 2.6f * r : textWidth(keyCaption(glyph), r * 0.8f) + r;
    }

    void drawArrows(Vector2 c, float r, Color color)
    {
        const float h = r * 0.34f, gap = r * 0.12f, w = r * 0.38f;
        triangle({c.x - gap, c.y - h}, {c.x - gap - w, c.y}, {c.x - gap, c.y + h}, color);
        triangle({c.x + gap, c.y - h}, {c.x + gap + w, c.y}, {c.x + gap, c.y + h}, color);
    }

    // The gamepad's button, as the game draws it on PlayStation (a ring around the symbol), Xbox
    // letters for other pads, or a key cap. x is the left edge; returns the width.
    float drawGlyph(Glyph glyph, float x, float y, float r)
    {
        const float width = glyphWidth(glyph, r);
        const Vector2 c{x + width / 2.0f, y};
        if (!usingPad())
        {
            const Rectangle cap{x, y - r * 0.9f, width, r * 1.8f};
            const float edge = std::max(1.0f, r * 0.12f);
            DrawRectangleRounded(cap, 0.3f, 6, Color{225, 225, 230, 255});
            DrawRectangleRounded({cap.x + edge, cap.y + edge, cap.width - 2 * edge, cap.height - 2 * edge}, 0.3f, 6,
                                 Color{28, 28, 32, 255});
            if (glyph == Glyph::Change)
                drawArrows(c, r, WHITE);
            else
            {
                const float size = r * 0.8f;
                drawText(keyCaption(glyph), {c.x - textWidth(keyCaption(glyph), size) / 2.0f, c.y - size / 2.0f}, size, WHITE);
            }
            return width;
        }
        if (glyph != Glyph::Change) // the game's symbol, its ring r wide
        {
            const ArtTextures &textures = art();
            const Texture2D &symbol = (playStationPad() ? textures.playStation : textures.xbox)[int(glyph)];
            const float side = 2.0f * r * 32.0f / 26.0f;
            if (symbol.id != 0)
            {
                DrawTexturePro(symbol, {0.0f, 0.0f, float(symbol.width), float(symbol.height)},
                               {c.x - side / 2.0f, c.y - side / 2.0f, side, side}, {0.0f, 0.0f}, 0.0f, WHITE);
                return width;
            }
        }
        if (glyph == Glyph::Change || playStationPad())
        {
            DrawCircleV(c, r, Color{10, 10, 12, 255});
            DrawRing(c, r * 0.8f, r, 0.0f, 360.0f, 40, Color{225, 225, 230, 255});
            const float s = r * 0.38f, t = r * 0.17f;
            if (glyph == Glyph::Confirm)
            {
                DrawLineEx({c.x - s, c.y - s}, {c.x + s, c.y + s}, t, Color{150, 165, 255, 255});
                DrawLineEx({c.x - s, c.y + s}, {c.x + s, c.y - s}, t, Color{150, 165, 255, 255});
            }
            else if (glyph == Glyph::Back)
                DrawRing(c, s - t / 2.0f, s + t / 2.0f, 0.0f, 360.0f, 32, Color{255, 105, 110, 255});
            else if (glyph == Glyph::Clear)
                DrawRectangleLinesEx({c.x - s * 0.85f, c.y - s * 0.85f, s * 1.7f, s * 1.7f}, t * 0.9f, Color{240, 140, 200, 255});
            else
                drawArrows(c, r, WHITE);
            return width;
        }
        static const std::pair<const char *, Color> kLetters[] = {
            {"A", Color{96, 176, 48, 255}}, {"B", Color{220, 48, 40, 255}}, {"X", Color{32, 112, 224, 255}}};
        const auto &[letter, color] = kLetters[int(glyph)];
        DrawCircleV(c, r, color);
        DrawRing(c, r * 0.86f, r, 0.0f, 360.0f, 40, Fade(BLACK, 0.45f));
        const float size = r * 1.3f;
        DrawTextPro(menuFont(size), letter, {c.x - MeasureTextEx(menuFont(size), letter, size, 0.0f).x / 2.0f, c.y - size / 2.0f},
                    {0.0f, 0.0f}, 0.0f, size, 0.0f, WHITE);
        return width;
    }

    // The bottom bar with the buttons to press, on black at its left as in the game.
    void drawFooter(const Layout &l, const std::vector<Hint> &hints)
    {
        const float top = 89.5f * l.unit, thickness = 8.5f * l.unit;
        drawStripes({-thickness, top}, l.width + 2.0f * thickness, thickness, 0.0f);
        const float r = 2.4f * l.unit, size = 3.1f * l.unit, gap = 2.5f * l.unit, y = top + thickness / 2.0f;
        float end = l.left + 0.04f * l.span;
        for (const Hint &hint : hints)
            end += glyphWidth(hint.glyph, r) + 0.8f * r + textWidth(capitals(hint.label), size) + gap;
        DrawRectangle(0, int(top), int(end), int(thickness + 1.0f), BLACK);
        DrawRectangleGradientH(int(end), int(top), int(8.0f * l.unit), int(thickness + 1.0f), BLACK, Fade(BLACK, 0.0f));
        float x = l.left + 0.04f * l.span;
        for (const Hint &hint : hints)
        {
            const std::string label = capitals(hint.label);
            x += drawGlyph(hint.glyph, x, y, r) + 0.8f * r;
            drawText(label, {x, y - size / 2.0f}, size, WHITE);
            x += textWidth(label, size) + gap;
        }
    }

    void drawNote(const Layout &l, const std::string &note)
    {
        const float size = 2.8f * l.unit;
        drawText(note, {(l.width - textWidth(note, size)) / 2.0f, 85.0f * l.unit}, size, Color{205, 205, 210, 255});
    }

    void drawChevron(Vector2 c, float size, bool right, bool selected)
    {
        if (const Texture2D &arrow = selected ? art().arrowSelected : art().arrow; arrow.id != 0) // the game's
        {
            const float side = size * 1.15f, w = float(arrow.width);
            DrawTexturePro(arrow, {0.0f, 0.0f, right ? w : -w, float(arrow.height)},
                           {c.x - side / 2.0f, c.y - side / 2.0f, side, side}, {0.0f, 0.0f}, 0.0f, WHITE);
            return;
        }
        const Color color = selected ? WHITE : kGrey;
        const float d = right ? 1.0f : -1.0f, t = size * 0.26f;
        const Vector2 tip{c.x + d * size * 0.22f, c.y};
        DrawLineEx({c.x - d * size * 0.22f, c.y - size * 0.45f}, tip, t, color);
        DrawLineEx(tip, {c.x - d * size * 0.22f, c.y + size * 0.45f}, t, color);
        DrawCircleV(tip, t / 2.0f, color);
    }

    // The value of a main-page row between its arrows; the volume as slanted bars like the game's.
    void drawValue(int row, float y, float size, float half, bool selected)
    {
        const float center = g_menu.valueCenter;
        const float spread = selected ? half + size * 0.15f : half;
        if (row == RowControls)
        {
            drawChevron({center + spread, y}, size, true, selected);
            return;
        }
        if (row > RowControls)
            return;
        drawChevron({center - spread, y}, size, false, selected);
        drawChevron({center + spread, y}, size, true, selected);
        if (row == RowVolume)
        {
            const int steps = 20, filled = ps2x::hostSettings().volume / 5;
            const float step = (half - size * 0.7f) * 2.0f / float(steps), w = step * 0.62f, h = size * 0.42f;
            float x = center - step * float(steps) / 2.0f;
            const Texture2D &full = art().stepFull, &empty = art().stepEmpty;
            for (int i = 0; i < steps; ++i, x += step)
            {
                if (full.id != 0 && empty.id != 0) // the game's steps: their bar is 22 of 32 texels high
                {
                    const Texture2D &texture = i < filled ? full : empty;
                    const float side = 2.0f * h * 32.0f / 22.0f;
                    DrawTexturePro(texture, {0.0f, 0.0f, float(texture.width), float(texture.height)},
                                   {x + step / 2.0f - side / 2.0f, y - side / 2.0f, side, side}, {0.0f, 0.0f}, 0.0f,
                                   i < filled ? WHITE : Color{110, 110, 116, 255});
                }
                else
                    quad({x + h * 0.5f, y - h}, {x + h * 0.5f + w, y - h}, {x + w, y + h}, {x, y + h},
                         i < filled ? Color{240, 92, 30, 255} : Color{86, 86, 92, 255});
            }
            return;
        }
        const std::string value = mainValue(row);
        drawText(value, {center - textWidth(value, size) / 2.0f, y - size / 2.0f}, size, WHITE);
    }

    std::vector<Hint> pageHints()
    {
        if (g_menu.page == Page::Main)
            return {{Glyph::Confirm, "Select"}, {Glyph::Change, "Change"}, {Glyph::Back, "Back"}};
        if (g_menu.capturing >= 0)
            return {{Glyph::Back, "Cancel"}};
        return {{Glyph::Confirm, "Change"}, {Glyph::Change, "Input"}, {Glyph::Clear, "Clear"}, {Glyph::Back, "Back"}};
    }

    void drawMenu()
    {
        const Layout l = layout();
        DrawRectangle(0, 0, int(l.width), int(l.height), Fade(BLACK, 0.5f));
        const bool controls = g_menu.page == Page::Controls;
        drawHeader(l, controls ? "Controls" : "Settings", controls ? "Keyboard and mouse" : "");
        const float top = 21.0f * l.unit, bottom = 82.5f * l.unit;
        drawPanel(l, top, bottom);
        const int rowCount = controls ? kControlRowCount : MainRowCount;
        const float rowHeight = (bottom - top - 2.0f * l.unit) / float(rowCount);
        const float size = std::min(rowHeight * 0.6f, 4.0f * l.unit);
        const float labelX = l.left + 0.07f * l.span, half = 0.15f * l.span;
        g_menu.valueCenter = l.left + 0.72f * l.span;
        g_menu.valueLeft = g_menu.valueCenter - half - size;
        g_menu.rows.assign(size_t(rowCount), Rectangle{});
        g_menu.cells.clear();
        for (int row = 0; row < rowCount; ++row)
        {
            const float rowTop = top + l.unit + float(row) * rowHeight, y = rowTop + rowHeight / 2.0f;
            g_menu.rows[size_t(row)] = Rectangle{l.left + 0.04f * l.span, rowTop, 0.92f * l.span, rowHeight};
            const bool selected = row == g_menu.row;
            const float labelSize = selected && g_menu.capturing < 0 ? size * 1.12f : size;
            const std::string label = !controls ? mainLabel(row)
                                      : row == kRowReset ? "Reset to defaults"
                                      : row == kRowBack ? "Back"
                                                        : kButtonLabels[row];
            drawText(label, {labelX, y - labelSize / 2.0f}, labelSize, selected ? kOrange : WHITE);
            if (!controls)
            {
                drawValue(row, y, size, half, selected);
                continue;
            }
            if (row >= 16)
                continue;
            // Three inputs a button.
            const float gap = 0.012f * l.span, cellsLeft = l.left + 0.38f * l.span;
            const float cellWidth = (l.left + 0.96f * l.span - cellsLeft - 2.0f * gap) / 3.0f;
            for (int column = 0; column < 3; ++column)
            {
                const Rectangle cell{cellsLeft + float(column) * (cellWidth + gap), rowTop + rowHeight * 0.1f, cellWidth,
                                     rowHeight * 0.8f};
                g_menu.cells.push_back(cell);
                const bool current = selected && column == g_menu.column;
                DrawRectangleRec(cell, current ? Color{255, 140, 16, 60} : Color{255, 255, 255, 16});
                if (current)
                    DrawRectangleLinesEx(cell, std::max(1.0f, 0.25f * l.unit), kOrange);
                const int input = ps2x::hostSettings().keys[size_t(row)][size_t(column)];
                const std::string text = g_menu.capturing == row && current ? "Press..."
                                         : input == 0                        ? "-"
                                                                             : ps2x::inputName(input);
                const float textSize = textWidth(text, size * 0.85f) > cellWidth - size ? size * 0.6f : size * 0.85f;
                drawText(text, {cell.x + (cellWidth - textWidth(text, textSize)) / 2.0f, y - textSize / 2.0f}, textSize,
                         current ? kOrange : Color{215, 215, 220, 255});
            }
        }
        std::string note;
        if (!controls)
            note = mainNote(g_menu.row);
        else if (g_menu.capturing >= 0)
            note = g_menu.notice.empty() ? "Press a key, a mouse button or turn the wheel" : g_menu.notice;
        else
            note = "A key presses one button only. Gamepads keep their own layout";
        drawNote(l, note);
        drawFooter(l, pageHints());
    }

    // The game's question style: a black band between two lines, the choices under the text.
    void drawQuitQuestion()
    {
        const Layout l = layout();
        // Over the menu, darker: its rows would show through the panel's edges.
        DrawRectangle(0, 0, int(l.width), int(l.height), Fade(BLACK, g_quitFromMenu ? 0.7f : 0.5f));
        drawPanel(l, 36.0f * l.unit, 66.0f * l.unit, 0.92f); // hides the menu's rows behind it
        const std::string question = "Quit the game and go back to the desktop?";
        const std::string detail = "Anything the game has not saved will be lost";
        const float size = 4.0f * l.unit, small = 2.9f * l.unit;
        drawText(question, {(l.width - textWidth(question, size)) / 2.0f, 42.0f * l.unit}, size, WHITE);
        drawText(detail, {(l.width - textWidth(detail, small)) / 2.0f, 48.5f * l.unit}, small, Color{205, 205, 210, 255});
        const float r = 2.8f * l.unit, y = 58.5f * l.unit, labelSize = 3.6f * l.unit;
        const Hint choices[2] = {{Glyph::Back, "No"}, {Glyph::Confirm, "Yes"}};
        for (int i = 0; i < 2; ++i)
        {
            const float width = glyphWidth(choices[i].glyph, r) + 0.8f * r + textWidth(choices[i].label, labelSize);
            const float x = l.width * (i == 0 ? 0.36f : 0.64f) - width / 2.0f;
            g_quitButtons[i] = Rectangle{x - r, y - 2.0f * r, width + 2.0f * r, 4.0f * r};
            const bool hover = CheckCollisionPointRec(GetMousePosition(), g_quitButtons[i]);
            const float labelX = x + drawGlyph(choices[i].glyph, x, y, r) + 0.8f * r;
            drawText(choices[i].label, {labelX, y - labelSize / 2.0f}, labelSize, hover ? kOrange : WHITE);
        }
    }

    void drawSaving()
    {
        const Layout l = layout();
        drawPanel(l, 70.0f * l.unit, 82.0f * l.unit, 0.92f);
        const std::string title = "Saving to the memory card", detail = "The game closes when the save is done";
        const float size = 3.6f * l.unit, small = 2.8f * l.unit;
        drawText(title, {(l.width - textWidth(title, size)) / 2.0f, 72.5f * l.unit}, size, kOrange);
        drawText(detail, {(l.width - textWidth(detail, small)) / 2.0f, 77.0f * l.unit}, small, WHITE);
    }

    void drawFps(uint64_t framesShown)
    {
        static Clock::time_point start = Clock::now();
        static uint64_t startFrames = framesShown;
        static int fps = 0;
        const auto now = Clock::now();
        if (now - start >= std::chrono::seconds(1))
        {
            const double seconds = std::chrono::duration<double>(now - start).count();
            fps = int(double(framesShown - startFrames) / seconds + 0.5);
            start = now;
            startFrames = framesShown;
        }
        const int font = std::clamp(GetScreenHeight() / 36, 12, 28);
        const bool paused = ps2x::emulationPaused();
        const std::string text = paused ? "Paused" : std::to_string(fps) + " FPS";
        DrawRectangle(font / 2, font / 2, MeasureText(text.c_str(), font) + font, font * 3 / 2, Fade(BLACK, 0.6f));
        DrawText(text.c_str(), font, font * 3 / 4, font, paused || fps >= 48 ? GREEN : fps >= 30 ? YELLOW : RED);
    }
}

namespace ps2x
{
    void setMenuArt(MenuArt lent)
    {
        g_art = std::move(lent);
        g_artChanged = true;
    }

    void drawCornerNotice(const std::string &hint, float progress)
    {
        const std::string text = capitals(hint); // a button hint, in capitals like the game's
        const float size = std::clamp(float(GetScreenHeight()) / 32.0f, 12.0f, 30.0f);
        const float width = textWidth(text, size) + 2.0f * size, height = 2.0f * size;
        const float left = float(GetScreenWidth()) - width - size, top = float(GetScreenHeight()) - height - size;
        DrawRectangleRec({left, top, width, height}, Fade(BLACK, 0.75f));
        DrawRectangleRec({left, top, width, std::max(1.0f, size / 12.0f)}, Color{225, 225, 230, 255});
        drawText(text, {left + size, top + size / 2.0f}, size, WHITE);
        if (progress > 0.0f)
            DrawRectangleRec({left, top + height - size / 5.0f, width * std::min(progress, 1.0f), size / 5.0f}, kOrange);
    }

    void updateSettingsMenu()
    {
        const auto now = Clock::now();
        bool selectHeld = false;
        const Input pad = readPad(now, selectHeld);
        Input in = g_menu.capturing >= 0 ? Input{} : readKeys(); // a key being bound is not a command
        if ((g_menu.capturing < 0 && GetKeyPressed() != 0) || mouseMoved() || IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            ps2x::noteInputDevice(ps2x::InputDevice::Keyboard);
        in.up |= pad.up;
        in.down |= pad.down;
        in.left |= pad.left;
        in.right |= pad.right;
        in.activate |= pad.activate;
        in.back |= pad.back;
        in.clear |= pad.clear;
        in.toggle |= pad.toggle;
        in.close |= pad.close;

        const bool wasOpen = g_menu.open, wasAsking = g_quitQuestion;
        const Page page = g_menu.page;
        const int capturing = g_menu.capturing;
        const bool busy = cardBusy(now);
        if (g_waitingForSave)
        {
            // The game runs on until its save is written (a save never takes this long).
            g_quitConfirmed = !busy || now - g_waitStart >= std::chrono::seconds(10);
            if (g_quitConfirmed)
                std::cout << "[menu] quit after the memory card " << (busy ? "stayed busy for 10 s" : "save") << std::endl;
        }
        else if (g_quitQuestion)
        {
            updateQuitQuestion(in);
            g_quitConfirmed = g_waitingForSave && !busy; // yes, with no save under way: at once
            if (g_waitingForSave && busy)
                std::cout << "[menu] quit: waiting for the memory card save" << std::endl;
        }
        else if (!g_menu.open && g_escapeAsksToQuit.load(std::memory_order_acquire) && IsKeyPressed(KEY_ESCAPE))
            askToQuit(false);
        else if (!g_menu.open && (in.toggle || selectHeld))
            openMenu();
        else if (g_menu.open && (in.toggle || in.close))
            closeMenu();
        else if (g_menu.open)
        {
            if (g_menu.page == Page::Main)
                updateMain(in);
            else
                updateControls(in);
        }
        // The front end's sounds for what changed (a changed value plays its own in changeMain).
        if (g_quitQuestion != wasAsking)
            cue(g_quitQuestion || g_waitingForSave ? CueSelect : CueBack);
        else if (g_menu.open != wasOpen)
            cue(g_menu.open ? CueSelect : CueBack);
        else if (g_menu.open && g_menu.page != page)
            cue(g_menu.page == Page::Controls ? CueSelect : CueBack);
        else if (g_menu.capturing >= 0 && capturing < 0)
            cue(CueSelect);
        if (!g_menu.open && g_menu.releasing && !menuKeyHeld())
            g_menu.releasing = false;
        g_captures.store(g_menu.open || g_menu.releasing || g_quitQuestion, std::memory_order_release);
        // Whole wheel notches go to the pad; a fraction waits for the next.
        g_wheel = g_menu.open || g_quitQuestion ? 0.0f : g_wheel + GetMouseWheelMove();
        while (g_wheel >= 1.0f || g_wheel <= -1.0f)
        {
            const int notch = g_wheel > 0.0f ? 1 : -1;
            g_wheel -= float(notch);
            const int pending = g_wheelNotches.load(std::memory_order_relaxed);
            if (pending * notch < 4) // at most four notches waiting
                g_wheelNotches.fetch_add(notch, std::memory_order_relaxed);
        }
        setEmulationPaused(g_menu.open || g_quitQuestion);
    }

    void setPictureSize(uint32_t width, uint32_t height)
    {
        g_pictureWidth = width;
        g_pictureHeight = height;
    }

    void setGameAspectProbe(std::function<float()> probe)
    {
        g_gameAspect = std::move(probe);
    }

    float gameAspect()
    {
        return g_gameAspect ? g_gameAspect() : 0.0f;
    }

    float pictureAspect()
    {
        const float game = gameAspect();
        if (game > 0.0f)
            return game;
        return hostSettings().aspect == "16:9" ? 16.0f / 9.0f : 4.0f / 3.0f;
    }

    int takeMouseWheelNotch()
    {
        int pending = g_wheelNotches.load(std::memory_order_relaxed);
        while (pending != 0)
        {
            const int notch = pending > 0 ? 1 : -1;
            if (g_wheelNotches.compare_exchange_weak(pending, pending - notch, std::memory_order_relaxed))
                return notch;
        }
        return 0;
    }

    bool settingsMenuOpen()
    {
        return g_menu.open || g_quitQuestion;
    }

    void setEscapeAsksToQuit(bool asks)
    {
        g_escapeAsksToQuit.store(asks, std::memory_order_release);
    }

    bool escapeAsksToQuit()
    {
        return g_escapeAsksToQuit.load(std::memory_order_acquire);
    }

    void requestQuitConfirmation()
    {
        if (g_waitingForSave)
            return;
        // A second close request while asking quits.
        if (g_quitQuestion)
            answerQuit(true);
        else
        {
            g_menu.open = false;
            askToQuit(false);
        }
    }

    bool quitConfirmed()
    {
        return g_quitConfirmed;
    }

    bool settingsMenuCapturesInput()
    {
        return g_captures.load(std::memory_order_acquire);
    }

    void drawSettingsOverlay(uint64_t framesShown)
    {
        if (hostSettings().showFps)
            drawFps(framesShown);
        if (g_menu.open)
            drawMenu();
        if (g_quitQuestion)
            drawQuitQuestion();
        if (g_waitingForSave)
            drawSaving();
    }
}
