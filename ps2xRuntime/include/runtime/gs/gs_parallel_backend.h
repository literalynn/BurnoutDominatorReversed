#pragma once
#include "runtime/gs/gs_backend.h"

#include <memory>

struct GSRegisters;

// GS backend on paraLLEl-GS (Vulkan compute). Throws if no usable Vulkan
// device is found; the caller then keeps the CPU rasterizer.
// Present waits for this frame's readback, or at a VBlank rate above the console's returns the
// previous frame's (runtime/vblank_rate.h). The backend implements GSDeferredReadback.
std::unique_ptr<GSRasterBackend> CreateParallelGSBackend(GSRegisters &registers);

// A backend whose Present can leave the wait for the frame's readback, and the copy of its
// pixels, to the thread that takes the frame: the thread that presents then records the next
// frame while the GPU draws this one.
class GSDeferredReadback
{
public:
    // From now on, at the console's VBlank rate, Present submits the frame's readback and
    // returns no frame; takeReadback, right after it on the same thread, returns the readback
    // to finish the frame with, or -1.
    virtual void deferReadbacks() = 0;
    virtual int takeReadback() = 0;
    // Waits for that readback and returns its frame. Any thread, but before the second Present
    // after the one that submitted it, which reuses its buffer.
    virtual PresentationFrame finishReadback(int readback) = 0;

protected:
    ~GSDeferredReadback() = default;
};

// Runs inner's GIF processing on a thread of its own (calls with a result wait for it,
// except Present, which returns the last frame that thread finished presenting, and finishes
// its readback when inner is a GSDeferredReadback).
std::unique_ptr<GSRasterBackend> CreateAsyncGSBackend(std::unique_ptr<GSRasterBackend> inner);
