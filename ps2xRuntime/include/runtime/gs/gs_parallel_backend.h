#pragma once
#include "runtime/gs/gs_backend.h"

#include <memory>

struct GSRegisters;

// GS backend on paraLLEl-GS (Vulkan compute). Throws if no usable Vulkan
// device is found; the caller then keeps the CPU rasterizer.
std::unique_ptr<GSRasterBackend> CreateParallelGSBackend(GSRegisters &registers);
