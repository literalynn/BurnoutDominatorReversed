#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace ps2x
{
// Fills interleaved stereo int16 frames at 48 kHz and returns how many it wrote.
using AudioPull = std::function<size_t(int16_t *frames, size_t count)>;

// Plays the SPU2 output on the host: frames are pulled from the audio thread and
// resampled at the rate they are produced, so a game running below full speed sounds
// slowed down rather than broken up. `device` needs raylib's audio device
// (InitAudioDevice). BDR_AUDIO_WAV=file also writes everything pulled to a WAV file.
void startHostAudio(AudioPull pull, bool device);
// Stops pulling before the source goes away, and closes the WAV file.
void stopHostAudio();
// Without a device nothing pulls: moves the produced frames to the WAV file, if any.
// Call it regularly from the host loop.
void pumpHostAudio();

// The playback rate the resampler settles on for a queue `level` frames deep: the queue
// stays near its target while the rate follows the production speed.
double hostAudioRateFor(double level);
}
