#include "runtime/video_skip.h"

#include "ps2_host_backend.h"
#include "runtime/host_settings.h"
#include "runtime/input_device.h"
#include "runtime/settings_menu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>

namespace
{
    using Clock = std::chrono::steady_clock;
    constexpr Clock::duration kHold = std::chrono::seconds(1);

    std::atomic<ps2x::VideoCheck> g_check{nullptr};
    std::atomic<bool> g_playing{false};
    std::atomic<bool> g_skipping{false};    // from the end of the hold until Start is released
    std::atomic<Clock::rep> g_holdStart{0}; // when Start was pressed during the video, 0 = not held
    Clock::time_point g_playStart;          // EE thread
}

namespace ps2x
{
    void setVideoCheck(VideoCheck check)
    {
        g_check.store(check, std::memory_order_release);
    }

    void updateVideoSkip(const uint8_t *rdram)
    {
        const VideoCheck check = g_check.load(std::memory_order_acquire);
        const bool playing = hostSettings().videoSkip && check && check(rdram);
        if (g_playing.exchange(playing, std::memory_order_acq_rel) == playing)
            return;
        if (playing)
        {
            g_playStart = Clock::now();
            std::printf("[video] playing\n");
        }
        else
            std::printf("[video] ended after %.1f s\n", std::chrono::duration<double>(Clock::now() - g_playStart).count());
    }

    bool holdToSkipVideo(bool startHeld)
    {
        if (!startHeld)
        {
            g_holdStart.store(0, std::memory_order_relaxed);
            g_skipping.store(false, std::memory_order_release);
            return false;
        }
        if (g_skipping.load(std::memory_order_acquire))
            return true;
        if (!g_playing.load(std::memory_order_acquire))
        {
            g_holdStart.store(0, std::memory_order_relaxed);
            return false;
        }
        const Clock::time_point now = Clock::now();
        const Clock::rep start = g_holdStart.load(std::memory_order_relaxed);
        if (start == 0)
        {
            g_holdStart.store(now.time_since_epoch().count(), std::memory_order_relaxed);
            return false;
        }
        if (now - Clock::time_point(Clock::duration(start)) < kHold)
            return false;
        // Until Start is released the game no longer sees it (a press it reads after the videos
        // must be a new one) and every video that plays is ended.
        g_holdStart.store(0, std::memory_order_relaxed);
        g_skipping.store(true, std::memory_order_release);
        std::printf("[video] skipping\n");
        return true;
    }

    bool videoSkipping()
    {
        return g_skipping.load(std::memory_order_acquire);
    }

    void drawVideoSkipHint()
    {
        if (!g_playing.load(std::memory_order_acquire))
            return;
        std::string text = "Skipping video";
        float held = 0.0f;
        if (!videoSkipping())
        {
            const int key = hostSettings().keys[12][0]; // the first keyboard input bound to Start
            const bool keyboard = buttonPromptDevice() == InputDevice::Keyboard && key != 0;
            text = "Hold " + (keyboard ? inputName(key) : std::string("Start")) + " to skip";
            if (const Clock::rep start = g_holdStart.load(std::memory_order_relaxed); start != 0)
                held = std::min(1.0f, std::chrono::duration<float>(Clock::now() - Clock::time_point(Clock::duration(start))) /
                                          std::chrono::duration<float>(kHold));
        }
        drawCornerNotice(text, held);
    }
}
