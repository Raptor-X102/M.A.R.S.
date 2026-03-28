#include "silicon_probe/app/config.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

std::string lower_case(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

silicon_probe::infra::LogLevel parse_log_level(const std::string& raw_level) {
    const std::string level = lower_case(raw_level);
    if (level == "trace") {
        return spdlog::level::trace;
    }
    if (level == "debug") {
        return spdlog::level::debug;
    }
    if (level == "info") {
        return spdlog::level::info;
    }
    if (level == "warning" || level == "warn") {
        return spdlog::level::warn;
    }
    if (level == "error") {
        return spdlog::level::err;
    }
    if (level == "critical") {
        return spdlog::level::critical;
    }
    if (level == "off") {
        return spdlog::level::off;
    }

    throw std::invalid_argument("Unsupported log level: " + raw_level);
}

} // namespace

namespace silicon_probe::app {

CommandLineParser::CommandLineParser()
    : cli_("SiliconProbe - CPU cache probing tool") {
    configure();
}

ApplicationConfig CommandLineParser::parse(int argc, char** argv) {
    cli_.parse(argc, argv);

    config_.logging.level = parse_log_level(log_level_name_);
    if (no_console_) {
        config_.logging.console_output = false;
    }
    config_.print_summary = !no_summary_;

    if (bind_cpu_ >= 0) {
        config_.cache.environment.cpu = bind_cpu_;
    }

    if (measure_l1_ || measure_l2_ || measure_l3_) {
        config_.cache.levels.reset();
        config_.cache.levels.set(0, measure_l1_);
        config_.cache.levels.set(1, measure_l2_);
        config_.cache.levels.set(2, measure_l3_);
    }

    return config_;
}

CLI::App& CommandLineParser::cli() noexcept {
    return cli_;
}

void CommandLineParser::configure() {
    cli_.set_help_all_flag("--help-all", "Show all options");

    cli_.add_option("--log-level", log_level_name_, "Log level: trace|debug|info|warning|error|critical|off")
        ->default_val(log_level_name_)
        ->check(CLI::IsMember({"trace", "debug", "info", "warning", "error", "critical", "off"}, CLI::ignore_case));
    cli_.add_option("--log-file", config_.logging.log_file, "Write logs to a file as well");
    cli_.add_flag("--no-console", no_console_, "Disable console logging");
    cli_.add_flag("--no-summary", no_summary_, "Skip summary output");

    cli_.add_flag("--measure-l1", measure_l1_, "Measure only L1d when used with explicit level selection flags");
    cli_.add_flag("--measure-l2", measure_l2_, "Measure only L2 when used with explicit level selection flags");
    cli_.add_flag("--measure-l3", measure_l3_, "Measure only L3 when used with explicit level selection flags");

    cli_.add_flag("--realtime-priority", config_.cache.environment.realtime_priority, "Temporarily elevate scheduling priority during measurement");
    cli_.add_flag("--lock-frequency", config_.cache.environment.lock_frequency, "Temporarily switch the CPU governor to performance");
    cli_.add_option("--bind-cpu", bind_cpu_, "Bind the probing thread to a specific CPU id")
        ->check(CLI::Range(0, std::numeric_limits<int>::max()));

    cli_.add_option("--l1-max", config_.cache.l1_max, "Upper bound for L1 search in bytes")->check(CLI::PositiveNumber);
    cli_.add_option("--l2-max", config_.cache.l2_max, "Upper bound for L2 search in bytes")->check(CLI::PositiveNumber);
    cli_.add_option("--l3-max", config_.cache.l3_max, "Upper bound for L3 search in bytes")->check(CLI::PositiveNumber);
    cli_.add_option("--cache-min-lines", config_.cache.cache_min_lines, "Minimum working set in cache lines for the L1 sweep")->check(CLI::PositiveNumber);
    cli_.add_option("--seed", config_.cache.seed, "Seed used for pointer-chasing permutation");
    cli_.add_option("--warmup-iterations", config_.cache.warmup_iterations, "Number of warmup passes before each measurement")->check(CLI::PositiveNumber);
    cli_.add_option("--precision", config_.cache.precision, "Boundary refinement precision in bytes")->check(CLI::PositiveNumber);
    cli_.add_option("--target-accesses", config_.cache.target_accesses, "Target total memory accesses per probe point")->check(CLI::PositiveNumber);
    cli_.add_option("--min-iterations", config_.cache.min_iterations, "Lower bound for measurement loop iterations")->check(CLI::PositiveNumber);
    cli_.add_option("--max-iterations", config_.cache.max_iterations, "Upper bound for measurement loop iterations")->check(CLI::PositiveNumber);
}

} // namespace silicon_probe::app
