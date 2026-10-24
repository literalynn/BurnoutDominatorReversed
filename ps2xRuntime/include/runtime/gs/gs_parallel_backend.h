#pragma once
#include "runtime/gs/gs_backend.h"

#include <memory>

struct GSRegisters;

// GS backend on paraLLEl-GS (Vulkan compute). Throws if no usable Vulkan
// device is found; the caller then keeps the CPU rasterizer.
// Present waits for this frame's readback, or at a VBlank rate above the console's returns the
// previous frame's (runtime/vblank_rate.h).
std::unique_ptr<GSRasterBackend> CreateParallelGSBackend(GSRegisters &registers);

// Runs inner's GIF processing on a thread of its own (calls with a result wait for it,
// except Present, which returns the last frame that thread finished presenting).
std::unique_ptr<GSRasterBackend> CreateAsyncGSBackend(std::unique_ptr<GSRasterBackend> inner);
