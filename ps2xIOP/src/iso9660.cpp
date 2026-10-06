#include "ps2x/iop/iso9660.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace ps2x::iop
{
    namespace
    {
        bool fail(std::string *error, std::string_view message)
        {
            if (error)
                *error = message;
            return false;
        }

        uint16_t le16(const uint8_t *p)
        {
            return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8u));
        }

        uint16_t be16(const uint8_t *p)
        {
            return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8u) | p[1]);
        }

        uint32_t le32(const uint8_t *p)
        {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8u) |
                   (static_cast<uint32_t>(p[2]) << 16u) | (static_cast<uint32_t>(p[3]) << 24u);
        }

        uint32_t be32(const uint8_t *p)
        {
            return (static_cast<uint32_t>(p[0]) << 24u) | (static_cast<uint32_t>(p[1]) << 16u) |
                   (static_cast<uint32_t>(p[2]) << 8u) | static_cast<uint32_t>(p[3]);
        }

        bool both16(const uint8_t *p, uint16_t &value, std::string *error)
        {
            value = le16(p);
            return value == be16(p + 2u) || fail(error, "ISO9660 dual-endian 16-bit field disagrees");
        }

        bool both32(const uint8_t *p, uint32_t &value, std::string *error)
        {
            value = le32(p);
            return value == be32(p + 4u) || fail(error, "ISO9660 dual-endian 32-bit field disagrees");
        }

        bool rangeWithin(uint64_t offset, uint64_t size, uint64_t total)
        {
            return offset <= total && size <= total - offset;
        }

        bool normalizeComponent(std::string_view input, std::string &output, std::string *error)
        {
            output.assign(input);
            const size_t version = output.rfind(';');
            if (version != std::string::npos)
            {
                if (version + 1u == output.size() ||
                    !std::all_of(output.begin() + static_cast<std::ptrdiff_t>(version + 1u), output.end(),
                                 [](unsigned char ch) { return ch >= '0' && ch <= '9'; }))
                    return fail(error, "Unsupported nonnumeric ISO9660 filename version");
                output.resize(version);
            }
            if (!output.empty() && output.back() == '.')
                output.pop_back();
            if (output.empty() || output == "." || output == "..")
                return fail(error, "Unsafe empty/dot ISO9660 path component");
            for (char &ch : output)
            {
                const auto byte = static_cast<unsigned char>(ch);
                if (byte < 0x20u || byte >= 0x7Fu || ch == '/' || ch == '\\' || ch == ':' || ch == ';')
                    return fail(error, "Unsafe or unsupported ISO9660 path component");
                if (ch >= 'a' && ch <= 'z')
                    ch = static_cast<char>(ch - 'a' + 'A');
            }
            return true;
        }

        bool normalizePath(std::string_view guestPath, std::vector<std::string> &parts, std::string *error)
        {
            if (guestPath.empty() || guestPath.size() > 1024u)
                return fail(error, "ISO9660 path is empty or exceeds 1024 bytes");
            std::string path(guestPath);
            std::replace(path.begin(), path.end(), '\\', '/');
            const size_t colon = path.find(':');
            if (colon != std::string::npos)
            {
                std::string device = path.substr(0u, colon);
                for (char &ch : device)
                    if (ch >= 'a' && ch <= 'z')
                        ch = static_cast<char>(ch - 'a' + 'A');
                if (device != "CDROM0" && device != "CDROM")
                    return fail(error, "ISO9660 path has an unsupported device prefix");
                path.erase(0u, colon + 1u);
            }
            size_t cursor = 0u;
            while (cursor < path.size())
            {
                if (path[cursor] == '/')
                {
                    ++cursor;
                    continue;
                }
                const size_t end = path.find('/', cursor);
                const size_t length = (end == std::string::npos ? path.size() : end) - cursor;
                if (length > 255u)
                    return fail(error, "ISO9660 path component exceeds 255 bytes");
                std::string part;
                if (!normalizeComponent(std::string_view(path).substr(cursor, length), part, error))
                    return false;
                parts.push_back(std::move(part));
                if (parts.size() > 64u)
                    return fail(error, "ISO9660 path exceeds 64 levels");
                cursor += length;
            }
            return !parts.empty() || fail(error, "ISO9660 path has no file component");
        }

        bool parseRecord(const uint8_t *record, size_t length, uint64_t volumeSize,
                         Iso9660File &result, std::string *error)
        {
            if (length < 34u || record[0] != length || record[32] == 0u ||
                33u + static_cast<size_t>(record[32]) > length)
                return fail(error, "Truncated or invalid ISO9660 directory record");
            if (record[25] & 0x80u)
                return fail(error, "Unsupported multi-extent ISO9660 file");
            if (record[26] != 0u || record[27] != 0u)
                return fail(error, "Unsupported interleaved ISO9660 file");
            uint16_t volume = 0u;
            uint32_t lsn = 0u;
            uint32_t size = 0u;
            if (!both16(record + 28u, volume, error) || !both32(record + 2u, lsn, error) ||
                !both32(record + 10u, size, error))
                return false;
            if (volume != 1u)
                return fail(error, "Unsupported multi-volume ISO9660 file");
            const uint64_t dataLsn = static_cast<uint64_t>(lsn) + record[1];
            if (dataLsn > std::numeric_limits<uint32_t>::max() ||
                !rangeWithin(dataLsn * Iso9660SectorSize, size, volumeSize))
                return fail(error, "ISO9660 file extent is outside the volume");
            result = {};
            result.lsn = static_cast<uint32_t>(dataLsn);
            result.size = size;
            result.directory = (record[25] & 2u) != 0u;
            std::copy_n(record + 18u, 7u, result.date.begin());
            // The special self/parent identifiers have no ordinary filename.
            if (record[32] == 1u && (record[33] == 0u || record[33] == 1u))
                return true;
            return normalizeComponent(std::string_view(reinterpret_cast<const char *>(record + 33u), record[32]), result.name, error);
        }

        struct FileSource
        {
            std::ifstream file;
            uint64_t size = 0u;

            bool open(const std::filesystem::path &path, std::string *error)
            {
                file.open(path, std::ios::binary);
                if (!file)
                    return fail(error, "Cannot open configured ISO image");
                file.seekg(0, std::ios::end);
                const std::streampos end = file.tellg();
                if (end < std::streampos(0))
                    return fail(error, "Cannot determine configured ISO image size");
                size = static_cast<uint64_t>(static_cast<std::streamoff>(end));
                return true;
            }

            bool read(uint64_t offset, void *destination, size_t count)
            {
                if (!rangeWithin(offset, count, size) ||
                    offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
                    count > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max()))
                    return false;
                file.clear();
                file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
                if (!file)
                    return false;
                file.read(static_cast<char *>(destination), static_cast<std::streamsize>(count));
                return file.gcount() == static_cast<std::streamsize>(count);
            }
        };
    }

    bool findIso9660File(const Iso9660ReadAt &readAt, uint64_t imageSize,
                         std::string_view guestPath, Iso9660File &result, std::string *error)
    {
        result = {};
        if (error)
            error->clear();
        if (!readAt)
            return fail(error, "No ISO9660 image reader");
        std::vector<std::string> parts;
        if (!normalizePath(guestPath, parts, error))
            return false;
        std::array<uint8_t, Iso9660SectorSize> primary{};
        std::array<uint8_t, Iso9660SectorSize> sector{};
        bool foundPrimary = false;
        bool foundTerminator = false;
        for (uint32_t lsn = 16u; lsn < 272u; ++lsn)
        {
            const uint64_t offset = static_cast<uint64_t>(lsn) * Iso9660SectorSize;
            if (!rangeWithin(offset, sector.size(), imageSize) || !readAt(offset, sector.data(), sector.size()))
                return fail(error, "Truncated ISO9660 volume descriptor sequence");
            if (std::memcmp(sector.data() + 1u, "CD001", 5u) != 0 || sector[6] != 1u)
                return fail(error, "Invalid ISO9660 volume descriptor signature/version");
            if (sector[0] == 1u && !foundPrimary)
            {
                primary = sector;
                foundPrimary = true;
            }
            if (sector[0] == 255u)
            {
                foundTerminator = true;
                break;
            }
        }
        if (!foundPrimary || !foundTerminator)
            return fail(error, "Missing primary ISO9660 descriptor or terminator");
        uint32_t volumeSectors = 0u;
        uint16_t blockSize = 0u;
        uint16_t volumeSetSize = 0u;
        uint16_t volumeSequence = 0u;
        if (!both32(primary.data() + 80u, volumeSectors, error) || !both16(primary.data() + 128u, blockSize, error) ||
            !both16(primary.data() + 120u, volumeSetSize, error) || !both16(primary.data() + 124u, volumeSequence, error))
            return false;
        if (volumeSetSize != 1u || volumeSequence != 1u)
            return fail(error, "Unsupported multi-volume ISO9660 descriptor");
        if (blockSize != Iso9660SectorSize)
            return fail(error, "Unsupported ISO9660 logical block size (expected 2048)");
        const uint64_t volumeSize = static_cast<uint64_t>(volumeSectors) * Iso9660SectorSize;
        if (volumeSectors < 18u || volumeSize > imageSize)
            return fail(error, "Declared ISO9660 volume exceeds image bounds");
        Iso9660File current;
        const size_t rootLength = primary[156u];
        if (rootLength < 34u || rootLength > primary.size() - 156u)
            return fail(error, "Invalid ISO9660 root directory record size");
        if (!parseRecord(primary.data() + 156u, rootLength, volumeSize, current, error))
            return false;
        if (primary[156u + 32u] != 1u || primary[156u + 33u] != 0u)
            return fail(error, "Invalid ISO9660 root directory identifier");
        if (!current.directory)
            return fail(error, "ISO9660 root record is not a directory");

        std::unordered_set<uint32_t> ancestors;
        std::string canonical;
        for (size_t depth = 0u; depth < parts.size(); ++depth)
        {
            if (!current.directory)
                return fail(error, "ISO9660 path descends through a regular file");
            if (!ancestors.insert(current.lsn).second)
                return fail(error, "ISO9660 directory extent cycle");
            if (current.size > 32u * 1024u * 1024u)
                return fail(error, "Unsupported ISO9660 directory larger than 32 MiB");
            std::optional<Iso9660File> match;
            std::unordered_set<std::string> names;
            for (uint64_t cursor = 0u; cursor < current.size; cursor += Iso9660SectorSize)
            {
                const size_t count = static_cast<size_t>(std::min<uint64_t>(Iso9660SectorSize, current.size - cursor));
                const uint64_t offset = static_cast<uint64_t>(current.lsn) * Iso9660SectorSize + cursor;
                if (!rangeWithin(offset, count, volumeSize) || !readAt(offset, sector.data(), count))
                    return fail(error, "Cannot read bounded ISO9660 directory block");
                size_t position = 0u;
                while (position < count && sector[position] != 0u)
                {
                    const size_t length = sector[position];
                    if (length < 34u || length > count - position)
                        return fail(error, "ISO9660 directory record crosses block boundary");
                    Iso9660File entry;
                    if (!parseRecord(sector.data() + position, length, volumeSize, entry, error))
                        return false;
                    position += length;
                    if (entry.name.empty())
                        continue;
                    if (!names.insert(entry.name).second)
                        return fail(error, "Ambiguous duplicate normalized ISO9660 filename");
                    if (names.size() > 100000u)
                        return fail(error, "Unsupported ISO9660 directory exceeding 100000 entries");
                    if (entry.name == parts[depth])
                        match = std::move(entry);
                }
            }
            if (!match)
                return fail(error, "File not found in primary ISO9660 namespace");
            if (!canonical.empty())
                canonical += '/';
            canonical += match->name;
            current = std::move(*match);
        }
        current.path = std::move(canonical);
        result = std::move(current);
        return true;
    }

    bool findIso9660File(const std::filesystem::path &image,
                         std::string_view guestPath, Iso9660File &result, std::string *error)
    {
        result = {};
        if (error)
            error->clear();
        FileSource source;
        if (!source.open(image, error))
            return false;
        return findIso9660File([&source](uint64_t offset, void *destination, size_t count)
                               { return source.read(offset, destination, count); },
                               source.size, guestPath, result, error);
    }

    bool readIso9660Sectors(const Iso9660ReadAt &readAt, uint64_t imageSize,
                            uint32_t lsn, uint32_t sectors, void *destination,
                            size_t byteCount, std::string *error)
    {
        if (error)
            error->clear();
        const uint64_t offset = static_cast<uint64_t>(lsn) * Iso9660SectorSize;
        const uint64_t requested = static_cast<uint64_t>(sectors) * Iso9660SectorSize;
        if (byteCount > requested || !rangeWithin(offset, requested, imageSize))
            return fail(error, "Original ISO sector range exceeds image/buffer bounds");
        if (byteCount == 0u)
            return true;
        if (!destination || !readAt || !readAt(offset, destination, byteCount))
            return fail(error, "Short or failed read from original ISO image");
        return true;
    }

    bool readIso9660Sectors(const std::filesystem::path &image,
                            uint32_t lsn, uint32_t sectors, void *destination,
                            size_t byteCount, std::string *error)
    {
        if (error)
            error->clear();
        FileSource source;
        if (!source.open(image, error))
            return false;
        return readIso9660Sectors([&source](uint64_t offset, void *target, size_t count)
                                  { return source.read(offset, target, count); },
                                  source.size, lsn, sectors, destination, byteCount, error);
    }
}
