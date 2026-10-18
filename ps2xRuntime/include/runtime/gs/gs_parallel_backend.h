#pragma once
#include "runtime/gs/gs_backend.h"

#include <memory>

struct GSRegisters;

// GS backend on paraLLEl-GS (Vulkan compute). Throws if no usable Vulkan
// device is found; the caller then keeps the CPU rasterizer.
std::unique_ptr<GSRasterBackend> CreateParallelGSBackend(GSRegisters &registers);

// Runs inner's GIF processing on a thread of its own (calls with a result wait for it).
std::unique_ptr<GSRasterBackend> CreateAsyncGSBackend(std::unique_ptr<GSRasterBackend> inner);
