#pragma once
#include "runtime/gs/gs_backend.h"

#include <memory>

struct GSRegisters;

// GS backend on paraLLEl-GS (Vulkan compute). Throws if no usable Vulkan
// device is found; the caller then keeps the CPU rasterizer.
// Present returns the previous frame, whose readback has had a frame to land, unless
// presentCurrentFrame: then it waits for this one (for a caller on a thread of its own).
std::unique_ptr<GSRasterBackend> CreateParallelGSBackend(GSRegisters &registers, bool presentCurrentFrame = false);

// Runs inner's GIF processing on a thread of its own (calls with a result wait for it,
// except Present, which returns the last frame that thread finished presenting).
std::unique_ptr<GSRasterBackend> CreateAsyncGSBackend(std::unique_ptr<GSRasterBackend> inner);
