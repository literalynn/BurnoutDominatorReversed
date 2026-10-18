#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace burnout {
// Registers disc patches that make the English messages speak of this computer and its save
// folder instead of the memory card (PS2), MEMORY CARD slot 1 and the console.
void registerPcTexts();

// Within each NUL-terminated UTF-16 string of a text file, replaces `from` with `to` where the
// result is not longer (the file keeps its layout); no-break spaces count as spaces. Returns the
// number of strings changed. Exposed for the tests.
size_t replaceUtf16Text(std::vector<uint8_t>& file, const std::vector<std::pair<std::u16string, std::u16string>>& table);
} // namespace burnout
