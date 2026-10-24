// Burnout Dominator at more frames a second than the console's 60 (settings.ini refresh_rate).
//
// The game steps its simulation by dt = 1 / rate (0x1E68A0) and times race steps with a governor
// (0x1E6668, FINDINGS.md E019), at 60 or 50 steps a second, half that in the front end. With a
// VBlank rate set (runtime/vblank_rate.h), the wrappers below give both that rate instead, then
// call the original function. The few places that assume 60 or 50 steps a second and change what
// the player sees get the rate too, so the game moves as it does at 60 Hz (FINDINGS.md E021).
// Without a VBlank rate every wrapper only calls the original.
#include "burnout_frame_rate.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/vblank_rate.h"

#include <cstdint>
#include <cstring>

namespace burnout {
namespace {
constexpr uint32_t kPalFlag = 0x432E98;     // byte, 1 when the game runs at 50 Hz
constexpr uint32_t kCameraRate = 0x3C18C0;  // 60.0f, read by the camera only
constexpr uint32_t kStepSeconds = 0x4915BC; // dt, float

PS2Runtime::RecompiledFunction g_setRate = nullptr;
PS2Runtime::RecompiledFunction g_setStep = nullptr;
PS2Runtime::RecompiledFunction g_camera = nullptr;
PS2Runtime::RecompiledFunction g_powf = nullptr;
PS2Runtime::RecompiledFunction g_menuBox = nullptr;
PS2Runtime::RecompiledFunction g_vblankCount = nullptr;

uint32_t consoleRate(const uint8_t* rdram) {
    return rdram[kPalFlag] ? 50u : 60u;
}

// 0x1E68A0(a0 = steps a second) sets dt (0x432F6C) and the step in ms (0x432F68).
void setRate(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    if (const uint32_t rate = ps2x::vblankRate()) {
        const int32_t steps = static_cast<int32_t>(int64_t(GPR_S32(ctx, 4)) * rate / consoleRate(rdram));
        SET_GPR_S32(ctx, 4, steps);
        // The camera damps its motion round(60.0 * dt * steps this frame) times: once a step at
        // any rate with this, each one 60 / rate of the console's (power below).
        const float perSecond = static_cast<float>(steps);
        std::memcpy(rdram + kCameraRate, &perSecond, sizeof(perSecond));
    }
    g_setRate(rdram, ctx, runtime);
}

// 0x1E6668(f12 = step in ms, f13 = threshold in ms, a0 = governor) sets the race governor's step.
void setStep(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    if (const uint32_t rate = ps2x::vblankRate()) {
        const float scale = static_cast<float>(consoleRate(rdram)) / static_cast<float>(rate);
        ctx->f[12] *= scale;
        ctx->f[13] *= scale;
    }
    g_setStep(rdram, ctx, runtime);
}

// At 50 Hz the camera (0x169998) raises its damping per step to the power 1.2 with powf
// (0x37C5A8, called from 0x169B64 and 0x169B74). That branch runs at any VBlank rate, the
// power being 60 * dt. The flag is set only while the camera runs, and put back however it
// returns: if it returns to the scheduler before it reads the flag, that frame keeps the
// damping of one console step.
void camera(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    if (!ps2x::vblankRate() || rdram[kPalFlag]) {
        g_camera(rdram, ctx, runtime);
        return;
    }
    rdram[kPalFlag] = 1;
    g_camera(rdram, ctx, runtime);
    rdram[kPalFlag] = 0;
}

void power(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t returnAddress = GPR_U32(ctx, 31);
    if ((returnAddress == 0x169B6Cu || returnAddress == 0x169B7Cu) && ps2x::vblankRate()) {
        float dt = 0.0f;
        std::memcpy(&dt, rdram + kStepSeconds, sizeof(dt));
        ctx->f[13] = 60.0f * dt;
    }
    g_powf(rdram, ctx, runtime);
}

// Menu boxes (0x24A260) slide for duration * 30 frames (25 at 50 Hz), the front end's frame rate:
// the duration (float at a3) is scaled to the VBlank rate while the function runs.
void menuBox(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t rate = ps2x::vblankRate();
    if (!rate) {
        g_menuBox(rdram, ctx, runtime);
        return;
    }
    const uint32_t address = GPR_U32(ctx, 7) & (PS2_RAM_SIZE - 1) & ~3u;
    float duration = 0.0f;
    std::memcpy(&duration, rdram + address, sizeof(duration));
    const float scaled = duration * static_cast<float>(rate) / static_cast<float>(consoleRate(rdram));
    std::memcpy(rdram + address, &scaled, sizeof(scaled));
    g_menuBox(rdram, ctx, runtime);
    std::memcpy(rdram + address, &duration, sizeof(duration));
}

// At each race start (0x208390) the 64-bit clock 0x1E6C30 adds a COP0 Count wrap for every 873.8
// VBlanks (728.2 at 50 Hz) since its last call, counted by the VBlank handler 0x1B1C48 and read by
// 0x1B9F80 (a leaf): the count it returns is converted to console VBlanks. The count jumps when
// the rate changes (in the front end); that race start then adds no wrap or too many, and resets
// the governor right after.
void vblankCount(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    g_vblankCount(rdram, ctx, runtime);
    if (const uint32_t rate = ps2x::vblankRate())
        SET_GPR_U32(ctx, 2, static_cast<uint32_t>(uint64_t(GPR_U32(ctx, 2)) * consoleRate(rdram) / rate));
}

void wrap(PS2Runtime& runtime, uint32_t address, PS2Runtime::RecompiledFunction& original,
          PS2Runtime::RecompiledFunction wrapper) {
    if (runtime.hasFunction(address)) {
        original = runtime.lookupFunction(address);
        runtime.replaceFunction(address, wrapper);
    }
}
} // namespace

void installFrameRate(PS2Runtime& runtime) {
    wrap(runtime, 0x1E68A0, g_setRate, setRate);
    wrap(runtime, 0x1E6668, g_setStep, setStep);
    wrap(runtime, 0x169998, g_camera, camera);
    wrap(runtime, 0x37C5A8, g_powf, power);
    wrap(runtime, 0x24A260, g_menuBox, menuBox);
    wrap(runtime, 0x1B9F80, g_vblankCount, vblankCount);
}
} // namespace burnout
