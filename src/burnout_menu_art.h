#pragma once

#include "runtime/settings_menu.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace burnout
{
// The F1 menu in the front end's own font, art and sounds, read from the player's extracted disc
// (docs/FINDINGS.md E027): LANGUAGE/FONTS/DIRTYERA.BIN, images of FE/FEMAIN.BIN, SOUND/FE.AWD.
// Anything missing or unreadable stays empty, and the menu draws its own instead.
ps2x::MenuArt loadMenuArt(const std::filesystem::path &disc);

// The decoders, which return nothing for data they cannot read.
// A texture entry (of an FE/*.BIN movie or a font): its level 0 as RGBA.
std::optional<ps2x::MenuImage> decodeTexture(const std::vector<uint8_t> &file, size_t entry);
// Where image index (from 0) of an FE/*.BIN movie has its texture entry.
std::optional<size_t> movieImage(const std::vector<uint8_t> &archive, const std::string &movie, size_t index);
// A LANGUAGE/FONTS/*.BIN font.
std::optional<ps2x::MenuFont> decodeFont(const std::vector<uint8_t> &file);
// A wave of a SOUND/*.AWD dictionary, by name.
std::optional<ps2x::MenuSound> awdSound(const std::vector<uint8_t> &file, const std::string &name);
}
