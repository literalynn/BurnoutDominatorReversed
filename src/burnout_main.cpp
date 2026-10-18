// Project runner: original disc sectors, local saves and bounded diagnostics.
#include "burnout_profile.h"
#include "ps2_runtime.h"
#include "runtime/ee_idle_loops.h"
#include "runtime/ee_scheduler.h"
#include "runtime/run_marks.h"
#include "runtime/host_settings.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
struct Options {
    std::filesystem::path elf, iso, disc, saves, dumpFrames;
    std::filesystem::path state, loadState;  // save states: F5/F7 slot, state restored at start
    std::filesystem::path settings;          // settings.ini (display, quality, gamepads)
    uint64_t saveStateAt = 0;                // VSync at which to save to the slot (headless use)
    std::vector<uint32_t> traceCalls, traceWatch;
    bool headless = false;
    int seconds = 0;          // 0: unbounded (windowed only)
    int statusMs = 0;         // 0: no periodic status line
    int dumpEveryMs = 1000;
};

constexpr const char* kUsage =
    "Usage: burnout_dominator <ELF> [--iso <ISO>] --disc <directory> --save <directory>\n"
    "       [--headless] [--seconds N] [--status-ms N] [--dump-frames <directory> [--dump-every-ms N]]\n"
    "       [--trace-calls 0xADDR,...] [--trace-watch 0xADDR,...]\n"
    "       [--state <file>] [--load-state <file>] [--save-state-at VSYNC] [--settings <file>]\n"
    "Save states: F5 saves to --state (default <save>/../states/quick.bdrstate), F7 loads it.";

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
        else if (key == "--state") options.state = std::filesystem::absolute(argv[i]);
        else if (key == "--load-state") options.loadState = std::filesystem::absolute(argv[i]);
        else if (key == "--settings") options.settings = std::filesystem::absolute(argv[i]);
        else if (key == "--save-state-at") options.saveStateAt = static_cast<uint64_t>(parseInt(key, argv[i], 1, 100000000));
        else if (key == "--trace-calls" || key == "--trace-watch") {
            auto& target = key == "--trace-calls" ? options.traceCalls : options.traceWatch;
            std::stringstream list(argv[i]);
            for (std::string item; std::getline(list, item, ',');)
                target.push_back(static_cast<uint32_t>(std::stoul(item, nullptr, 16)));
        }
        else throw std::runtime_error("Unknown option " + key + "\n" + kUsage);
    }
    if (options.headless && options.seconds == 0) options.seconds = 10;
    if (options.state.empty()) options.state = options.saves.parent_path() / "states" / "quick.bdrstate";
    if (options.settings.empty()) options.settings = options.saves.parent_path() / "settings.ini";
    if (!options.loadState.empty() && !std::filesystem::is_regular_file(options.loadState))
        throw std::runtime_error("Save state not found: " + options.loadState.string());
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
// Logs jumps of the race camera, in windowed play (with F9 marks they locate what a player
// saw) or with BDR_CAMERA_WATCH=1; BDR_CAMERA_WATCH=0 turns it off. 0x43EE10 holds the race camera's position
// and 0x43ED30 the player car's (followed over consecutive race frames). Sampled every 2 ms
// from the host; a camera that moves much further than the car between two samples is
// reported, and so is a car that jumps (a reset after a crash).
class CameraWatch {
public:
    CameraWatch(PS2Runtime& runtime, bool windowed) : m_runtime(runtime) {
        const char* value = std::getenv("BDR_CAMERA_WATCH");
        if (value ? (*value && *value != '0') : windowed) {
            s_instance = this;
            ps2x::setMarkListener([](uint64_t) { if (s_instance) s_instance->printHistory(); });
            m_thread = std::thread([this] { run(); });
        }
    }
    ~CameraWatch() {
        m_stop = true;
        if (m_thread.joinable())
            m_thread.join();
        ps2x::setMarkListener(nullptr);
        s_instance = nullptr;
    }

private:
    struct Vec { float x, y, z; };
    // One game frame as the watch saw it: when the camera or the car moved.
    struct Sample {
        float ms;
        uint32_t vsync;
        Vec camera, car;
    };

    Vec read(uint32_t address) const {
        Vec v{};
        std::memcpy(&v, m_runtime.memory().getRDRAM() + address, sizeof(v));
        return v;
    }
    static float distance(const Vec& a, const Vec& b) {
        return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
    }

    // F9: the last three seconds of camera and car positions, a line per game frame.
    void printHistory() {
        std::lock_guard<std::mutex> lock(m_mutex);
        const size_t count = std::min(m_count, m_history.size());
        std::printf("[mark] camera history, %zu frames (ms since the first, vsync, camera, car, camera to car):\n", count);
        for (size_t i = 0; i < count; ++i) {
            const Sample& sample = m_history[(m_count - count + i) % m_history.size()];
            std::printf("[mark-cam] %.1f %u cam %.2f %.2f %.2f car %.2f %.2f %.2f dist %.2f\n",
                        sample.ms - m_history[(m_count - count) % m_history.size()].ms, sample.vsync, sample.camera.x,
                        sample.camera.y, sample.camera.z, sample.car.x, sample.car.y, sample.car.z,
                        distance(sample.camera, sample.car));
        }
        std::fflush(stdout);
    }

    void run() {
        const auto start = std::chrono::steady_clock::now();
        Vec camera = read(0x43EE10u), car = read(0x43ED30u);
        while (!m_stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            const Vec nextCamera = read(0x43EE10u), nextCar = read(0x43ED30u);
            const float cameraStep = distance(camera, nextCamera), carStep = distance(car, nextCar);
            if (cameraStep == 0.0f && carStep == 0.0f)
                continue;
            const auto now = std::chrono::steady_clock::now();
            const auto vsync = static_cast<unsigned long long>(m_runtime.eeScheduler().currentVSyncTick());
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_history[m_count++ % m_history.size()] =
                    Sample{std::chrono::duration<float, std::milli>(now - start).count(), uint32_t(vsync), nextCamera, nextCar};
            }
            if (std::isfinite(cameraStep) && cameraStep > 4.0f && cameraStep > 3.0f * carStep + 2.0f)
                std::printf("[camjump] vsync=%llu camera (%.1f %.1f %.1f) -> (%.1f %.1f %.1f) moved %.1f, car %.1f\n", vsync,
                            camera.x, camera.y, camera.z, nextCamera.x, nextCamera.y, nextCamera.z, cameraStep, carStep);
            else if (std::isfinite(carStep) && carStep > 15.0f)
                std::printf("[carjump] vsync=%llu car moved %.1f, camera %.1f\n", vsync, carStep, cameraStep);
            camera = nextCamera;
            car = nextCar;
        }
    }

    static inline CameraWatch* s_instance = nullptr;
    PS2Runtime& m_runtime;
    std::atomic<bool> m_stop{false};
    std::mutex m_mutex;
    std::array<Sample, 150> m_history{};
    size_t m_count = 0;
    std::thread m_thread;
};
} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse(argc, argv);
        // The window, the GS backend and the pad read these settings when they start.
        std::string settingsErrors;
        if (!ps2x::loadHostSettings(options.settings, settingsErrors) && !options.headless)
            ps2x::writeHostSettings(options.settings);
        std::cerr << settingsErrors;
        PS2Runtime runtime;
        runtime.setMissingFunctionPolicy(PS2Runtime::MissingFunctionPolicy::Stop);
        const char* gsThread = std::getenv("BDR_GS_THREAD"); // BDR_GS_THREAD=0 overrides settings.ini
        runtime.setGsThread(gsThread ? *gsThread != '0' : ps2x::hostSettings().gsThread);
        if (options.headless) {
            if (!runtime.memory().initialize() || !runtime.syncCoreSubsystems())
                throw std::runtime_error("Memory/core initialization failed");
        } else if (!runtime.initialize("Burnout Dominator - recompilation experiment"))
            throw std::runtime_error("Graphics/audio initialization failed");
        if (!runtime.loadELF(options.elf.string()))
            throw std::runtime_error("ELF loading failed");
        g_watched = options.traceWatch;
        traceCalls(runtime, options.traceCalls);
        // Frame-end wait of 0x336240 (0x336318-0x336354): it rereads the DMA list words
        // gp-0x7930/-0x792C and flag gp-0x7938, which interrupt handlers change, and stores
        // nothing itself. Its per-pass callback gp-0x7DD8 is unset or, in races, 0x1B49F0,
        // which only lowers a minimum (0x1B4A10: if (a1 < obj->min) obj->min = a1) with the
        // same a1 every pass. Either way, passes after the first only wait for an event.
        ps2x::registerIdleLoop(0x336318u, [](const uint8_t* rdram, const R5900Context& ctx) {
            uint32_t callback = 0;
            const uint32_t address = (getRegU32(&ctx, 28) - 0x7DD8u) & (PS2_RAM_SIZE - 1) & ~3u;
            std::memcpy(&callback, rdram + address, sizeof(callback));
            return callback == 0u || callback == 0x1B49F0u;
        });
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
        CameraWatch cameraWatch(runtime, !options.headless);
        PS2Runtime::RunOptions run;
        run.timeLimit = std::chrono::seconds(options.seconds);
        run.statusInterval = std::chrono::milliseconds(options.statusMs);
        run.frameDumpDirectory = options.dumpFrames;
        run.frameDumpInterval = std::chrono::milliseconds(options.dumpEveryMs);
        run.stateFile = options.state;
        run.loadStateFile = options.loadState;
        run.saveStateAtVsync = options.saveStateAt;
        const auto result = options.headless ? runtime.runHeadless(run) : runtime.run(run);
        return exitCode(runtime, result);
    } catch (const std::exception& error) {
        std::cerr << "[BDR] " << error.what() << '\n';
        return 1;
    }
}
