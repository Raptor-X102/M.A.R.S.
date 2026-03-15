#include "logger.hpp"

namespace logging {

// --- LogStream Implementation ---

LogStream::LogStream(Logger* logger, LogLevel level, const char* file, int line, const char* func)
    : logger_(logger), level_(level), file_(file), line_(line), func_(func) {}

LogStream::~LogStream() {
    if (logger_) {
        logger_->log(level_, oss_.str(), file_, line_, func_);
    }
}

// Move constructor
LogStream::LogStream(LogStream&& other) noexcept
    : logger_(other.logger_)
    , level_(other.level_)
    , file_(other.file_)
    , line_(other.line_)
    , func_(other.func_)
    , oss_(std::move(other.oss_)) {
    
    other.logger_ = nullptr; // Invalidate source
}

// Move assignment
LogStream& LogStream::operator=(LogStream&& other) noexcept {
    if (this != &other) {
        // Flush current buffer before moving
        if (logger_) {
            logger_->log(level_, oss_.str(), file_, line_, func_);
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

// --- Logger Implementation ---

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::initialize(const Config& config) {
    Logger& logger = instance();
    std::lock_guard<std::mutex> lock(logger.mutex_);
    
    logger.config_ = config;
    
    if (!config.log_file.empty()) {
        logger.file_stream_.open(config.log_file.c_str(), 
                                  std::ios::out | std::ios::app);
        if (!logger.file_stream_.is_open()) {
            std::cerr << "[Logger] Failed to open log file: " 
                      << config.log_file << std::endl;
        }
    }
    
    logger.initialized_ = true;
}

void Logger::shutdown() {
    Logger& logger = instance();
    std::lock_guard<std::mutex> lock(logger.mutex_);
    logger.flush();
    if (logger.file_stream_.is_open()) {
        logger.file_stream_.close();
    }
    logger.initialized_ = false;
}

Logger::Logger() : initialized_(false) {}

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
    
    if (file_stream_.is_open()) {
        file_stream_.close();
    }
    
    file_stream_.open(filepath.c_str(), std::ios::out | std::ios::app);
    if (!file_stream_.is_open()) {
        std::cerr << "[Logger] Failed to open log file: " << filepath << std::endl;
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

void Logger::log(LogLevel level, const std::string& message,
                 const char* file, int line, const char* func) {
    if (level < config_.min_level) {
        return;
    }

    std::string formatted;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        formatted = format_message(level, message, file, line, func, std::time(NULL));
        write_output(formatted);
        
        if (config_.flush_on_write || level >= LOG_ERROR) {
            flush();
        }
    }
}

LogStream Logger::stream(LogLevel level, const char* file, int line, const char* func) {
    return LogStream(this, level, file, line, func);
}

std::string Logger::format_message(LogLevel level, const std::string& message,
                                   const char* file, int line, 
                                   const char* func, std::time_t timestamp) {
    std::ostringstream oss;
    
    if (config_.include_timestamp) {
        oss << format_timestamp(timestamp) << " ";
    }
    
    if (config_.include_level) {
        oss << "[" << log_level_to_string(level) << "] ";
    }
    
    if (config_.include_location) {
        const char* filename = std::strrchr(file, '/');
        filename = filename ? filename + 1 : file;
#ifdef _WIN32
        const char* win_filename = std::strrchr(filename, '\\');
        filename = win_filename ? win_filename + 1 : filename;
#endif
        oss << "[" << filename << ":" << line;
        if (func) {
            oss << " (" << func << ")";
        }
        oss << "] ";
    }
    
    oss << message;
    
    return oss.str();
}

std::string Logger::format_timestamp(std::time_t timestamp) {
    std::tm* tm_info = std::localtime(&timestamp);
    char buffer[26];
    std::strftime(buffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);
    return std::string(buffer);
}

void Logger::write_output(const std::string& formatted_message) {
    if (config_.console_output) {
        std::cout << formatted_message << std::endl;
    }
    
    if (file_stream_.is_open()) {
        file_stream_ << formatted_message << std::endl;
    }
}

void Logger::flush() {
    if (config_.console_output) {
        std::cout.flush();
    }
    if (file_stream_.is_open()) {
        file_stream_.flush();
    }
}

} // namespace logging
