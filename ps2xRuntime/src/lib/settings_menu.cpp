#include "runtime/settings_menu.h"

#include "ps2_host_backend.h"
#include "runtime/ee_idle_loops.h"
#include "runtime/host_settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>

namespace
{
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
        RowHudSmoothing,
        RowAspect,
        RowFps,
        RowFullscreen,
        RowSmooth,
        RowVibration,
        RowFastLoading,
        RowRefresh,
        RowDeadzone,
        RowControls,
        RowClose,
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

    struct Menu
    {
        bool open = false;
        bool releasing = false; // closed, but a key used in the menu may still be held
        Page page = Page::Main;
        int row = 0;
        int column = 0;     // controls page: the first or second input of a button
        int capturing = -1; // controls page: the button waiting for an input
        bool mouseArmed = false;
        std::string notice;
        // Where the last draw put each row (and, on the controls page, each input cell).
        std::vector<Rectangle> rows;
        std::vector<Rectangle> cells;
    };

    Menu g_menu;
    std::atomic<bool> g_captures{false};
    std::atomic<int> g_wheelNotches{0};
    float g_wheel = 0.0f;
    std::atomic<bool> g_escapeAsksToQuit{false};
    bool g_quitQuestion = false;
    bool g_quitConfirmed = false;
    Rectangle g_quitButtons[2] = {}; // quit, keep playing

    const Color kAccent{255, 170, 30, 255};

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
        return false;
    }

    void closeMenu()
    {
        g_menu.open = false;
        g_menu.releasing = true;
        g_menu.capturing = -1;
        g_menu.notice.clear();
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
        case RowHudSmoothing: return "HUD anti-aliasing";
        case RowAspect: return "Picture shape";
        case RowFps: return "Show FPS";
        case RowFullscreen: return "Full screen (F11)";
        case RowSmooth: return "Smooth scaling";
        case RowVibration: return "Gamepad vibration";
        case RowFastLoading: return "Fast loading";
        case RowRefresh: return "Frame rate (next start)";
        case RowDeadzone: return "Stick dead zone";
        case RowControls: return "Keyboard and mouse controls";
        default: return "Close (F1)";
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
        case RowHudSmoothing: return onOff(s.hudSmoothing);
        case RowAspect: return s.aspect == "stretch" ? "Stretch" : s.aspect;
        case RowFps: return onOff(s.showFps);
        case RowFullscreen: return onOff(IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE));
        case RowSmooth: return onOff(s.smoothScaling);
        case RowVibration: return onOff(s.vibration);
        case RowFastLoading: return onOff(s.fastLoading);
        case RowRefresh: return s.refreshRate == "ask" ? "Ask" : s.refreshRate + " Hz";
        case RowDeadzone: return std::to_string(int(s.stickDeadzone * 100.0f + 0.5f)) + "%";
        case RowControls: return ">";
        default: return "";
        }
    }

    // Changes a main-page value by one step; activate is Enter or a click.
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
        case RowHudSmoothing:
            s.hudSmoothing = !s.hudSmoothing;
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
        case RowVibration:
            s.vibration = !s.vibration;
            break;
        case RowFastLoading:
            s.fastLoading = !s.fastLoading;
            break;
        case RowRefresh:
        {
            static const char *kRates[] = {"60", "50", "ask"};
            int index = 0;
            while (index < 3 && s.refreshRate != kRates[index])
                ++index;
            s.refreshRate = kRates[(index % 3 + (step < 0 ? 2 : 1)) % 3];
            break;
        }
        case RowDeadzone:
            s.stickDeadzone = std::clamp(s.stickDeadzone + 0.02f * float(step == 0 ? 1 : step), 0.0f, 0.5f);
            break;
        case RowControls:
            if (activate || step > 0)
            {
                g_menu.page = Page::Controls;
                g_menu.row = 0;
                g_menu.column = 0;
            }
            return;
        default:
            if (activate)
                closeMenu();
            return;
        }
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

    void updateMain()
    {
        if (IsKeyPressed(KEY_ESCAPE))
        {
            closeMenu();
            return;
        }
        if (pressed(KEY_UP))
            g_menu.row = (g_menu.row + MainRowCount - 1) % MainRowCount;
        if (pressed(KEY_DOWN))
            g_menu.row = (g_menu.row + 1) % MainRowCount;
        if (pressed(KEY_LEFT))
            changeMain(g_menu.row, -1, false);
        else if (pressed(KEY_RIGHT))
            changeMain(g_menu.row, 1, false);
        else if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_SPACE))
            changeMain(g_menu.row, 0, true);
        const int hovered = rowUnderMouse();
        if (hovered >= 0 && (GetMouseDelta().x != 0.0f || GetMouseDelta().y != 0.0f))
            g_menu.row = hovered;
        if (hovered >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            changeMain(hovered, 1, true);
        else if (hovered >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
            changeMain(hovered, -1, false);
    }

    void updateControls()
    {
        if (g_menu.capturing >= 0)
        {
            updateCapture();
            return;
        }
        if (IsKeyPressed(KEY_ESCAPE))
        {
            g_menu.page = Page::Main;
            g_menu.row = RowControls;
            return;
        }
        if (pressed(KEY_UP))
            g_menu.row = (g_menu.row + kControlRowCount - 1) % kControlRowCount;
        if (pressed(KEY_DOWN))
            g_menu.row = (g_menu.row + 1) % kControlRowCount;
        if (pressed(KEY_LEFT))
            g_menu.column = (g_menu.column + 2) % 3;
        if (pressed(KEY_RIGHT))
            g_menu.column = (g_menu.column + 1) % 3;
        const bool activate = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_SPACE);
        if (activate && g_menu.row < 16)
            startCapture(g_menu.row, g_menu.column);
        else if (activate && g_menu.row == kRowReset)
        {
            ps2x::hostSettings().keys = ps2x::HostSettings::defaultKeys();
            save();
        }
        else if (activate)
        {
            g_menu.page = Page::Main;
            g_menu.row = RowControls;
            return;
        }
        if (g_menu.row < 16 && (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressed(KEY_DELETE)))
        {
            ps2x::hostSettings().keys[g_menu.row][g_menu.column] = 0;
            save();
        }

        const Vector2 mouse = GetMousePosition();
        const bool moved = GetMouseDelta().x != 0.0f || GetMouseDelta().y != 0.0f;
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
            save();
        }
        else if (hovered == kRowBack && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            g_menu.page = Page::Main;
            g_menu.row = RowControls;
        }
    }

    void drawRowBackground(const Rectangle &rect, bool selected)
    {
        if (selected)
            DrawRectangleRec(rect, Color{255, 170, 30, 60});
    }

    void drawMenu()
    {
        const int width = GetScreenWidth(), height = GetScreenHeight();
        DrawRectangle(0, 0, width, height, Fade(BLACK, 0.6f));
        const bool controls = g_menu.page == Page::Controls;
        const int rowCount = controls ? kControlRowCount : MainRowCount;
        // Rows, a title, a gap and three hint lines must fit.
        const int font = std::clamp(height / (rowCount + 9) * 2 / 3, 10, 32);
        const int rowHeight = font * 3 / 2;
        const int panelWidth = std::min(width - 2 * font, font * 30);
        const int panelHeight = rowHeight * (rowCount + 5);
        const int left = (width - panelWidth) / 2, top = std::max(0, (height - panelHeight) / 2);
        DrawRectangle(left, top, panelWidth, panelHeight, Color{20, 20, 24, 235});
        DrawRectangleLines(left, top, panelWidth, panelHeight, kAccent);
        const int pad = font;
        DrawText(controls ? "CONTROLS - KEYBOARD AND MOUSE" : "SETTINGS", left + pad, top + font / 2, font, kAccent);

        g_menu.rows.assign(size_t(rowCount), Rectangle{});
        g_menu.cells.clear();
        int y = top + rowHeight * 2;
        for (int row = 0; row < rowCount; ++row, y += rowHeight)
        {
            const Rectangle rect{float(left + pad / 2), float(y - font / 4), float(panelWidth - pad), float(rowHeight)};
            g_menu.rows[size_t(row)] = rect;
            const bool selected = row == g_menu.row;
            if (!controls)
            {
                drawRowBackground(rect, selected);
                DrawText(mainLabel(row).c_str(), left + pad, y, font, selected ? WHITE : LIGHTGRAY);
                const std::string value = selected && row < RowControls ? "< " + mainValue(row) + " >" : mainValue(row);
                DrawText(value.c_str(), left + panelWidth - pad - MeasureText(value.c_str(), font), y, font,
                         selected ? kAccent : LIGHTGRAY);
                continue;
            }
            if (row >= 16)
            {
                drawRowBackground(rect, selected);
                DrawText(row == kRowReset ? "Reset to defaults" : "Back (Esc)", left + pad, y, font,
                         selected ? WHITE : LIGHTGRAY);
                continue;
            }
            DrawText(kButtonLabels[row], left + pad, y, font, selected ? WHITE : LIGHTGRAY);
            const int cellWidth = (panelWidth - 2 * pad) * 23 / 100;
            for (int column = 0; column < 3; ++column)
            {
                const Rectangle cell{float(left + panelWidth - pad - (3 - column) * (cellWidth + pad / 2)),
                                     float(y - font / 4), float(cellWidth), float(rowHeight - 2)};
                g_menu.cells.push_back(cell);
                const bool current = selected && column == g_menu.column;
                DrawRectangleRec(cell, current ? Color{255, 170, 30, 70} : Color{255, 255, 255, 18});
                const int input = ps2x::hostSettings().keys[size_t(row)][size_t(column)];
                const std::string text = g_menu.capturing == row && current ? "press..."
                                         : input == 0                        ? "-"
                                                                             : ps2x::inputName(input);
                const int cellFont = MeasureText(text.c_str(), font) > cellWidth - font / 2 ? font * 2 / 3 : font;
                DrawText(text.c_str(), int(cell.x) + font / 3, y + (font - cellFont) / 2, cellFont, current ? kAccent : LIGHTGRAY);
            }
        }

        y += rowHeight / 2;
        const char *hints[3];
        if (!controls)
        {
            hints[0] = "Up/Down: choose   Left/Right or click: change   Enter: open";
            hints[1] = g_menu.row == RowAspect ? "16:9: also turn on widescreen in the game's options"
                                               : "Saved to settings.ini at once";
            hints[2] = "Esc or F1: back to the game (paused while this is open)";
        }
        else if (g_menu.capturing >= 0)
        {
            hints[0] = "Press a key, a mouse button or turn the wheel";
            hints[1] = g_menu.notice.empty() ? "Esc: cancel" : g_menu.notice.c_str();
            hints[2] = "";
        }
        else
        {
            hints[0] = "Enter or click: change   Left/Right: first, second or third input";
            hints[1] = "Backspace or right click: clear   A key presses one button only";
            hints[2] = "Esc: back. Gamepads keep their own layout";
        }
        for (const char *hint : hints)
        {
            DrawText(hint, left + pad, y, font * 3 / 4, GRAY);
            y += rowHeight * 3 / 4;
        }
    }

    void updateQuitQuestion()
    {
        const Vector2 mouse = GetMousePosition();
        const bool click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_Y) ||
            (click && CheckCollisionPointRec(mouse, g_quitButtons[0])))
            g_quitConfirmed = true;
        else if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_N) ||
                 (click && CheckCollisionPointRec(mouse, g_quitButtons[1])))
        {
            g_quitQuestion = false;
            g_menu.releasing = true;
        }
    }

    void drawQuitQuestion()
    {
        const int width = GetScreenWidth(), height = GetScreenHeight();
        DrawRectangle(0, 0, width, height, Fade(BLACK, 0.6f));
        const int font = std::clamp(height / 24, 14, 40);
        const int panelWidth = std::min(width - 2 * font, font * 18), panelHeight = font * 6;
        const int left = (width - panelWidth) / 2, top = (height - panelHeight) / 2;
        DrawRectangle(left, top, panelWidth, panelHeight, Color{20, 20, 24, 240});
        DrawRectangleLines(left, top, panelWidth, panelHeight, kAccent);
        const char *title = "Quit the game?";
        DrawText(title, left + (panelWidth - MeasureText(title, font)) / 2, top + font, font, WHITE);
        const char *labels[2] = {"Quit (Enter)", "Keep playing (Esc)"};
        const int buttonWidth = (panelWidth - 3 * font) / 2;
        for (int i = 0; i < 2; ++i)
        {
            g_quitButtons[i] = Rectangle{float(left + font + i * (buttonWidth + font)), float(top + font * 3),
                                         float(buttonWidth), float(font * 2)};
            const bool hover = CheckCollisionPointRec(GetMousePosition(), g_quitButtons[i]);
            DrawRectangleRec(g_quitButtons[i], hover ? Color{255, 170, 30, 90} : Color{255, 255, 255, 25});
            const int labelFont = font * 2 / 3;
            DrawText(labels[i], int(g_quitButtons[i].x) + (buttonWidth - MeasureText(labels[i], labelFont)) / 2,
                     int(g_quitButtons[i].y) + (font * 2 - labelFont) / 2, labelFont, i == 0 ? kAccent : LIGHTGRAY);
        }
    }

    void drawFps(uint64_t framesShown)
    {
        using Clock = std::chrono::steady_clock;
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
    void updateSettingsMenu()
    {
        if (g_quitQuestion)
            updateQuitQuestion();
        else if (!g_menu.open && g_escapeAsksToQuit.load(std::memory_order_acquire) && IsKeyPressed(KEY_ESCAPE))
            g_quitQuestion = true;
        else if (IsKeyPressed(KEY_F1))
        {
            if (g_menu.open)
                closeMenu();
            else
            {
                g_menu = Menu{};
                g_menu.open = true;
            }
        }
        else if (g_menu.open)
        {
            if (g_menu.page == Page::Main)
                updateMain();
            else
                updateControls();
        }
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
        // A second close request while asking quits.
        if (g_quitQuestion)
            g_quitConfirmed = true;
        g_quitQuestion = true;
        g_menu.open = false;
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
    }
}
