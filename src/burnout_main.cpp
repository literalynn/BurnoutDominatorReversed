// Project runner: original disc sectors, local saves and bounded diagnostics.
#include "ps2_runtime.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
struct Options {
    std::filesystem::path elf, iso, disc, saves, dumpFrames;
    bool headless = false;
    int seconds = 0;          // 0: unbounded (windowed only)
    int statusMs = 0;         // 0: no periodic status line
    int dumpEveryMs = 1000;
};

constexpr const char* kUsage =
    "Usage: burnout_dominator <ELF> --iso <ISO> --disc <directory> --save <directory>\n"
    "       [--headless] [--seconds N] [--status-ms N] [--dump-frames <directory> [--dump-every-ms N]]";

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
        else throw std::runtime_error("Unknown option " + key + "\n" + kUsage);
    }
    if (options.headless && options.seconds == 0) options.seconds = 10;
    if (!std::filesystem::is_regular_file(options.elf) || !std::filesystem::is_regular_file(options.iso))
        throw std::runtime_error("Original guest ELF and ISO must exist. Use tools/project.py to verify their identity.");
    if (!std::filesystem::is_directory(options.disc))
        throw std::runtime_error("Extracted disc directory does not exist");
    return options;
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
        // loadELF configures roots; apply our explicit mount afterwards.
        auto paths = PS2Runtime::getIoPaths();
        paths.cdImage = options.iso;
        paths.cdRoot = options.disc;
        paths.hostRoot = options.disc;
        paths.mcRoot = options.saves;
        std::filesystem::create_directories(options.saves);
        PS2Runtime::setIoPaths(paths);
        std::cout << "[BDR] Original ISO: " << options.iso << '\n';

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
