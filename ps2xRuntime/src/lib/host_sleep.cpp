#include "runtime/host_sleep.h"

#include <algorithm>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace ps2x
{
#ifdef _WIN32
    namespace
    {
        struct HighResolutionTimer
        {
            HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                   TIMER_ALL_ACCESS);
            ~HighResolutionTimer()
            {
                if (handle)
                    CloseHandle(handle);
            }
        };
    }
#endif

    void sleepPrecisely(std::chrono::nanoseconds duration)
    {
        if (duration <= std::chrono::nanoseconds::zero())
            return;
#ifdef _WIN32
        thread_local HighResolutionTimer timer;
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<long long>(1, duration.count() / 100); // relative, in 100 ns units
        if (timer.handle && SetWaitableTimerEx(timer.handle, &due, 0, nullptr, nullptr, nullptr, 0))
        {
            WaitForSingleObject(timer.handle, INFINITE);
            return;
        }
#endif
        std::this_thread::sleep_for(duration);
    }

    bool waitForComposition()
    {
#ifdef _WIN32
        using DwmFlushFn = HRESULT(WINAPI *)();
        static const auto flush = [] {
            const HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
            return dwm ? reinterpret_cast<DwmFlushFn>(GetProcAddress(dwm, "DwmFlush")) : nullptr;
        }();
        return flush && SUCCEEDED(flush());
#else
        return false;
#endif
    }
}
