#pragma once

#include <cstdint>

namespace ps2x
{
// Skipping the game's videos: while one plays (the runner tells how to see it, setVideoCheck),
// holding Start for a second runs the game faster than real time until the video ends, with the
// picture and the sound held back. The game itself runs every frame of the video, so it ends up
// exactly where watching would have left it. settings.ini video_skip turns it off.
using VideoCheck = bool (*)(const uint8_t *rdram);
void setVideoCheck(VideoCheck check);

// EE thread, at every VBlank: whether a video is being skipped (the scheduler then stops waiting
// for the host clock).
bool updateVideoSkip(const uint8_t *rdram);
// Pad reads: whether Start is held (gamepad or keyboard). Returns true while the game must not see
// it: from the moment the skip starts until Start is released.
bool holdToSkipVideo(bool startHeld);
// The skip is running: the window shows no picture and the speakers play nothing. Any thread.
bool videoSkipping();
// Window thread: "Hold START to skip" while a video plays, how long Start has been held, or
// "Skipping video".
void drawVideoSkipHint();
}
