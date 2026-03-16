#include "cpu_info.hpp"
#include "logger.hpp"

int main() {
    logging::Logger::Config config;
    config.min_level = logging::LOG_INFO;
    config.console_output = true;
    config.flush_on_write = true;
    config.include_timestamp = true;
    config.include_location = true;
    config.include_level = true;
    logging::Logger::initialize(config);
    
    try {
        CPUInfo cpu;
        
        cpu.measure_l1();
        cpu.measure_l2();
        cpu.measure_l3();
        
        cpu.print_summary();
        
    } catch (const os::PermissionError& e) {
        LOG_ERROR_STREAM << "Permission error: " << e.what();
        LOG_ERROR("Run with sudo for accurate measurements");
        logging::Logger::shutdown();
        return 1;
    } catch (const std::exception& e) {
        LOG_ERROR_STREAM << "Error: " << e.what();
        logging::Logger::shutdown();
        return 1;
    }
    
    logging::Logger::shutdown();
    return 0;
}
