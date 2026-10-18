#include "runtime/host_audio.h"
#include "runtime/host_settings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#if !defined(PLATFORM_VITA)
#include "raylib.h"
#endif

namespace
{
    constexpr unsigned kSampleRate = 48000u;
    // Queue depth the resampler aims for, two VSyncs: the SPU2 hands its output over in
    // bursts of about one VSync, when the EE reaches the VBlank.
    constexpr double kTargetFrames = 1920.0;
    // Beyond this the queue is cut back to the target (after a stall or a state load).
    constexpr size_t kMaxFrames = 4u * 1920u;
    // A queue empty this long (a pause, a state load, a long stall) is refilled to the target
    // before playing on.
    constexpr size_t kStarvedFrames = 2400u;

    struct HostAudio
    {
        std::mutex mutex; // the queue and the source; never held while calling raylib
        ps2x::AudioPull pull;
        std::vector<int16_t> queue; // interleaved frames, those from `head` on not played yet
        size_t head = 0u;
        double position = 0.0;         // read position past `head`, in frames
        double level = kTargetFrames;  // queue depth, smoothed
        int16_t last[2] = {0, 0};
        size_t emptyFrames = 0u;
        bool starved = false;
        std::FILE *wav = nullptr;
        uint32_t wavFrames = 0u;
        bool streaming = false;
        ps2x::HostAudioStats stats;
#if !defined(PLATFORM_VITA)
        AudioStream stream{};
#endif
    };

    HostAudio &audio()
    {
        static HostAudio state;
        return state;
    }

    void writeWavHeader(std::FILE *file, uint32_t frames)
    {
        const uint32_t dataBytes = frames * 4u;
        const auto u32 = [file](uint32_t value) { std::fwrite(&value, 4, 1, file); };
        const auto u16 = [file](uint16_t value) { std::fwrite(&value, 2, 1, file); };
        std::fwrite("RIFF", 1, 4, file);
        u32(36u + dataBytes);
        std::fwrite("WAVEfmt ", 1, 8, file);
        u32(16u);
        u16(1u); // PCM
        u16(2u);
        u32(kSampleRate);
        u32(kSampleRate * 4u);
        u16(4u);
        u16(16u);
        std::fwrite("data", 1, 4, file);
        u32(dataBytes);
    }

    // Moves what the source has produced to the queue (`keep`) and the WAV file.
    void drain(HostAudio &state, bool keep)
    {
        int16_t chunk[1024 * 2];
        for (int round = 0; round < 256; ++round)
        {
            const size_t got = state.pull(chunk, 1024u);
            if (got == 0u)
                break;
            if (keep)
                state.queue.insert(state.queue.end(), chunk, chunk + got * 2u);
            if (state.wav)
            {
                std::fwrite(chunk, 4, got, state.wav);
                state.wavFrames += static_cast<uint32_t>(got);
            }
            if (got < 1024u)
                break;
        }
    }

#if !defined(PLATFORM_VITA)
    // raylib's audio thread, under raylib's audio lock.
    void fill(void *buffer, unsigned int frames)
    {
        HostAudio &state = audio();
        auto *out = static_cast<int16_t *>(buffer);
        std::lock_guard<std::mutex> lock(state.mutex);
        if (!state.pull)
        {
            std::memset(out, 0, size_t(frames) * 4u);
            return;
        }
        drain(state, true);
        size_t available = state.queue.size() / 2u - state.head;
        if (available > kMaxFrames)
        {
            state.stats.droppedFrames += available - size_t(kTargetFrames);
            state.head += available - size_t(kTargetFrames);
            available = size_t(kTargetFrames);
            state.position = 0.0;
            state.level = kTargetFrames;
        }
        // After a gap, play again only once the queue is full and at rate 1: resuming at once
        // from a near-empty queue made the rate dip, then overshoot (the sound slowed down and
        // sped up after the settings menu or a state load).
        if (available == 0u)
            state.emptyFrames += frames;
        else if (!state.starved)
            state.emptyFrames = 0u;
        if (state.emptyFrames >= kStarvedFrames)
            state.starved = true;
        if (state.starved && available >= size_t(kTargetFrames))
        {
            state.starved = false;
            state.emptyFrames = 0u;
            state.level = kTargetFrames;
            state.position = 0.0;
        }
        if (state.starved)
            available = 0u;
        // Smoothed over half a second, which irons out the VSync bursts. An empty queue
        // (a short stall) leaves it alone, so playback resumes at the rate it had.
        if (available > 0u)
            state.level += (double(available) - state.level) * std::min(1.0, frames / (0.5 * kSampleRate));
        const double rate = ps2x::hostAudioRateFor(state.level);
        state.stats.rate = rate;
        const int16_t *in = state.queue.data() + state.head * 2u;
        const int volume = std::clamp(ps2x::hostSettings().volume, 0, 100);
        for (unsigned int i = 0; i < frames; ++i)
        {
            const size_t index = size_t(state.position);
            if (index + 1u < available)
            {
                const double fraction = state.position - double(index);
                for (int c = 0; c < 2; ++c)
                {
                    const double a = in[index * 2u + c], b = in[(index + 1u) * 2u + c];
                    state.last[c] = int16_t(std::lrint(a + (b - a) * fraction));
                }
                state.position += rate;
            }
            else
            {
                // Underrun: fade out what played last instead of clicking.
                ++state.stats.underrunFrames;
                for (int c = 0; c < 2; ++c)
                    state.last[c] = int16_t(state.last[c] * 31 / 32);
            }
            out[i * 2u] = int16_t(state.last[0] * volume / 100);
            out[i * 2u + 1u] = int16_t(state.last[1] * volume / 100);
        }
        const size_t consumed = std::min(size_t(state.position), available > 0u ? available - 1u : 0u);
        state.head += consumed;
        state.position = std::max(0.0, state.position - double(consumed));
        if (state.head >= 8192u)
        {
            state.queue.erase(state.queue.begin(), state.queue.begin() + std::ptrdiff_t(state.head * 2u));
            state.head = 0u;
        }
    }
#endif
}

namespace ps2x
{
    double hostAudioRateFor(double level)
    {
        // Proportional: at full speed the queue sits at the target and plays at rate 1;
        // a producer at 80% settles the queue at 80% of the target and the rate at 0.8.
        return std::clamp(level / kTargetFrames, 0.5, 1.5);
    }

    void startHostAudio(AudioPull pull, bool device)
    {
        stopHostAudio();
        HostAudio &state = audio();
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.pull = std::move(pull);
            state.queue.clear();
            state.head = 0u;
            state.position = 0.0;
            state.level = kTargetFrames;
            state.last[0] = state.last[1] = 0;
            state.emptyFrames = 0u;
            state.starved = false;
            if (const char *path = std::getenv("BDR_AUDIO_WAV"); path && *path)
            {
                state.wav = std::fopen(path, "wb");
                state.wavFrames = 0u;
                if (state.wav)
                    writeWavHeader(state.wav, 0u);
            }
        }
#if !defined(PLATFORM_VITA)
        if (device && IsAudioDeviceReady())
        {
            state.stream = LoadAudioStream(kSampleRate, 16, 2);
            SetAudioStreamCallback(state.stream, fill);
            PlayAudioStream(state.stream);
            state.streaming = true;
        }
#else
        (void)device;
#endif
    }

    void stopHostAudio()
    {
        HostAudio &state = audio();
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.pull = nullptr; // the audio thread plays silence from here on
            if (state.wav)
            {
                std::fseek(state.wav, 0, SEEK_SET);
                writeWavHeader(state.wav, state.wavFrames);
                std::fclose(state.wav);
                state.wav = nullptr;
            }
        }
#if !defined(PLATFORM_VITA)
        if (state.streaming)
        {
            StopAudioStream(state.stream);
            UnloadAudioStream(state.stream);
            state.streaming = false;
        }
#endif
    }

    HostAudioStats hostAudioStats()
    {
        HostAudio &state = audio();
        std::lock_guard<std::mutex> lock(state.mutex);
        return state.stats;
    }

    void pumpHostAudio()
    {
        HostAudio &state = audio();
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.pull && !state.streaming)
            drain(state, false);
    }
}
