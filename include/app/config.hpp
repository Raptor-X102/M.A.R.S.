#pragma once

#include "measurement/cache/cache_measurer.hpp"
#include "measurement/rob/rob_measurer.hpp"
#include "measurement/branch_history_table/branch_history_table_measurer.hpp"
#include "measurement/return_address_stack/return_address_stack_measurer.hpp"
#include "measurement/exec_ports/exec_ports_measurer.hpp"
#include "measurement/uops_cache/uops_cache_measurer.hpp"
#include "measurement/branch_target_buffer/branch_target_buffer_measurer.hpp"
#include "measurement/store_to_load_forwarding/store_to_load_forwarding_measurer.hpp"
#include "measurement/write_buffer/write_buffer_measurer.hpp"
#include "infra/logging.hpp"

#include <CLI/CLI.hpp>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>

namespace silicon_probe::app {

struct BootstrapOptions {
    std::filesystem::path config_path = "config/mars_example.yaml";
    infra::LoggingConfig logging;
    bool print_summary = true;
};

struct ApplicationConfig {
    infra::LoggingConfig logging;
    cache::CacheMeasurer::Config cache;
    rob::RobMeasurer::Config rob;
    branch_history_table::BranchHistoryTableMeasurer::Config bht;
    return_address_stack::ReturnAddressStackMeasurer::Config ras;
    exec_ports::ExecPortsMeasurer::Config exec_ports;
    uops_cache::UopsCacheMeasurer::Config uops_cache;
    branch_target_buffer::BranchTargetBufferMeasurer::Config btb;
    store_to_load_forwarding::StoreToLoadForwardingMeasurer::Config s2l_fwd;
    write_buffer::WriteBufferMeasurer::Config write_buffer;
    bool print_summary = true;
};

class BootstrapOptionsParser {
  private:
    static const std::map<std::string, std::string>& log_level_aliases() {
        static const std::map<std::string, std::string> aliases{
            {"trace", "trace"}, {"debug", "debug"}, {"info", "info"},         {"warning", "warn"}, {"warn", "warn"},
            {"error", "err"},   {"err", "err"},     {"critical", "critical"}, {"off", "off"},
        };
        return aliases;
    }

    void configure() {
        cli_.set_help_all_flag("--help-all", "Show all options");

        cli_.add_option("-c,--config", options_.config_path, "Path to the YAML benchmark configuration file")
            ->default_val(options_.config_path.string());

        cli_.add_option("--log-level", log_level_name_, "Log level: trace|debug|info|warning|error|critical|off")
            ->default_val(log_level_name_)
            ->transform(CLI::CheckedTransformer(log_level_aliases(), CLI::ignore_case));

        cli_.add_option("--log-file", options_.logging.log_file, "Write logs to a file as well");

        cli_.add_flag("--no-console", no_console_, "Disable console logging");
        cli_.add_flag("--no-summary", no_summary_, "Skip summary output");
    }

    CLI::App cli_;
    BootstrapOptions options_;
    std::string log_level_name_ = "info";

    bool no_summary_ = false;
    bool no_console_ = false;

  public:
    BootstrapOptionsParser() : cli_("SiliconProbe - CPU microarchitecture benchmark tool") { configure(); }

    BootstrapOptions parse(int argc, char** argv) {
        cli_.parse(argc, argv);

        options_.logging.level = spdlog::level::from_str(log_level_name_);
        if (options_.logging.level == spdlog::level::off && log_level_name_ != "off") {
            throw std::invalid_argument("Unsupported log level: " + log_level_name_);
        }

        options_.logging.console_output = !no_console_;
        options_.print_summary = !no_summary_;

        return options_;
    }

    int exit(const CLI::ParseError& error) const { return cli_.exit(error); }
};

class ApplicationConfigLoader {
  public:
    ApplicationConfig load(const BootstrapOptions& options) const;
};

} // namespace silicon_probe::app
