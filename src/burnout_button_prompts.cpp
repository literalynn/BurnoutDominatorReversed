#include "burnout_button_prompts.h"

#include "runtime/gs/gif_packet_filter.h"
#include "runtime/gs/ps2_gs_psmct16.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt4.h"
#include "runtime/gs/ps2_gs_psmt8.h"
#include "runtime/host_settings.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>

namespace burnout
{
namespace
{
enum class Family : uint8_t
{
    MenuFace,     // front end (APT movies): PSMT8 32x32, 3 mip levels, 256 colours
    MenuShoulder, // PSMT8 64x32, 3 mip levels, 256 colours
    HudFace,      // races (GLOBAL?.TXD): PSMT4 32x32, 16 colours
    HudShoulder,  // PSMT8 64x32, 256 colours
};

struct Texture
{
    Family family;
    uint8_t button; // padButtonNames() index: cross 4, circle 5, square 6, triangle 7, L1 8, R1 9, L2 10, R2 11
    uint8_t width, height, levels;
};

// In PromptTexture order.
constexpr Texture kTextures[] = {
    {Family::MenuFace, 7, 32, 32, 3},     {Family::MenuFace, 6, 32, 32, 3},     {Family::MenuFace, 4, 32, 32, 3},
    {Family::MenuFace, 5, 32, 32, 3},     {Family::MenuShoulder, 9, 64, 32, 3}, {Family::MenuShoulder, 8, 64, 32, 3},
    {Family::HudFace, 4, 32, 32, 1},      {Family::HudShoulder, 8, 64, 32, 1},  {Family::HudShoulder, 10, 64, 32, 1},
    {Family::HudShoulder, 9, 64, 32, 1},  {Family::HudShoulder, 11, 64, 32, 1},
};
static_assert(std::size(kTextures) == size_t(PromptTexture::Count));

const Texture &textureOf(PromptTexture texture)
{
    return kTextures[size_t(texture)];
}

// ---- Drawing: shapes painted with 4x4 samples a pixel, premultiplied colours ----

struct Rgba
{
    float r = 0, g = 0, b = 0, a = 0;
};

Rgba rgb(int r, int g, int b)
{
    return {r / 255.0f, g / 255.0f, b / 255.0f, 1.0f};
}

Rgba mix(const Rgba &x, const Rgba &y, float t)
{
    return {x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, x.a + (y.a - x.a) * t};
}

using Shape = std::function<bool(float, float)>;

struct Canvas
{
    int width, height;
    std::vector<Rgba> pixels;

    Canvas(int w, int h) : width(w), height(h), pixels(size_t(w * h)) {}

    void paint(const Rgba &ink, const Shape &shape)
    {
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                int covered = 0;
                for (int sy = 0; sy < 4; ++sy)
                    for (int sx = 0; sx < 4; ++sx)
                        covered += shape(x + (sx + 0.5f) / 4.0f, y + (sy + 0.5f) / 4.0f);
                Rgba &pixel = pixels[size_t(y * width + x)];
                const float c = covered / 16.0f;
                pixel = {ink.r * c + pixel.r * (1 - c), ink.g * c + pixel.g * (1 - c), ink.b * c + pixel.b * (1 - c),
                         ink.a * c + pixel.a * (1 - c)};
            }
    }

    Canvas half() const
    {
        Canvas out(width / 2, height / 2);
        for (int y = 0; y < out.height; ++y)
            for (int x = 0; x < out.width; ++x)
            {
                Rgba sum;
                for (int i = 0; i < 4; ++i)
                {
                    const Rgba &p = pixels[size_t((2 * y + i / 2) * width + 2 * x + i % 2)];
                    sum = {sum.r + p.r / 4, sum.g + p.g / 4, sum.b + p.b / 4, sum.a + p.a / 4};
                }
                out.pixels[size_t(y * out.width + x)] = sum;
            }
        return out;
    }
};

Shape disc(float cx, float cy, float r)
{
    return [=](float x, float y) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; };
}

Shape ring(float cx, float cy, float inner, float outer)
{
    return [=](float x, float y)
    {
        const float d = (x - cx) * (x - cx) + (y - cy) * (y - cy);
        return d >= inner * inner && d <= outer * outer;
    };
}

Shape roundRect(float x0, float y0, float x1, float y1, float r)
{
    return [=](float x, float y)
    {
        const float dx = std::max({x0 + r - x, 0.0f, x - (x1 - r)});
        const float dy = std::max({y0 + r - y, 0.0f, y - (y1 - r)});
        return x >= x0 && x <= x1 && y >= y0 && y <= y1 && dx * dx + dy * dy <= r * r;
    };
}

// A stroke font: polylines on an 8 x 12 grid, each point an x digit and a y hex digit,
// strokes separated by spaces. Drawn by distance to the segments, as bold as the original art.
const char *glyph(char c)
{
    switch (c)
    {
    case 'A': return "0c30508c 1878";
    case 'B': return "0c005072745606 56888a6c0c";
    case 'C': return "826020020a2c6c8a";
    case 'D': return "005083895c0c00";
    case 'E': return "80000c8c 0666";
    case 'F': return "80000c 0666";
    case 'G': return "826020020a2c6c8a8646";
    case 'H': return "000c 808c 0686";
    case 'I': return "404c 1070 1c7c";
    case 'J': return "30808a6c2c0a";
    case 'K': return "000c 8008 358c";
    case 'L': return "000c8c";
    case 'M': return "0c0047808c";
    case 'N': return "0c008c80";
    case 'O': return "2060828a6c2c0a0220";
    case 'P': return "0c006082856707";
    case 'Q': return "2060828a6c2c0a0220 598c";
    case 'R': return "0c006082856707 478c";
    case 'S': return "82602002042666888a6c2c0a";
    case 'T': return "0080 404c";
    case 'U': return "000a2c6c8a80";
    case 'V': return "004c80";
    case 'W': return "002c456c80";
    case 'X': return "008c 800c";
    case 'Y': return "004680 464c";
    case 'Z': return "00800c8c";
    case '0': return "2060828a6c2c0a0220 2a62";
    case '1': return "22404c 1c7c";
    case '2': return "02206082840c8c";
    case '3': return "01206082846636 66888a6c2c0b";
    case '4': return "6c600888";
    case '5': return "80000565878a6c0c";
    case '6': return "7020020a2c6c8a876505";
    case '7': return "00803c";
    case '8': return "206082846626040220 2666888a6c2c0a0826";
    case '9': return "1c6c8a82602002052787";
    case '-': return "1676";
    case '+': return "1676 424a";
    case '*': return "424a 1579 7519";
    case '=': return "1474 1878";
    case '.': return "4c";
    case ',': return "4a3c";
    case ';': return "44 4a3c";
    case '\'': return "4043";
    case '`': return "3053";
    case '/': return "701c";
    case '\\': return "107c";
    case '[': return "60202c6c";
    case ']': return "20606c2c";
    case '\1': return "4c41 054185";           // up
    case '\2': return "404b 074b87";           // down
    case '\3': return "8616 52165a";           // left
    case '\4': return "0676 32763a";           // right
    case '\5': return "808818 45184b";         // Enter
    case '\6': return "4085656c2c250540";      // Shift
    case '\7': return "080c8c88";              // Space
    default: return "02206082844748 4c";       // ?
    }
}

// label centred on (cx, cy), as large as fits maxWidth x maxHeight; weight is the stroke's
// half width in grid units, outline widens the strokes without moving them.
Shape text(const std::string &label, float cx, float cy, float maxWidth, float maxHeight, float weight,
           float outline = 0.0f)
{
    struct Segment
    {
        float x0, y0, x1, y1;
    };
    if (label.empty())
        return [](float, float) { return false; };
    std::vector<Segment> segments;
    const float advance = 8 + 2 * weight + 2;
    const float unitsWide = label.size() * advance - 2 + 0.0f, unitsHigh = 12 + 2 * weight;
    const float scale = std::min(maxWidth / unitsWide, maxHeight / unitsHigh);
    const float left = cx - unitsWide * scale / 2 + weight * scale, top = cy - unitsHigh * scale / 2 + weight * scale;
    for (size_t i = 0; i < label.size(); ++i)
    {
        const char *g = glyph(label[i]);
        float px = 0, py = 0;
        bool first = true;
        for (; *g; ++g)
        {
            if (*g == ' ')
            {
                first = true;
                continue;
            }
            const float x = left + (i * advance + (g[0] - '0')) * scale;
            const float y = top + (g[1] <= '9' ? g[1] - '0' : g[1] - 'a' + 10) * scale;
            ++g;
            const bool single = first && (g[1] == '\0' || g[1] == ' ');
            if (!first || single)
                segments.push_back(single ? Segment{x, y, x, y} : Segment{px, py, x, y});
            px = x;
            py = y;
            first = false;
        }
    }
    const float radius = (weight + outline) * scale;
    return [segments, radius](float x, float y)
    {
        for (const Segment &s : segments)
        {
            const float dx = s.x1 - s.x0, dy = s.y1 - s.y0, length = dx * dx + dy * dy;
            const float t = length > 0 ? std::clamp(((x - s.x0) * dx + (y - s.y0) * dy) / length, 0.0f, 1.0f) : 0.0f;
            const float ex = x - s.x0 - t * dx, ey = y - s.y0 - t * dy;
            if (ex * ex + ey * ey <= radius * radius)
                return true;
        }
        return false;
    };
}

// The palette: the inks and, between every two of them, evenly spaced blends (the edges).
// It depends on the inks only, so textures that share a palette upload draw with one.
ButtonPrompts::Art quantise(const Canvas &canvas, const std::vector<Rgba> &inks, int levels, size_t colours)
{
    std::vector<Rgba> palette(inks);
    const size_t pairs = inks.size() * (inks.size() - 1) / 2, steps = (colours - inks.size()) / pairs;
    for (size_t i = 0; i < inks.size(); ++i)
        for (size_t j = i + 1; j < inks.size(); ++j)
            for (size_t k = 1; k <= steps; ++k)
                palette.push_back(mix(inks[i], inks[j], float(k) / float(steps + 1)));
    palette.resize(colours);

    ButtonPrompts::Art art;
    for (size_t i = 0; i < colours; ++i)
    {
        const Rgba &c = palette[i];
        if (c.a <= 0)
            continue;
        auto channel = [&](float v) { return uint32_t(std::lround(std::clamp(v / c.a, 0.0f, 1.0f) * 255)); };
        art.palette[i] = channel(c.r) | channel(c.g) << 8 | channel(c.b) << 16 | uint32_t(std::lround(c.a * 128)) << 24;
    }
    Canvas level = canvas;
    for (int l = 0; l < levels; ++l)
    {
        if (l > 0)
            level = level.half();
        for (const Rgba &p : level.pixels)
        {
            size_t best = 0;
            float bestDistance = 1e9f;
            for (size_t i = 0; i < colours; ++i)
            {
                const Rgba &c = palette[i];
                const float d = (p.r - c.r) * (p.r - c.r) + (p.g - c.g) * (p.g - c.g) + (p.b - c.b) * (p.b - c.b) +
                                (p.a - c.a) * (p.a - c.a);
                if (d < bestDistance)
                    bestDistance = d, best = i;
            }
            art.levels[size_t(l)].push_back(uint8_t(best));
        }
    }
    return art;
}

// ---- GS memory ----

uint64_t load64(const uint8_t *p)
{
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

uint32_t field(uint64_t value, int shift, int bits)
{
    return uint32_t(value >> shift) & ((1u << bits) - 1u);
}

// The byte address of a pixel (block numbers grow with x and y), or -1 for a format the
// symbols are not uploaded in.
int64_t pixelAddress(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
{
    switch (psm)
    {
    case 0x00: case 0x01: case 0x1b: case 0x24: case 0x2c: return GSPSMCT32::addrPSMCT32(bp, bw, x, y);
    case 0x02: return GSPSMCT16::addrPSMCT16(bp, bw, x, y);
    case 0x0a: return GSPSMCT16::addrPSMCT16S(bp, bw, x, y);
    case 0x13: return GSPSMT8::addrPSMT8(bp, bw, x, y);
    case 0x14: return GSPSMT4::addrPSMT4(bp, bw, x, y) / 2;
    default: return -1;
    }
}

uint32_t bitsPerPixel(uint32_t psm)
{
    switch (psm)
    {
    case 0x01: case 0x31: return 24;
    case 0x02: case 0x0a: case 0x32: case 0x3a: return 16;
    case 0x13: case 0x1b: return 8;
    case 0x14: case 0x24: case 0x2c: return 4;
    default: return 32;
    }
}

struct Geometry
{
    uint32_t bp, bw, psm, x, y, w, h;
};

Geometry geometryOf(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg)
{
    return {field(bitbltbuf, 32, 14), field(bitbltbuf, 48, 6), field(bitbltbuf, 56, 6), field(trxpos, 32, 11),
            field(trxpos, 48, 11),    field(trxreg, 0, 12),    field(trxreg, 32, 12)};
}

// The upload's bytes with a level's texels or the palette replaced: written as the GS would,
// relative to the transfer's base, then read back in transfer order.
bool replace(const Geometry &g, std::vector<uint8_t> &bytes, const ButtonPrompts::Art &art, const Texture &texture,
             uint8_t level)
{
    if ((g.psm != 0x00 && g.psm != 0x02) || g.w == 0 || g.h == 0)
        return false;
    const uint32_t bpp = g.psm == 0x00 ? 4 : 2;
    if (bytes.size() != size_t(g.w) * g.h * bpp)
        return false;
    std::vector<uint8_t> memory(4 * 8192);
    auto rect = [&](bool write)
    {
        for (uint32_t y = 0; y < g.h; ++y)
            for (uint32_t x = 0; x < g.w; ++x)
            {
                const auto a = size_t(pixelAddress(g.psm, 0, g.bw, g.x + x, g.y + y));
                if (a + bpp > memory.size())
                    return false;
                uint8_t *p = bytes.data() + (size_t(y) * g.w + x) * bpp;
                write ? std::memcpy(memory.data() + a, p, bpp) : std::memcpy(p, memory.data() + a, bpp);
            }
        return true;
    };
    const auto base = size_t(pixelAddress(g.psm, 0, g.bw, g.x, g.y));
    if (!rect(true) || base % 256 != 0)
        return false;
    const auto block = uint32_t(base / 256);
    const bool fourBit = texture.family == Family::HudFace;
    if (level == kPaletteLevel)
    {
        const uint32_t colours = fourBit ? 16 : 256;
        for (uint32_t i = 0; i < colours; ++i)
        {
            // CSM1: 256 entries swap bits 3 and 4 of the index; 16 entries are 8 x 2.
            const uint32_t p = fourBit ? i : (i & 0xe7) | (i & 0x08) << 1 | (i & 0x10) >> 1;
            const uint32_t a = GSPSMCT32::addrPSMCT32(block, 1, fourBit ? p & 7 : p & 15, fourBit ? p >> 3 : p >> 4);
            if (a + 4 > memory.size())
                return false;
            std::memcpy(memory.data() + a, &art.palette[i], 4);
        }
    }
    else
    {
        const uint32_t w = texture.width >> level, h = texture.height >> level;
        const std::vector<uint8_t> &texels = art.levels[level];
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint8_t index = texels[y * w + x];
                if (fourBit)
                {
                    const uint32_t n = GSPSMT4::addrPSMT4(block, 2, x, y);
                    if (n / 2 >= memory.size())
                        return false;
                    uint8_t &b = memory[n / 2];
                    b = n & 1 ? uint8_t((b & 0x0f) | index << 4) : uint8_t((b & 0xf0) | index);
                }
                else
                {
                    const uint32_t a = GSPSMT8::addrPSMT8(block, 2, x, y);
                    if (a >= memory.size())
                        return false;
                    memory[a] = index;
                }
            }
    }
    return rect(false);
}

bool traceEnabled()
{
    static const bool enabled = []
    {
        const char *value = std::getenv("BDR_TRACE_PROMPTS");
        return value && *value == '1';
    }();
    return enabled;
}

const char *kTextureNames[] = {"buttonTriangle", "buttonSquare", "buttonCross", "buttonCircle", "button_R1",
                               "button_L1",      "CrossButton",  "L1Button",    "L2Button",     "R1Button",
                               "R2Button"};
const char *kDeviceNames[] = {"keyboard", "xbox", "playstation"};
}

const std::vector<KnownUpload> &gameUploads()
{
    using T = PromptTexture;
    // FE/*.BIN APT images 21-26 and DATA/GLOBAL?.TXD (docs/FINDINGS.md E026). R1Button and
    // R2Button upload the same palette.
    static const std::vector<KnownUpload> uploads = {
        {1024, 0x1b17f1c2aa3cd313, T::MenuTriangle, 0}, {256, 0xd096eec9c29be5a4, T::MenuTriangle, 1},
        {128, 0x717bd4d08dca8542, T::MenuTriangle, 2},  {1024, 0x3f9cb2e96274fa4a, T::MenuTriangle, kPaletteLevel},
        {1024, 0x670ad8a53c9d60b4, T::MenuSquare, 0},   {256, 0x3081e499999488ad, T::MenuSquare, 1},
        {128, 0x4dd5a913d164ba0b, T::MenuSquare, 2},    {1024, 0x51de288923e19e8e, T::MenuSquare, kPaletteLevel},
        {1024, 0x6e07b032698f5d39, T::MenuCross, 0},    {256, 0xad92fd20ce374e37, T::MenuCross, 1},
        {128, 0x6b5e939f92fd5bfe, T::MenuCross, 2},     {1024, 0x573b5253678779ce, T::MenuCross, kPaletteLevel},
        {1024, 0x661996854bda6ff0, T::MenuCircle, 0},   {256, 0x8db6239f16fc01ee, T::MenuCircle, 1},
        {128, 0x7c2a28da04cceac1, T::MenuCircle, 2},    {1024, 0x4e89f7283b767a6c, T::MenuCircle, kPaletteLevel},
        {2048, 0x5ad4e079228603b1, T::MenuR1, 0},       {512, 0x6ec68dfde56071f9, T::MenuR1, 1},
        {128, 0x753381f9bd797074, T::MenuR1, 2},        {1024, 0xce3223010ecf94bd, T::MenuR1, kPaletteLevel},
        {2048, 0xdf4a5bd38f111c25, T::MenuL1, 0},       {512, 0xcbe0d36f088117cf, T::MenuL1, 1},
        {128, 0x8c7d82cee7090836, T::MenuL1, 2},        {1024, 0xe3d5416ebd12efa1, T::MenuL1, kPaletteLevel},
        {512, 0x86ce5ffe2a3576cd, T::HudCross, 0},      {96, 0xc597ab1049b4beb3, T::HudCross, kPaletteLevel},
        {2048, 0x24dc60f7db889054, T::HudL1, 0},        {1024, 0x2fe1a429d009be06, T::HudL1, kPaletteLevel},
        {2048, 0x9e725a09f9e8a48a, T::HudL2, 0},        {1024, 0x70364b7e4b65a306, T::HudL2, kPaletteLevel},
        {2048, 0x4612e5c2e6caa270, T::HudR1, 0},        {1024, 0x9a9411082b4b3a6e, T::HudR1, kPaletteLevel},
        {2048, 0x8af05c72053a4903, T::HudR2, 0},
    };
    return uploads;
}

uint64_t uploadHash(const uint8_t *data, size_t size)
{
    // FNV-1a a word at a time, the shift bringing high bits down: every upload of a symbol's size
    // is hashed, on the thread that feeds the GS.
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i + 8 <= size; i += 8)
    {
        hash = (hash ^ load64(data + i)) * 0x100000001b3ull;
        hash ^= hash >> 29;
    }
    return hash;
}

std::string promptLabel(int input, bool shortLabel)
{
    struct Named
    {
        int input;
        const char *label, *shortLabel;
    };
    static constexpr Named kNamed[] = {
        {32, "SPACE", "\7"},   {256, "ESC", "ESC"},    {257, "ENTER", "\5"},  {258, "TAB", "TAB"},
        {259, "BKSP", "BSP"},  {260, "INS", "INS"},    {261, "DEL", "DEL"},   {262, "\4", "\4"},
        {263, "\3", "\3"},     {264, "\2", "\2"},      {265, "\1", "\1"},     {266, "PGUP", "PGU"},
        {267, "PGDN", "PGD"},  {268, "HOME", "HOM"},   {269, "END", "END"},   {280, "CAPS", "CAP"},
        {281, "SCRL", "SCR"},  {282, "NUM", "NUM"},    {283, "PRTSC", "PRT"}, {284, "PAUSE", "PAU"},
        {330, ".", "."},       {331, "/", "/"},        {332, "*", "*"},       {333, "-", "-"},
        {334, "+", "+"},       {335, "ENTER", "\5"},   {336, "=", "="},       {340, "SHIFT", "\6"},
        {341, "CTRL", "CTL"},  {342, "ALT", "ALT"},    {343, "SUPER", "SUP"}, {344, "SHIFT", "\6"},
        {345, "CTRL", "CTL"},  {346, "ALT", "ALT"},    {347, "SUPER", "SUP"}, {348, "MENU", "MNU"},
        {ps2x::kMouseInput + 0, "LMB", "LMB"},         {ps2x::kMouseInput + 1, "RMB", "RMB"},
        {ps2x::kMouseInput + 2, "MMB", "MMB"},         {ps2x::kMouseInput + 3, "M4", "M4"},
        {ps2x::kMouseInput + 4, "M5", "M5"},           {ps2x::kMouseInput + 5, "M6", "M6"},
        {ps2x::kMouseInput + 6, "M7", "M7"},           {ps2x::kMouseWheelUp, "WH\1", "WH\1"},
        {ps2x::kMouseWheelDown, "WH\2", "WH\2"},
    };
    if (input == 0)
        return "";
    // Letters, digits and punctuation keys have their character's code.
    if ((input >= 'A' && input <= 'Z') || (input >= '0' && input <= '9') ||
        (input > 0 && input < 128 && std::strchr("',-./;=[\\]`", input)))
        return std::string(1, char(input));
    if (input >= 290 && input <= 301)
        return "F" + std::to_string(input - 289);
    if (input >= 320 && input <= 329)
        return (shortLabel ? "" : "NUM") + std::to_string(input - 320);
    for (const Named &named : kNamed)
        if (named.input == input)
            return shortLabel ? named.shortLabel : named.label;
    return "?";
}

ButtonPrompts::Art ButtonPrompts::draw(PromptTexture which, const PromptStyle &style)
{
    const Texture &texture = textureOf(which);
    const int button = texture.button;
    const bool face = texture.family == Family::MenuFace || texture.family == Family::HudFace;
    static const char *const kXbox[] = {"A", "B", "X", "Y", "LB", "RB", "LT", "RT"};
    const bool keyboard = style.device == ps2x::InputDevice::Keyboard;
    const std::string label = keyboard ? promptLabel(style.keys[size_t(button - 4)], face) : kXbox[button - 4];
    // Short labels shrink the keycap's letters; one character stays large.
    const float capHeight = label.size() <= 1 ? 14.0f : label.size() == 2 ? 11.0f : 9.0f;

    Canvas canvas(texture.width, texture.height);
    std::vector<Rgba> inks{Rgba{}};
    const Rgba black = rgb(16, 16, 16), white = rgb(240, 240, 240);
    switch (texture.family)
    {
    case Family::MenuFace:
        if (keyboard)
        {
            inks.insert(inks.end(), {rgb(48, 48, 54), white});
            canvas.paint(white, roundRect(3, 3, 29, 29, 6));
            canvas.paint(inks[1], roundRect(5, 5, 27, 27, 4));
            canvas.paint(white, text(label, 16, 16, 18, capHeight, 1.4f));
        }
        else
        {
            // Xbox face buttons: A green, B red, X blue, Y yellow, in the game's black disc and ring.
            static const Rgba kColours[] = {rgb(96, 200, 64), rgb(232, 64, 48), rgb(64, 132, 240), rgb(244, 200, 40)};
            inks.insert(inks.end(), {black, white, kColours[button - 4]});
            canvas.paint(black, disc(16, 16, 13));
            canvas.paint(white, ring(16, 16, 10, 12));
            canvas.paint(inks[3], text(label, 16, 16, 12, 13, 1.4f));
        }
        break;
    case Family::MenuShoulder:
        inks.insert(inks.end(), {rgb(44, 44, 44), rgb(150, 150, 150), white});
        canvas.paint(inks[2], roundRect(1, 1, 63, 31, 5));
        canvas.paint(inks[1], roundRect(3, 3, 62, 30, 4));
        canvas.paint(white, text(label, 32, 16, 52, 19, 1.7f));
        break;
    case Family::HudFace:
        inks.insert(inks.end(), {rgb(0, 0, 0), rgb(255, 255, 255)});
        if (keyboard)
        {
            canvas.paint(inks[2], roundRect(2, 2, 30, 30, 6));
            canvas.paint(inks[1], roundRect(4, 4, 28, 28, 4));
            canvas.paint(inks[2], text(label, 16, 16, 19, capHeight + 1, 1.4f));
        }
        else
        {
            canvas.paint(inks[1], disc(16, 16, 14.5f));
            canvas.paint(inks[2], ring(16, 16, 11, 13));
            canvas.paint(inks[2], text(label, 16, 16, 13, 14, 1.4f));
        }
        break;
    case Family::HudShoulder:
        inks.insert(inks.end(), {rgb(8, 8, 8), rgb(128, 128, 128), rgb(255, 255, 255)});
        canvas.paint(inks[1], roundRect(1, 3, 63, 29, 8));
        canvas.paint(inks[2], roundRect(3, 5, 61, 27, 6));
        canvas.paint(inks[1], text(label, 32, 16, 50, 16, 1.5f, 1.0f));
        canvas.paint(inks[3], text(label, 32, 16, 50, 16, 1.5f));
        break;
    }
    return quantise(canvas, inks, texture.levels, texture.family == Family::HudFace ? 16 : 256);
}

ButtonPrompts::ButtonPrompts(std::vector<KnownUpload> uploads) : m_uploads(std::move(uploads)) {}

void ButtonPrompts::reset()
{
    m_paths = {};
    m_transfer = {};
    m_pending = 0;
    m_resident.clear();
}

const uint8_t *ButtonPrompts::filter(uint32_t pathId, const uint8_t *data, uint32_t &size, const PromptStyle &style)
{
    if (!data)
    {
        reset();
        return data;
    }
    Path &path = m_paths[pathId & 3u];
    m_out.clear();
    m_patches.clear();
    // A new style: upload again the symbols in GS memory, between two GIF tags of this path
    // and outside any transfer, then restore the transfer registers the game may still use.
    if (!(style == m_style) && path.left == 0 && m_pending == 0)
    {
        m_style = style;
        m_art = {};
        for (const Resident &resident : m_resident)
            appendUpload(resident.transfer, styled(resident));
        if (!m_resident.empty())
            appendRegisters({{0x50, m_transfer.bitbltbuf}, {0x51, m_transfer.trxpos}, {0x52, m_transfer.trxreg}, {0x3f, 0}});
        if (traceEnabled())
            std::cerr << "[prompts] " << kDeviceNames[size_t(style.device)] << ": " << m_resident.size()
                      << " uploads again" << std::endl;
    }
    const size_t prefix = m_out.size();

    uint32_t offset = 0;
    while (offset + 16 <= size)
    {
        if (path.left == 0)
        {
            const uint64_t tag = load64(data + offset);
            path.regs = load64(data + offset + 8);
            offset += 16;
            const uint32_t nloop = field(tag, 0, 15), flg = field(tag, 58, 2);
            path.nreg = field(tag, 60, 4) ? field(tag, 60, 4) : 16;
            path.reg = 0;
            path.addressData = false;
            if (flg == 0)
                for (uint32_t i = 0; i < path.nreg; ++i)
                    path.addressData |= ((path.regs >> (4 * i)) & 15) == 0xe;
            path.left = flg == 0 ? nloop * path.nreg : flg == 1 ? (nloop * path.nreg + 1) / 2 : nloop;
            path.image = flg >= 2;
            // A whole host-to-GS transfer in one IMAGE tag of this packet: maybe a symbol.
            if (path.image && nloop != 0 && nloop * 16 == m_pending && offset + nloop * 16 <= size)
                match(data + offset, nloop * 16, offset);
            continue;
        }
        if (path.addressData)
        {
            if (((path.regs >> (4 * path.reg)) & 15) == 0xe)
                writeRegister(data[offset + 8], load64(data + offset));
            path.reg = (path.reg + 1) % path.nreg;
            --path.left;
            offset += 16;
            continue;
        }
        const uint32_t qwords = std::min(path.left, (size - offset) / 16);
        if (path.image)
            m_pending -= std::min(m_pending, qwords * 16);
        path.left -= qwords;
        offset += qwords * 16;
    }

    if (prefix == 0 && m_patches.empty())
        return data;
    m_out.insert(m_out.end(), data, data + size);
    for (const auto &[at, bytes] : m_patches)
        std::memcpy(m_out.data() + prefix + at, bytes.data(), bytes.size());
    size = uint32_t(m_out.size());
    return m_out.data();
}

void ButtonPrompts::writeRegister(uint8_t address, uint64_t value)
{
    switch (address)
    {
    case 0x50: m_transfer.bitbltbuf = value; break;
    case 0x51: m_transfer.trxpos = value; break;
    case 0x52: m_transfer.trxreg = value; break;
    case 0x53:
    {
        const uint32_t direction = value & 3;
        const Geometry g = geometryOf(m_transfer.bitbltbuf, m_transfer.trxpos, m_transfer.trxreg);
        m_pending = direction == 0 ? g.w * g.h * bitsPerPixel(g.psm) / 8 : 0;
        if (direction != 0 && direction != 2)
            break;
        // GS memory written: what it held there is gone.
        const int64_t first = pixelAddress(g.psm, g.bp, g.bw, g.x, g.y);
        const int64_t last = pixelAddress(g.psm, g.bp, g.bw, g.x + std::max(g.w, 1u) - 1, g.y + std::max(g.h, 1u) - 1);
        auto overlaps = [&](const Resident &r)
        { return first < 0 || (int64_t(r.lastBlock) >= first / 256 && int64_t(r.firstBlock) <= last / 256); };
        m_resident.erase(std::remove_if(m_resident.begin(), m_resident.end(), overlaps), m_resident.end());
        break;
    }
    default: break;
    }
}

void ButtonPrompts::match(const uint8_t *image, uint32_t bytes, size_t offset)
{
    if (std::none_of(m_uploads.begin(), m_uploads.end(), [&](const KnownUpload &u) { return u.size == bytes; }))
        return;
    const uint64_t hash = uploadHash(image, bytes);
    for (size_t i = 0; i < m_uploads.size(); ++i)
    {
        if (m_uploads[i].size != bytes || m_uploads[i].hash != hash)
            continue;
        const Geometry g = geometryOf(m_transfer.bitbltbuf, m_transfer.trxpos, m_transfer.trxreg);
        const int64_t first = pixelAddress(g.psm, g.bp, g.bw, g.x, g.y);
        const int64_t last = pixelAddress(g.psm, g.bp, g.bw, g.x + g.w - 1, g.y + g.h - 1);
        if (first < 0)
            return;
        Resident resident{m_transfer, uint32_t(first / 256), uint32_t(last / 256), i, {image, image + bytes}};
        if (m_style.device != ps2x::InputDevice::PlayStation)
            m_patches.emplace_back(offset, styled(resident));
        if (traceEnabled())
            std::cerr << "[prompts] " << kTextureNames[size_t(m_uploads[i].texture)] << " level "
                      << int(m_uploads[i].level) << " at block 0x" << std::hex << resident.firstBlock << std::dec
                      << " (" << kDeviceNames[size_t(m_style.device)] << ")" << std::endl;
        m_resident.push_back(std::move(resident));
        return;
    }
}

std::vector<uint8_t> ButtonPrompts::styled(const Resident &resident)
{
    std::vector<uint8_t> bytes = resident.original;
    if (m_style.device == ps2x::InputDevice::PlayStation)
        return bytes;
    const KnownUpload &upload = m_uploads[resident.upload];
    std::optional<Art> &art = m_art[size_t(upload.texture)];
    if (!art)
        art = draw(upload.texture, m_style);
    const Transfer &t = resident.transfer;
    if (!replace(geometryOf(t.bitbltbuf, t.trxpos, t.trxreg), bytes, *art, textureOf(upload.texture), upload.level))
        return resident.original;
    return bytes;
}

void ButtonPrompts::appendRegisters(const std::vector<std::pair<uint8_t, uint64_t>> &registers)
{
    // GIF tag: NLOOP registers, PACKED, one A+D descriptor (EOP clear: the game's packet follows).
    const uint64_t tag[2] = {uint64_t(registers.size()) | uint64_t(1) << 60, 0xe};
    m_out.insert(m_out.end(), reinterpret_cast<const uint8_t *>(tag), reinterpret_cast<const uint8_t *>(tag + 2));
    for (const auto &[address, value] : registers)
    {
        const uint64_t qword[2] = {value, address};
        m_out.insert(m_out.end(), reinterpret_cast<const uint8_t *>(qword), reinterpret_cast<const uint8_t *>(qword + 2));
    }
}

void ButtonPrompts::appendUpload(const Transfer &transfer, const std::vector<uint8_t> &bytes)
{
    appendRegisters({{0x50, transfer.bitbltbuf}, {0x51, transfer.trxpos}, {0x52, transfer.trxreg}, {0x53, 0}});
    const uint64_t tag[2] = {uint64_t(bytes.size() / 16) | uint64_t(2) << 58, 0};
    m_out.insert(m_out.end(), reinterpret_cast<const uint8_t *>(tag), reinterpret_cast<const uint8_t *>(tag + 2));
    m_out.insert(m_out.end(), bytes.begin(), bytes.end());
}

void installButtonPrompts()
{
    ps2x::setGifPacketFilter([](uint32_t path, const uint8_t *data, uint32_t &size) -> const uint8_t *
    {
        static ButtonPrompts prompts;
        PromptStyle style;
        style.device = ps2x::buttonPromptDevice();
        if (style.device == ps2x::InputDevice::Keyboard)
            for (size_t i = 0; i < style.keys.size(); ++i)
                style.keys[i] = ps2x::hostSettings().keys[4 + i][0];
        return prompts.filter(path, data, size, style);
    });
}
}
