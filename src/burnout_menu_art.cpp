#include "burnout_menu_art.h"

#include "burnout_button_prompts.h"
#include "../ps2xIOP/src/emulator/core/iop_spu2.h" // the SPU2 ADPCM decoder
#include "runtime/gs/ps2_gs_psmct16.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt4.h"
#include "runtime/gs/ps2_gs_psmt8.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace burnout
{
namespace
{
// Little-endian reads; one past the end throws, and the decoders give up.
template <typename T>
T load(const std::vector<uint8_t> &file, size_t at)
{
    if (at > file.size() || file.size() - at < sizeof(T))
        throw std::out_of_range("past the end of the file");
    T value;
    std::memcpy(&value, file.data() + at, sizeof(T));
    return value;
}

std::string cString(const std::vector<uint8_t> &file, size_t at)
{
    std::string text;
    for (char c; text.size() < 64 && (c = char(load<uint8_t>(file, at))) != '\0'; ++at)
        text += c;
    return text;
}

uint32_t field(uint64_t value, int shift, int bits)
{
    return uint32_t(value >> shift) & ((1u << bits) - 1u);
}

// PSMCT32 with alpha 0x80 for opaque, as RGBA.
void putColour(uint8_t *rgba, uint32_t colour)
{
    rgba[0] = uint8_t(colour);
    rgba[1] = uint8_t(colour >> 8);
    rgba[2] = uint8_t(colour >> 16);
    rgba[3] = uint8_t(std::min(255u, (colour >> 24) * 2u));
}

// An IMAGE transfer: the transfer registers it was sent with and where its data is in the file.
struct Upload
{
    uint64_t bitbltbuf, trxpos, trxreg;
    size_t data, bytes;
};

// The GIF packets in [at, end): PACKED A+D register writes and IMAGE data.
void gifUploads(const std::vector<uint8_t> &file, size_t at, size_t end, uint64_t (&registers)[3],
                std::vector<Upload> &uploads)
{
    while (at + 16 <= end)
    {
        const uint64_t tag = load<uint64_t>(file, at);
        at += 16;
        const uint32_t loops = field(tag, 0, 15), format = field(tag, 58, 2), count = field(tag, 60, 4);
        if (format == 0)
            for (uint32_t i = 0; i < loops * (count ? count : 16u); ++i, at += 16)
            {
                const uint8_t address = load<uint8_t>(file, at + 8);
                if (address >= 0x50 && address <= 0x52) // BITBLTBUF, TRXPOS, TRXREG
                    registers[address - 0x50] = load<uint64_t>(file, at);
            }
        else if (format == 2)
        {
            uploads.push_back({registers[0], registers[1], registers[2], at, size_t(loops) * 16u});
            at += size_t(loops) * 16u;
        }
        else
            return;
    }
}

constexpr uint32_t kGsMemory = 4u << 20;

uint32_t pixelAddress(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
{
    return (psm == 0x02 ? GSPSMCT16::addrPSMCT16(bp, bw, x, y) : GSPSMCT32::addrPSMCT32(bp, bw, x, y)) % kGsMemory;
}

ps2x::MenuImage crop(const ps2x::MenuImage &image, int left, int width)
{
    ps2x::MenuImage out{width, image.height, {}};
    for (int y = 0; y < image.height; ++y)
    {
        const auto row = image.rgba.begin() + (ptrdiff_t(y) * image.width + left) * 4;
        out.rgba.insert(out.rgba.end(), row, row + ptrdiff_t(width) * 4);
    }
    return out;
}

std::vector<uint8_t> readFile(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
}

std::optional<ps2x::MenuImage> decodeTexture(const std::vector<uint8_t> &file, size_t entry)
{
    try
    {
        // The entry: its size at +0xC, bits a texel at +0x14, and at +0x110 a DMA chain of GS
        // uploads (level 0, the mip levels, the palette last). REF tags give addresses from +0x104.
        const uint32_t width = load<uint32_t>(file, entry + 0xC), height = load<uint32_t>(file, entry + 0x10);
        const uint32_t depth = load<uint32_t>(file, entry + 0x14), base = load<uint32_t>(file, entry + 0x104);
        if (width == 0 || height == 0 || width > 1024 || height > 1024 || (depth != 4 && depth != 8))
            return std::nullopt;
        uint64_t registers[3] = {};
        std::vector<Upload> uploads;
        size_t at = entry + 0x110;
        for (int i = 0; i < 16; ++i)
        {
            const uint64_t tag = load<uint64_t>(file, at);
            const size_t bytes = size_t(field(tag, 0, 16)) * 16u;
            const uint32_t id = field(tag, 28, 3), address = uint32_t(tag >> 32);
            if (bytes == 0)
                break;
            if (id == 1) // CNT: the packet follows the tag
            {
                gifUploads(file, at + 16, at + 16 + bytes, registers, uploads);
                at += 16 + bytes;
            }
            else if (id == 3) // REF: the packet is elsewhere in the entry
            {
                const size_t packet = entry + (address >= base ? address - base : address);
                gifUploads(file, packet, packet + bytes, registers, uploads);
                at += 16;
            }
            else
                break;
        }
        if (uploads.size() < 2)
            return std::nullopt;

        // Written to GS memory as the GS would, then read back as the texture and its palette.
        std::vector<uint8_t> memory(kGsMemory);
        for (const Upload &u : uploads)
        {
            const uint32_t bp = field(u.bitbltbuf, 32, 14), bw = field(u.bitbltbuf, 48, 6), psm = field(u.bitbltbuf, 56, 6);
            const uint32_t x0 = field(u.trxpos, 32, 11), y0 = field(u.trxpos, 48, 11);
            const uint32_t w = field(u.trxreg, 0, 12), h = field(u.trxreg, 32, 12);
            const uint32_t bpp = psm == 0x00 ? 4 : psm == 0x02 ? 2 : 0;
            if (bpp == 0 || size_t(w) * h * bpp > u.bytes)
                return std::nullopt;
            load<uint8_t>(file, u.data + u.bytes - 1);
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                    std::memcpy(memory.data() + pixelAddress(psm, bp, bw, x0 + x, y0 + y),
                                file.data() + u.data + (size_t(y) * w + x) * bpp, bpp);
        }
        const Upload &first = uploads.front(), &last = uploads.back();
        const uint32_t tbp = field(first.bitbltbuf, 32, 14), tbw = std::max(2u, width / 64u);
        const uint32_t cbp = pixelAddress(field(last.bitbltbuf, 56, 6), field(last.bitbltbuf, 32, 14),
                                          field(last.bitbltbuf, 48, 6), field(last.trxpos, 32, 11), field(last.trxpos, 48, 11)) /
                             256u;
        // CSM1 palettes: 256 colours swap bits 3 and 4 of the index; 16 colours are 8 x 2.
        uint32_t palette[256];
        for (uint32_t i = 0; i < (depth == 8 ? 256u : 16u); ++i)
        {
            const uint32_t p = depth == 8 ? (i & 0xE7u) | (i & 0x08u) << 1 | (i & 0x10u) >> 1 : i;
            const uint32_t x = depth == 8 ? p & 15u : p & 7u, y = depth == 8 ? p >> 4 : p >> 3;
            std::memcpy(&palette[i], memory.data() + GSPSMCT32::addrPSMCT32(cbp, 1, x, y) % kGsMemory, 4);
        }
        ps2x::MenuImage image{int(width), int(height), std::vector<uint8_t>(size_t(width) * height * 4u)};
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                uint32_t index;
                if (depth == 8)
                    index = memory[GSPSMT8::addrPSMT8(tbp, tbw, x, y) % kGsMemory];
                else
                {
                    const uint32_t nibble = GSPSMT4::addrPSMT4(tbp, tbw, x, y) % (2u * kGsMemory);
                    index = (memory[nibble / 2u] >> (nibble & 1u) * 4u) & 15u;
                }
                putColour(image.rgba.data() + (size_t(y) * width + x) * 4u, palette[index]);
            }
        return image;
    }
    catch (const std::out_of_range &)
    {
        return std::nullopt;
    }
}

std::optional<size_t> movieImage(const std::vector<uint8_t> &archive, const std::string &movie, size_t index)
{
    try
    {
        // FE/*.BIN: a count at 8 and a table at 12 of offsets to (name, movie) offset pairs. A
        // movie has its image count at +4 and at +8 its table of image entries, from the movie.
        const uint32_t count = load<uint32_t>(archive, 8), table = load<uint32_t>(archive, 12);
        for (uint32_t i = 0; i < count && i < 4096u; ++i)
        {
            const uint32_t pair = load<uint32_t>(archive, table + 4u * size_t(i));
            if (cString(archive, load<uint32_t>(archive, pair)) != movie)
                continue;
            const size_t data = load<uint32_t>(archive, pair + 4u);
            if (index >= load<uint32_t>(archive, data + 4))
                return std::nullopt;
            return data + load<uint32_t>(archive, data + load<uint32_t>(archive, data + 8) + 4u * index);
        }
    }
    catch (const std::out_of_range &)
    {
    }
    return std::nullopt;
}

std::optional<ps2x::MenuFont> decodeFont(const std::vector<uint8_t> &file)
{
    try
    {
        // The name, then at 0x44 the glyph, character and kerning pair counts, at 0x50 the size,
        // at 0x64 where the glyphs, characters, pairs and texture are.
        const uint32_t glyphs = load<uint32_t>(file, 0x44), codes = load<uint32_t>(file, 0x48);
        const uint32_t pairs = load<uint32_t>(file, 0x4C), sizeBits = load<uint32_t>(file, 0x50);
        const uint32_t glyphsAt = load<uint32_t>(file, 0x64), codesAt = load<uint32_t>(file, 0x68);
        const uint32_t pairsAt = load<uint32_t>(file, 0x6C), textureAt = load<uint32_t>(file, 0x70);
        if (glyphs == 0 || glyphs > 4096u || codes > 65536u || pairs > 65536u || (sizeBits >> 23) - 127u > 10u)
            return std::nullopt; // the size must be 1 to 2047 (checked on the bits: release builds use /fp:fast)
        ps2x::MenuFont font;
        std::memcpy(&font.size, &sizeBits, 4);
        for (uint32_t i = 0; i < glyphs; ++i)
        {
            // 32 bytes as floats: the pen's place in the atlas (x, baseline y), the box's width and
            // height, left and top (from the pen to the box, in the atlas as on screen), the advance.
            float g[7];
            for (int k = 0; k < 7; ++k)
                g[k] = load<float>(file, glyphsAt + 32u * size_t(i) + 4u * k);
            font.glyphs.push_back({g[0] + g[4], g[1] - g[5], g[2], g[3], g[4], g[5], g[6]});
        }
        for (uint32_t i = 0; i < codes; ++i) // 8 bytes: the character, its glyph
        {
            const uint16_t glyph = load<uint16_t>(file, codesAt + 8u * size_t(i) + 2u);
            if (glyph < glyphs)
                font.codes[load<uint16_t>(file, codesAt + 8u * size_t(i))] = glyph;
        }
        for (uint32_t i = 0; i < pairs; ++i) // 8 bytes: two glyphs, the change to the advance
            font.kerning[{load<uint16_t>(file, pairsAt + 8u * size_t(i)), load<uint16_t>(file, pairsAt + 8u * size_t(i) + 2u)}] =
                float(load<int16_t>(file, pairsAt + 8u * size_t(i) + 4u));
        std::optional<ps2x::MenuImage> atlas = decodeTexture(file, textureAt);
        if (!atlas)
            return std::nullopt;
        font.atlas = std::move(*atlas);
        return font;
    }
    catch (const std::out_of_range &)
    {
        return std::nullopt;
    }
}

std::optional<ps2x::MenuSound> awdSound(const std::vector<uint8_t> &file, const std::string &name)
{
    try
    {
        // The waves' records start at 0x60, each one's first word plus 0x10 is the next: the name
        // at +0x68, the rate at +0x10, the size at +0x18, at +0x4C where it starts in the data
        // (from the offset at 8). The data is SPU2 ADPCM, the last block flagged as the end.
        const uint32_t data = load<uint32_t>(file, 8);
        for (size_t at = 0x60, n = 0; at + 0x80 <= data && n < 4096; ++n)
        {
            if (cString(file, at + 0x68) == name)
            {
                const uint32_t rate = load<uint32_t>(file, at + 0x10), bytes = load<uint32_t>(file, at + 0x18);
                const size_t begin = size_t(data) + load<uint32_t>(file, at + 0x4C);
                if (rate == 0 || rate > 96000u || bytes % 16u != 0u)
                    return std::nullopt;
                load<uint8_t>(file, begin + bytes - 1);
                ps2x::MenuSound sound{int(rate), {}};
                int32_t history1 = 0, history2 = 0;
                for (size_t block = begin; block < begin + bytes; block += 16)
                {
                    uint16_t words[8];
                    int16_t samples[28];
                    std::memcpy(words, file.data() + block, 16);
                    ps2x::iop::detail::Spu2::decodeAdpcm(words, history1, history2, samples);
                    sound.samples.insert(sound.samples.end(), samples, samples + 28);
                    if (file[block + 1] & 1u)
                        break;
                }
                return sound;
            }
            const size_t next = size_t(load<uint32_t>(file, at)) + 0x10;
            if (next <= at)
                break;
            at = next;
        }
    }
    catch (const std::out_of_range &)
    {
    }
    return std::nullopt;
}

ps2x::MenuArt loadMenuArt(const std::filesystem::path &disc)
{
    ps2x::MenuArt art;
    std::string missing;
    if (std::optional<ps2x::MenuFont> font = decodeFont(readFile(disc / "LANGUAGE" / "FONTS" / "DIRTYERA.BIN")))
        art.font = std::move(*font);
    else
        missing += " font";

    const std::vector<uint8_t> fe = readFile(disc / "FE" / "FEMAIN.BIN");
    const auto image = [&](const char *movie, size_t index, int width, int height)
    {
        const std::optional<size_t> entry = movieImage(fe, movie, index);
        std::optional<ps2x::MenuImage> decoded = entry ? decodeTexture(fe, *entry) : std::nullopt;
        if (decoded && decoded->width == width && decoded->height == height)
            return std::move(*decoded);
        missing += " " + std::string(movie) + "#" + std::to_string(index);
        return ps2x::MenuImage{};
    };
    // c_elements' images, from 0: 0 and 11 the empty and full volume steps, 14 to 16 the square,
    // cross and circle, 31 and 32 the chevron and the selected one; c_screen's 40 the stripes.
    art.stepEmpty = image("c_elements", 0, 32, 32);
    art.stepFull = image("c_elements", 11, 32, 32);
    art.playStation[0] = image("c_elements", 15, 32, 32);
    art.playStation[1] = image("c_elements", 16, 32, 32);
    art.playStation[2] = image("c_elements", 14, 32, 32);
    art.arrow = image("c_elements", 31, 32, 32);
    art.arrowSelected = image("c_elements", 32, 32, 32);
    // Its stripes repeat every 22 texels: columns 20 to 217 hold 9 whole ones on every row.
    if (const ps2x::MenuImage stripes = image("c_screen", 40, 256, 32); !stripes.rgba.empty())
        art.stripes = crop(stripes, 20, 198);
    // A, B and X as the game's button prompts draw them for Xbox pads.
    PromptStyle xbox;
    xbox.device = ps2x::InputDevice::Xbox;
    const PromptTexture faces[3] = {PromptTexture::MenuCross, PromptTexture::MenuCircle, PromptTexture::MenuSquare};
    for (int i = 0; i < 3; ++i)
    {
        const ButtonPrompts::Art drawn = ButtonPrompts::draw(faces[i], xbox);
        art.xbox[i] = {32, 32, std::vector<uint8_t>(32 * 32 * 4)};
        for (size_t p = 0; p < drawn.levels[0].size() && p < 32 * 32; ++p)
            putColour(art.xbox[i].rgba.data() + p * 4, drawn.palette[drawn.levels[0][p]]);
    }

    const std::vector<uint8_t> awd = readFile(disc / "SOUND" / "FE.AWD");
    const auto sound = [&](const char *name)
    {
        std::optional<ps2x::MenuSound> decoded = awdSound(awd, name);
        if (!decoded)
            missing += " " + std::string(name);
        return decoded ? std::move(*decoded) : ps2x::MenuSound{};
    };
    art.left = sound("hscrollL");
    art.right = sound("hscrollR");
    art.select = sound("selectL");
    art.back = sound("backL");
    if (!missing.empty())
        std::cout << "[menu] not found on the disc, drawn or played without:" << missing << std::endl;
    return art;
}
}
