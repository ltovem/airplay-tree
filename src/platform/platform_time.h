/*!
 * @file platform_time.h
 * @brief Cross-platform time / monotonic clock utilities
 */
#ifndef AIRPLAY2_PLATFORM_TIME_H
#define AIRPLAY2_PLATFORM_TIME_H

#include <cstdint>

namespace airplay2 {
namespace platform {

/*!
 * @brief Monotonic timestamp in microseconds
 *
 * Not wall-clock time; only safe for measuring intervals.
 */
uint64_t time_now_us();

/*!
 * @brief Wall-clock UTC timestamp in microseconds since epoch
 */
uint64_t wallclock_us();

/*!
 * @brief Sleep current thread for given microseconds
 */
void sleep_us(uint64_t us);

inline void sleep_ms(uint32_t ms) { sleep_us(uint64_t(ms) * 1000); }

} // namespace platform
} // namespace airplay2

#endif // AIRPLAY2_PLATFORM_TIME_H
