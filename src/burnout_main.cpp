// Project runner: original disc sectors, local saves and bounded diagnostics.
#include "burnout_profile.h"
#include "ps2_runtime.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
struct Options {
    std::filesystem::path elf, iso, disc, saves, dumpFrames;
    std::vector<uint32_t> traceCalls, traceWatch;
    bool headless = false;
    int seconds = 0;          // 0: unbounded (windowed only)
    int statusMs = 0;         // 0: no periodic status line
    int dumpEveryMs = 1000;
};

constexpr const char* kUsage =
    "Usage: burnout_dominator <ELF> [--iso <ISO>] --disc <directory> --save <directory>\n"
    "       [--headless] [--seconds N] [--status-ms N] [--dump-frames <directory> [--dump-every-ms N]]\n"
    "       [--trace-calls 0xADDR,...] [--trace-watch 0xADDR,...]";

int parseInt(const std::string& key, const char* text, int low, int high) {
    std::size_t consumed = 0;
    const int value = std::stoi(text, &consumed);
    if (consumed != std::string(text).size() || value < low || value > high)
        throw std::runtime_error(key + " must be " + std::to_string(low) + ".." + std::to_string(high));
    return value;
}

Options parse(int argc, char** argv) {
    if (argc < 2) throw std::runtime_error(kUsage);
    Options options;
    options.elf = std::filesystem::absolute(argv[1]);
    options.disc = options.elf.parent_path();
    options.saves = std::filesystem::current_path() / "local" / "saves";
    for (int i = 2; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--headless") { options.headless = true; continue; }
        if (++i == argc) throw std::runtime_error("Missing value for " + key);
        if (key == "--iso") options.iso = std::filesystem::absolute(argv[i]);
        else if (key == "--disc") options.disc = std::filesystem::absolute(argv[i]);
        else if (key == "--save") options.saves = std::filesystem::absolute(argv[i]);
        else if (key == "--seconds") options.seconds = parseInt(key, argv[i], 1, 86400);
        else if (key == "--status-ms") options.statusMs = parseInt(key, argv[i], 10, 600000);
        else if (key == "--dump-frames") options.dumpFrames = std::filesystem::absolute(argv[i]);
        else if (key == "--dump-every-ms") options.dumpEveryMs = parseInt(key, argv[i], 16, 600000);
        else if (key == "--trace-calls" || key == "--trace-watch") {
            auto& target = key == "--trace-calls" ? options.traceCalls : options.traceWatch;
            std::stringstream list(argv[i]);
            for (std::string item; std::getline(list, item, ',');)
                target.push_back(static_cast<uint32_t>(std::stoul(item, nullptr, 16)));
        }
        else throw std::runtime_error("Unknown option " + key + "\n" + kUsage);
    }
    if (options.headless && options.seconds == 0) options.seconds = 10;
    if (!std::filesystem::is_regular_file(options.elf))
        throw std::runtime_error("Original guest ELF must exist. Use tools/project.py to verify its identity.");
    // Without --iso, the runtime serves disc sectors from a virtual image of
    // --disc: a diagnostic mode for machines that only hold part of the disc.
    if (!options.iso.empty() && !std::filesystem::is_regular_file(options.iso))
        throw std::runtime_error("ISO not found: " + options.iso.string());
    if (!std::filesystem::is_directory(options.disc))
        throw std::runtime_error("Extracted disc directory does not exist");
    return options;
}

// --trace-calls: wrap guest functions in the dispatch table to log each call
// (caller, a0-a3) and each normal return (v0). Observation only.
std::unordered_map<uint32_t, PS2Runtime::RecompiledFunction> g_traced;
std::vector<uint32_t> g_watched;  // --trace-watch: 32-bit words printed with each trace line
std::mutex g_traceMutex;

void printWatched(const uint8_t* rdram) {
    for (const uint32_t address : g_watched) {
        uint32_t value = 0;
        std::memcpy(&value, rdram + (address & (PS2_RAM_SIZE - 1) & ~3u), sizeof(value));
        std::cout << " [0x" << address << "]=0x" << value;
    }
}

void tracedCall(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t pc = ctx->pc;
    const uint32_t ra = getRegU32(ctx, 31);
    {
        std::lock_guard<std::mutex> lock(g_traceMutex);
        std::cout << std::hex << "[trace] call 0x" << pc << " ra=0x" << ra << " a0=0x" << getRegU32(ctx, 4)
                  << " a1=0x" << getRegU32(ctx, 5) << " a2=0x" << getRegU32(ctx, 6) << " a3=0x"
                  << getRegU32(ctx, 7);
        printWatched(rdram);
        std::cout << std::dec << std::endl;
    }
    g_traced.at(pc)(rdram, ctx, runtime);
    if (ctx->pc == ra) {
        std::lock_guard<std::mutex> lock(g_traceMutex);
        std::cout << std::hex << "[trace] return 0x" << pc << " v0=0x" << getRegU32(ctx, 2);
        printWatched(rdram);
        std::cout << std::dec << std::endl;
    }
}

void traceCalls(PS2Runtime& runtime, const std::vector<uint32_t>& addresses) {
    for (const uint32_t address : addresses) {
        const uint32_t slot = (address - g_ps2RecompiledFunctionTableBase) >> 2;
        if (address < g_ps2RecompiledFunctionTableBase || slot >= g_ps2RecompiledFunctionTableSlotCount ||
            !g_ps2RecompiledFunctionTable[slot] || g_ps2RecompiledFunctionTable[slot] == &tracedCall) {
            std::cerr << "[trace] no function at 0x" << std::hex << address << std::dec << '\n';
            continue;
        }
        g_traced[address] = g_ps2RecompiledFunctionTable[slot];
        runtime.replaceFunction(address, &tracedCall);
    }
}

int exitCode(const PS2Runtime& runtime, const PS2Runtime::RunResult& result) {
    std::cout << "[BDR] result: deadline=" << result.deadlineReached
              << " finished=" << result.gameThreadFinished
              << " failed=" << result.gameThreadFailed
              << " missing_function=" << runtime.hasReportedMissingFunction()
              << " frames_dumped=" << result.framesDumped
              << " pc=0x" << std::hex << runtime.m_debugPc.load() << std::dec << '\n';
    if (result.gameThreadFailed) return 1;
    if (runtime.hasReportedMissingFunction()) return 3;
    // A time-limited stop is a bounded observation, never a successful game test.
    if (result.deadlineReached) return 124;
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse(argc, argv);
        PS2Runtime runtime;
        runtime.setMissingFunctionPolicy(PS2Runtime::MissingFunctionPolicy::Stop);
        if (options.headless) {
            if (!runtime.memory().initialize() || !runtime.syncCoreSubsystems())
                throw std::runtime_error("Memory/core initialization failed");
        } else if (!runtime.initialize("Burnout Dominator - recompilation experiment"))
            throw std::runtime_error("Graphics/audio initialization failed");
        if (!runtime.loadELF(options.elf.string()))
            throw std::runtime_error("ELF loading failed");
        g_watched = options.traceWatch;
        traceCalls(runtime, options.traceCalls);
        // loadELF configures roots; apply our explicit mount afterwards.
        auto paths = PS2Runtime::getIoPaths();
        paths.cdImage = options.iso;
        paths.cdRoot = options.disc;
        paths.hostRoot = options.disc;
        paths.mcRoot = options.saves;
        std::filesystem::create_directories(options.saves);
        PS2Runtime::setIoPaths(paths);
        if (options.iso.empty())
            std::cout << "[BDR] No ISO: disc sectors come from a virtual image of " << options.disc
                      << " (sector numbers differ from the original disc)\n";
        else
            std::cout << "[BDR] Original ISO: " << options.iso << '\n';

        bdr_profile::Sampler profile; // BDR_PROFILE=1
        PS2Runtime::RunOptions run;
        run.timeLimit = std::chrono::seconds(options.seconds);
        run.statusInterval = std::chrono::milliseconds(options.statusMs);
        run.frameDumpDirectory = options.dumpFrames;
        run.frameDumpInterval = std::chrono::milliseconds(options.dumpEveryMs);
        const auto result = options.headless ? runtime.runHeadless(run) : runtime.run(run);
        return exitCode(runtime, result);
    } catch (const std::exception& error) {
        std::cerr << "[BDR] " << error.what() << '\n';
        return 1;
    }
}
