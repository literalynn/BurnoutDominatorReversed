#pragma once

#include "runtime/input_device.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace burnout
{
// The game's button symbols drawn for the device in use (settings.ini button_prompts).
// They are small palette textures the game uploads to the GS (docs/FINDINGS.md E026):
// ButtonPrompts finds their uploads by content in the GIF packets, replaces the texels and
// the palette, and when the device changes uploads again those still in GS memory.

// The symbol textures, in kTextures order.
enum class PromptTexture : uint8_t
{
    MenuTriangle,
    MenuSquare,
    MenuCross,
    MenuCircle,
    MenuR1,
    MenuL1,
    HudCross,
    HudL1,
    HudL2,
    HudR1,
    HudR2,
    Count
};

constexpr uint8_t kPaletteLevel = 3; // KnownUpload::level of a palette upload

// One upload of a symbol texture: mip level 0-2 or the palette, found by size and hash.
struct KnownUpload
{
    uint32_t size;
    uint64_t hash; // uploadHash() of the IMAGE data
    PromptTexture texture;
    uint8_t level;
};

// The game's uploads (identical in every language and front-end file).
const std::vector<KnownUpload> &gameUploads();
// A 64-bit hash of whole 8-byte words (uploads are whole qwords).
uint64_t uploadHash(const uint8_t *data, size_t size);

struct PromptStyle
{
    ps2x::InputDevice device = ps2x::InputDevice::PlayStation;
    // Keyboard: the first input bound to cross, circle, square, triangle, L1, R1, L2 and R2.
    std::array<int, 8> keys{};

    bool operator==(const PromptStyle &other) const { return device == other.device && keys == other.keys; }
};

// A keyboard or mouse input (HostSettings::keys) as a prompt shows it: "W", "SHIFT", "LMB"...
// Short labels, for the round buttons, have three characters at most; \1 to \7 are the up,
// down, left and right arrows, Enter, Shift and Space symbols.
std::string promptLabel(int input, bool shortLabel);

class ButtonPrompts
{
public:
    explicit ButtonPrompts(std::vector<KnownUpload> uploads = gameUploads());

    // The packet to send instead (data itself when nothing changes); size follows. A GIF
    // path's packets arrive in order; null data means that GS memory was reset or reloaded.
    const uint8_t *filter(uint32_t path, const uint8_t *data, uint32_t &size, const PromptStyle &style);

    struct Art
    {
        std::array<std::vector<uint8_t>, 3> levels; // palette indices, rows of texels
        std::array<uint32_t, 256> palette{};        // PSMCT32, alpha 0x80 = opaque
    };
    static Art draw(PromptTexture texture, const PromptStyle &style);

private:
    struct Path
    {
        uint32_t left = 0; // qwords before the next GIF tag
        uint32_t nreg = 1;
        uint32_t reg = 0;
        uint64_t regs = 0;
        bool addressData = false; // PACKED with A+D registers: the qwords are read
        bool image = false;
    };
    struct Transfer
    {
        uint64_t bitbltbuf = 0, trxpos = 0, trxreg = 0;
    };
    struct Resident
    {
        Transfer transfer;
        uint32_t firstBlock, lastBlock;
        size_t upload;
        std::vector<uint8_t> original;
    };

    void reset();
    void writeRegister(uint8_t address, uint64_t value);
    void match(const uint8_t *image, uint32_t bytes, size_t offset);
    std::vector<uint8_t> styled(const Resident &resident);
    void appendUpload(const Transfer &transfer, const std::vector<uint8_t> &bytes);
    void appendRegisters(const std::vector<std::pair<uint8_t, uint64_t>> &registers);

    std::vector<KnownUpload> m_uploads;
    std::array<Path, 4> m_paths{};
    Transfer m_transfer; // the GS's transfer registers
    uint32_t m_pending = 0;   // bytes still to come in the current host-to-GS transfer
    std::vector<Resident> m_resident;
    PromptStyle m_style; // the symbols GS memory holds
    std::array<std::optional<Art>, size_t(PromptTexture::Count)> m_art;
    std::vector<std::pair<size_t, std::vector<uint8_t>>> m_patches;
    std::vector<uint8_t> m_out;
};

// Filters the GS's GIF packets through a ButtonPrompts that follows the settings and the pad.
void installButtonPrompts();
}
