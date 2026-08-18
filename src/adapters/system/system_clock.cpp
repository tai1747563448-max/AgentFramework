#include "adapters/system/system_clock.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace agent {

std::string SystemClock::now_utc() const {
    const auto now = std::chrono::system_clock::now();
    const auto epoch_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch());
    const auto milliseconds = epoch_ms.count() % 1000;
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif

    std::ostringstream result;
    result << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
           << std::setfill('0') << std::setw(3) << milliseconds << 'Z';
    return result.str();
}

std::int64_t SystemClock::monotonic_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace agent
