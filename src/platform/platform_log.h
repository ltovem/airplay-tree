/*!
 * @file platform_log.h
 * @brief Cross-platform logging utilities
 */
#ifndef AIRPLAY2_PLATFORM_LOG_H
#define AIRPLAY2_PLATFORM_LOG_H

#include <cstdio>
#include <cstdarg>
#include <functional>
#include <string>

namespace airplay2 {
namespace platform {

enum LogLevel : int {
    LOG_ERROR = 0,
    LOG_WARN  = 1,
    LOG_INFO  = 2,
    LOG_DEBUG = 3,
    LOG_TRACE = 4
};

using LogCallback = std::function<void(int level, const std::string& msg)>;

inline LogCallback& global_log_callback() {
    static LogCallback cb;
    return cb;
}

inline int& global_log_level() {
    static int level = LOG_INFO;
    return level;
}

inline void set_log_callback(LogCallback cb) {
    global_log_callback() = std::move(cb);
}

inline void set_log_level(int level) {
    global_log_level() = level;
}

inline const char* log_level_tag(int level) {
    switch (level) {
        case LOG_ERROR: return "E";
        case LOG_WARN:  return "W";
        case LOG_INFO:  return "I";
        case LOG_DEBUG: return "D";
        case LOG_TRACE: return "T";
        default: return "?";
    }
}

inline void log_message(int level, const char* file, int line, const char* fmt, ...) {
    if (level > global_log_level()) return;

    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    char msg[2560];
    const char* short_file = file;
    for (const char* p = file; *p; ++p) {
        if (*p == '/' || *p == '\\') short_file = p + 1;
    }
    snprintf(msg, sizeof(msg), "[%s][%s:%d] %s",
             log_level_tag(level), short_file, line, buf);

    if (global_log_callback()) {
        global_log_callback()(level, msg);
    } else {
#if AP2_PLATFORM_ANDROID
        __android_log_print(ANDROID_LOG_ERROR + level, "airplay2lib", "%s", msg);
#else
        std::fprintf(stderr, "%s\n", msg);
        std::fflush(stderr);
#endif
    }
}

} // namespace platform
} // namespace airplay2

#define AP2_LOGE(...) ::airplay2::platform::log_message(::airplay2::platform::LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define AP2_LOGW(...) ::airplay2::platform::log_message(::airplay2::platform::LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define AP2_LOGI(...) ::airplay2::platform::log_message(::airplay2::platform::LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define AP2_LOGD(...) ::airplay2::platform::log_message(::airplay2::platform::LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define AP2_LOGT(...) ::airplay2::platform::log_message(::airplay2::platform::LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)

#endif // AIRPLAY2_PLATFORM_LOG_H
