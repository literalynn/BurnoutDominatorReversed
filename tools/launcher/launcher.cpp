// The launcher's window, for Windows, Linux and macOS. tools/launcher.py does the work: this program starts it (or
// finds the one already running for this folder), sends it the player's clicks and draws what it answers, with the
// console lines and state changes it streams (/events). BurnoutDominator.exe holds it on Windows; BurnoutDominator.py
// builds it on Linux and macOS. tools/launcher.html is the same thing as a page, for a Git checkout.
//
//   bdr_launcher --python <python> --script <tools/launcher.py>
//
// Exit codes: 0 closed, 1 the launcher could not start, 2 no window could open, 3 an update changed this program:
// start it again (BurnoutDominator.py builds it first).
#include "system.h"

#include "raylib.h"
#include "runtime/host_settings.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

extern "C"
{
    // GLFW, inside raylib: wake the window from another thread, and see every click and wheel step (raylib keeps one
    // state per frame, and a frame waits for events: a press and its release can come in the same wait).
    struct GLFWwindow;
    typedef void (*GLFWmousebuttonfun)(GLFWwindow *, int, int, int);
    typedef void (*GLFWscrollfun)(GLFWwindow *, double, double);
    GLFWwindow *glfwGetCurrentContext(void);
    GLFWmousebuttonfun glfwSetMouseButtonCallback(GLFWwindow *, GLFWmousebuttonfun);
    GLFWscrollfun glfwSetScrollCallback(GLFWwindow *, GLFWscrollfun);
    void glfwPostEmptyEvent(void);
}

namespace
{
    namespace fs = std::filesystem;
    using Clock = std::chrono::steady_clock;

    fs::path pathOf(const std::string &utf8)
    {
        return fs::path(std::u8string_view(reinterpret_cast<const char8_t *>(utf8.data()), utf8.size()));
    }

    // --- JSON, as tools/launcher.py writes it --------------------------------

    struct Json
    {
        enum Kind { Null, False, True, Number, String, Array, Object } kind = Null;
        std::string text; // a string's value, a number as written
        std::vector<Json> items;
        struct Field; // not std::pair: a pair may not hold the incomplete Json (libstdc++ with Clang refuses it)
        std::vector<Field> fields;

        const Json &operator[](std::string_view key) const;
        const Json &operator[](size_t index) const { return index < items.size() ? items[index] : none(); }
        static const Json &none();
        bool yes() const { return kind == True; }
        long long integer() const { return kind == Number ? std::strtoll(text.c_str(), nullptr, 10) : 0; }
    };

    struct Json::Field
    {
        std::string first;
        Json second;
        Field(std::string key, Json value) : first(std::move(key)), second(std::move(value)) {}
    };

    // Out of the class: a Json made or destroyed in its body would need Field complete there.
    const Json &Json::none()
    {
        static const Json nothing;
        return nothing;
    }

    const Json &Json::operator[](std::string_view key) const
    {
        for (const auto &field : fields)
            if (field.first == key)
                return field.second;
        return none();
    }

    struct JsonReader
    {
        const char *at, *end;

        void space()
        {
            while (at < end && (*at == ' ' || *at == '\n' || *at == '\r' || *at == '\t'))
                ++at;
        }
        bool take(char c)
        {
            space();
            if (at < end && *at == c)
            {
                ++at;
                return true;
            }
            return false;
        }
        unsigned hex4()
        {
            unsigned code = 0;
            for (int i = 0; i < 4 && at < end; ++i, ++at)
                code = code * 16 + unsigned(std::isdigit(static_cast<unsigned char>(*at)) ? *at - '0' : (*at | 32) - 'a' + 10);
            return code;
        }
        std::string string() // after the opening quote
        {
            std::string out;
            while (at < end && *at != '"')
            {
                if (*at != '\\')
                {
                    out += *at++;
                    continue;
                }
                if (++at == end)
                    break;
                const char c = *at++;
                if (c == 'u')
                {
                    unsigned code = hex4();
                    if (code >= 0xD800 && code < 0xDC00 && end - at >= 6 && at[0] == '\\' && at[1] == 'u')
                    {
                        at += 2;
                        code = 0x10000 + ((code - 0xD800) << 10) + (hex4() - 0xDC00);
                    }
                    int size = 0;
                    const char *bytes = CodepointToUTF8(int(code), &size);
                    out.append(bytes, size_t(size));
                }
                else
                    out += c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c == 'b' ? '\b' : c == 'f' ? '\f' : c;
            }
            if (at < end)
                ++at;
            return out;
        }
        Json value(int depth)
        {
            Json v;
            space();
            if (at >= end || depth > 64)
                return v;
            if (*at == '{')
            {
                ++at;
                v.kind = Json::Object;
                if (take('}'))
                    return v;
                do
                {
                    if (!take('"'))
                        break;
                    std::string key = string();
                    if (!take(':'))
                        break;
                    v.fields.emplace_back(std::move(key), value(depth + 1));
                } while (take(','));
                take('}');
            }
            else if (*at == '[')
            {
                ++at;
                v.kind = Json::Array;
                if (take(']'))
                    return v;
                do
                    v.items.push_back(value(depth + 1));
                while (take(','));
                take(']');
            }
            else if (*at == '"')
            {
                ++at;
                v.kind = Json::String;
                v.text = string();
            }
            else
            {
                const char *start = at;
                while (at < end && (std::isalnum(static_cast<unsigned char>(*at)) || *at == '-' || *at == '+' || *at == '.'))
                    ++at;
                v.text.assign(start, at);
                v.kind = v.text == "true" ? Json::True : v.text == "false" ? Json::False : v.text == "null" || v.text.empty() ? Json::Null : Json::Number;
                if (at == start)
                    at = end; // not JSON: stop here
            }
            return v;
        }
    };

    Json parseJson(std::string_view text)
    {
        JsonReader reader{text.data(), text.data() + text.size()};
        return reader.value(0);
    }

    std::string quote(std::string_view text)
    {
        std::string out = "\"";
        for (const char c : text)
        {
            if (c == '"' || c == '\\')
                out += '\\';
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                out += escaped;
            }
            else
                out += c;
        }
        return out + "\"";
    }

    Json failure(const std::string &text)
    {
        Json answer, error;
        answer.kind = Json::Object;
        error.kind = Json::String;
        error.text = text;
        answer.fields.emplace_back("error", error);
        return answer;
    }

    // --- The backend: commands over HTTP, events over a stream ------------------

    struct Link
    {
        int port = 0;
        std::string token;
        std::mutex lock;
        std::condition_variable commandsReady, alarmMoved;
        struct Command
        {
            std::string name, body;
            std::function<void(const Json &)> done;
        };
        std::deque<Command> commands;
        std::vector<std::function<void()>> inbox; // run by the window's thread
        Clock::time_point alarm = Clock::time_point::max();
        bool open = false; // the window exists: it can be woken

        void deliver(std::function<void()> work)
        {
            const std::lock_guard<std::mutex> hold(lock);
            inbox.push_back(std::move(work));
            if (open)
                glfwPostEmptyEvent();
        }
        void wakeAt(Clock::time_point when)
        {
            const std::lock_guard<std::mutex> hold(lock);
            if (when < alarm)
            {
                alarm = when;
                alarmMoved.notify_one();
            }
        }
    };
    Link *g_link = nullptr; // never freed: its threads run until the process ends

    Json post(const Link &link, const std::string &name, const std::string &body, int timeoutMs)
    {
        const std::string request = "POST /api/" + name + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(link.port) +
                                    "\r\nX-BDR-Token: " + link.token + "\r\nContent-Type: application/json\r\nContent-Length: " +
                                    std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        std::string response;
        sys::exchange(link.port, request, timeoutMs, [&](const char *data, size_t size) {
            response.append(data, size);
            return true;
        });
        const size_t start = response.find("\r\n\r\n");
        if (start == std::string::npos || response.compare(0, 12, "HTTP/1.1 200") != 0)
            return failure("The launcher does not answer.");
        return parseJson(std::string_view(response).substr(start + 4));
    }

    void runCommands(Link *link)
    {
        for (;;)
        {
            std::unique_lock<std::mutex> hold(link->lock);
            link->commandsReady.wait(hold, [&] { return !link->commands.empty(); });
            Link::Command command = std::move(link->commands.front());
            link->commands.pop_front();
            hold.unlock();
            Json answer = post(*link, command.name, command.body, 120000);
            link->deliver([done = std::move(command.done), answer = std::move(answer)] { done(answer); });
        }
    }

    void runAlarm(Link *link)
    {
        std::unique_lock<std::mutex> hold(link->lock);
        for (;;)
        {
            if (link->alarm == Clock::time_point::max())
                link->alarmMoved.wait(hold);
            else if (link->alarmMoved.wait_until(hold, link->alarm) == std::cv_status::timeout && Clock::now() >= link->alarm)
            {
                link->alarm = Clock::time_point::max();
                if (link->open)
                    glfwPostEmptyEvent();
            }
        }
    }

    void opened();
    void lost();
    void received(const Json &message);

    // The event stream: "data: {...}" blocks, and a comment every two seconds that keeps the connection checked.
    void runEvents(Link *link)
    {
        const std::string request = "GET /events?t=" + link->token + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(link->port) + "\r\n\r\n";
        for (;;)
        {
            std::string buffer;
            bool open = false;
            sys::exchange(link->port, request, 10000, [&](const char *data, size_t size) {
                buffer.append(data, size);
                if (!open)
                {
                    const size_t head = buffer.find("\r\n\r\n");
                    if (head == std::string::npos)
                        return buffer.size() < 65536;
                    if (buffer.compare(0, 12, "HTTP/1.1 200") != 0)
                        return false;
                    buffer.erase(0, head + 4);
                    open = true;
                    link->deliver(opened);
                }
                for (size_t end; (end = buffer.find("\n\n")) != std::string::npos; buffer.erase(0, end + 2))
                    if (buffer.compare(0, 6, "data: ") == 0)
                        link->deliver([message = parseJson(std::string_view(buffer).substr(6, end - 6))] { received(message); });
                return true;
            });
            link->deliver(lost); // also wakes the window while the launcher cannot be reached
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    // --- Looks ------------------------------------------------------------------

    // The page's colours (tools/launcher.html).
    constexpr Color kBg{10, 10, 11, 255}, kSurface{17, 17, 19, 255}, kRaised{24, 24, 27, 255}, kLine{35, 35, 39, 255},
        kText{236, 236, 238, 255}, kMute{138, 138, 147, 255}, kRed{229, 32, 46, 255}, kRedHover{240, 48, 61, 255},
        kHover{32, 32, 36, 255}, kHoverLine{48, 48, 54, 255}, kTrack{42, 42, 48, 255}, kDone{58, 58, 65, 255},
        kLog{7, 7, 8, 255}, kLogText{201, 201, 207, 255}, kHint{85, 85, 93, 255}, kOff{93, 93, 102, 255}, kNone{0, 0, 0, 0};

    Color alpha(Color c, float a) { return Color{c.r, c.g, c.b, static_cast<unsigned char>(c.a * a)}; }

    float g_s = 1; // pixels per CSS pixel
    float u(float v) { return v * g_s; }
    float hairline() { return std::max(1.0f, std::round(g_s)); }

    struct Face
    {
        Font font{};
        float size = 10, spacing = 0; // drawn at the size it was made for: crisp
        std::vector<float> advance;   // by codepoint
        bool own = false;
    };
    enum { fText, fBold, fSmall, fNote, fTiny, fHead, fName, fNameLight, fGo, fMono, fCount };
    Face g_faces[fCount];
    const Face &F(int which) { return g_faces[which]; }

    struct FontFile
    {
        std::vector<unsigned char> data;
        float ascent = 0, descent = 0, units = 1;
    };

    // A TrueType or OpenType file (not a collection) with the metrics stb_truetype sizes glyphs by.
    FontFile readFont(const std::string &path)
    {
        FontFile file;
        std::string ext = path.size() > 4 ? path.substr(path.size() - 4) : "";
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        std::ifstream stream(pathOf(path), std::ios::binary);
        if ((ext != ".ttf" && ext != ".otf") || !stream)
            return file;
        std::vector<unsigned char> data((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        const auto u16 = [&](size_t at) { return at + 2 <= data.size() ? unsigned(data[at] << 8 | data[at + 1]) : 0u; };
        const auto u32 = [&](size_t at) { return size_t(u16(at)) << 16 | u16(at + 2); };
        size_t head = 0, hhea = 0;
        for (unsigned i = 0, count = u16(4); i < count && 12 + 16 * (i + 1) <= data.size(); ++i)
        {
            const std::string_view tag(reinterpret_cast<const char *>(&data[12 + 16 * i]), 4);
            if (tag == "head")
                head = u32(12 + 16 * i + 8);
            else if (tag == "hhea")
                hhea = u32(12 + 16 * i + 8);
        }
        if (!head || !hhea || !u16(head + 18))
            return file;
        file.units = float(u16(head + 18));
        file.ascent = float(int16_t(u16(hhea + 4)));
        file.descent = float(int16_t(u16(hhea + 6)));
        file.data = std::move(data);
        return file;
    }

#ifndef _WIN32
    std::string run(const std::string &command)
    {
        std::string out;
        if (FILE *pipe = popen(command.c_str(), "r"))
        {
            char block[512];
            for (size_t n; (n = fread(block, 1, sizeof(block), pipe)) > 0;)
                out.append(block, n);
            pclose(pipe);
        }
        return out;
    }
#endif

    // The system's fonts: 0 text, 1 bold, 2 monospace, 3 symbols (✕ ○ □ △ when the text font lacks them).
    FontFile systemFont(int kind)
    {
        std::vector<std::string> paths;
#ifdef _WIN32
        const char *windows = std::getenv("WINDIR");
        static const char *const names[] = {"segoeui.ttf", "seguisb.ttf", "consola.ttf", "seguisym.ttf"};
        paths.push_back(std::string(windows ? windows : "C:\\Windows") + "\\Fonts\\" + names[kind]);
#elif defined(__APPLE__)
        static const char *const names[] = {"Arial.ttf", "Arial Bold.ttf", "Courier New.ttf", "Arial Unicode.ttf"};
        for (const char *folder : {"/System/Library/Fonts/Supplemental/", "/Library/Fonts/"})
            paths.push_back(std::string(folder) + names[kind]);
#else
        static const char *const patterns[] = {"sans-serif", "sans-serif:bold", "monospace", "sans-serif:charset=2715"};
        paths.push_back(run(std::string("fc-match -f '%{file}' '") + patterns[kind] + "' 2>/dev/null"));
        static const char *const names[] = {"DejaVuSans.ttf", "DejaVuSans-Bold.ttf", "DejaVuSansMono.ttf", "DejaVuSans.ttf"};
        for (const char *folder : {"/usr/share/fonts/truetype/dejavu/", "/usr/share/fonts/TTF/", "/usr/share/fonts/dejavu-sans-fonts/",
                                   "/usr/share/fonts/dejavu/", "/usr/share/fonts/dejavu-sans-mono-fonts/"})
            paths.push_back(std::string(folder) + names[kind]);
#endif
        for (const std::string &path : paths)
            if (FontFile file = readFont(path); !file.data.empty())
                return file;
        return {};
    }

    std::vector<int> codepoints()
    {
        std::vector<int> list; // ASCII first: raylib looks glyphs up in order
        for (int c = 32; c < 127; ++c)
            list.push_back(c);
        for (int c = 160; c < 384; ++c)
            list.push_back(c);
        for (int c = 0x2010; c < 0x2028; ++c)
            list.push_back(c);
        for (int c : {0x2030, 0x2039, 0x203A, 0x20AC, 0x2122, 0x2190, 0x2191, 0x2192, 0x2193, 0x2713, 0x2715, 0x25A1, 0x25B3, 0x25CB})
            list.push_back(c);
        return list;
    }

    // `em` pixels a CSS font-size would give; glyphs `file` lacks come from `fallback`.
    Face loadFace(const FontFile &file, const FontFile &fallback, float em)
    {
        Face face;
        if (file.data.empty()) // no font file: raylib's own, at a whole multiple of its size
        {
            face.font = GetFontDefault();
            face.size = 10.0f * std::max(1.0f, std::round(em / 9.0f));
            face.spacing = face.size / 10.0f;
        }
        else
        {
            // stb_truetype sizes the ascent-to-descent height; CSS sizes the em.
            const auto pixels = [em](const FontFile &f) { return std::max(1, int(std::lround(em * (f.ascent - f.descent) / f.units))); };
            const auto ascent = [](const FontFile &f, int size) { return int(f.ascent * float(size) / (f.ascent - f.descent)); };
            const int size = pixels(file);
            std::vector<int> wanted = codepoints(), lacking;
            GlyphInfo *glyphs = LoadFontData(file.data.data(), int(file.data.size()), size, wanted.data(), int(wanted.size()), FONT_DEFAULT);
            std::vector<GlyphInfo> kept;
            for (size_t i = 0; glyphs && i < wanted.size(); ++i)
                if (glyphs[i].image.data)
                    kept.push_back(glyphs[i]);
                else
                    lacking.push_back(wanted[i]);
            if (!lacking.empty() && !fallback.data.empty())
            {
                const int other = pixels(fallback), shift = ascent(file, size) - ascent(fallback, other);
                GlyphInfo *extra = LoadFontData(fallback.data.data(), int(fallback.data.size()), other, lacking.data(), int(lacking.size()), FONT_DEFAULT);
                for (size_t i = 0; extra && i < lacking.size(); ++i)
                    if (extra[i].image.data)
                    {
                        extra[i].offsetY += shift;
                        kept.push_back(extra[i]);
                    }
                MemFree(extra); // the images now belong to `kept`
            }
            MemFree(glyphs);
            face.font.baseSize = size;
            face.font.glyphCount = int(kept.size());
            face.font.glyphPadding = 4;
            face.font.glyphs = static_cast<GlyphInfo *>(MemAlloc(unsigned(kept.size() * sizeof(GlyphInfo))));
            std::copy(kept.begin(), kept.end(), face.font.glyphs);
            Image atlas = GenImageFontAtlas(face.font.glyphs, &face.font.recs, face.font.glyphCount, size, face.font.glyphPadding, 0);
            face.font.texture = LoadTextureFromImage(atlas);
            UnloadImage(atlas);
            for (int i = 0; i < face.font.glyphCount; ++i)
            {
                UnloadImage(face.font.glyphs[i].image);
                face.font.glyphs[i].image = Image{};
            }
            face.size = float(size);
            face.own = true;
        }
        face.advance.assign(0x2800, 0.0f);
        const float scale = face.size / float(face.font.baseSize);
        for (int i = 0; i < face.font.glyphCount; ++i)
        {
            const GlyphInfo &glyph = face.font.glyphs[i];
            if (glyph.value >= 0 && glyph.value < 0x2800)
                face.advance[size_t(glyph.value)] = float(glyph.advanceX ? glyph.advanceX : int(face.font.recs[i].width)) * scale + face.spacing;
        }
        const float unknown = face.advance['?'];
        for (float &advance : face.advance)
            advance = advance > 0 ? advance : unknown;
        face.advance[' '] = face.advance[' '] > 0 ? face.advance[' '] : unknown;
        return face;
    }

    void loadFaces()
    {
        static const FontFile text = systemFont(0), bold = systemFont(1), mono = systemFont(2), symbols = systemFont(3);
        // CSS sizes of the page: body 14, hints 12.5, notes 13, footer 12, card titles 11, name 16, Play 17, console 12.
        const struct { const FontFile &file; float em; } faces[fCount] = {
            {text, 14}, {bold, 14}, {text, 12.5f}, {text, 13}, {text, 12}, {bold, 11}, {bold, 16}, {text, 16}, {bold, 17}, {mono, 12}};
        for (int i = 0; i < fCount; ++i)
        {
            if (g_faces[i].own)
                UnloadFont(g_faces[i].font);
            g_faces[i] = loadFace(faces[i].file.data.empty() ? text : faces[i].file, symbols, faces[i].em * g_s);
        }
    }

    int nextCodepoint(std::string_view s, size_t &i)
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = c < 0x80 ? 1 : c >> 5 == 6 ? 2 : c >> 4 == 14 ? 3 : c >> 3 == 30 ? 4 : 1;
        if (i + n > s.size())
            n = 1;
        int code = n == 1 ? (c < 0x80 ? c : '?') : c & (0x7F >> n);
        for (size_t k = 1; k < n; ++k)
            code = code << 6 | (s[i + k] & 0x3F);
        i += n;
        return code;
    }

    float advanceOf(const Face &f, int c) { return c >= 0 && c < int(f.advance.size()) ? f.advance[size_t(c)] : f.advance['?']; }

    float measure(const Face &f, std::string_view s, float spacing = 0)
    {
        float width = 0;
        for (size_t i = 0; i < s.size();)
            width += advanceOf(f, nextCodepoint(s, i)) + spacing;
        return width;
    }

    // --- Drawing ----------------------------------------------------------------

    bool g_draw = true; // false while a card measures its content
    std::vector<Rectangle> g_clips;

    Rectangle snap(Rectangle r)
    {
        const float x = std::round(r.x), y = std::round(r.y);
        return {x, y, std::round(r.x + r.width) - x, std::round(r.y + r.height) - y};
    }

    Rectangle overlap(Rectangle a, Rectangle b)
    {
        const float x = std::max(a.x, b.x), y = std::max(a.y, b.y);
        return {x, y, std::max(0.0f, std::min(a.x + a.width, b.x + b.width) - x), std::max(0.0f, std::min(a.y + a.height, b.y + b.height) - y)};
    }

    void scissor()
    {
        if (!g_draw)
            return;
        if (g_clips.empty())
            EndScissorMode();
        else
        {
            const Rectangle r = snap(g_clips.back());
            BeginScissorMode(int(r.x), int(r.y), int(r.width), int(r.height));
        }
    }
    void pushClip(Rectangle r)
    {
        g_clips.push_back(g_clips.empty() ? r : overlap(r, g_clips.back()));
        scissor();
    }
    void popClip()
    {
        g_clips.pop_back();
        scissor();
    }
    bool visible(float y, float height) { return g_clips.empty() || (y < g_clips.back().y + g_clips.back().height && y + height > g_clips.back().y); }

    // A rounded box with a border inside its edge.
    void box(Rectangle r, float radius, Color border, Color fill, float thickness = 0)
    {
        if (!g_draw)
            return;
        r = snap(r);
        if (border.r == fill.r && border.g == fill.g && border.b == fill.b && border.a == fill.a)
            border = kNone; // one shape: a line over its own fill shows a ring where the two edges blend
        const float line = thickness > 0 ? thickness : hairline();
        const Rectangle inner{r.x + line, r.y + line, r.width - 2 * line, r.height - 2 * line};
        const auto roundness = [](Rectangle of, float rad) { return std::min(1.0f, 2 * rad / std::max(1.0f, std::min(of.width, of.height))); };
        if (fill.a)
            DrawRectangleRounded(border.a ? inner : r, roundness(border.a ? inner : r, std::max(0.0f, radius - (border.a ? line : 0))), 0, fill);
        if (border.a)
            DrawRectangleRoundedLinesEx(inner, roundness(inner, std::max(0.0f, radius - line)), 0, line, border);
    }

    // Text on one line; `middle` is the middle of its line box.
    float print(const Face &f, std::string_view s, float x, float middle, Color color, float spacing = 0)
    {
        x = std::round(x);
        const float y = std::round(middle - f.size / 2);
        for (size_t i = 0; i < s.size();)
        {
            const int c = nextCodepoint(s, i);
            if (g_draw && c != ' ')
                DrawTextCodepoint(f.font, c, {x, y}, f.size, color);
            x += advanceOf(f, c) + spacing;
        }
        return x;
    }

    // The lines `text` takes in `width`: it breaks at spaces, inside a word longer than a line, and at '\n'.
    std::vector<std::string_view> wrap(const Face &f, std::string_view text, float width)
    {
        std::vector<std::string_view> lines;
        for (size_t start = 0;;)
        {
            const size_t newline = std::min(text.find('\n', start), text.size());
            const std::string_view para = text.substr(start, newline - start);
            size_t from = 0;
            do
            {
                float w = 0;
                size_t i = from, cut = para.size(), space = std::string_view::npos;
                while (i < para.size())
                {
                    const size_t at = i;
                    const int c = nextCodepoint(para, i);
                    if (c == ' ')
                        space = at;
                    w += advanceOf(f, c);
                    if (w > width && at > from)
                    {
                        cut = space != std::string_view::npos && space > from ? space : at;
                        break;
                    }
                }
                std::string_view line = para.substr(from, cut - from);
                while (!line.empty() && line.back() == ' ')
                    line.remove_suffix(1);
                lines.push_back(line);
                for (from = cut; from < para.size() && para[from] == ' ';)
                    ++from;
            } while (from < para.size());
            if (newline >= text.size())
                return lines;
            start = newline + 1;
        }
    }

    // Wrapped text from (x, top); returns its height.
    float paragraph(const Face &f, std::string_view text, float x, float top, float width, float lineHeight, Color color, bool center = false)
    {
        const std::vector<std::string_view> lines = wrap(f, text, width);
        for (size_t i = 0; i < lines.size(); ++i)
            print(f, lines[i], center ? x + (width - measure(f, lines[i])) / 2 : x, top + lineHeight * (float(i) + 0.5f), color);
        return lineHeight * float(lines.size());
    }

    // The changelog's Markdown, as far as it goes: **bold** and `code`.
    struct Run
    {
        std::string text;
        int style; // 0 plain, 1 bold, 2 code
    };
    std::vector<Run> runs(std::string_view line)
    {
        std::vector<Run> out;
        std::string plain;
        for (size_t i = 0; i < line.size();)
        {
            const bool bold = line.compare(i, 2, "**") == 0;
            if (bold || line[i] == '`')
            {
                const std::string_view mark = bold ? "**" : "`";
                const size_t close = line.find(mark, i + mark.size());
                if (close != std::string_view::npos && close > i + mark.size())
                {
                    if (!plain.empty())
                        out.push_back({std::exchange(plain, {}), 0});
                    out.push_back({std::string(line.substr(i + mark.size(), close - i - mark.size())), bold ? 1 : 2});
                    i = close + mark.size();
                    continue;
                }
            }
            plain += line[i++];
        }
        if (!plain.empty())
            out.push_back({plain, 0});
        return out;
    }

    float rich(const std::vector<Run> &list, int plainFace, int boldFace, float x, float top, float width, float lineHeight, Color plain, Color strong)
    {
        struct Piece
        {
            float x;
            int line;
            std::string_view text;
            const Run *run;
            bool first, last;
        };
        std::vector<Piece> pieces;
        float cx = 0;
        int line = 0;
        const float pad = u(6);
        for (const Run &r : list)
        {
            const Face &face = F(r.style == 1 ? boldFace : r.style == 2 ? fMono : plainFace);
            const float edge = r.style == 2 ? pad : 0;
            for (size_t i = 0; i <= r.text.size();)
            {
                const size_t space = std::min(r.text.find(' ', i), r.text.size());
                const std::string_view word = std::string_view(r.text).substr(i, space - i);
                const bool first = i == 0, last = space == r.text.size();
                const float w = measure(face, word) + (first ? edge : 0) + (last ? edge : 0);
                if (cx > 0 && cx + w > width)
                {
                    ++line;
                    cx = 0;
                }
                pieces.push_back({cx + (first ? edge : 0), line, word, &r, first, last});
                cx += w + (last ? 0 : advanceOf(face, ' '));
                i = space + 1;
            }
        }
        const auto middle = [&](int l) { return top + lineHeight * (float(l) + 0.5f); };
        float left = 0;
        for (size_t k = 0; g_draw && k < pieces.size(); ++k) // code boxes under the text, one per line of a span
        {
            const Piece &p = pieces[k];
            if (p.run->style != 2)
                continue;
            if (p.first || pieces[k - 1].line != p.line)
                left = p.x - (p.first ? pad : 0);
            if (p.last || pieces[k + 1].line != p.line)
            {
                const float right = p.x + measure(F(fMono), p.text) + (p.last ? pad : 0);
                box({x + left, middle(p.line) - F(fMono).size / 2 - hairline(), right - left, F(fMono).size + 2 * hairline()}, u(5), kLine, kBg);
            }
        }
        for (const Piece &p : pieces)
        {
            const int style = p.run->style;
            print(F(style == 1 ? boldFace : style == 2 ? fMono : plainFace), p.text, x + p.x, middle(p.line), style == 1 ? strong : plain);
        }
        return pieces.empty() ? 0 : lineHeight * float(line + 1);
    }

    // --- Input --------------------------------------------------------------------

    struct Input
    {
        Vector2 mouse{};
        bool click = false; // a left press nothing has taken yet
        bool pressed[8]{};
        float wheel = 0;
        std::vector<int> keys, chars;
        bool free = true; // the mouse is not over the open list
    } in;
    bool g_pressed[8];
    float g_wheel;
    GLFWmousebuttonfun g_raylibButton;
    GLFWscrollfun g_raylibScroll;
    int g_cursor = MOUSE_CURSOR_DEFAULT;
    std::string g_drag; // what the mouse drags: a slider's setting or a scroll bar
    float g_dragFrom = 0, g_dragMouse = 0;

    void onButton(GLFWwindow *window, int button, int action, int mods)
    {
        if (action == 1 && button >= 0 && button < 8)
            g_pressed[button] = true;
        g_raylibButton(window, button, action, mods);
    }
    void onScroll(GLFWwindow *window, double x, double y)
    {
        g_wheel += float(y);
        g_raylibScroll(window, x, y);
    }

    bool hot(Rectangle r)
    {
        return g_draw && in.free && CheckCollisionPointRec(in.mouse, r) && (g_clips.empty() || CheckCollisionPointRec(in.mouse, g_clips.back()));
    }
    bool take()
    {
        return std::exchange(in.click, false);
    }
    bool hit(Rectangle r)
    {
        if (!hot(r))
            return false;
        g_cursor = MOUSE_CURSOR_POINTING_HAND;
        return take();
    }
    bool keyHit(int key) { return std::find(in.keys.begin(), in.keys.end(), key) != in.keys.end() || IsKeyPressedRepeat(key); }

    // --- What the window shows ------------------------------------------------------

    enum Page { kPlay, kConsole, kSettings, kUpdates };

    struct Field
    {
        std::string text;
        size_t caret = 0;
        float scroll = 0;
        bool edited = false, commit = false;
        Rectangle box{};
    };
    Field *g_focus = nullptr;

    struct Popup
    {
        std::string id;
        Rectangle box{};
        std::vector<std::string> labels;
        int chosen = -1;
        bool open = false;
    } g_popup;

    struct Window
    {
        Json state;
        bool have = false, connected = false, restart = false;
        std::string build;
        long long front = 0;
        std::intptr_t backend = 0; // the launcher this window started, 0 when it found one running
        int stopped = -1;          // its exit code once it ended
        std::string program;       // this program's file (Windows): an update replaces it
        fs::file_time_type programTime{};
        Page page = kPlay, nextPage = kPlay;
        float scroll[4]{}, content[4]{};
        std::vector<std::string> log;
        bool logOpen = false, logAtEnd = true;
        float logScroll = 0;
        Field iso, newName, newSaves;
        std::deque<std::pair<std::string, Field>> fields; // a deque: g_focus points into it
        bool newOpen = false;
        std::string profile, note;
        std::vector<std::pair<std::string, std::string>> values; // settings.ini, in file order
        long long settingsStamp = -1;
        std::string listening; // the PS2 button whose "+" waits for a key
        Rectangle listenBox{};
        std::string toast;
        double toastUntil = 0;
        Texture2D icon{};
    } g;

    void toast(const std::string &text)
    {
        g.toast = text;
        g.toastUntil = GetTime() + 5;
    }

    const std::string *value(std::string_view key)
    {
        for (const auto &item : g.values)
            if (item.first == key)
                return &item.second;
        return nullptr;
    }
    void setValue(const std::string &key, const std::string &text)
    {
        for (auto &item : g.values)
            if (item.first == key)
                item.second = text;
    }
    void setValues(const Json &items)
    {
        g.values.clear();
        for (const Json &item : items.items)
            if (item["key"].kind == Json::String)
                g.values.emplace_back(item["key"].text, item["value"].text);
        g.listening.clear();
    }
    Field &field(const std::string &key)
    {
        for (auto &item : g.fields)
            if (item.first == key)
                return item.second;
        return g.fields.emplace_back(key, Field{}).second;
    }

    bool programChanged()
    {
        std::error_code error;
        return g.program.empty() || fs::last_write_time(pathOf(g.program), error) != g.programTime;
    }

    void applyState(const Json &s)
    {
        if (g.have && s["build"].text != g.build && programChanged())
            g.restart = true; // the backend restarted after an update that also changed this program
        if (g.have && s["front"].integer() > g.front)
        {
            if (IsWindowMinimized())
                RestoreWindow();
            SetWindowFocused(); // the launcher was started again
        }
        g.build = s["build"].text;
        g.front = s["front"].integer();
        g.have = true;
        g.state = s;
        if (!g.iso.edited && g_focus != &g.iso)
        {
            g.iso.text = s["iso"].text;
            g.iso.caret = g.iso.text.size();
        }
        if (!(g_popup.open && g_popup.id == "profile"))
            g.profile = s["profile"].text;
        // Drawn again only when the file is newer than what the window shows: an answer that was already on its
        // way when the window saved a setting must not put the old value back.
        if (s["settings_stamp"].integer() > g.settingsStamp)
        {
            g.settingsStamp = s["settings_stamp"].integer();
            setValues(s["settings"]);
        }
        g.note.clear();
    }

    void send(const std::string &name, const std::string &body, std::function<void(const Json &)> done)
    {
        Link &link = *g_link;
        if (name == "browse" || name == "browse-saves") // a dialog: the other commands do not wait for it
        {
            std::thread([&link, name, body, done = std::move(done)] {
                Json answer = post(link, name, body, 0);
                link.deliver([done, answer = std::move(answer)] { done(answer); });
            }).detach();
            return;
        }
        const std::lock_guard<std::mutex> hold(link.lock);
        if (name == "state" && !link.commands.empty() && link.commands.back().name == "state")
            return; // one is already waiting
        link.commands.push_back({name, body, std::move(done)});
        link.commandsReady.notify_one();
    }

    void api(const std::string &name, const std::string &body = "{}", std::function<void(const Json &)> then = {})
    {
        send(name, body, [then = std::move(then)](const Json &answer) {
            if (!answer["error"].text.empty())
                toast(answer["error"].text);
            else if (then)
                then(answer);
        });
    }

    void act(const std::string &name, const std::string &body = "{}", std::function<void(const Json &)> then = {})
    {
        api(name, body, [then = std::move(then)](const Json &answer) {
            if (answer["steps"].kind == Json::Object)
                applyState(answer);
            if (then)
                then(answer);
        });
    }

    void appendLog(std::string_view text)
    {
        for (const char c : text)
        {
            if (c == '\r')
                continue;
            if (c == '\n')
            {
                if (!g.logOpen)
                    g.log.emplace_back();
                g.logOpen = false;
                continue;
            }
            if (!g.logOpen)
                g.log.emplace_back();
            g.logOpen = true;
            g.log.back() += c == '\t' ? std::string(4, ' ') : std::string(1, c);
        }
        if (g.log.size() > 20000)
            g.log.erase(g.log.begin(), g.log.begin() + 4000);
    }

    void opened()
    {
        g.connected = true;
        g.log.clear(); // a new connection sends the console again
        g.logOpen = false;
        act("state");
    }
    void lost()
    {
        g.connected = false;
    }
    void received(const Json &message)
    {
        for (const Json &line : message["lines"].items)
            appendLog(line.text);
        if (message["state"].yes())
            act("state");
    }

    void saveSetting(const std::string &key, const std::string &text, bool redraw)
    {
        setValue(key, text);
        api("settings", "{\"values\": {" + quote(key) + ": " + quote(text) + "}}", [key, text, redraw](const Json &answer) {
            g.settingsStamp = answer["settings_stamp"].integer();
            if (redraw)
                setValues(answer["settings"]);
        });
    }

    // "LEFT_SHIFT" -> "Left Shift", "KP_1" -> "Keypad 1", "MOUSE_WHEEL_UP" -> "Wheel Up"
    std::string keyLabel(std::string name)
    {
        if (name.rfind("KP_", 0) == 0)
            name = "KEYPAD_" + name.substr(3);
        else if (name.rfind("MOUSE_WHEEL_", 0) == 0)
            name = "WHEEL_" + name.substr(12);
        std::string out;
        for (size_t start = 0;;)
        {
            const size_t end = std::min(name.find('_', start), name.size());
            std::string word = name.substr(start, end - start);
            if (word.size() > 2 || word == "UP")
                std::transform(word.begin() + 1, word.end(), word.begin() + 1, [](unsigned char c) { return char(std::tolower(c)); });
            out += (out.empty() ? "" : " ") + word;
            if (end == name.size())
                return out;
            start = end + 1;
        }
    }

    std::vector<std::string> words(const std::string &text)
    {
        std::vector<std::string> out;
        for (size_t start = 0; start < text.size();)
        {
            const size_t end = std::min(text.find(' ', start), text.size());
            if (end > start && text.compare(start, end - start, "NONE") != 0)
                out.push_back(text.substr(start, end - start));
            start = end + 1;
        }
        return out;
    }
    std::string joined(const std::vector<std::string> &list, const char *empty)
    {
        std::string out;
        for (const std::string &word : list)
            out += (out.empty() ? "" : " ") + word;
        return out.empty() ? empty : out;
    }

    // --- Widgets --------------------------------------------------------------------

    enum Look { kPlain, kPrimary, kQuiet };

    float buttonWidth(std::string_view label) { return std::round(measure(F(fText), label) + u(30)); }

    bool button(float x, float y, std::string_view label, Look look = kPlain, bool enabled = true)
    {
        const Rectangle r = snap({x, y, buttonWidth(label), u(39)});
        const bool over = enabled && hot(r);
        if (over)
            g_cursor = MOUSE_CURSOR_POINTING_HAND;
        if (g_draw)
        {
            Color back = look == kPrimary ? kRed : look == kQuiet ? kNone : kRaised;
            Color line = back.a ? (look == kPrimary ? kRed : kLine) : kNone, ink = look == kPrimary ? WHITE : look == kQuiet ? kMute : kText;
            if (over)
            {
                back = look == kPrimary ? kRedHover : look == kQuiet ? kRaised : kHover;
                line = look == kPrimary ? kRedHover : look == kQuiet ? kLine : kHoverLine;
                ink = look == kQuiet ? kText : ink;
            }
            const float a = enabled ? 1.0f : 0.4f;
            box(r, u(8), alpha(line, a), alpha(back, a));
            print(F(fText), label, r.x + u(15), r.y + r.height / 2, alpha(ink, a));
        }
        return over && take();
    }

    size_t previousCodepoint(const std::string &s, size_t i)
    {
        while (i > 0 && (static_cast<unsigned char>(s[--i]) & 0xC0) == 0x80)
        {
        }
        return i;
    }
    size_t nextCodepointAt(const std::string &s, size_t i)
    {
        nextCodepoint(s, i);
        return std::min(i, s.size());
    }

    void edit(Field &f, size_t limit, bool numeric)
    {
        const bool control = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) || IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
        const auto insert = [&](std::string text) {
            if (numeric)
                text.erase(std::remove_if(text.begin(), text.end(), [](char c) { return !std::isdigit(static_cast<unsigned char>(c)) && c != '-'; }), text.end());
            const std::string all = f.text + text;
            size_t count = 0;
            for (size_t i = 0; i < all.size(); ++count)
                nextCodepoint(all, i);
            if (text.empty() || (limit && count > limit))
                return;
            f.text.insert(f.caret, text);
            f.caret += text.size();
            f.edited = true;
        };
        for (const int c : in.chars)
        {
            int size = 0;
            const char *bytes = CodepointToUTF8(c, &size);
            insert(std::string(bytes, size_t(size)));
        }
        if (control && keyHit(KEY_V))
            if (const char *clip = GetClipboardText())
            {
                std::string text = clip;
                std::replace_if(text.begin(), text.end(), [](char c) { return c == '\r' || c == '\n' || c == '\t'; }, ' ');
                insert(text);
            }
        if (keyHit(KEY_BACKSPACE) && f.caret > 0)
        {
            const size_t from = previousCodepoint(f.text, f.caret);
            f.text.erase(from, f.caret - from);
            f.caret = from;
            f.edited = true;
        }
        if (keyHit(KEY_DELETE) && f.caret < f.text.size())
        {
            f.text.erase(f.caret, nextCodepointAt(f.text, f.caret) - f.caret);
            f.edited = true;
        }
        if (keyHit(KEY_LEFT))
            f.caret = previousCodepoint(f.text, f.caret);
        if (keyHit(KEY_RIGHT) && f.caret < f.text.size())
            f.caret = nextCodepointAt(f.text, f.caret);
        if (keyHit(KEY_HOME))
            f.caret = 0;
        if (keyHit(KEY_END))
            f.caret = f.text.size();
        if (keyHit(KEY_ENTER) || keyHit(KEY_KP_ENTER))
            f.commit = true;
    }

    // A text box; true when its text was committed (Enter, or a click elsewhere).
    bool input(Field &f, Rectangle r, std::string_view placeholder, float padding, size_t limit = 0, bool numeric = false)
    {
        r = snap(r);
        const Face &face = F(fText);
        const float inner = r.x + padding, middle = r.y + r.height / 2;
        if (hot(r))
        {
            g_cursor = MOUSE_CURSOR_IBEAM;
            if (take())
            {
                g_focus = &f;
                f.caret = 0;
                for (float x = inner - f.scroll; f.caret < f.text.size();)
                {
                    size_t next = f.caret;
                    const float w = advanceOf(face, nextCodepoint(f.text, next));
                    if (x + w / 2 > in.mouse.x)
                        break;
                    x += w;
                    f.caret = next;
                }
            }
        }
        const bool focused = g_focus == &f;
        if (g_draw)
        {
            f.box = r;
            if (focused)
                edit(f, limit, numeric);
            const float caret = measure(face, std::string_view(f.text).substr(0, f.caret)), room = r.width - 2 * padding;
            f.scroll = !focused ? 0 : std::clamp(f.scroll, caret - room, caret);
            if (focused)
                box({r.x - u(3), r.y - u(3), r.width + u(6), r.height + u(6)}, u(11), alpha(kRed, 0.14f), kNone, u(3));
            box(r, u(8), focused ? kRed : kLine, kBg);
            pushClip({inner - 1, r.y, room + 2, r.height});
            if (f.text.empty())
                print(face, placeholder, inner, middle, kHint);
            print(face, f.text, inner - f.scroll, middle, kText);
            if (focused)
                DrawRectangleRec(snap({inner - f.scroll + caret, middle - face.size / 2, hairline(), face.size}), kText);
            popClip();
        }
        return std::exchange(f.commit, false);
    }

    // A drop-down list; the index chosen this frame, -1 otherwise.
    int dropdown(const std::string &id, Rectangle r, const std::vector<std::string> &labels, const std::string &current)
    {
        r = snap(r);
        int chosen = -1;
        if (g_draw && g_popup.id == id && g_popup.chosen >= 0)
            chosen = std::exchange(g_popup.chosen, -1);
        if (hit(r))
        {
            float width = r.width;
            for (const std::string &label : labels)
                width = std::max(width, measure(F(fText), label) + u(36));
            const float height = u(30) * float(labels.size()) + u(8);
            const float below = r.y + r.height + u(4), above = r.y - u(4) - height;
            g_popup = {id, snap({r.x, below + height > float(GetScreenHeight()) && above > 0 ? above : below, width, height}), labels, -1, true};
        }
        if (g_draw)
        {
            const bool open = g_popup.open && g_popup.id == id;
            box(r, u(8), open ? kRed : kLine, kBg);
            pushClip({r.x, r.y, r.width - u(24), r.height});
            print(F(fText), current, r.x + u(11), r.y + r.height / 2, kText);
            popClip();
            const Vector2 tip{r.x + r.width - u(15), r.y + r.height / 2 + u(2.5f)};
            DrawLineEx({tip.x - u(4), tip.y - u(4)}, tip, u(1.5f), kMute);
            DrawLineEx(tip, {tip.x + u(4), tip.y - u(4)}, u(1.5f), kMute);
        }
        return chosen;
    }
    float dropdownWidth(const std::vector<std::string> &labels, float padding)
    {
        float width = 0;
        for (const std::string &label : labels)
            width = std::max(width, measure(F(fText), label));
        return std::round(width + 2 * padding + u(24));
    }

    void drawPopup()
    {
        if (!g_popup.open)
            return;
        box(g_popup.box, u(8), kLine, kRaised);
        for (size_t i = 0; i < g_popup.labels.size(); ++i)
        {
            const Rectangle item = snap({g_popup.box.x + u(4), g_popup.box.y + u(4) + u(30) * float(i), g_popup.box.width - u(8), u(30)});
            if (CheckCollisionPointRec(in.mouse, item))
            {
                box(item, u(6), kNone, kTrack);
                g_cursor = MOUSE_CURSOR_POINTING_HAND;
            }
            print(F(fText), g_popup.labels[i], item.x + u(10), item.y + item.height / 2, kText);
        }
    }

    bool toggle(float x, float y, bool on)
    {
        const Rectangle r = snap({x, y, u(38), u(22)});
        if (g_draw)
        {
            box(r, r.height / 2, kNone, on ? kRed : kTrack);
            DrawCircleV({r.x + u(on ? 27.0f : 11.0f), r.y + r.height / 2}, u(8), on ? WHITE : kMute);
        }
        return hit(r);
    }

    std::string capitalized(std::string text)
    {
        if (!text.empty())
            text[0] = char(std::toupper(static_cast<unsigned char>(text[0])));
        return text;
    }

    float segmentsWidth(const Json &options)
    {
        float width = u(6);
        for (const Json &option : options.items)
            width += std::round(measure(F(fText), capitalized(option.text)) + u(24));
        return width;
    }
    // A choice of a few: the option clicked, -1 otherwise.
    int segments(float x, float y, const Json &options, const std::string &current)
    {
        box({x, y, segmentsWidth(options), u(35)}, u(9), kLine, kBg);
        int clicked = -1;
        float cx = x + u(3);
        for (size_t i = 0; i < options.items.size(); ++i)
        {
            const std::string label = capitalized(options[i].text);
            const Rectangle r = snap({cx, y + u(3), std::round(measure(F(fText), label) + u(24)), u(29)});
            const bool on = options[i].text == current;
            if (on)
                box(r, u(7), kNone, kTrack);
            print(F(fText), label, r.x + u(12), r.y + r.height / 2, on ? kText : kMute);
            if (hit(r))
                clicked = int(i);
            cx += r.width;
        }
        return clicked;
    }

    // A slider; true when it is let go. `shown` follows the mouse while it drags.
    bool slider(const std::string &key, float x, float y, int low, int high, double &shown)
    {
        const Rectangle track = snap({x, y + u(10.5f) - u(2), u(170), u(4)});
        const auto at = [&](float mouseX) { return std::round(low + std::clamp((mouseX - track.x) / track.width, 0.0f, 1.0f) * float(high - low)); };
        bool released = false;
        if (g_drag == key && g_draw)
        {
            shown = at(in.mouse.x);
            released = !IsMouseButtonDown(MOUSE_BUTTON_LEFT);
            if (released)
                g_drag.clear();
        }
        else if (hit({track.x - u(8), y, track.width + u(16), u(21)}))
        {
            g_drag = key;
            shown = at(in.mouse.x);
        }
        if (g_draw)
        {
            const float t = high > low ? std::clamp(float(shown - low) / float(high - low), 0.0f, 1.0f) : 0;
            box(track, u(2), kNone, kTrack);
            box({track.x, track.y, track.width * t, track.height}, u(2), kNone, kRed);
            DrawCircleV({track.x + track.width * t, track.y + track.height / 2}, u(8), kRed);
        }
        return released;
    }

    void scrollbar(const std::string &id, Rectangle view, float content, float &scroll)
    {
        const float most = std::max(0.0f, content - view.height);
        scroll = std::clamp(scroll, 0.0f, most);
        if (most <= 0)
            return;
        const float length = std::max(u(30), view.height * view.height / content);
        const Rectangle thumb = snap({view.x + view.width - u(10), view.y + (view.height - length) * scroll / most, u(10), length});
        if (g_drag == id)
        {
            scroll = std::clamp(g_dragFrom + (in.mouse.y - g_dragMouse) * most / (view.height - length), 0.0f, most);
            if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT))
                g_drag.clear();
        }
        else if (hot(thumb) && take())
        {
            g_drag = id;
            g_dragFrom = scroll;
            g_dragMouse = in.mouse.y;
        }
        box({thumb.x + u(2), thumb.y + u(2), thumb.width - u(4), thumb.height - u(4)}, u(3), kNone, g_drag == id || hot(thumb) ? kDone : kTrack);
    }

    // A card: its title, then what `content` lays out from (x, y) in width w (it returns its height).
    float card(float x, float y, float w, const std::string &title, const std::function<float(float, float, float)> &content,
               Color border = kLine, Color titleColor = kMute, bool plainTitle = false)
    {
        const auto inner = [&]() {
            float h = u(17);
            if (!title.empty())
            {
                const float line = plainTitle ? u(21) : u(16.5f);
                std::string shown = title;
                if (!plainTitle)
                    std::transform(shown.begin(), shown.end(), shown.begin(), [](unsigned char c) { return char(std::toupper(c)); });
                print(F(plainTitle ? fBold : fHead), shown, x + u(19), y + h + line / 2, titleColor, plainTitle ? 0 : std::round(u(1)));
                h += line + u(12);
            }
            return h + content(x + u(19), y + h, w - u(38)) + u(19);
        };
        const bool drawing = g_draw;
        g_draw = false;
        const float height = std::round(inner());
        g_draw = drawing && visible(y, height);
        if (g_draw)
        {
            box({x, y, w, height}, u(12), border, kSurface);
            inner();
        }
        g_draw = drawing;
        return height;
    }

    // --- Pages ------------------------------------------------------------------------

    bool busy() { return !g.state["running"].text.empty(); }

    float playPage(float x, float y, float w, Rectangle view)
    {
        const Json &s = g.state;
        const float top = y;
        if (!s["missing"].text.empty())
            y += card(x, y, w, "Before the first installation", [&](float cx, float cy, float cw) {
                return paragraph(F(fText), s["missing"].text, cx, cy, cw, u(21), kText);
            }, alpha(kRed, 0.45f), kRed) + u(14);
        const bool narrow = float(GetScreenWidth()) <= u(940);
        const float leftW = narrow ? w : w - u(340) - u(18);
        float left = y;
        left += card(x, left, leftW, "Game disc", [&](float cx, float cy, float cw) {
            const float browse = buttonWidth("Browse");
            input(g.iso, {cx, cy, cw - browse - u(8), u(39)}, "Path of your Burnout Dominator ISO", u(11));
            if (button(cx + cw - browse, cy, "Browse"))
                api("browse", "{}", [](const Json &answer) {
                    if (!answer["path"].text.empty())
                    {
                        g.iso.text = answer["path"].text;
                        g.iso.caret = g.iso.text.size();
                        g.iso.edited = true;
                    }
                });
            return u(49) + paragraph(F(fSmall), "Your own Burnout Dominator ISO, European version (SLES-54627). Keep it where it is: the game reads it at every launch.",
                                     cx, cy + u(49), cw, u(18.75f), kMute);
        }) + u(14);
        left += card(x, left, leftW, "Installation", [&](float cx, float cy, float cw) {
            int i = 0;
            const float column = (cw - u(18)) / 2;
            for (const auto &step : s["steps"].fields)
            {
                const float sx = cx + float(i % 2) * (column + u(18)), middle = cy + float(i / 2) * u(29) + u(10.5f);
                const Vector2 centre{std::round(sx + u(8)), std::round(middle)};
                if (g_draw && step.second.yes())
                {
                    DrawCircleV(centre, u(8), kDone);
                    const Vector2 a{centre.x - u(3.7f), centre.y + u(0.3f)}, b{centre.x - u(1.3f), centre.y + u(2.7f)}, c{centre.x + u(3.7f), centre.y - u(2.7f)};
                    DrawLineEx(a, b, u(1.7f), kText);
                    DrawLineEx(b, c, u(1.7f), kText);
                }
                else if (g_draw)
                    DrawRing(centre, u(8) - u(1.5f), u(8), 0, 360, 0, alpha(kRed, 0.7f));
                print(F(fText), step.first, sx + u(26), middle, step.second.yes() ? kText : kMute);
                ++i;
            }
            const float row = cy + float((i + 1) / 2) * u(29) - u(8) + u(16);
            const float install = buttonWidth("Generate the code and build the game");
            const std::vector<std::string_view> note = wrap(F(fNote), "30 to 90 minutes the first time, a few minutes after an update.", cw - install - u(6));
            const float height = std::max(u(39), u(19.5f) * float(note.size()));
            if (button(cx, row + (height - u(39)) / 2, "Generate the code and build the game", kPlain, !busy()))
                act("install", "{\"iso\": " + quote(g.iso.text) + "}", [](const Json &) {
                    g.iso.edited = false;
                    g.nextPage = kConsole;
                });
            for (size_t k = 0; k < note.size(); ++k)
                print(F(fNote), note[k], cx + install + u(6), row + (height - u(19.5f) * float(note.size())) / 2 + u(19.5f) * (float(k) + 0.5f), kMute);
            return row - cy + height;
        }) + u(14);
        left += card(x, left, leftW, "Save profile", [&](float cx, float cy, float cw) {
            std::vector<std::string> names;
            for (const Json &name : s["profiles"].items)
                names.push_back(name.text);
            const int chosen = dropdown("profile", {cx, cy, u(260), u(39)}, names, g.profile);
            if (chosen >= 0)
            {
                g.profile = names[size_t(chosen)];
                act("select", "{\"profile\": " + quote(g.profile) + "}");
            }
            float bx = cx + u(268);
            if (button(bx, cy, "New"))
            {
                g.newOpen = !g.newOpen;
                g_focus = g.newOpen ? &g.newName : g_focus;
            }
            bx += buttonWidth("New") + u(8);
            if (button(bx, cy, "Open its folder", kQuiet))
                api("open-profile", "{\"profile\": " + quote(g.profile) + "}");
            float h = u(39);
            if (g.newOpen)
            {
                h += u(10);
                const bool create = input(g.newName, {cx, cy + h, u(260), u(39)}, "Name of the new profile", u(11), 40) && g_focus == &g.newName;
                h += u(39) + u(8);
                const float browse = buttonWidth("Browse");
                input(g.newSaves, {cx, cy + h, cw - browse - u(8), u(39)}, "Optional: a folder of saves to copy into it", u(11));
                if (button(cx + cw - browse, cy + h, "Browse"))
                    api("browse-saves", "{}", [](const Json &answer) {
                        if (!answer["path"].text.empty())
                        {
                            g.newSaves.text = answer["path"].text;
                            g.newSaves.caret = g.newSaves.text.size();
                        }
                    });
                h += u(39) + u(10);
                h += rich(runs("To bring saves from elsewhere, give their folder: a save folder (it holds icon.sys), the folder above it, or a profile's "
                               "folder (another installation's `save\\Default`, or its `work\\local`). They are copied; the originals stay where they are."),
                          fSmall, fSmall, cx, cy + h, cw, u(18.75f), kMute, kMute) + u(10);
                if (button(cx, cy + h, "Create the profile", kPrimary) || create)
                {
                    const std::string saves = g.newSaves.text;
                    act("new-profile", "{\"name\": " + quote(g.newName.text) + ", \"saves\": " + quote(saves) + "}", [saves](const Json &) {
                        if (saves.find_first_not_of(' ') != std::string::npos)
                            toast("The saves were copied into the new profile.");
                        g.newName = Field{};
                        g.newSaves = Field{};
                        g.newOpen = false;
                        g_focus = nullptr;
                    });
                }
                h += u(39);
            }
            const std::string hint = "Each profile has its own memory card and save states" + (s["saves"].text.empty() ? std::string(".") : ", in " + s["saves"].text + ".");
            return h + u(10) + paragraph(F(fSmall), hint, cx, cy + h + u(10), cw, u(18.75f), kMute);
        }) + u(14);

        // The Play card stays in view while the page scrolls (sticky in the page).
        const auto launch = [&](float cx, float cy, float cw) {
            const bool ready = s["ready"].yes() && !busy();
            const Rectangle go = snap({cx, cy + u(5), cw, u(56)});
            const bool over = ready && hot(go);
            if (g_draw)
            {
                box(go, u(10), kNone, !ready ? kRaised : over ? kRedHover : kRed);
                print(F(fGo), "Play", go.x + (go.width - measure(F(fGo), "Play", std::round(u(0.34f)))) / 2, go.y + go.height / 2, ready ? WHITE : kOff, std::round(u(0.34f)));
            }
            if (over)
                g_cursor = MOUSE_CURSOR_POINTING_HAND;
            if (over && take())
                act("play", "{\"profile\": " + quote(g.profile) + "}", [](const Json &) { g.nextPage = kConsole; });
            const std::string state = busy() ? (!s["phase"].text.empty() ? s["phase"].text : s["running"].text + " running")
                                      : s["ready"].yes() ? "Ready."
                                                         : "Give the ISO, then generate the code and build the game.";
            const float text = std::max(u(21), paragraph(F(fNote), state, cx, cy + u(5) + u(56) + u(14), cw, u(19.5f), busy() ? kText : kMute, true));
            const float stopY = cy + u(5) + u(56) + u(14) + text + u(10);
            if (button(cx + (cw - buttonWidth("Stop")) / 2, stopY, "Stop", kQuiet, busy()))
                act("stop");
            return stopY + u(39) - cy;
        };
        const float launchX = narrow ? x : x + leftW + u(18), launchW = narrow ? w : u(340);
        float launchY = narrow ? left : y;
        if (!narrow)
        {
            const bool drawing = g_draw;
            g_draw = false;
            const float height = card(launchX, launchY, launchW, "", launch);
            g_draw = drawing;
            launchY = std::max(launchY, std::min(view.y, left - u(14) - height));
        }
        const float launchH = card(launchX, launchY, launchW, "", launch);
        return std::max(left, (narrow ? launchY : y) + launchH + u(14)) - top;
    }

    float logLines(float width, std::vector<std::pair<size_t, size_t>> *rows)
    {
        const size_t columns = static_cast<size_t>(std::max(1.0f, std::floor(width / advanceOf(F(fMono), 'M'))));
        size_t count = 0;
        for (size_t line = 0; line < g.log.size(); ++line)
        {
            size_t length = 0;
            for (size_t i = 0; i < g.log[line].size();)
                nextCodepoint(g.log[line], i), ++length;
            const size_t pieces = std::max<size_t>(1, (length + columns - 1) / columns);
            if (rows)
                for (size_t k = 0; k < pieces; ++k)
                    rows->emplace_back(line, k * columns);
            count += pieces;
        }
        return float(count);
    }

    void consolePage(float x, float y, float w, float bottom)
    {
        float bx = x;
        if (button(bx, y, "Stop", kPlain, busy()))
            act("stop");
        bx += buttonWidth("Stop") + u(8);
        if (button(bx, y, "Copy all", kQuiet))
        {
            std::string all;
            for (const std::string &line : g.log)
                all += line + "\n";
            SetClipboardText(all.c_str());
        }
        bx += buttonWidth("Copy all") + u(8);
        if (button(bx, y, "Clear", kQuiet))
            g.log.clear(), g.logOpen = false;
        bx += buttonWidth("Clear") + u(8);
        if (busy())
            print(F(fNote), g.state["running"].text + " running.", bx + u(6), y + u(19.5f), kMute);

        const Rectangle frame = snap({x, y + u(51), w, std::max(u(160), bottom - y - u(51))});
        box(frame, u(12), kLine, kLog);
        const Rectangle text{frame.x + u(17), frame.y + u(15), frame.width - u(34) - u(6), frame.height - u(30)};
        const float lineHeight = std::round(u(18));
        std::vector<std::pair<size_t, size_t>> rows;
        logLines(text.width, &rows);
        const float content = float(rows.size()) * lineHeight, most = std::max(0.0f, content - text.height);
        if (in.free && in.wheel != 0 && CheckCollisionPointRec(in.mouse, frame))
        {
            g.logScroll -= in.wheel * 3 * lineHeight;
            in.wheel = 0;
            g.logAtEnd = g.logScroll >= most - u(30);
        }
        if (g.logAtEnd)
            g.logScroll = most;
        pushClip({frame.x + 1, text.y, frame.width - 2, text.height});
        const size_t first = size_t(std::max(0.0f, g.logScroll) / lineHeight);
        const size_t columns = static_cast<size_t>(std::max(1.0f, std::floor(text.width / advanceOf(F(fMono), 'M'))));
        for (size_t r = first; r < rows.size() && float(r) * lineHeight - g.logScroll < text.height; ++r)
        {
            const std::string &line = g.log[rows[r].first];
            size_t from = 0, to;
            for (size_t skipped = 0; skipped < rows[r].second && from < line.size(); ++skipped)
                nextCodepoint(line, from);
            to = from;
            for (size_t taken = 0; taken < columns && to < line.size(); ++taken)
                nextCodepoint(line, to);
            print(F(fMono), std::string_view(line).substr(from, to - from), text.x, text.y + float(r) * lineHeight - g.logScroll + lineHeight / 2, kLogText);
        }
        popClip();
        const float before = g.logScroll;
        scrollbar("log", {frame.x, text.y, frame.width - u(3), text.height}, content, g.logScroll);
        if (g.logScroll != before)
            g.logAtEnd = g.logScroll >= most - u(30);
    }

    float controlWidth(const Json &row)
    {
        if (row["choice"].kind == Json::Array)
            return segmentsWidth(row["choice"]);
        if (row["range"].kind == Json::Array)
            return u(170 + 12 + 44);
        if (row["select"].kind == Json::Array)
        {
            std::vector<std::string> labels;
            for (const Json &option : row["select"].items)
                labels.push_back(option.text + row["unit"].text);
            return dropdownWidth(labels, u(10));
        }
        if (row["number"].kind == Json::Array)
            return row["and"].kind == Json::String ? u(84) * 2 + u(16) + measure(F(fText), "\xC3\x97") : u(84);
        if (row["text"].yes())
            return u(220);
        return u(38);
    }
    float controlHeight(const Json &row)
    {
        return row["range"].kind == Json::Array ? u(21) : row["choice"].kind == Json::Array || row["select"].kind == Json::Array ||
                                                           row["number"].kind == Json::Array || row["text"].yes()
                                                       ? u(35)
                                                       : u(22);
    }

    void numberBox(const std::string &key, float x, float y, const Json &limits)
    {
        Field &f = field(key);
        const std::string *current = value(key);
        if (g_focus != &f && !f.commit && current)
            f.text = *current;
        if (input(f, {x, y, u(84), u(35)}, "", u(9), 0, true))
        {
            const long long low = limits[0].integer(), high = limits[1].integer();
            f.text = std::to_string(std::clamp(std::strtoll(f.text.c_str(), nullptr, 10), low, high));
            f.caret = std::min(f.caret, f.text.size());
            saveSetting(key, f.text, false);
        }
    }

    void control(const Json &row, float x, float y)
    {
        const std::string &key = row["key"].text;
        const std::string current = value(key) ? *value(key) : "";
        if (row["choice"].kind == Json::Array)
        {
            const int clicked = segments(x, y, row["choice"], current);
            if (clicked >= 0)
                saveSetting(key, row["choice"][size_t(clicked)].text, false);
        }
        else if (row["range"].kind == Json::Array)
        {
            static double shown;
            double at = g_drag == key ? shown : std::strtod(current.c_str(), nullptr);
            if (slider(key, x, y, int(row["range"][0].integer()), int(row["range"][1].integer()), at))
                saveSetting(key, std::to_string(std::lround(at)), false);
            if (g_drag == key)
                shown = at;
            const std::string text = (g_drag == key ? std::to_string(std::lround(at)) : current) + row["unit"].text;
            print(F(fText), text, x + u(226) - measure(F(fText), text), y + u(10.5f), kMute);
        }
        else if (row["select"].kind == Json::Array)
        {
            std::vector<std::string> options, labels;
            bool listed = false;
            for (const Json &option : row["select"].items)
                listed = listed || option.text == current;
            if (!listed)
                options.push_back(current);
            for (const Json &option : row["select"].items)
                options.push_back(option.text);
            for (const std::string &option : options)
                labels.push_back(option + row["unit"].text);
            const int chosen = dropdown(key, {x, y, controlWidth(row), u(35)}, labels, current + row["unit"].text);
            if (chosen >= 0)
                saveSetting(key, options[size_t(chosen)], false);
        }
        else if (row["number"].kind == Json::Array)
        {
            numberBox(key, x, y, row["number"]);
            if (row["and"].kind == Json::String)
            {
                print(F(fText), "\xC3\x97", x + u(84) + u(8), y + u(17.5f), kMute);
                numberBox(row["and"].text, x + u(84) + u(16) + measure(F(fText), "\xC3\x97"), y, row["number"]);
            }
        }
        else if (row["text"].yes())
        {
            Field &f = field(key);
            if (g_focus != &f && !f.commit)
                f.text = current;
            if (input(f, {x, y, u(220), u(35)}, "", u(10)))
            {
                const size_t start = f.text.find_first_not_of(' '), end = f.text.find_last_not_of(' ');
                saveSetting(key, start == std::string::npos ? "" : f.text.substr(start, end - start + 1), false);
            }
        }
        else if (toggle(x, y, current == "true"))
            saveSetting(key, current == "true" ? "false" : "true", false);
    }

    // One line of the Settings page: what it is on the left, its control on the right.
    float settingRow(const Json &row, float x, float y, float w, bool first, bool last)
    {
        const float above = first ? u(2) : u(11), below = last ? 0 : u(11);
        if (!first && g_draw)
            DrawRectangleRec(snap({x, y, w, hairline()}), kLine);
        const float controlW = controlWidth(row), whatW = w - controlW - u(18);
        const std::vector<std::string_view> label = wrap(F(fText), row["label"].text, whatW);
        const std::vector<std::string_view> help = wrap(F(fSmall), row["help"].text, whatW);
        const float whatH = u(21) * float(label.size()) + (row["help"].text.empty() ? 0 : u(2) + u(17.5f) * float(help.size()));
        const float height = std::max(whatH, controlHeight(row));
        float ty = y + above + (height - whatH) / 2;
        for (const std::string_view line : label)
            print(F(fText), line, x, (ty += u(21)) - u(10.5f), kText);
        ty += u(2);
        for (size_t i = 0; !row["help"].text.empty() && i < help.size(); ++i)
            print(F(fSmall), help[i], x, (ty += u(17.5f)) - u(8.75f), kMute);
        control(row, x + w - controlW, y + above + (height - controlHeight(row)) / 2);
        return above + height + below;
    }

    void addInput(const std::string &key, int input)
    {
        std::vector<std::string> inputs = words(value(key) ? *value(key) : "");
        const std::string name = ps2x::inputName(input);
        if (std::find(inputs.begin(), inputs.end(), name) == inputs.end())
            inputs.push_back(name);
        saveSetting(key, joined(inputs, "NONE"), true);
    }

    // One PS2 button: its keys as chips (a click removes one) and "+", which takes the next key, mouse button or wheel move.
    float bindRow(const Json &button, float x, float y, float w, float cell = 0)
    {
        const std::string name = button[0].text, key = "key_" + name;
        const std::vector<std::string> inputs = words(value(key) ? *value(key) : "");
        const Face &face = F(fNote);
        struct Chip
        {
            std::string text;
            int index; // -1: the "+"
            float width;
        };
        std::vector<Chip> chips;
        for (size_t i = 0; i < inputs.size(); ++i)
        {
            const std::string text = keyLabel(inputs[i]);
            chips.push_back({text, int(i), std::round(measure(face, text) + u(7) + measure(face, "\xC3\x97") + u(20))});
        }
        const bool listening = g.listening == name;
        const std::string plus = listening ? "Press a key or a mouse button" : "+";
        if (inputs.size() < 3)
            chips.push_back({plus, -1, std::round(measure(face, plus) + u(20))});
        const float whatW = std::max(measure(F(fText), button[1].text), measure(F(fSmall), button[2].text));
        const float room = std::max(u(60), w - u(18) - std::min(whatW, w * 0.4f));
        // Lines of chips, filled from the right as flex-wrap with justify-content: end does.
        std::vector<std::vector<const Chip *>> lines(1);
        float used = 0;
        for (const Chip &chip : chips)
        {
            if (!lines.back().empty() && used + u(6) + chip.width > room)
                lines.emplace_back(), used = 0;
            used += (lines.back().empty() ? 0 : u(6)) + chip.width;
            lines.back().push_back(&chip);
        }
        const float chipsH = u(28) * float(lines.size()) + u(6) * float(lines.size() - 1);
        const float whatH = u(21) + (button[2].text.empty() ? 0 : u(19.5f));
        const float height = std::max({chipsH, whatH, cell - u(18)});
        if (g_draw)
            DrawRectangleRec(snap({x, y, w, hairline()}), kLine);
        print(F(fText), button[1].text, x, y + u(9) + (height - whatH) / 2 + u(10.5f), kText);
        if (!button[2].text.empty())
            print(F(fSmall), button[2].text, x, y + u(9) + (height - whatH) / 2 + u(21) + u(10), kMute);
        for (size_t l = 0; l < lines.size(); ++l)
        {
            float width = 0;
            for (const Chip *chip : lines[l])
                width += chip->width + (width > 0 ? u(6) : 0);
            float cx = x + w - width;
            for (const Chip *chip : lines[l])
            {
                const Rectangle r = snap({cx, y + u(9) + (height - chipsH) / 2 + float(l) * u(34), chip->width, u(28)});
                const bool over = hot(r);
                if (g_draw)
                {
                    if (chip->index < 0 && listening)
                        box(r, u(7), kRed, alpha(kRed, 0.14f));
                    else
                        box(r, u(7), over ? alpha(kRed, 0.6f) : chip->index < 0 ? kNone : kLine, kRaised);
                    if (chip->index < 0 && !listening) // the dashed border of "+"
                        for (float d = r.x + u(7); d < r.x + r.width - u(7); d += u(6))
                        {
                            const Color dash = over ? alpha(kRed, 0.6f) : kLine;
                            DrawRectangleRec(snap({d, r.y, u(3), hairline()}), dash);
                            DrawRectangleRec(snap({d, r.y + r.height - hairline(), u(3), hairline()}), dash);
                        }
                    if (chip->index < 0 && !listening)
                    {
                        const Color dash = over ? alpha(kRed, 0.6f) : kLine;
                        for (float d = r.y + u(7); d < r.y + r.height - u(7); d += u(6))
                        {
                            DrawRectangleRec(snap({r.x, d, hairline(), u(3)}), dash);
                            DrawRectangleRec(snap({r.x + r.width - hairline(), d, hairline(), u(3)}), dash);
                        }
                    }
                    const float tx = print(face, chip->text, r.x + u(10), r.y + r.height / 2, chip->index < 0 && !listening && !over ? kMute : kText);
                    if (chip->index >= 0)
                        print(face, "\xC3\x97", tx + u(7), r.y + r.height / 2, kMute);
                }
                if (chip->index < 0 && g_draw && listening)
                    g.listenBox = r;
                if (over)
                    g_cursor = MOUSE_CURSOR_POINTING_HAND;
                if (over && take())
                {
                    if (chip->index >= 0)
                    {
                        std::vector<std::string> left = inputs;
                        left.erase(left.begin() + chip->index);
                        saveSetting(key, joined(left, "NONE"), true);
                    }
                    else
                        g.listening = listening ? "" : name;
                }
                cx += chip->width + u(6);
            }
        }
        return u(9) + height + u(9);
    }

    float settingsPage(float x, float y, float w)
    {
        const float top = y;
        if (g.values.empty())
            return card(x, y, w, "", [&](float cx, float cy, float cw) {
                return paragraph(F(fText), "The game writes its settings file at its first launch; the settings appear here afterwards.", cx, cy - u(1), cw, u(21), kText);
            }) + u(14);
        y += paragraph(F(fSmall), "Changes are saved at once and apply the next time the game starts. F1 in the game changes the same settings.", x, y, w, u(18.75f), kMute) + u(14);
        std::vector<std::string> shown;
        const Json &table = g.state["table"];
        const bool narrow = float(GetScreenWidth()) <= u(940);
        const float columnW = narrow ? w : (w - u(14)) / 2;
        float bottom = y, cy = y;
        for (size_t c = 0; c < table["columns"].items.size(); ++c)
        {
            const float cx = narrow ? x : x + float(c) * (columnW + u(14));
            cy = narrow ? cy : y;
            for (const Json &group : table["columns"][c].items)
            {
                std::vector<const Json *> rows;
                for (const Json &row : group["rows"].items)
                    if (value(row["key"].text) && (row["and"].kind != Json::String || value(row["and"].text)))
                    {
                        rows.push_back(&row);
                        shown.push_back(row["key"].text);
                        shown.push_back(row["and"].text);
                    }
                if (!rows.empty())
                    cy += card(cx, cy, columnW, group["title"].text, [&](float rx, float ry, float rw) {
                        float h = 0;
                        for (size_t i = 0; i < rows.size(); ++i)
                            h += settingRow(*rows[i], rx, ry + h, rw, i == 0, i + 1 == rows.size());
                        return h;
                    }) + u(14);
            }
            bottom = std::max(bottom, cy);
        }
        y = bottom;
        std::vector<const Json *> buttons;
        for (const Json &button : table["buttons"].items)
            if (value("key_" + button[0].text))
            {
                buttons.push_back(&button);
                shown.push_back("key_" + button[0].text);
            }
        if (!buttons.empty())
            y += card(x, y, w, "Keyboard and mouse", [&](float cx, float cy2, float cw) {
                float h = paragraph(F(fSmall), "Up to three keys, mouse buttons or wheel moves for each PS2 button. Press + and then the key to add; click a key to remove it.",
                                    cx, cy2, cw, u(18.75f), kMute) + u(6);
                const bool one = float(GetScreenWidth()) <= u(940);
                const float bw = one ? cw : (cw - u(36)) / 2;
                const size_t across = one ? 1 : 2;
                for (size_t i = 0; i < buttons.size(); i += across)
                {
                    float rowH = 0;
                    const bool drawing = g_draw;
                    g_draw = false;
                    for (size_t k = 0; k < across && i + k < buttons.size(); ++k)
                        rowH = std::max(rowH, bindRow(*buttons[i + k], cx, 0, bw));
                    g_draw = drawing;
                    for (size_t k = 0; k < across && i + k < buttons.size(); ++k)
                        bindRow(*buttons[i + k], cx + float(k) * (bw + u(36)), cy2 + h, bw, rowH);
                    h += rowH;
                }
                return h - u(9);
            }) + u(14);
        std::vector<Json> other;
        for (const auto &item : g.values)
            if (std::find(shown.begin(), shown.end(), item.first) == shown.end())
            {
                Json row;
                row.kind = Json::Object;
                Json key, yes;
                key.kind = Json::String;
                key.text = item.first;
                yes.kind = Json::True;
                row.fields = {{"key", key}, {"label", key}, {"text", yes}};
                other.push_back(row);
            }
        if (!other.empty())
            y += card(x, y, w, "Other", [&](float cx, float cy2, float cw) {
                float h = 0;
                for (size_t i = 0; i < other.size(); ++i)
                    h += settingRow(other[i], cx, cy2 + h, cw, i == 0, i + 1 == other.size());
                return h;
            }) + u(14);
        return y - top;
    }

    float updatesPage(float x, float y, float w)
    {
        const float top = y;
        const Json &updates = g.state["updates"];
        float bx = x;
        if (button(bx, y, "Check for updates"))
        {
            g.note = "Checking...";
            api("check-updates");
        }
        bx += buttonWidth("Check for updates") + u(8);
        if (button(bx, y, "Update now", kPrimary, updates["behind"].yes() && !busy()))
            act("update", "{}", [](const Json &) { g.nextPage = kConsole; });
        bx += buttonWidth("Update now") + u(8);
        print(F(fNote), g.note.empty() ? updates["note"].text : g.note, bx + u(6), y + u(19.5f), kMute);
        y += u(39) + u(12);
        if (updates["behind"].yes())
            y += paragraph(F(fSmall), "What the update brings", x, y, w, u(18.75f), kMute) + u(12);
        for (const Json &entry : (updates["behind"].yes() ? updates["new"] : g.state["changelog"]).items)
            y += card(x, y, w, entry[0].text, [&](float cx, float cy, float cw) {
                float h = 0;
                const std::string &body = entry[1].text;
                for (size_t start = 0; start < body.size();)
                {
                    const size_t end = std::min(body.find('\n', start), body.size());
                    std::string_view line = std::string_view(body).substr(start, end - start);
                    start = end + 1;
                    if (line.find_first_not_of(" \t") == std::string_view::npos)
                        continue;
                    line.remove_prefix(line.find_first_not_of(" \t"));
                    if (line.rfind("- ", 0) == 0 || line == "-")
                        line.remove_prefix(std::min<size_t>(2, line.size()));
                    while (!line.empty() && line.front() == ' ')
                        line.remove_prefix(1);
                    if (g_draw)
                        DrawCircleV({std::round(cx + u(7)), std::round(cy + h + u(10.5f))}, u(2.5f), kLogText);
                    h += rich(runs(line), fText, fBold, cx + u(18), cy + h, cw - u(18), u(21), kLogText, kText) + u(6);
                }
                return h;
            }, kLine, kText, true) + u(14);
        return y - top;
    }

    // --- The frame ---------------------------------------------------------------------

    void frame()
    {
        const float W = float(GetScreenWidth()), H = float(GetScreenHeight());
        in.mouse = GetMousePosition();
        std::copy(std::begin(g_pressed), std::end(g_pressed), in.pressed);
        std::fill(std::begin(g_pressed), std::end(g_pressed), false);
        in.click = in.pressed[MOUSE_BUTTON_LEFT];
        in.wheel = std::exchange(g_wheel, 0.0f);
        in.keys.clear();
        in.chars.clear();
        for (int key; (key = GetKeyPressed()) != 0;)
            in.keys.push_back(key);
        for (int c; (c = GetCharPressed()) != 0;)
            in.chars.push_back(c);
        in.free = true;
        g_cursor = MOUSE_CURSOR_DEFAULT;

        if (g_popup.open)
        {
            in.free = !CheckCollisionPointRec(in.mouse, g_popup.box);
            if (in.click)
            {
                const int index = int((in.mouse.y - g_popup.box.y - u(4)) / u(30));
                if (!in.free && index >= 0 && index < int(g_popup.labels.size()))
                    g_popup.chosen = index;
                g_popup.open = false;
                in.click = false;
            }
            if (keyHit(KEY_ESCAPE))
                g_popup.open = false;
        }
        if (!g.listening.empty()) // the next key, mouse button or wheel move goes to a PS2 button
        {
            int taken = 0;
            if (!IsWindowFocused())
                g.listening.clear();
            else if (!in.keys.empty())
                taken = in.keys.front();
            else if (in.wheel != 0)
                taken = in.wheel > 0 ? ps2x::kMouseWheelUp : ps2x::kMouseWheelDown;
            else
                for (int b = 0; b < 8 && !taken; ++b)
                    if (in.pressed[b] && !(b == MOUSE_BUTTON_LEFT && CheckCollisionPointRec(in.mouse, g.listenBox)))
                        taken = ps2x::kMouseInput + b;
            if (taken)
            {
                addInput("key_" + g.listening, taken);
                g.listening.clear();
                in.keys.clear();
                in.chars.clear();
                in.wheel = 0;
                in.click = false;
            }
        }
        if (in.click && g_focus && !CheckCollisionPointRec(in.mouse, g_focus->box))
        {
            g_focus->commit = true; // a click elsewhere ends the edit, as a page's change event does
            g_focus = nullptr;
        }

        BeginDrawing();
        ClearBackground(kBg);
        const Json &s = g.state;

        // Header
        if (g.icon.id)
            DrawTexturePro(g.icon, {0, 0, float(g.icon.width), float(g.icon.height)}, snap({u(28), u(18), u(32), u(32)}), {0, 0}, 0, WHITE);
        const float nameX = print(F(fName), "Burnout Dominator", u(72), u(34), kText);
        print(F(fNameLight), "Reversed", nameX + advanceOf(F(fNameLight), ' '), u(34), kMute);
        if (!s["version"].text.empty())
        {
            const std::string version = "v" + s["version"].text;
            const float vw = std::round(measure(F(fTiny), version) + u(22));
            box({W - u(28) - vw, u(22), vw, u(24)}, u(12), kLine, kNone);
            print(F(fTiny), version, W - u(28) - vw + u(11), u(34), kMute);
        }

        // Tabs
        static const char *const tabs[] = {"Play", "Console", "Settings", "Updates"};
        float tx = u(28);
        for (int i = 0; i < 4; ++i)
        {
            const float tw = std::round(measure(F(fText), tabs[i]) + u(28) + (i == kUpdates && s["updates"]["behind"].yes() ? u(13) : 0));
            const Rectangle r = snap({tx, u(64), tw, u(41)});
            const bool over = hot(r);
            print(F(fText), tabs[i], r.x + u(14), r.y + u(18.5f), g.page == i || over ? kText : kMute);
            if (i == kUpdates && s["updates"]["behind"].yes())
                DrawCircleV({r.x + r.width - u(17), r.y + u(18.5f)}, u(3), kRed);
            if (g.page == i)
                box({r.x + u(14), r.y + r.height - u(2), r.width - u(28), u(2)}, u(1), kNone, kRed);
            if (hit(r))
            {
                g.nextPage = Page(i);
                if (i == kConsole)
                    g.logAtEnd = true;
            }
            tx += tw + u(4);
        }

        // The line under the tabs moves while something runs.
        const Rectangle progress = snap({0, u(105), W, u(2)});
        DrawRectangleRec(progress, kLine);
        const bool animate = busy() && IsWindowFocused() && !IsWindowMinimized();
        if (busy())
        {
            float t = float(std::fmod(GetTime(), 1.4) / 1.4);
            t = t < 0.5f ? 2 * t * t : 1 - 2 * (1 - t) * (1 - t);
            const float start = -0.3f * W + t * 1.32f * W;
            DrawRectangleRec(overlap(snap({start, progress.y, 0.3f * W, progress.height}), progress), kRed);
        }

        // The page
        const float footer = std::round(u(33));
        const Rectangle view{0, progress.y + progress.height, W, H - footer - progress.y - progress.height};
        const float pageW = std::min(W - u(56), u(1180)), pageX = std::round((W - pageW) / 2);
        if (!g.have)
        {
            const std::string text = g.stopped >= 0 ? "The launcher stopped (exit code " + std::to_string(g.stopped) + "). Close this window and start it again."
                                                    : "Starting the launcher...";
            paragraph(F(fText), text, pageX, view.y + view.height / 2 - u(10.5f), pageW, u(21), kMute, true);
        }
        else if (g.page == kConsole)
            consolePage(pageX, view.y + u(22), pageW, view.y + view.height - u(26));
        else
        {
            float &scroll = g.scroll[g.page];
            if (in.free && in.wheel != 0 && CheckCollisionPointRec(in.mouse, view))
                scroll -= in.wheel * u(60);
            scroll = std::clamp(scroll, 0.0f, std::max(0.0f, g.content[g.page] - view.height));
            pushClip(view);
            const float top = view.y + u(22) - std::round(scroll);
            const float height = g.page == kPlay ? playPage(pageX, top, pageW, view) : g.page == kSettings ? settingsPage(pageX, top, pageW) : updatesPage(pageX, top, pageW);
            g.content[g.page] = u(22) + height + u(26);
            popClip();
            scrollbar("page", view, g.content[g.page], scroll);
        }

        // Footer
        DrawRectangleRec(snap({0, H - footer, W, hairline()}), kLine);
        if (g.have)
        {
            pushClip({0, H - footer, W * 0.65f, footer});
            print(F(fTiny), "Work folder: " + s["work"].text, u(28), H - footer / 2, kMute);
            popClip();
        }
        std::string status = busy() ? s["running"].text + " running" : "";
        if (g.have && !g.connected)
            status = g.stopped >= 0 ? "The launcher stopped (exit code " + std::to_string(g.stopped) + "): close this window and start it again."
                                    : "Reconnecting to the launcher...";
        print(F(fTiny), status, W - u(28) - measure(F(fTiny), status), H - footer / 2, g.have && !g.connected ? kRed : kMute);

        // Toast
        if (!g.toast.empty() && GetTime() < g.toastUntil)
        {
            const std::vector<std::string_view> lines = wrap(F(fText), g.toast, W * 0.8f - u(35));
            float tw = 0;
            for (const std::string_view line : lines)
                tw = std::max(tw, measure(F(fText), line));
            const Rectangle r = snap({(W - tw - u(35)) / 2, H - u(44) - u(22) - u(21) * float(lines.size()), tw + u(35), u(22) + u(21) * float(lines.size())});
            box(r, u(10), kLine, kRaised);
            DrawRectangleRec(snap({r.x, r.y + u(5), u(3), r.height - u(10)}), kRed);
            paragraph(F(fText), g.toast, r.x + u(19), r.y + u(11), tw + 1, u(21), kText);
            g_link->wakeAt(Clock::now() + std::chrono::milliseconds(int((g.toastUntil - GetTime()) * 1000) + 10));
        }
        drawPopup();
        EndScissorMode();
        static int cursor = MOUSE_CURSOR_DEFAULT;
        if (g_cursor != cursor) // raylib makes a new system cursor at each call
            SetMouseCursor(cursor = g_cursor);
        if (animate)
            g_link->wakeAt(Clock::now() + std::chrono::milliseconds(33));
        g.page = g.nextPage; // after the frame: a field on the page it leaves commits its edit first
        EndDrawing();
    }

    [[maybe_unused]] bool selfCheck()
    {
        const Json j = parseJson(R"({"a": [1, "x\u00e9\ud83d\ude00", true], "b": {"c": null}, "n": 1730000000123456789, "s": "\"q\"\n"})");
        return j["a"][1].text == "x\xC3\xA9\xF0\x9F\x98\x80" && j["a"][2].yes() && j["b"]["c"].kind == Json::Null &&
               j["n"].integer() == 1730000000123456789LL && j["s"].text == "\"q\"\n" && quote("a\"\\\n") == "\"a\\\"\\\\\\u000a\"" &&
               keyLabel("LEFT_SHIFT") == "Left Shift" && keyLabel("KP_1") == "Keypad 1" && keyLabel("MOUSE_WHEEL_UP") == "Wheel Up" &&
               keyLabel("F1") == "F1" && runs("a **b** `c d`").size() == 4 && words("A  NONE B") == std::vector<std::string>{"A", "B"};
    }

    std::string hashName(const std::string &text)
    {
        unsigned long long hash = 14695981039346656037ull; // FNV-1a
        for (const unsigned char c : text)
            hash = (hash ^ c) * 1099511628211ull;
        char out[17];
        std::snprintf(out, sizeof(out), "%016llx", hash);
        return out;
    }

    bool readSession(const fs::path &file, Link &link)
    {
        std::ifstream stream(file, std::ios::binary);
        const Json session = parseJson(std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()));
        link.port = int(session["port"].integer());
        link.token = session["token"].text;
        return link.port > 0 && !link.token.empty();
    }
}

int launcherMain(int argc, char **argv)
{
    assert(selfCheck());
    std::string python, script;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], "--python") == 0)
            python = argv[++i];
        else if (std::strcmp(argv[i], "--script") == 0)
            script = argv[++i];
    }
    if (python.empty() || script.empty())
    {
        std::fprintf(stderr, "Usage: %s --python <python> --script <tools/launcher.py>\n", argc > 0 ? argv[0] : "bdr_launcher");
        return 1;
    }

    // One window per folder. The launcher it talks to writes its address in `session`, and removes it when it ends.
    const std::string name = "bdr-launcher-window-" + hashName(script);
    std::error_code error;
    const fs::path session = fs::temp_directory_path(error) / (name + ".json");
    g_link = new Link;
    Link &link = *g_link;
    if (!sys::claim(name))
    {
        sys::allowForeground();
        for (int tries = 0; tries < 20; ++tries) // it may still be starting
        {
            if (readSession(session, link) && post(link, "front", "{}", 5000)["error"].text.empty())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        return 0;
    }
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIDDEN | FLAG_MSAA_4X_HINT);
    InitWindow(1280, 720, "Burnout Dominator Reversed");
    if (!IsWindowReady()) // no OpenGL 3.3: the caller opens the page instead
    {
        sys::release();
        return 2;
    }
    // A launcher still running for this folder (the game outlives the window) is taken over, not started twice.
    if (!readSession(session, link) || !post(link, "state", "{}", 5000)["error"].text.empty())
    {
        link.port = sys::freePort();
        link.token = sys::randomToken();
        const std::u8string file = session.u8string();
        g.backend = sys::spawn({python, script, "--port", std::to_string(link.port), "--token", link.token, "--session", std::string(file.begin(), file.end())});
        if (!g.backend)
        {
            CloseWindow();
            sys::release();
            return 1;
        }
    }
    g.program = sys::programPath();
    if (!g.program.empty())
        g.programTime = fs::last_write_time(pathOf(g.program), error);
    SetExitKey(KEY_NULL); // Escape is a key to bind, not a way out
#ifndef __APPLE__
    g_s = std::max(1.0f, GetWindowScaleDPI().x);
#endif
    // 1280 x 720 page pixels, as the page's window had, centred on the screen and inside it.
    const int monitor = GetCurrentMonitor();
    const int width = std::min(int(1280 * g_s), GetMonitorWidth(monitor) * 9 / 10), height = std::min(int(720 * g_s), GetMonitorHeight(monitor) * 9 / 10);
    SetWindowSize(width, height);
    SetWindowPosition(int(GetMonitorPosition(monitor).x) + (GetMonitorWidth(monitor) - width) / 2, int(GetMonitorPosition(monitor).y) + (GetMonitorHeight(monitor) - height) / 2);
    SetWindowMinSize(int(720 * g_s), int(480 * g_s));
    sys::darkTitleBar(GetWindowHandle());
    {
        const auto load = [&](const char *name) {
            std::ifstream png(pathOf(script).parent_path() / name, std::ios::binary);
            const std::vector<unsigned char> data((std::istreambuf_iterator<char>(png)), std::istreambuf_iterator<char>());
            return LoadImageFromMemory(".png", data.data(), int(data.size()));
        };
#ifndef _WIN32
        // The window's icon is the grey B, the page shows the red one. Windows takes BurnoutDominator.exe's own icon.
        Image grey = load("launcher_icon.png");
        if (grey.data)
            SetWindowIcon(grey);
        UnloadImage(grey);
#endif
        Image icon = load("launcher.png");
        if (icon.data)
        {
            g.icon = LoadTextureFromImage(icon);
            GenTextureMipmaps(&g.icon);
            SetTextureFilter(g.icon, TEXTURE_FILTER_TRILINEAR);
            UnloadImage(icon);
        }
    }
    loadFaces();
    GLFWwindow *window = glfwGetCurrentContext();
    g_raylibButton = glfwSetMouseButtonCallback(window, onButton);
    g_raylibScroll = glfwSetScrollCallback(window, onScroll);
    {
        const std::lock_guard<std::mutex> hold(link.lock);
        link.open = true;
    }
    std::thread(runCommands, &link).detach();
    std::thread(runAlarm, &link).detach();
    std::thread(runEvents, &link).detach();

    frame(); // drawn before it shows
    ClearWindowState(FLAG_WINDOW_HIDDEN);
    EnableEventWaiting();
    while (!WindowShouldClose() && !g.restart)
    {
        std::vector<std::function<void()>> work;
        {
            const std::lock_guard<std::mutex> hold(link.lock);
            work.swap(link.inbox);
        }
        for (const auto &task : work)
            task();
        if (g.backend && !g.connected && g.stopped < 0)
        {
            g.stopped = sys::exitCode(g.backend);
            if (g.stopped == 0) // it handed over to a new launcher after an update
                g.backend = 0, g.stopped = -1;
            if (g.stopped == 2) // it could not start and said why
                break;
        }
        const float scale = std::max(1.0f, GetWindowScaleDPI().x);
#ifndef __APPLE__
        if (scale != g_s) // moved to a screen with another scale
        {
            g_s = scale;
            loadFaces();
        }
#else
        (void)scale;
#endif
        frame();
    }
    {
        const std::lock_guard<std::mutex> hold(link.lock);
        link.open = false;
    }
    CloseWindow();
    sys::release(); // before an update's new program starts and claims this folder
    return g.restart ? 3 : 0;
}

#ifndef _WIN32
int main(int argc, char **argv)
{
    return launcherMain(argc, argv);
}
#endif
