#pragma once

#include "log_level.hpp"
#include <string>
#include <fstream>
#include <iostream>
#include <sstream>
#include <mutex>
#include <ctime>
#include <cstring>

namespace logging {

class Logger; // Forward declaration

class LogStream {
public:
    LogStream(Logger* logger, LogLevel level, const char* file, int line, const char* func);
    ~LogStream();

    // Разрешаем перемещение (Move Semantics)
    LogStream(LogStream&& other) noexcept;
    LogStream& operator=(LogStream&& other) noexcept;

    // Запрещаем копирование (так как внутри stringstream)
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

    void log(LogLevel level, const std::string& message, 
             const char* file, int line, const char* func);

    LogStream stream(LogLevel level, const char* file, int line, const char* func);

    void flush();

private:
    Logger();
    ~Logger();

    std::string format_message(LogLevel level, const std::string& message,
                               const char* file, int line, 
                               const char* func, std::time_t timestamp);
    std::string format_timestamp(std::time_t timestamp);
    void write_output(const std::string& formatted_message);

    mutable std::mutex mutex_;
    Config config_;
    std::ofstream file_stream_;
    bool initialized_;
};

#define LOG(level, msg) \
    ::logging::Logger::instance().log(level, msg, __FILE__, __LINE__, __FUNCTION__)

#define LOG_STREAM(level) \
    ::logging::Logger::instance().stream(level, __FILE__, __LINE__, __FUNCTION__)

#define LOG_TRACE(msg) LOG(::logging::LOG_TRACE, msg)
#define LOG_DEBUG(msg) LOG(::logging::LOG_DEBUG, msg)
#define LOG_INFO(msg)  LOG(::logging::LOG_INFO, msg)
#define LOG_WARNING(msg) LOG(::logging::LOG_WARNING, msg)
#define LOG_ERROR(msg) LOG(::logging::LOG_ERROR, msg)
#define LOG_FATAL(msg) LOG(::logging::LOG_FATAL, msg)

#define LOG_TRACE_STREAM LOG_STREAM(::logging::LOG_TRACE)
#define LOG_DEBUG_STREAM LOG_STREAM(::logging::LOG_DEBUG)
#define LOG_INFO_STREAM  LOG_STREAM(::logging::LOG_INFO)
#define LOG_WARNING_STREAM LOG_STREAM(::logging::LOG_WARNING)
#define LOG_ERROR_STREAM LOG_STREAM(::logging::LOG_ERROR)
#define LOG_FATAL_STREAM LOG_STREAM(::logging::LOG_FATAL)

} // namespace logging
