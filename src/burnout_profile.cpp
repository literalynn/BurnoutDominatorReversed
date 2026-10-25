#include "burnout_profile.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <intrin.h>
#include <tlhelp32.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace bdr_profile {

#ifdef _WIN32
namespace {
constexpr size_t kMaxSamples = 1u << 21;

// Opens the thread with that name.
HANDLE openThread(const std::string& name) {
    const std::wstring wideName(name.begin(), name.end());
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return nullptr;
    HANDLE found = nullptr;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snapshot, &entry); ok && !found; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != pid)
            continue;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                   FALSE, entry.th32ThreadID);
        if (!thread)
            continue;
        PWSTR description = nullptr;
        if (SUCCEEDED(GetThreadDescription(thread, &description)) && description) {
            if (std::wstring(description) == wideName)
                found = thread;
            LocalFree(description);
        }
        if (!found)
            CloseHandle(thread);
    }
    CloseHandle(snapshot);
    return found;
}

// Opens the thread named by BDR_PROFILE_THREAD, "GameThread" by default.
HANDLE openGameThread() {
    const char* wanted = std::getenv("BDR_PROFILE_THREAD");
    return openThread(wanted && wanted[0] ? wanted : "GameThread");
}

// The return addresses of a context, walked with the loaded modules' unwind tables only.
int unwind(CONTEXT context, uint64_t* pcs, int capacity) {
    int depth = 0;
    for (; depth < capacity && context.Rip != 0; ++depth) {
        pcs[depth] = context.Rip;
        DWORD64 imageBase = 0;
        RUNTIME_FUNCTION* function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
        if (!function) {
            // A leaf function: the return address is at the top of the stack.
            context.Rip = *reinterpret_cast<const uint64_t*>(context.Rsp);
            context.Rsp += 8;
            continue;
        }
        void* handlerData = nullptr;
        DWORD64 establisherFrame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData,
                         &establisherFrame, nullptr);
    }
    return depth;
}

void printFrames(const uint64_t* pcs, int depth);

// Prints the call stack of a thread, suspended while it is walked.
void printStack(HANDLE thread) {
    // Nothing may allocate or take a lock while the target is suspended (it may hold the
    // heap or DbgHelp lock): unwind first, then resume before naming the frames.
    uint64_t pcs[48];
    int depth = 0;
    if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(thread, &context))
            depth = unwind(context, pcs, 48);
        ResumeThread(thread);
    }
    printFrames(pcs, depth);
}

void printFrames(const uint64_t* pcs, int depth) {
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    if (!SymInitialize(process, nullptr, TRUE))
        return;
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
    for (int i = 0; i < depth; ++i) {
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 511;
        DWORD64 displacement = 0;
        const bool named = SymFromAddr(process, pcs[i], &displacement, symbol) != FALSE;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisplacement = 0;
        const bool lined = SymGetLineFromAddr64(process, pcs[i], &lineDisplacement, &line) != FALSE;
        std::printf("[stack] %2d %s+0x%llx  %s:%lu\n", i, named ? symbol->Name : "?",
                    static_cast<unsigned long long>(displacement), lined ? line.FileName : "?",
                    lined ? static_cast<unsigned long>(line.LineNumber) : 0ul);
    }
    std::fflush(stdout);
    SymCleanup(process);
}

LONG WINAPI reportCrash(EXCEPTION_POINTERS* exception) {
    const EXCEPTION_RECORD& record = *exception->ExceptionRecord;
    std::printf("[crash] exception 0x%08lx at 0x%llx", static_cast<unsigned long>(record.ExceptionCode),
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(record.ExceptionAddress)));
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2)
        std::printf(" (%s 0x%llx)", record.ExceptionInformation[0] == 1 ? "write" : "read",
                    static_cast<unsigned long long>(record.ExceptionInformation[1]));
    std::printf("\n");
    uint64_t pcs[48];
    const int depth = unwind(*exception->ContextRecord, pcs, 48);
    printFrames(pcs, depth);
    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace
#endif

void installCrashReport() {
#ifdef _WIN32
    SetUnhandledExceptionFilter(&reportCrash);
#endif
}

double threadCpuSeconds(const char* name) {
#ifdef _WIN32
    HANDLE thread = openThread(name);
    if (!thread)
        return -1.0;
    // GetThreadTimes charges whole 15.6 ms clock ticks to whichever thread they land on,
    // which misses most of a thread that waits thousands of times a second. The cycle
    // count is taken at every context switch, in time-stamp counter ticks.
    ULONG64 cycles = 0;
    const bool ok = QueryThreadCycleTime(thread, &cycles) != FALSE;
    CloseHandle(thread);
    static const double tscPerSecond = [] {
        LARGE_INTEGER frequency, start, end;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
        const uint64_t tscStart = __rdtsc();
        Sleep(50);
        QueryPerformanceCounter(&end);
        return static_cast<double>(__rdtsc() - tscStart) * static_cast<double>(frequency.QuadPart) /
               static_cast<double>(end.QuadPart - start.QuadPart);
    }();
    return ok ? static_cast<double>(cycles) / tscPerSecond : -1.0;
#else
    (void)name;
    return -1.0;
#endif
}

struct Sampler::State {
    std::atomic<bool> stop{false};
    std::thread worker;
    std::vector<uint64_t> samples;
    bool reported = false;
};

Sampler::Sampler() {
    const char* value = std::getenv("BDR_PROFILE");
    if (!value || value[0] == '\0' || value[0] == '0')
        return;
#ifdef _WIN32
    state_ = std::make_unique<State>();
    state_->samples.reserve(kMaxSamples);
    State* state = state_.get();
    state->worker = std::thread([this, state] {
        HANDLE target = nullptr;
        while (!state->stop.load(std::memory_order_acquire) && !(target = openGameThread()))
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!target)
            return;
        // BDR_PROFILE_DELAY_S=N: skip the first N seconds (menus) before sampling.
        if (const char* delay = std::getenv("BDR_PROFILE_DELAY_S")) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(std::atoi(delay));
            while (!state->stop.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                              TIMER_ALL_ACCESS);
        // BDR_PROFILE_SECONDS=N: sample for N seconds, then print the report and
        // the thread's call stack from here, even if the thread never returns.
        const char* seconds = std::getenv("BDR_PROFILE_SECONDS");
        const auto sampleUntil = seconds ? std::chrono::steady_clock::now() + std::chrono::seconds(std::atoi(seconds))
                                         : std::chrono::steady_clock::time_point::max();
        while (!state->stop.load(std::memory_order_acquire) && state->samples.size() < kMaxSamples &&
               std::chrono::steady_clock::now() < sampleUntil) {
            if (timer) {
                LARGE_INTEGER due;
                due.QuadPart = -10000; // 1 ms
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, 10);
            } else {
                Sleep(1);
            }
            // Nothing may allocate or take a lock while the target is suspended.
            if (SuspendThread(target) == static_cast<DWORD>(-1))
                break;
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            uint64_t rip = 0;
            if (GetThreadContext(target, &context))
                rip = context.Rip;
            ResumeThread(target);
            if (rip != 0)
                state->samples.push_back(rip);
        }
        if (timer)
            CloseHandle(timer);
        if (seconds) {
            // BDR_PROFILE_STACKS=N: N call stacks 200 ms apart instead of one.
            const char* stacks = std::getenv("BDR_PROFILE_STACKS");
            const int count = stacks ? std::max(1, std::atoi(stacks)) : 1;
            // A thread that has exited cannot be walked any more: stop there.
            for (int i = 0; i < count && WaitForSingleObject(target, 0) == WAIT_TIMEOUT; ++i) {
                std::printf("[stack] --- %d\n", i);
                printStack(target);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            report();
        }
        CloseHandle(target);
    });
#else
    std::cerr << "[profile] BDR_PROFILE is only implemented on Windows\n";
#endif
}

Sampler::~Sampler() {
    if (!state_)
        return;
    state_->stop.store(true, std::memory_order_release);
    if (state_->worker.joinable())
        state_->worker.join();
    report();
}

void Sampler::report() {
#ifdef _WIN32
    if (!state_ || state_->reported)
        return;
    state_->reported = true;
    const std::vector<uint64_t>& samples = state_->samples;
    if (samples.empty()) {
        std::cout << "[profile] no samples\n";
        return;
    }
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    const bool symbols = SymInitialize(process, nullptr, TRUE) != FALSE;

    std::map<uint64_t, uint32_t> perAddress;
    for (const uint64_t rip : samples)
        ++perAddress[rip];

    // BDR_PROFILE_DUMP=FILE: every sampled address with its count, module, symbol and offset, to
    // map the samples onto a compiler listing (/FAcs) where the PDB has no inlined lines.
    const char* dumpPath = std::getenv("BDR_PROFILE_DUMP");
    FILE* dump = dumpPath ? std::fopen(dumpPath, "w") : nullptr;
    std::map<std::string, uint32_t> perFunction;
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
    for (const auto& [address, count] : perAddress) {
        std::string name;
        DWORD64 displacement = 0;
        if (symbols) {
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 511;
            if (SymFromAddr(process, address, &displacement, symbol))
                name = symbol->Name;
        }
        if (name.empty()) {
            char text[32];
            std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
            name = text;
        }
        perFunction[name] += count;
        if (dump) {
            IMAGEHLP_MODULE64 module{};
            module.SizeOfStruct = sizeof(module);
            const bool moduled = symbols && SymGetModuleInfo64(process, address, &module);
            std::fprintf(dump, "%u\t%s\t%s\t0x%llx\n", count, moduled ? module.ModuleName : "?", name.c_str(),
                         static_cast<unsigned long long>(displacement));
        }
    }
    if (dump)
        std::fclose(dump);

    std::vector<std::pair<std::string, uint32_t>> ranked(perFunction.begin(), perFunction.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::cout << "[profile] " << samples.size() << " samples of the profiled thread"
              << (symbols ? "" : " (no symbols)") << '\n';
    const size_t shown = std::min<size_t>(ranked.size(), 45);
    for (size_t i = 0; i < shown; ++i) {
        std::printf("[profile] %5.1f%% %7u  %s\n", 100.0 * ranked[i].second / static_cast<double>(samples.size()),
                    ranked[i].second, ranked[i].first.c_str());
    }
    // The hottest addresses with their symbol offset and source line: a large
    // offset means the address has no symbol of its own (inlined or folded code).
    std::vector<std::pair<uint64_t, uint32_t>> hot(perAddress.begin(), perAddress.end());
    std::sort(hot.begin(), hot.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; symbols && i < std::min<size_t>(hot.size(), 30); ++i) {
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 511;
        DWORD64 displacement = 0;
        const bool named = SymFromAddr(process, hot[i].first, &displacement, symbol) != FALSE;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisplacement = 0;
        const bool lined = SymGetLineFromAddr64(process, hot[i].first, &lineDisplacement, &line) != FALSE;
        // The module and offset in it: a system DLL without its PDB names its code after the
        // nearest export, so this is what to look up with dumpbin /disasm.
        IMAGEHLP_MODULE64 module{};
        module.SizeOfStruct = sizeof(module);
        const bool moduled = SymGetModuleInfo64(process, hot[i].first, &module) != FALSE;
        std::printf("[profile] %7u  %s!%s+0x%llx (rva 0x%llx)  %s:%lu\n", hot[i].second,
                    moduled ? module.ModuleName : "?", named ? symbol->Name : "?",
                    static_cast<unsigned long long>(displacement),
                    static_cast<unsigned long long>(moduled ? hot[i].first - module.BaseOfImage : 0u),
                    lined ? line.FileName : "?", lined ? static_cast<unsigned long>(line.LineNumber) : 0ul);
    }
    // The hottest source lines, summed over every address that maps to them: this is
    // where inlined code (the compiled VU programs) shows up.
    std::map<std::string, uint32_t> perLine;
    for (const auto& [address, count] : perAddress) {
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisplacement = 0;
        if (symbols && SymGetLineFromAddr64(process, address, &lineDisplacement, &line)) {
            const char* file = std::strrchr(line.FileName, '\\');
            perLine[std::string(file ? file + 1 : line.FileName) + ":" + std::to_string(line.LineNumber)] += count;
        }
    }
    std::vector<std::pair<std::string, uint32_t>> lines(perLine.begin(), perLine.end());
    std::sort(lines.begin(), lines.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < std::min<size_t>(lines.size(), 40); ++i)
        std::printf("[profile:line] %5.1f%% %7u  %s\n", 100.0 * lines[i].second / static_cast<double>(samples.size()),
                    lines[i].second, lines[i].first.c_str());
    std::cout.flush();
    if (symbols)
        SymCleanup(process);
#endif
}

} // namespace bdr_profile
