#include "runtime/disc_patches.h"

#include "ps2x/iop/iso9660.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>

namespace
{
    struct Registered
    {
        std::string path; // upper case, '/' separated, no leading '/'
        ps2x::DiscFilePatch patch;
    };

    std::mutex g_mutex;
    std::vector<Registered> g_patches;
    std::map<std::string, std::vector<ps2x::DiscOverlay>> g_cache; // by host file

    std::string canonical(std::string path)
    {
        std::replace(path.begin(), path.end(), '\\', '/');
        std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return char(std::toupper(c)); });
        while (!path.empty() && path.front() == '/')
            path.erase(path.begin());
        return path;
    }

    std::shared_ptr<const std::vector<uint8_t>> patched(std::vector<uint8_t> bytes, const Registered &entry)
    {
        const size_t size = bytes.size();
        entry.patch(bytes);
        if (bytes.size() != size)
        {
            std::fprintf(stderr, "[disc] patch for %s changed its size, ignored\n", entry.path.c_str());
            return nullptr;
        }
        return std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    }
}

namespace ps2x
{
    void addDiscFilePatch(std::string discPath, DiscFilePatch patch)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_patches.push_back({canonical(std::move(discPath)), std::move(patch)});
        g_cache.clear();
    }

    std::vector<DiscOverlay> discOverlaysFor(const std::filesystem::path &hostFile, const std::filesystem::path &cdImage,
                                             const std::filesystem::path &cdRoot)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_patches.empty())
            return {};
        const std::string key = canonical(hostFile.lexically_normal().generic_string());
        if (const auto cached = g_cache.find(key); cached != g_cache.end())
            return cached->second;

        std::vector<DiscOverlay> overlays;
        if (!cdImage.empty() && key == canonical(cdImage.lexically_normal().generic_string()))
        {
            for (const Registered &entry : g_patches)
            {
                ps2x::iop::Iso9660File file;
                std::string error;
                if (!ps2x::iop::findIso9660File(cdImage, entry.path, file, &error) || file.directory)
                    continue;
                std::vector<uint8_t> bytes(file.size);
                std::ifstream image(cdImage, std::ios::binary);
                image.seekg(std::streamoff(uint64_t(file.lsn) * ps2x::iop::Iso9660SectorSize));
                if (!image.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(bytes.size())))
                    continue;
                if (auto data = patched(std::move(bytes), entry))
                    overlays.push_back({uint64_t(file.lsn) * ps2x::iop::Iso9660SectorSize, std::move(data)});
            }
        }
        else if (!cdRoot.empty())
        {
            const std::string root = canonical(cdRoot.lexically_normal().generic_string());
            for (const Registered &entry : g_patches)
            {
                if (key != root + (root.empty() || root.back() == '/' ? "" : "/") + entry.path)
                    continue;
                std::ifstream file(hostFile, std::ios::binary);
                std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
                if (auto data = patched(std::move(bytes), entry))
                    overlays.push_back({0u, std::move(data)});
            }
        }
        g_cache.emplace(key, overlays);
        return overlays;
    }

    void applyDiscOverlays(const std::vector<DiscOverlay> &overlays, uint64_t offset, void *data, size_t size)
    {
        for (const DiscOverlay &overlay : overlays)
        {
            const uint64_t begin = std::max(offset, overlay.offset);
            const uint64_t end = std::min(offset + size, overlay.offset + overlay.bytes->size());
            if (begin < end)
                std::memcpy(static_cast<uint8_t *>(data) + (begin - offset), overlay.bytes->data() + (begin - overlay.offset),
                            size_t(end - begin));
        }
    }
}
