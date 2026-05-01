#pragma once

#include <memory>
#include <spdlog/common.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

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

namespace detail {

static std::string make_pattern(const LoggingConfig& config) {
    std::string pattern{};

    if (config.include_timestamp) {
        pattern += "%Y-%m-%d %H:%M:%S ";
    }
    pattern += "[%^%l%$] ";

    if (config.include_location) {
        pattern += "[%s:%# (%!)] ";
    }
    pattern += "%v";

    return pattern;
}

}  // namespace detail

class Logger {
   public:
    explicit Logger(const LoggingConfig& config) {
        std::vector<spdlog::sink_ptr> sinks{};
        sinks.reserve(2);

        if (config.console_output) {
            sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        }

        if (!config.log_file.empty()) {
            sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(config.log_file, true));
        }

        if (sinks.empty()) {
            sinks.push_back(std::make_shared<spdlog::sinks::null_sink_mt>());
        }

        auto logger = std::make_shared<spdlog::logger>("silicon_probe", sinks.begin(), sinks.end());
        logger->set_level(config.level);
        logger->set_pattern(detail::make_pattern(config));

        if (config.flush_on_write) {
            logger->flush_on(spdlog::level::trace);
        } else {
            logger->flush_on(spdlog::level::err);
        }

        spdlog::set_default_logger(std::move(logger));
        spdlog::set_level(config.level);
    }

    ~Logger() { spdlog::shutdown(); }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
};

}  // namespace silicon_probe::infra
