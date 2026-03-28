#pragma once

#include "silicon_probe/cache/cache_measurer.hpp"
#include "silicon_probe/infra/logging.hpp"

#include <CLI/CLI.hpp>
#include <string>

namespace silicon_probe::app {

struct ApplicationConfig {
    infra::LoggingConfig logging;
    cache::CacheMeasurer::Config cache;
    bool print_summary = true;
};

class CommandLineParser {
public:
    CommandLineParser();

    ApplicationConfig parse(int argc, char** argv);
    CLI::App& cli() noexcept;

private:
    void configure();

    CLI::App cli_;
    ApplicationConfig config_;
    std::string log_level_name_ = "info";
    bool measure_l1_ = false;
    bool measure_l2_ = false;
    bool measure_l3_ = false;
    bool no_summary_ = false;
    bool no_console_ = false;
    int bind_cpu_ = -1;
};

} // namespace silicon_probe::app
