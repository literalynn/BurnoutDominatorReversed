#include "runtime/ps2_vu1_native_ops.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>

#if PS2_VU_NATIVE_AVAILABLE && defined(_MSC_VER)
#include <intrin.h>
#endif

namespace ps2_vu_native
{
#ifdef PS2X_VU_NATIVE_PROGRAMS
// vu_native_programs.cpp, which the build translates from the game's code images.
extern const Program kGeneratedPrograms[];
extern const uint32_t kGeneratedProgramCount;
#endif

namespace
{
std::vector<Program> &programs()
{
#ifdef PS2X_VU_NATIVE_PROGRAMS
    static std::vector<Program> list(kGeneratedPrograms, kGeneratedPrograms + kGeneratedProgramCount);
#else
    static std::vector<Program> list;
#endif
    return list;
}

// The ops need SSE4.1 and FMA3 (ps2_vu1_native_ops.h).
bool cpuHasOps()
{
#if !PS2_VU_NATIVE_AVAILABLE
    return false;
#elif defined(_MSC_VER)
    int info[4];
    __cpuid(info, 1);
    constexpr int needed = 1 << 19 | 1 << 12 | 1 << 27; // SSE4.1, FMA, OSXSAVE
    return (info[2] & needed) == needed && (_xgetbv(0) & 6u) == 6u; // the OS saves XMM and YMM registers
#else
    __builtin_cpu_init(); // static initializers may run before libgcc's
    return __builtin_cpu_supports("sse4.1") && __builtin_cpu_supports("fma");
#endif
}
const bool g_cpuHasOps = cpuHasOps();

Mode initialMode()
{
    const char *setting = std::getenv("PS2_VU_NATIVE");
    if (setting && std::strcmp(setting, "0") == 0)
        return Mode::Off;
    if (PS2_VU_NATIVE_AVAILABLE && !g_cpuHasOps)
    {
        std::fputs("[vu1 native] the CPU lacks SSE4.1 or FMA3: the microcode runs the VU1 programs\n", stderr);
        return Mode::Off;
    }
    return setting && std::strcmp(setting, "shadow") == 0 ? Mode::Shadow : Mode::On;
}
Mode g_mode = initialMode();

struct Totals
{
    uint64_t compared = 0, matched = 0, cancelled = 0, declined = 0;
};
std::mutex g_mutex;
std::map<std::string, Totals> g_totals;

// A shadow run in progress: the translation's results, waiting for the microcode's.
struct Shadow
{
    const Program *program = nullptr;
    std::vector<uint8_t> data;
    VU1State state{};
    std::vector<uint8_t> nativeKicks, microcodeKicks;
    std::vector<uint32_t> nativeSizes, microcodeSizes;
    std::vector<uint8_t> inputData;
    VU1State inputState{};
    uint64_t cycles = 0;
};
Shadow g_shadow;

void collect(void *user, const uint8_t *packet, uint32_t bytes)
{
    auto *shadow = static_cast<Shadow *>(user);
    shadow->nativeKicks.insert(shadow->nativeKicks.end(), packet, packet + bytes);
    shadow->nativeSizes.push_back(bytes);
}

// The first difference between the translation's state and the microcode's, if any.
std::string stateDifference(const VU1State &native, const VU1State &microcode)
{
    char text[160];
    for (int reg = 1; reg < 32; ++reg)
        if (std::memcmp(native.vf[reg], microcode.vf[reg], sizeof(native.vf[reg])) != 0)
        {
            uint32_t a[4], b[4];
            std::memcpy(a, native.vf[reg], sizeof(a));
            std::memcpy(b, microcode.vf[reg], sizeof(b));
            std::snprintf(text, sizeof(text), "vf%d %08x %08x %08x %08x, microcode %08x %08x %08x %08x", reg, a[0], a[1], a[2], a[3],
                          b[0], b[1], b[2], b[3]);
            return text;
        }
    for (int reg = 1; reg < 16; ++reg)
        if (static_cast<int16_t>(native.vi[reg]) != static_cast<int16_t>(microcode.vi[reg]))
        {
            std::snprintf(text, sizeof(text), "vi%d %d, microcode %d", reg, native.vi[reg], microcode.vi[reg]);
            return text;
        }
    if (std::memcmp(native.acc, microcode.acc, sizeof(native.acc)) != 0)
        return "acc";
    if (std::memcmp(&native.q, &microcode.q, sizeof(float)) != 0)
        return "q";
    if (std::memcmp(&native.i, &microcode.i, sizeof(float)) != 0)
        return "i";
    if (native.mac != microcode.mac)
    {
        std::snprintf(text, sizeof(text), "mac %04x, microcode %04x", native.mac, microcode.mac);
        return text;
    }
    if (native.clip != microcode.clip)
    {
        std::snprintf(text, sizeof(text), "clip %06x, microcode %06x", native.clip, microcode.clip);
        return text;
    }
    if (native.pc != microcode.pc)
    {
        std::snprintf(text, sizeof(text), "pc %04x, microcode %04x", native.pc, microcode.pc);
        return text;
    }
    return {};
}

void dumpStart(const Shadow &shadow, uint64_t index)
{
    static const char *directory = std::getenv("PS2_VU_NATIVE_DUMP");
    if (!directory || !*directory)
        return;
    const std::string stem = std::string(directory) + "/" + shadow.program->name + "-mismatch" + std::to_string(index);
    if (FILE *file = std::fopen((stem + ".data").c_str(), "wb"))
    {
        std::fwrite(shadow.inputData.data(), 1, shadow.inputData.size(), file);
        std::fclose(file);
    }
    if (FILE *file = std::fopen((stem + ".state").c_str(), "wb"))
    {
        std::fwrite(&shadow.inputState, 1, sizeof(shadow.inputState), file);
        std::fclose(file);
    }
}
} // namespace

void add(const Program &program) { programs().push_back(program); }

Mode mode() { return g_mode; }
void setMode(Mode value) { g_mode = value; }

const Program *find(const uint8_t *code, uint32_t codeSize, uint32_t pc, uint64_t generation)
{
    if (!g_cpuHasOps)
        return nullptr;
    // Called at every VU1 MSCAL, from the thread that runs VU1; the microcode changes many times
    // a frame, so each program compares its bytes at most once per generation.
    struct Seen
    {
        uint64_t generation = ~0ull;
        bool matches = false;
    };
    static std::vector<Seen> seen;
    const std::vector<Program> &list = programs();
    seen.resize(list.size());
    for (size_t index = 0; index < list.size(); ++index)
    {
        const Program &program = list[index];
        if (program.pc != pc)
            continue;
        Seen &entry = seen[index];
        if (entry.generation != generation)
            entry = {generation, program.end <= codeSize && program.begin < program.end &&
                                     std::memcmp(code + program.begin, program.code, program.end - program.begin) == 0};
        if (entry.matches)
            return &program;
    }
    return nullptr;
}

bool run(const Program &program, Context &context)
{
#if PS2_VU_NATIVE_AVAILABLE
    constexpr unsigned mask = _MM_ROUND_MASK | _MM_DENORMALS_ZERO_MASK | _MM_FLUSH_ZERO_MASK;
    constexpr unsigned vu = _MM_ROUND_TOWARD_ZERO | _MM_DENORMALS_ZERO_ON | _MM_FLUSH_ZERO_ON;
    const unsigned previous = _mm_getcsr();
    _mm_setcsr((previous & ~mask) | vu);
    const bool ran = program.run(context);
    _mm_setcsr((_mm_getcsr() & ~mask) | (previous & mask));
    return ran;
#else
    (void)program;
    (void)context;
    return false;
#endif
}

std::string report()
{
    const std::lock_guard lock(g_mutex);
    std::string text;
    for (const auto &[name, totals] : g_totals)
    {
        char line[240];
        std::snprintf(line, sizeof(line),
                      "[vu1 native] %s: %llu starts compared, %llu identical, %llu not compared (unfinished), %llu left to the microcode\n",
                      name.c_str(), static_cast<unsigned long long>(totals.compared), static_cast<unsigned long long>(totals.matched),
                      static_cast<unsigned long long>(totals.cancelled), static_cast<unsigned long long>(totals.declined));
        text += line;
    }
    return text;
}

void totals(uint64_t &compared, uint64_t &identical)
{
    const std::lock_guard lock(g_mutex);
    compared = identical = 0;
    for (const auto &[name, counts] : g_totals)
    {
        compared += counts.compared;
        identical += counts.matched;
    }
}

void shadowStart(const Program &program, const uint8_t *data, const VU1State &state)
{
    Shadow &shadow = g_shadow;
    shadow.program = &program;
    shadow.inputData.assign(data, data + 16384);
    shadow.inputState = state;
    shadow.data = shadow.inputData;
    shadow.state = state;
    shadow.nativeKicks.clear();
    shadow.nativeSizes.clear();
    shadow.microcodeKicks.clear();
    shadow.microcodeSizes.clear();
    Context context{shadow.data.data(), &shadow.state, &collect, &shadow, 0};
    if (!run(program, context))
    {
        const std::lock_guard lock(g_mutex);
        ++g_totals[program.name].declined;
        shadow.program = nullptr;
        return;
    }
    shadow.cycles = context.cycles;
}

void shadowCancel()
{
    if (!g_shadow.program)
        return;
    {
        const std::lock_guard lock(g_mutex);
        ++g_totals[g_shadow.program->name].cancelled;
    }
    g_shadow.program = nullptr;
}

bool shadowActive() { return g_shadow.program != nullptr; }

void shadowKick(const uint8_t *packet, uint32_t bytes)
{
    Shadow &shadow = g_shadow;
    shadow.microcodeKicks.insert(shadow.microcodeKicks.end(), packet, packet + bytes);
    shadow.microcodeSizes.push_back(bytes);
}

void shadowFinish(const uint8_t *data, const VU1State &state, uint64_t cycles)
{
    Shadow &shadow = g_shadow;
    if (!shadow.program)
        return;
    std::string difference;
    if (shadow.nativeSizes != shadow.microcodeSizes || shadow.nativeKicks != shadow.microcodeKicks)
    {
        size_t offset = 0;
        while (offset < std::min(shadow.nativeKicks.size(), shadow.microcodeKicks.size()) &&
               shadow.nativeKicks[offset] == shadow.microcodeKicks[offset])
            ++offset;
        difference = "GIF packets differ at byte " + std::to_string(offset) + " (" + std::to_string(shadow.nativeSizes.size()) +
                     " packets, microcode " + std::to_string(shadow.microcodeSizes.size()) + ")";
    }
    else if (std::memcmp(shadow.data.data(), data, shadow.data.size()) != 0)
    {
        size_t offset = 0;
        while (shadow.data[offset] == data[offset])
            ++offset;
        const size_t qword = offset / 16;
        uint32_t a[4], b[4];
        std::memcpy(a, shadow.data.data() + qword * 16, sizeof(a));
        std::memcpy(b, data + qword * 16, sizeof(b));
        char text[160];
        std::snprintf(text, sizeof(text), "VU1 data memory differs at qword %zu: %08x %08x %08x %08x, microcode %08x %08x %08x %08x",
                      qword, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
        difference = text;
    }
    else
        difference = stateDifference(shadow.state, state);
    if (difference.empty() && shadow.cycles != cycles)
        difference = "cycles " + std::to_string(shadow.cycles) + ", microcode " + std::to_string(cycles);

    uint64_t compared;
    {
        const std::lock_guard lock(g_mutex);
        Totals &totals = g_totals[shadow.program->name];
        compared = ++totals.compared;
        if (difference.empty())
            ++totals.matched;
    }
    if (!difference.empty())
    {
        static unsigned printed = 0;
        if (printed < 20u)
        {
            ++printed;
            std::fprintf(stderr, "[vu1 native] %s start %llu: %s\n", shadow.program->name,
                         static_cast<unsigned long long>(compared), difference.c_str());
            dumpStart(shadow, compared);
        }
    }
    shadow.program = nullptr;
}

GifPacket gifPacket(const uint8_t *data, uint32_t qwordAddress, uint8_t *out, uint32_t capacity)
{
    // progressXgkick: tags and their data in order, the packet dropped where a tag's data
    // would not fit or the buffer is full.
    uint32_t bytes = 0;
    uint32_t source = (qwordAddress & 0x3FFu) * 16u;
    for (;;)
    {
        if (bytes > capacity - 16u)
            return {0u, bytes / 16u + 1u};
        uint64_t tag;
        std::memcpy(&tag, data + source, sizeof(tag));
        std::memcpy(out + bytes, data + source, 16);
        const uint32_t tagOffset = bytes;
        bytes += 16u;
        source = (source + 16u) & 0x3FFFu;
        const uint64_t nloop = tag & 0x7FFFu;
        const uint32_t format = static_cast<uint32_t>((tag >> 58) & 3u);
        uint64_t nreg = (tag >> 60) & 0xFu;
        if (nreg == 0u)
            nreg = 16u;
        uint64_t tagBytes = 16u;
        if (format == 0u)
            tagBytes += nloop * nreg * 16u;
        else if (format == 1u)
            tagBytes += ((nloop * nreg + 1u) & ~1ull) * 8u;
        else
            tagBytes += nloop * 16u;
        if (tagBytes > capacity - tagOffset)
            return {0u, bytes / 16u};
        for (uint64_t copied = 16u; copied < tagBytes; copied += 16u)
        {
            std::memcpy(out + bytes, data + source, 16);
            bytes += 16u;
            source = (source + 16u) & 0x3FFFu;
        }
        if ((tag >> 15) & 1u)
            return {bytes, bytes / 16u};
    }
}

#if PS2_VU_NATIVE_AVAILABLE
namespace ops
{
namespace
{
float normalized(float value)
{
    uint32_t raw;
    std::memcpy(&raw, &value, sizeof(raw));
    const uint32_t exponent = (raw >> 23) & 0xFFu;
    if (exponent == 0u)
        raw &= 0x80000000u;
    else if (exponent == 0xFFu)
        raw = (raw & 0x80000000u) | 0x7F7FFFFFu;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

// normalizeFmacExactResult: the lane's value and its MAC flags (1 zero, 2 sign, 4 underflow,
// 8 overflow) from the exact value.
uint32_t exactLane(float &value, double exact)
{
    const bool negative = std::signbit(exact);
    const double magnitude = std::fabs(exact);
    uint32_t flags = negative ? 2u : 0u;
    uint32_t raw = negative ? 0x80000000u : 0u;
    if (magnitude == 0.0)
    {
        flags |= 1u;
        std::memcpy(&value, &raw, sizeof(value));
    }
    else if (magnitude > static_cast<double>(FLT_MAX))
    {
        flags |= 8u;
        raw |= 0x7F7FFFFFu;
        std::memcpy(&value, &raw, sizeof(value));
    }
    else if (magnitude < static_cast<double>(FLT_MIN))
    {
        flags |= 5u;
        std::memcpy(&value, &raw, sizeof(value));
    }
    return flags;
}

template <bool Subtract>
double exactProductSum(float acc, float a, float b)
{
    const double product = static_cast<double>(normalized(a)) * static_cast<double>(normalized(b));
    return Subtract ? static_cast<double>(normalized(acc)) - product : static_cast<double>(normalized(acc)) + product;
}

uint32_t pack(const uint32_t laneFlags[4], int dest)
{
    uint32_t mac = 0;
    for (int lane = 0; lane < 4; ++lane)
    {
        if ((dest & (8 >> lane)) == 0)
            continue;
        const uint32_t bit = 3u - static_cast<uint32_t>(lane);
        const uint32_t flags = laneFlags[lane];
        mac |= ((flags & 1u) << bit) | (((flags >> 1) & 1u) << (bit + 4u)) | (((flags >> 2) & 1u) << (bit + 8u)) |
               (((flags >> 3) & 1u) << (bit + 12u));
    }
    return mac;
}
} // namespace

template <Arith Op>
uint32_t exactFlags(V acc, V a, V b, int dest)
{
    alignas(16) float accLanes[4], aLanes[4], bLanes[4];
    _mm_store_ps(accLanes, acc);
    _mm_store_ps(aLanes, a);
    _mm_store_ps(bLanes, b);
    uint32_t laneFlags[4];
    for (int lane = 0; lane < 4; ++lane)
    {
        const double left = normalized(aLanes[lane]), right = normalized(bLanes[lane]);
        double exact;
        if constexpr (Op == Arith::Add)
            exact = left + right;
        else if constexpr (Op == Arith::Sub)
            exact = left - right;
        else if constexpr (Op == Arith::Mul)
            exact = left * right;
        else
            exact = exactProductSum<Op == Arith::MSub>(accLanes[lane], aLanes[lane], bLanes[lane]);
        float ignored;
        laneFlags[lane] = exactLane(ignored, exact);
    }
    return pack(laneFlags, dest);
}
template uint32_t exactFlags<Arith::Add>(V, V, V, int);
template uint32_t exactFlags<Arith::Sub>(V, V, V, int);
template uint32_t exactFlags<Arith::Mul>(V, V, V, int);
template uint32_t exactFlags<Arith::MAdd>(V, V, V, int);
template uint32_t exactFlags<Arith::MSub>(V, V, V, int);

void unexpectedJump(Context &, uint32_t address)
{
    static bool printed = false;
    if (!printed)
    {
        printed = true;
        std::fprintf(stderr, "[vu1 native] JR to 0x%04x, which no BAL linked\n", address);
    }
}

uint64_t xgkick(Context &context, int32_t qwordAddress, uint64_t cycle)
{
    static thread_local std::vector<uint8_t> packet(64 * 1024); // VU1Interpreter::XgkickPipeline::kBufferSize
    const GifPacket sent = gifPacket(context.data, static_cast<uint32_t>(static_cast<uint16_t>(qwordAddress)), packet.data(),
                                     static_cast<uint32_t>(packet.size()));
    if (sent.bytes != 0u)
        context.kick(context.user, packet.data(), sent.bytes);
    // PATH1 reads its first qword in the cycle after the kick, then one every two.
    return cycle + 2u * sent.qwords - 1u;
}
} // namespace ops
#endif
} // namespace ps2_vu_native
