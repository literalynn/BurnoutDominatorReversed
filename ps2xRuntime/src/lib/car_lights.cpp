#include "runtime/car_lights.h"
#include "runtime/host_settings.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>

namespace ps2x
{
    namespace
    {
        // Hashes of the entries of the last frames: the game fills four lists in turn, 200
        // entries at most each, and the GS thread unpacks them a frame or two later.
        std::array<uint64_t, 1024> g_noted{};
        size_t g_next = 0;
        std::mutex g_mutex;

        uint64_t hashEntry(const uint8_t *entry)
        {
            uint64_t hash = 14695981039346656037ull;
            for (int i = 0; i < 64; ++i)
                hash = (hash ^ entry[i]) * 1099511628211ull;
            return hash;
        }

        uint32_t word(const uint8_t *entry, int index)
        {
            uint32_t value;
            std::memcpy(&value, entry + 4 * index, sizeof(value));
            return value;
        }
    }

    void noteCarLight(const uint8_t *entry)
    {
        const uint64_t hash = hashEntry(entry);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_noted[g_next] = hash;
        g_next = (g_next + 1) % g_noted.size();
    }

    void brightenCarLights(uint8_t *vu1Data, uint32_t qwordAddress, uint32_t qwordCount)
    {
        const int percent = hostSettings().carLights;
        if (percent == 100)
            return;
        for (uint32_t q = 0; q + 4 <= qwordCount; q += 4)
        {
            const uint32_t at = (qwordAddress + q) & 0x3FFu;
            uint8_t *entry = vu1Data + at * 16u;
            // The car light code's constants (400.0 and 0.5) first, to hash only its entries.
            if (at > 0x3FCu || word(entry, 3) != 0x43C80000u || word(entry, 5) != 0x3F000000u)
                continue;
            const uint64_t hash = hashEntry(entry);
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (std::find(g_noted.begin(), g_noted.end(), hash) == g_noted.end())
                    continue;
            }
            // Words 8 to 10: red, green and blue, 0 to about 96. The cap keeps them in the GS's 8 bits.
            for (int i = 8; i <= 10; ++i)
            {
                float colour;
                std::memcpy(&colour, entry + 4 * i, sizeof(colour));
                colour = std::min(colour * static_cast<float>(percent) / 100.0f, 255.0f);
                std::memcpy(entry + 4 * i, &colour, sizeof(colour));
            }
        }
    }
}
