#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace ps2x::iop
{
    constexpr uint32_t Iso9660SectorSize = 2048u;

    struct Iso9660File
    {
        uint32_t lsn = 0u;
        uint32_t size = 0u;
        bool directory = false;
        std::string name;
        std::string path;
        std::array<uint8_t, 8u> date{};
    };

    // The callback must return true only after reading every requested byte.
    // All offsets are uint64_t: PS2 DVD files commonly reside beyond 4 GiB.
    using Iso9660ReadAt = std::function<bool(uint64_t, void *, size_t)>;

    // Primary ISO9660 only. Unsupported multi-extent, interleaved, multi-volume
    // layouts and malformed bounds/endian fields fail with an explicit error.
    // Joliet and Rock Ridge aliases are intentionally not interpreted.
    // Lookup bounds: 64 path levels, 32 MiB/100000 entries per directory.
    bool findIso9660File(const Iso9660ReadAt &readAt, uint64_t imageSize,
                         std::string_view guestPath, Iso9660File &result,
                         std::string *error = nullptr);

    bool findIso9660File(const std::filesystem::path &image,
                         std::string_view guestPath, Iso9660File &result,
                         std::string *error = nullptr);

    // Raw original-image sector reads; validate the entire requested sector
    // range even when byteCount is smaller because a guest buffer is clipped.
    bool readIso9660Sectors(const Iso9660ReadAt &readAt, uint64_t imageSize,
                            uint32_t lsn, uint32_t sectors, void *destination,
                            size_t byteCount, std::string *error = nullptr);

    bool readIso9660Sectors(const std::filesystem::path &image,
                            uint32_t lsn, uint32_t sectors, void *destination,
                            size_t byteCount, std::string *error = nullptr);
}
