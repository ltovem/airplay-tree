/*!
 * @file platform_time.cpp
 * @brief Cross-platform time implementation
 */
#include "platform_time.h"

#if AP2_PLATFORM_WINDOWS
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <time.h>
    #include <unistd.h>
#endif

namespace airplay2 {
namespace platform {

uint64_t time_now_us() {
#if AP2_PLATFORM_WINDOWS
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER count;
    QueryPerformanceCounter(&count);
    return (uint64_t)((double)count.QuadPart * 1000000.0 / (double)freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
#endif
}

uint64_t wallclock_us() {
#if AP2_PLATFORM_WINDOWS
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli;
    uli.LowPart  = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    // FILETIME is 100-ns intervals since 1601-01-01; epoch diff is 11644473600 seconds
    const uint64_t EPOCH_DIFF_US = 11644473600ULL * 1000000ULL;
    return (uli.QuadPart / 10ULL) - EPOCH_DIFF_US;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
#endif
}

void sleep_us(uint64_t us) {
#if AP2_PLATFORM_WINDOWS
    if (us == 0) return;
    // Use high-resolution wait if < 16ms, else Sleep()
    if (us < 1000) {
        // Spin for sub-ms to avoid scheduler granularity
        uint64_t end = time_now_us() + us;
        while (time_now_us() < end) { /* spin */ }
        return;
    }
    DWORD ms = (DWORD)(us / 1000ULL);
    Sleep(ms ? ms : 1);
#else
    if (us >= 1000000ULL) {
        ::sleep((unsigned)(us / 1000000ULL));
    }
    usleep((useconds_t)(us % 1000000ULL));
#endif
}

} // namespace platform
} // namespace airplay2
