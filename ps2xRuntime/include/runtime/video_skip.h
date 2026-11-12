#pragma once

#include <cstdint>

namespace ps2x
{
// Skipping the game's videos: while one plays (the runner tells how to see it, setVideoCheck),
// holding Start for a second starts a skip, which lasts until Start is released: the runner ends
// the video that plays, and each one that starts meanwhile, the way the game ends a video that
// has played to its end. settings.ini video_skip turns it off.
using VideoCheck = bool (*)(const uint8_t *rdram);
void setVideoCheck(VideoCheck check);

// EE thread, at every VBlank: whether a video plays.
void updateVideoSkip(const uint8_t *rdram);
// Pad reads: whether Start is held (gamepad or keyboard). Returns true while the game must not see
// it: from the moment the skip starts until Start is released.
bool holdToSkipVideo(bool startHeld);
// A skip is on: the runner ends the videos that play. Any thread.
bool videoSkipping();
// Window thread: "Hold START to skip" while a video plays, how long Start has been held, or
// "Skipping video".
void drawVideoSkipHint();
}
