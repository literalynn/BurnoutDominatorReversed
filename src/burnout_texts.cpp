#include "burnout_texts.h"

#include "runtime/disc_patches.h"

namespace burnout {
namespace {
char16_t plain(char16_t c) { return c == 0x00A0 ? u' ' : c; }

bool matchesAt(const std::u16string& text, size_t at, const std::u16string& pattern) {
    if (at + pattern.size() > text.size())
        return false;
    for (size_t i = 0; i < pattern.size(); ++i)
        if (plain(text[at + i]) != plain(pattern[i]))
            return false;
    return true;
}

// The English texts (DATA/MEM/ENGLISH.LOC, LANGUAGE/STRINGS/MAINUK.BIN), longest phrases first.
// %1 and %2 are filled in by the game; a dropped one is simply not shown.
const std::vector<std::pair<std::u16string, std::u16string>>& englishTable() {
    static const std::vector<std::pair<std::u16string, std::u16string>> table = {
        {u"Do not remove memory card (PS2), controller, or reset/switch off the console when the icon is displayed.",
         u"Do not close the game or switch off the computer when the icon is displayed."},
        {u"Do not remove memory card (PS2), controller, or reset/switch off the console.",
         u"Do not close the game or switch off the computer."},
        {u"will be displayed when the memory card (PS2) in MEMORY CARD slot 1 is being accessed.",
         u"will be displayed while the game saves to this computer."},
        {u"If you wish to save game data, insert a memory card (PS2) with at least %2KB of free space.",
         u"Saving needs %2KB of free disk space."},
        {u"Insert a memory card (PS2) into MEMORY CARD slot %1 with at least %2KB of free space.",
         u"Saving needs %2KB of free disk space."},
        {u"Insert a memory card (PS2) with at least %2KB of free space.", u"Saving needs %2KB of free disk space."},
        {u"Insert original memory card (PS2) and please try again.", u"Please try again."},
        {u"The original memory card (PS2) is not inserted in MEMORY CARD slot 1!",
         u"The original save data is not on this computer!"},
        {u"The original memory card (PS2) was removed from MEMORY CARD slot 1.",
         u"The original save data was removed from this computer."},
        {u"Save game data on memory card (PS2) in MEMORY CARD slot 1 appears to be corrupt.",
         u"The save data on this computer appears to be corrupt."},
        {u"Save game data on memory card (PS2) in MEMORY CARD slot %1 appears to be corrupt.",
         u"The save data on this computer appears to be corrupt."},
        {u"Format memory card (PS2)?", u"Prepare it?"},
        {u"Insufficient space on the memory card (PS2) in MEMORY CARD slot %1.",
         u"Not enough free disk space on this computer."},
        {u"formatting the memory card (PS2) in MEMORY CARD slot %1", u"preparing the save folder on this computer"},
        {u"Formatting memory card (PS2) in MEMORY CARD slot %1", u"Preparing the save folder on this computer"},
        {u"format memory card (PS2) in MEMORY CARD slot %1", u"prepare the save folder on this computer"},
        {u"The memory card (PS2) in MEMORY CARD slot %1 is unformatted.",
         u"The save folder on this computer is not ready."},
        {u"The memory card (PS2) has been removed from MEMORY CARD slot 1.", u"The save data was removed from this computer."},
        {u"The memory card (PS2) was removed from MEMORY CARD slot %1.", u"The save data was removed from this computer."},
        {u"No memory card (PS2) inserted into MEMORY CARD slot %1.", u"No save data on this computer."},
        {u"No memory card (PS2) inserted into MEMORY CARD slot 1.", u"No save data on this computer."},
        {u"No memory card (PS2) inserted in MEMORY CARD slot %1.", u"No save data on this computer."},
        {u"save data present on memory card (PS2) in MEMORY CARD slot %2.", u"save data on this computer."},
        {u"Checking memory card (PS2) in MEMORY CARD slot %1", u"Checking the save data on this computer"},
        {u"memory card (PS2) in MEMORY CARD slot %1", u"this computer"},
        {u"Please insert a controller into controller port %1", u"Please connect a controller (port %1)"},
        {u"Challenge up to three other friends on one console", u"Challenge up to three friends on one computer"},
    };
    return table;
}
} // namespace

size_t replaceUtf16Text(std::vector<uint8_t>& file, const std::vector<std::pair<std::u16string, std::u16string>>& table) {
    size_t changed = 0;
    const size_t units = file.size() / 2;
    const auto unit = [&](size_t i) { return char16_t(file[2 * i] | (file[2 * i + 1] << 8)); };
    size_t start = 0;
    for (size_t i = 0; i <= units; ++i) {
        if (i < units && unit(i) != 0)
            continue;
        if (i > start) {
            std::u16string text;
            for (size_t j = start; j < i; ++j)
                text.push_back(unit(j));
            std::u16string result = text;
            for (const auto& [from, to] : table) {
                std::u16string next;
                for (size_t at = 0; at < result.size();) {
                    if (matchesAt(result, at, from)) {
                        next += to;
                        at += from.size();
                    } else
                        next.push_back(result[at++]);
                }
                result.swap(next);
            }
            if (result != text && result.size() <= text.size()) {
                for (size_t j = 0; j < text.size(); ++j) {
                    const char16_t c = j < result.size() ? result[j] : 0;
                    file[2 * (start + j)] = uint8_t(c & 0xFF);
                    file[2 * (start + j) + 1] = uint8_t(c >> 8);
                }
                ++changed;
            }
        }
        start = i + 1;
    }
    return changed;
}

void registerPcTexts() {
    for (const char* path : {"DATA/MEM/ENGLISH.LOC", "LANGUAGE/STRINGS/MAINUK.BIN"})
        ps2x::addDiscFilePatch(path, [](std::vector<uint8_t>& file) { replaceUtf16Text(file, englishTable()); });
}
} // namespace burnout
