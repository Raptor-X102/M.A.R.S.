#pragma once

#include <string>

namespace logging {

enum LogLevel {
    LOG_TRACE = 0,
    LOG_DEBUG = 1,
    LOG_INFO = 2,
    LOG_WARNING = 3,
    LOG_ERROR = 4,
    LOG_FATAL = 5,
    LOG_OFF = 6
};

inline std::string log_level_to_string(LogLevel level) {
    switch (level) {
        case LOG_TRACE:   return "TRACE";
        case LOG_DEBUG:   return "DEBUG";
        case LOG_INFO:    return "INFO";
        case LOG_WARNING: return "WARNING";
        case LOG_ERROR:   return "ERROR";
        case LOG_FATAL:   return "FATAL";
        case LOG_OFF:     return "OFF";
        default:          return "UNKNOWN";
    }
}

} // namespace logging
