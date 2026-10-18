#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ps2x
{
// Changes to disc files, made as the IOP reads them, from the disc image or from the extracted
// disc directory; the files on disk are never written. A patch rewrites the bytes of a whole
// file and must keep its size, or it is ignored. Register patches before the game starts.
using DiscFilePatch = std::function<void(std::vector<uint8_t> &file)>;
void addDiscFilePatch(std::string discPath, DiscFilePatch patch); // e.g. "DATA/MEM/ENGLISH.LOC"

// The patched bytes for a host file the IOP opens (the disc image, or one file of the
// extracted disc directory), with their offset in that file.
struct DiscOverlay
{
    uint64_t offset = 0u;
    std::shared_ptr<const std::vector<uint8_t>> bytes;
};
std::vector<DiscOverlay> discOverlaysFor(const std::filesystem::path &hostFile, const std::filesystem::path &cdImage,
                                         const std::filesystem::path &cdRoot);
// Copies the overlays' bytes over a read of `size` bytes at `offset` of that file.
void applyDiscOverlays(const std::vector<DiscOverlay> &overlays, uint64_t offset, void *data, size_t size);
}
