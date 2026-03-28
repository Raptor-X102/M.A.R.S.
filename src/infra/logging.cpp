#include "silicon_probe/infra/logging.hpp"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <memory>
#include <string>
#include <vector>

namespace {

std::string make_pattern(const silicon_probe::infra::LoggingConfig& config) {
    std::string pattern;

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

} // namespace

namespace silicon_probe::infra {

LoggingSession::LoggingSession(const LoggingConfig& config) {
    initialize_logging(config);
}

LoggingSession::~LoggingSession() {
    shutdown_logging();
}

void initialize_logging(const LoggingConfig& config) {
    std::vector<spdlog::sink_ptr> sinks;
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
    logger->set_pattern(make_pattern(config));

    if (config.flush_on_write) {
        logger->flush_on(spdlog::level::trace);
    } else {
        logger->flush_on(spdlog::level::err);
    }

    spdlog::set_default_logger(std::move(logger));
    spdlog::set_level(config.level);
}

void shutdown_logging() {
    spdlog::shutdown();
}

} // namespace silicon_probe::infra
