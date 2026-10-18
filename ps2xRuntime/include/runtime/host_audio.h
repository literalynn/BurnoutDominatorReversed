#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace ps2x
{
// Fills interleaved stereo int16 frames at 48 kHz and returns how many it wrote.
using AudioPull = std::function<size_t(int16_t *frames, size_t count)>;

// Plays the SPU2 output on the host: frames are pulled from the audio thread and played
// at the tempo they are produced, at their own pitch, so a game running below full speed
// sounds slower rather than lower or broken up. `device` needs raylib's audio device
// (InitAudioDevice). BDR_AUDIO_WAV=file also writes everything pulled to a WAV file.
void startHostAudio(AudioPull pull, bool device);
// Stops pulling before the source goes away, and closes the WAV file.
void stopHostAudio();
// Without a device nothing pulls: moves the produced frames to the WAV file, if any.
// Call it regularly from the host loop.
void pumpHostAudio();

// The playback tempo settled on for a queue `level` frames deep: the queue stays near
// its target while the tempo follows the production speed.
double hostAudioRateFor(double level);

// Changes the tempo of 48 kHz stereo frames without changing their pitch (WSOLA, as
// SoundTouch does): 25 ms segments joined by 5 ms crossfades, each taken at the offset
// within 8 ms of its nominal place that best continues the previous one.
class TimeStretch
{
public:
    static constexpr size_t kSequence = 1200u;
    static constexpr size_t kOverlap = 240u;
    static constexpr size_t kSeek = 384u;
    // Input frames one segment reads.
    static constexpr size_t kLookahead = kSeek + kSequence;

    // Appends segments to `out` (interleaved) until it holds `wanted` frames or `available`
    // input frames run short of a segment; returns the input frames consumed. A tempo
    // below 1 consumes less than it produces.
    size_t process(const int16_t *in, size_t available, double tempo, size_t wanted, std::vector<int16_t> &out);
    // Forgets the previous segment, after a gap.
    void reset();

private:
    std::array<int16_t, kOverlap * 2u> m_tail{};
    bool m_hasTail = false;
    double m_skip = 0.0;
};

// What the player heard so far: frames faded out for want of samples (the game fell
// behind), frames dropped because the queue ran too deep, and the current playback rate.
struct HostAudioStats
{
    uint64_t underrunFrames = 0u;
    uint64_t droppedFrames = 0u;
    double rate = 1.0; // the tempo
};
HostAudioStats hostAudioStats();
}
