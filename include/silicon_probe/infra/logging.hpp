#pragma once

#include <spdlog/common.h>
#include <spdlog/spdlog.h>

#include <string>

namespace silicon_probe::infra {

using LogLevel = spdlog::level::level_enum;

struct LoggingConfig {
    LogLevel level = spdlog::level::info;
    bool console_output = true;
    bool flush_on_write = true;
    bool include_timestamp = true;
    bool include_location = true;
    std::string log_file;
};

class LoggingSession {
public:
    explicit LoggingSession(const LoggingConfig& config);
    ~LoggingSession();

    LoggingSession(const LoggingSession&) = delete;
    LoggingSession& operator=(const LoggingSession&) = delete;
};

void initialize_logging(const LoggingConfig& config);
void shutdown_logging();

} // namespace silicon_probe::infra
