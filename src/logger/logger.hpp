#pragma once

#include "log_level.hpp"
#include <string>
#include <cstdio>
#include <mutex>
#include <ctime>
#include <sstream>
#include <memory>
#include <iostream>

namespace logging {

class Logger; // forward

// Пустой поток для неактивного уровня
class NullStream {
public:
    template<typename T>
    NullStream& operator<<(const T&) { return *this; }
    NullStream& operator<<(std::ostream& (*)(std::ostream&)) { return *this; }
};

// Настоящий поток для активного уровня
class LogStream {
public:
    LogStream(Logger* logger, LogLevel level, const char* file, int line, const char* func);
    ~LogStream();

    LogStream(LogStream&& other) noexcept;
    LogStream& operator=(LogStream&& other) noexcept;

    LogStream(const LogStream&) = delete;
    LogStream& operator=(const LogStream&) = delete;

    template<typename T>
    LogStream& operator<<(const T& value) {
        oss_ << value;
        return *this;
    }

    LogStream& operator<<(std::ostream& (*manip)(std::ostream&)) {
        oss_ << manip;
        return *this;
    }

private:
    Logger* logger_;
    LogLevel level_;
    const char* file_;
    int line_;
    const char* func_;
    std::ostringstream oss_;
};

// Прокси-класс для условного логирования
class LogStreamProxy {
public:
    LogStreamProxy(Logger* logger, LogLevel level, const char* file, int line, const char* func, bool active)
        : active_(active) {
        if (active_) {
            stream_ = std::make_unique<LogStream>(logger, level, file, line, func);
        }
    }

    template<typename T>
    LogStreamProxy& operator<<(const T& val) {
        if (active_) {
            (*stream_) << val;
        }
        return *this;
    }

    LogStreamProxy& operator<<(std::ostream& (*manip)(std::ostream&)) {
        if (active_) {
            (*stream_) << manip;
        }
        return *this;
    }

private:
    bool active_;
    std::unique_ptr<LogStream> stream_;
};

// Основной класс логгера
class Logger {
public:
    struct Config {
        LogLevel min_level;
        bool include_timestamp;
        bool include_location;
        bool include_level;
        std::string log_file;
        bool console_output;
        bool flush_on_write;

        Config() 
            : min_level(LOG_INFO)
            , include_timestamp(true)
            , include_location(true)
            , include_level(true)
            , log_file("")
            , console_output(true)
            , flush_on_write(false) {}
    };

    static Logger& instance();
    static void initialize(const Config& config);
    static void shutdown();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void set_level(LogLevel level);
    LogLevel get_level() const;

    void set_output_file(const std::string& filepath);
    void enable_console(bool enable);
    void set_flush_on_write(bool flush);

    // Форматный лог (printf-стиль)
    void log(LogLevel level, const char* file, int line, const char* func, const char* format, ...)
        __attribute__((format(printf, 6, 7)));

    void vlog(LogLevel level, const char* file, int line, const char* func, const char* format, va_list args);

    // Потоковый лог (создаёт LogStream при активном уровне)
    LogStream stream(LogLevel level, const char* file, int line, const char* func);

    void flush();

private:
    Logger();
    ~Logger();

    void write_formatted(const std::string& prefix, const std::string& body, LogLevel level);

    mutable std::mutex mutex_;
    Config config_;
    std::FILE* file_stream_;
    bool initialized_;
};

} // namespace logging

// ---------- ФОРМАТНЫЕ МАКРОСЫ (НЕ ВЫЧИСЛЯЮТ АРГУМЕНТЫ) ----------
#define LOG(level, ...) \
    do { \
        if (::logging::Logger::instance().get_level() <= (level)) { \
            ::logging::Logger::instance().log((level), __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__); \
        } \
    } while(0)

#define LOG_TRACE(...) LOG(::logging::LOG_TRACE, __VA_ARGS__)
#define LOG_DEBUG(...) LOG(::logging::LOG_DEBUG, __VA_ARGS__)
#define LOG_INFO(...)  LOG(::logging::LOG_INFO, __VA_ARGS__)
#define LOG_WARNING(...) LOG(::logging::LOG_WARNING, __VA_ARGS__)
#define LOG_ERROR(...) LOG(::logging::LOG_ERROR, __VA_ARGS__)
#define LOG_FATAL(...) LOG(::logging::LOG_FATAL, __VA_ARGS__)

// ---------- ПОТОКОВЫЕ МАКРОСЫ (ТОЖЕ НЕ ВЫЧИСЛЯЮТ АРГУМЕНТЫ) ----------
#define LOG_STREAM(level) \
    logging::LogStreamProxy(&::logging::Logger::instance(), (level), __FILE__, __LINE__, __FUNCTION__, \
        ::logging::Logger::instance().get_level() <= (level))

#define LOG_TRACE_STREAM LOG_STREAM(::logging::LOG_TRACE)
#define LOG_DEBUG_STREAM LOG_STREAM(::logging::LOG_DEBUG)
#define LOG_INFO_STREAM  LOG_STREAM(::logging::LOG_INFO)
#define LOG_WARNING_STREAM LOG_STREAM(::logging::LOG_WARNING)
#define LOG_ERROR_STREAM LOG_STREAM(::logging::LOG_ERROR)
#define LOG_FATAL_STREAM LOG_STREAM(::logging::LOG_FATAL)
