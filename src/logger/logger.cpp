#include "logger.hpp"
#include <cstdio>
#include <ctime>
#include <cstring>
#include <iostream>
#include <mutex>
#include <cstdarg>

namespace logging {

// ---------- LogStream ----------
LogStream::LogStream(Logger* logger, LogLevel level, const char* file, int line, const char* func)
    : logger_(logger), level_(level), file_(file), line_(line), func_(func) {}

LogStream::~LogStream() {
    if (logger_) {
        // Используем log() вместо vlog(), потому что у нас есть строка
        logger_->log(level_, file_, line_, func_, "%s", oss_.str().c_str());
    }
}

LogStream::LogStream(LogStream&& other) noexcept
    : logger_(other.logger_), level_(other.level_), file_(other.file_), line_(other.line_),
      func_(other.func_), oss_(std::move(other.oss_)) {
    other.logger_ = nullptr;
}

LogStream& LogStream::operator=(LogStream&& other) noexcept {
    if (this != &other) {
        if (logger_) {
            logger_->log(level_, file_, line_, func_, "%s", oss_.str().c_str());
        }
        logger_ = other.logger_;
        level_ = other.level_;
        file_ = other.file_;
        line_ = other.line_;
        func_ = other.func_;
        oss_ = std::move(other.oss_);
        other.logger_ = nullptr;
    }
    return *this;
}

// ---------- Logger ----------
Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::initialize(const Config& config) {
    std::lock_guard<std::mutex> lock(instance().mutex_);
    instance().config_ = config;
    if (!config.log_file.empty()) {
        instance().file_stream_ = std::fopen(config.log_file.c_str(), "a");
        if (!instance().file_stream_) {
            std::fprintf(stderr, "[Logger] Failed to open log file: %s\n", config.log_file.c_str());
        }
    }
    instance().initialized_ = true;
}

void Logger::shutdown() {
    auto& logger = instance();
    std::lock_guard<std::mutex> lock(logger.mutex_);
    logger.flush();
    if (logger.file_stream_) {
        std::fclose(logger.file_stream_);
        logger.file_stream_ = nullptr;
    }
    logger.initialized_ = false;
}

Logger::Logger() : file_stream_(nullptr), initialized_(false) {}

Logger::~Logger() {
    shutdown();
}

void Logger::set_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.min_level = level;
}

LogLevel Logger::get_level() const {
    return config_.min_level;
}

void Logger::set_output_file(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_stream_) {
        std::fclose(file_stream_);
    }
    file_stream_ = std::fopen(filepath.c_str(), "a");
    if (!file_stream_) {
        std::fprintf(stderr, "[Logger] Failed to open log file: %s\n", filepath.c_str());
    }
}

void Logger::enable_console(bool enable) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.console_output = enable;
}

void Logger::set_flush_on_write(bool flush) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.flush_on_write = flush;
}

void Logger::log(LogLevel level, const char* file, int line, const char* func, const char* format, ...) {
    if (level < config_.min_level) return;

    va_list args;
    va_start(args, format);
    vlog(level, file, line, func, format, args);
    va_end(args);
}

void Logger::vlog(LogLevel level, const char* file, int line, const char* func, const char* format, va_list args) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Префикс (время, уровень, место)
    std::string prefix;
    if (config_.include_timestamp) {
        std::time_t t = std::time(nullptr);
        char time_buf[32];
        std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        prefix += time_buf;
        prefix += ' ';
    }
    if (config_.include_level) {
        prefix += '[';
        prefix += log_level_to_string(level);
        prefix += "] ";
    }
    if (config_.include_location) {
        const char* filename = std::strrchr(file, '/');
        filename = filename ? filename + 1 : file;
#ifdef _WIN32
        const char* win_filename = std::strrchr(filename, '\\');
        filename = win_filename ? win_filename + 1 : filename;
#endif
        prefix += '[';
        prefix += filename;
        prefix += ':';
        prefix += std::to_string(line);
        if (func) {
            prefix += " (";
            prefix += func;
            prefix += ')';
        }
        prefix += "] ";
    }

    // Форматируем тело
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = std::vsnprintf(nullptr, 0, format, args_copy);
    va_end(args_copy);
    if (needed < 0) return;

    std::string body(needed + 1, '\0');
    std::vsnprintf(&body[0], body.size(), format, args);
    body.resize(needed);

    write_formatted(prefix, body, level);
}

void Logger::write_formatted(const std::string& prefix, const std::string& body, LogLevel level) {
    if (config_.console_output) {
        std::fwrite(prefix.data(), 1, prefix.size(), stdout);
        std::fwrite(body.data(), 1, body.size(), stdout);
        std::fputc('\n', stdout);
    }
    if (file_stream_) {
        std::fwrite(prefix.data(), 1, prefix.size(), file_stream_);
        std::fwrite(body.data(), 1, body.size(), file_stream_);
        std::fputc('\n', file_stream_);
        if (config_.flush_on_write || level >= LOG_ERROR) {
            std::fflush(file_stream_);
        }
    }
}

LogStream Logger::stream(LogLevel level, const char* file, int line, const char* func) {
    return LogStream(this, level, file, line, func);
}

void Logger::flush() {
    if (config_.console_output) {
        std::fflush(stdout);
    }
    if (file_stream_) {
        std::fflush(file_stream_);
    }
}

} // namespace logging
