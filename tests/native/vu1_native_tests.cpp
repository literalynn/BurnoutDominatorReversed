// Native VU programs against the microcode (docs/NATIVE_RENDERER.md). BDR_VU_NATIVE_CAPTURES
// names a directory of program starts recorded with PS2_VU_CAPTURE_DIR (vu1-*.code or
// vu0-*.code, .data and .state); each start of a translated program runs on VU1Interpreter in
// shadow mode, once stepped by the interpreter and once by the compiled routines, and the
// translation's GIF packets, VU memory, registers and cycles must be the microcode's. Without captures, or in a
// build without translations, there is nothing to compare. BDR_VU_NATIVE_BENCH=N then times N
// rounds of the starts with the compiled routines and with the translations. An MSVC build
// compiles no routine for these starts (compile_vu_programs.py --native-programs): the
// interpreter runs them in both passes.

#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_vu1_native.h"
#include "runtime/gs/gs_frontend.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
std::vector<uint8_t> readFile(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}

struct Start
{
    uint32_t unit; // 0: VU0, 1: VU1
    std::vector<uint8_t> code, data;
    VU1State state;
};

// What the game passes for each unit: its memories and the cycles a start may take.
struct Unit
{
    uint8_t *code, *data;
    uint32_t codeSize, dataSize, cycles;
};
} // namespace

int main()
{
    const char *directory = std::getenv("BDR_VU_NATIVE_CAPTURES");
    if (!directory || !*directory)
    {
        std::puts("vu1_native_tests: BDR_VU_NATIVE_CAPTURES is not set, nothing to compare");
        return 0;
    }
    PS2Memory memory;
    if (!memory.initialize())
    {
        std::puts("vu1_native_tests: memory does not initialize");
        return 1;
    }
    GS gs;
    gs.init(memory.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &memory.gs());

    const Unit units[2] = {{memory.getVU0Code(), memory.getVU0Data(), PS2_VU0_CODE_SIZE, PS2_VU0_DATA_SIZE, 1u << 24},
                           {memory.getVU1Code(), memory.getVU1Data(), PS2_VU1_CODE_SIZE, PS2_VU1_DATA_SIZE, 65536u}};
    // The code of a start in its unit's memory, as a new microcode generation.
    const auto upload = [&](const Start &start)
    {
        std::memcpy(units[start.unit].code, start.code.data(), start.code.size());
        if (start.unit)
            memory.markVU1CodeModified();
        else
            memory.markVU0CodeModified();
    };
    const auto find = [&](const Start &start)
    {
        return ps2_vu_native::find(units[start.unit].code, units[start.unit].codeSize, start.state.pc,
                                   start.unit ? memory.getVU1CodeGeneration() : memory.getVU0CodeGeneration(), start.unit);
    };

    std::vector<Start> starts;
    for (const auto &entry : std::filesystem::directory_iterator(directory))
    {
        const std::filesystem::path path = entry.path();
        const std::string name = path.filename().string();
        if (path.extension() != ".code" || (name.rfind("vu1-", 0) != 0 && name.rfind("vu0-", 0) != 0))
            continue;
        std::filesystem::path stem = path;
        stem.replace_extension();
        Start start{name[2] == '1' ? 1u : 0u, readFile(path), readFile(stem.string() + ".data"), {}};
        const std::vector<uint8_t> stateBytes = readFile(stem.string() + ".state");
        if (start.code.size() != units[start.unit].codeSize || start.data.size() != units[start.unit].dataSize ||
            stateBytes.size() != sizeof(VU1State))
            continue;
        std::memcpy(&start.state, stateBytes.data(), sizeof(start.state));
        upload(start);
        if (find(start))
            starts.push_back(std::move(start));
    }

    const auto runAll = [&](bool compiled, unsigned rounds)
    {
        VU1Interpreter vu0(VU1Interpreter::Unit::VU0), vu1(VU1Interpreter::Unit::VU1);
        vu0.setCompiledExecutionEnabled(compiled);
        vu1.setCompiledExecutionEnabled(compiled);
        for (unsigned round = 0; round < rounds; ++round)
            for (const Start &start : starts)
            {
                const Unit &unit = units[start.unit];
                VU1Interpreter &vu = start.unit ? vu1 : vu0;
                upload(start);
                std::memcpy(unit.data, start.data.data(), start.data.size());
                vu.state() = start.state;
                vu.execute(unit.code, unit.codeSize, unit.data, unit.dataSize, gs, &memory, start.state.pc, start.state.top,
                           start.state.itop, unit.cycles);
            }
    };

    ps2_vu_native::setMode(ps2_vu_native::Mode::Shadow);
    runAll(false, 1);
    runAll(true, 1);
    std::fputs(ps2_vu_native::report().c_str(), stdout);
    uint64_t compared = 0, identical = 0;
    ps2_vu_native::totals(compared, identical);
    std::printf("vu1_native_tests: %zu starts of %s run twice, %llu compared, %llu identical\n", starts.size(), directory,
                static_cast<unsigned long long>(compared), static_cast<unsigned long long>(identical));

    if (const char *bench = std::getenv("BDR_VU_NATIVE_BENCH"); bench && !starts.empty())
    {
        const unsigned rounds = static_cast<unsigned>(std::strtoul(bench, nullptr, 10));
        for (const ps2_vu_native::Mode mode : {ps2_vu_native::Mode::Off, ps2_vu_native::Mode::On})
        {
            ps2_vu_native::setMode(mode);
            const auto begin = std::chrono::steady_clock::now();
            runAll(true, rounds);
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
            std::printf("vu1_native_tests: %s %.2f us a start\n", mode == ps2_vu_native::Mode::On ? "native" : "compiled",
                        seconds * 1e6 / (static_cast<double>(rounds) * static_cast<double>(starts.size())));
        }
        // The translations alone: packets built but not sent to the GS.
        std::vector<uint8_t> data(PS2_VU1_DATA_SIZE);
        const auto begin = std::chrono::steady_clock::now();
        for (unsigned round = 0; round < rounds; ++round)
            for (const Start &start : starts)
            {
                upload(start);
                const ps2_vu_native::Program *program = find(start);
                std::memcpy(data.data(), start.data.data(), start.data.size());
                VU1State state = start.state;
                ps2_vu_native::Context context{data.data(), &state, [](void *, const uint8_t *, uint32_t) {}, nullptr, 0};
                ps2_vu_native::run(*program, context);
            }
        std::printf("vu1_native_tests: native without the GS %.2f us a start\n",
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count() * 1e6 /
                        (static_cast<double>(rounds) * static_cast<double>(starts.size())));
    }
    return compared == identical ? 0 : 1;
}
