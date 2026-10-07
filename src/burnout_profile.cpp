#include "burnout_profile.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace bdr_profile {

#ifdef _WIN32
namespace {
constexpr size_t kMaxSamples = 1u << 21;

// Opens the thread that the runtime names "GameThread".
HANDLE openGameThread() {
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
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                                   FALSE, entry.th32ThreadID);
        if (!thread)
            continue;
        PWSTR description = nullptr;
        if (SUCCEEDED(GetThreadDescription(thread, &description)) && description) {
            if (std::wstring(description) == L"GameThread")
                found = thread;
            LocalFree(description);
        }
        if (!found)
            CloseHandle(thread);
    }
    CloseHandle(snapshot);
    return found;
}
} // namespace
#endif

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
    state->worker = std::thread([state] {
        HANDLE target = nullptr;
        while (!state->stop.load(std::memory_order_acquire) && !(target = openGameThread()))
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!target)
            return;
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                              TIMER_ALL_ACCESS);
        while (!state->stop.load(std::memory_order_acquire) && state->samples.size() < kMaxSamples) {
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

    std::map<std::string, uint32_t> perFunction;
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
    for (const auto& [address, count] : perAddress) {
        std::string name;
        if (symbols) {
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 511;
            DWORD64 displacement = 0;
            if (SymFromAddr(process, address, &displacement, symbol))
                name = symbol->Name;
        }
        if (name.empty()) {
            char text[32];
            std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
            name = text;
        }
        perFunction[name] += count;
    }

    std::vector<std::pair<std::string, uint32_t>> ranked(perFunction.begin(), perFunction.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::cout << "[profile] " << samples.size() << " samples of the guest thread"
              << (symbols ? "" : " (no symbols)") << '\n';
    const size_t shown = std::min<size_t>(ranked.size(), 45);
    for (size_t i = 0; i < shown; ++i) {
        std::printf("[profile] %5.1f%% %7u  %s\n", 100.0 * ranked[i].second / static_cast<double>(samples.size()),
                    ranked[i].second, ranked[i].first.c_str());
    }
    std::cout.flush();
    if (symbols)
        SymCleanup(process);
#endif
}

} // namespace bdr_profile
