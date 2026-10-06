// Project runner: original disc sectors, local saves and bounded diagnostics.
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
struct Options {
    std::filesystem::path elf, iso, disc, saves;
    bool headless = false;
    int seconds = 10;
};

Options parse(int argc, char** argv) {
    if (argc < 2)
        throw std::runtime_error("Usage: burnout_dominator <ELF> --iso <ISO> --disc <directory> --save <directory> [--headless --seconds 10]");
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
        else if (key == "--seconds") {
            std::size_t consumed = 0;
            options.seconds = std::stoi(argv[i], &consumed);
            if (consumed != std::string(argv[i]).size() || options.seconds < 1 || options.seconds > 300)
                throw std::runtime_error("--seconds must be 1..300");
        } else throw std::runtime_error("Unknown option " + key);
    }
    if (!std::filesystem::is_regular_file(options.elf) || !std::filesystem::is_regular_file(options.iso))
        throw std::runtime_error("Original guest ELF and ISO must exist. Use tools/project.py to verify their identity.");
    if (!std::filesystem::is_directory(options.disc))
        throw std::runtime_error("Extracted disc directory does not exist");
    return options;
}

int diagnose(PS2Runtime& runtime, int seconds) {
    auto* ram = runtime.memory().getRDRAM();
    runtime.initializeEeKernelState(ram);
    runtime.cpu().r[4] = _mm_setzero_si128();
    runtime.cpu().r[5] = _mm_setzero_si128();
    runtime.cpu().r[29] = _mm_set_epi64x(0, PS2_RAM_SIZE - 0x10u);
    const auto initialPc = runtime.cpu().pc;
    runtime.eeScheduler().reset(ram, runtime.cpu());
    std::mutex mutex;
    std::condition_variable condition;
    bool finished = false;
    bool deadlineReached = false;
    std::thread watchdog([&] {
        std::unique_lock lock(mutex);
        if (!condition.wait_for(lock, std::chrono::seconds(seconds), [&] { return finished; })) {
            deadlineReached = true;
            runtime.requestStop();
        }
    });
    std::exception_ptr failure;
    try { runtime.eeScheduler().run(); }
    catch (...) { failure = std::current_exception(); }
    {
        std::lock_guard lock(mutex);
        finished = true;
    }
    condition.notify_all();
    watchdog.join();
    std::cout << "[BDR smoke] entry=0x" << std::hex << initialPc
              << " pc=0x" << runtime.cpu().pc << std::dec
              << " deadline=" << deadlineReached
              << " missing_function=" << runtime.hasReportedMissingFunction() << '\n';
    if (failure) std::rethrow_exception(failure);
    if (runtime.hasReportedMissingFunction()) return 3;
    // A watchdog stop is a bounded observation, never a successful game test.
    if (deadlineReached) return 124;
    std::cout << "[BDR smoke] Scheduler returned; gameplay remains unverified.\n";
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
        } else if (!runtime.initialize("Burnout Dominator — recompilation experiment"))
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
        if (options.headless) return diagnose(runtime, options.seconds);
        runtime.run();
        return runtime.hasReportedMissingFunction() ? 3 : 0;
    } catch (const std::exception& error) {
        std::cerr << "[BDR] " << error.what() << '\n';
        return 1;
    }
}
