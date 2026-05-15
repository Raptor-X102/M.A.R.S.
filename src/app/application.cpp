#include "app/application.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "core/measurer_registry.hpp"
#include "core/probe_service.hpp"
#include "core/summary_printer.hpp"
#include "infra/logging.hpp"
#include "measurement/cache/cache_measurer.hpp"
#include "measurement/rob/rob_measurer.hpp"
#include "measurement/tlb/tlb_measurer.hpp"
#include "platform/os_errors.hpp"

namespace silicon_probe::app {

namespace {

constexpr size_t kBannerInnerWidth = 76;

std::string pad_right(std::string text, size_t width) {
    if (text.size() > width) {
        text.resize(width);
        return text;
    }
    text.append(width - text.size(), ' ');
    return text;
}

std::string center_text(std::string text, size_t width) {
    if (text.size() >= width) {
        text.resize(width);
        return text;
    }

    const size_t total_padding = width - text.size();
    const size_t left_padding  = total_padding / 2;
    const size_t right_padding = total_padding - left_padding;
    return std::string(left_padding, ' ') + text + std::string(right_padding, ' ');
}

void print_banner_line(std::ostream& stream, const std::string& text) {
    stream << "│ " << pad_right(text, kBannerInnerWidth) << " │\n";
}

std::string log_level_name(infra::LogLevel level) {
    switch (level) {
        case spdlog::level::trace:
            return "trace";
        case spdlog::level::debug:
            return "debug";
        case spdlog::level::info:
            return "info";
        case spdlog::level::warn:
            return "warn";
        case spdlog::level::err:
            return "error";
        case spdlog::level::critical:
            return "critical";
        case spdlog::level::off:
            return "off";
        default:
            return "unknown";
    }
}

std::vector<std::string> enabled_modules(const ApplicationConfig& config) {
    std::vector<std::string> modules;

    if (config.cache.enabled)
        modules.push_back("cache");
    if (config.tlb.enabled)
        modules.push_back("tlb");
    if (config.rob.enabled)
        modules.push_back("rob");
    if (config.bht.enabled)
        modules.push_back("bht");
    if (config.ras.enabled)
        modules.push_back("ras");
    if (config.exec_ports.enabled)
        modules.push_back("exec-ports");
    if (config.uops_cache.enabled)
        modules.push_back("uops-cache");
    if (config.btb.enabled)
        modules.push_back("btb");
    if (config.s2l_fwd.enabled)
        modules.push_back("stlf");
    if (config.write_buffer.enabled)
        modules.push_back("write-buffer");

    return modules;
}

void print_startup_banner(std::ostream& stream, const ApplicationConfig& config) {
    const auto modules = enabled_modules(config);

    std::string modules_text;

    for (size_t i = 0; i < modules.size(); ++i) {
        if (i != 0) {
            modules_text += ", ";
        }

        modules_text += modules[i];
    }

    if (modules_text.empty()) {
        modules_text = "none";
    }

    constexpr bool kUseAnsiColor = true;

    constexpr std::string_view kReset  = "\033[0m";
    constexpr std::string_view kDim    = "\033[2m";
    constexpr std::string_view kCyan   = "\033[36m";
    constexpr std::string_view kBlue   = "\033[34m";
    constexpr std::string_view kGreen  = "\033[32m";
    constexpr std::string_view kYellow = "\033[33m";
    constexpr std::string_view kBold   = "\033[1m";

    const auto write_raw_line = [&](std::string_view line, std::string_view color = "") {
        if constexpr (kUseAnsiColor) {
            stream << color << line << kReset << '\n';
        } else {
            stream << line << '\n';
        }
    };

    const auto write_banner_line = [&](const std::string& text, std::string_view color = "") {
        if constexpr (kUseAnsiColor) {
            stream << color;
        }

        print_banner_line(stream, text);

        if constexpr (kUseAnsiColor) {
            stream << kReset;
        }
    };

    // clang-format off
    write_raw_line("╭──────────────────────────────────────────────────────────────────────────────╮", kBlue);

    write_banner_line(center_text("M.A.R.S. / MICROARCHITECTURE RECONNAISSANCE SYSTEM", kBannerInnerWidth), kBold);
    write_raw_line("├──────────────────────────────────────────────────────────────────────────────┤", kBlue);
    write_banner_line("");

    constexpr std::array<const char*, 7> logo = {
        " __  __        _        ____       ____",
        "|  \\/  |      / \\      |  _ \\     / ___|",
        "| |\\/| |     / _ \\     | |_) |    \\___ \\",
        " | |  | |    / ___ \\    |  _ <      ___) |",
        "|_|  |_|   /_/   \\_\\   |_| \\_\\    |____/",
        "                                      ",
        "        MICROARCHITECTURE RECON SYSTEM",
    };

    for (const char* line : logo) {
        write_banner_line(center_text(line, kBannerInnerWidth), kCyan);
    }

    write_banner_line("");
    write_banner_line(center_text("MICROARCHITECTURE ANALYSIS & RECONNAISSANCE", kBannerInnerWidth), kBold);
    write_banner_line(center_text("active probing | timing analysis | hardware discovery", kBannerInnerWidth), kDim);
    write_banner_line("");

    write_raw_line("├──────────────────────────────────────────────────────────────────────────────┤", kBlue);

    write_banner_line(
        std::string("  [LOG]    level=") + std::string(log_level_name(config.logging.level)) +
            " | summary=" + std::string(config.print_summary ? "on" : "off"),
        kGreen
    );

    write_banner_line(
        std::string("  [OUTPUT] console=on | file=") +
            (config.logging.log_file.empty() ? std::string("off") : config.logging.log_file),
        kGreen
    );

    write_banner_line(std::string("  [MODULES] ") + modules_text, kCyan);
    write_banner_line("  [STATUS] measurement pipeline armed", kYellow);
    write_banner_line("");

    write_banner_line(
        center_text("[ M.A.R.S. | SCANNING TARGET MICROARCHITECTURE ]", kBannerInnerWidth),
        kBold
    );

    write_raw_line("╰──────────────────────────────────────────────────────────────────────────────╯", kBlue);
    stream << '\n';
    // clang-format on
}

}  // namespace

int execute(const ApplicationConfig& config) {
    infra::Logger logger{config.logging};

    try {
        if (config.logging.console_output) {
            print_startup_banner(std::cout, config);
        }

        core::MeasurerRegistry registry{};
        if (config.cache.enabled) {
            registry.register_measurer(std::make_unique<cache::CacheMeasurer>(config.cache));
        }
        if (config.tlb.enabled) {
            registry.register_measurer(std::make_unique<tlb::TlbMeasurer>(config.tlb));
        }
        if (config.rob.enabled) {
            registry.register_measurer(std::make_unique<rob::RobMeasurer>(config.rob));
        }
        if (config.bht.enabled) {
            registry.register_measurer(std::make_unique<branch_history_table::BranchHistoryTableMeasurer>(config.bht));
        }
        if (config.ras.enabled) {
            registry.register_measurer(std::make_unique<return_address_stack::ReturnAddressStackMeasurer>(config.ras));
        }
        if (config.exec_ports.enabled) {
            registry.register_measurer(std::make_unique<exec_ports::ExecPortsMeasurer>(config.exec_ports));
        }
        if (config.uops_cache.enabled) {
            registry.register_measurer(std::make_unique<uops_cache::UopsCacheMeasurer>(config.uops_cache));
        }
        if (config.btb.enabled) {
            registry.register_measurer(std::make_unique<branch_target_buffer::BranchTargetBufferMeasurer>(config.btb));
        }
        if (config.s2l_fwd.enabled) {
            registry.register_measurer(
                std::make_unique<store_to_load_forwarding::StoreToLoadForwardingMeasurer>(config.s2l_fwd)
            );
        }
        if (config.write_buffer.enabled) {
            registry.register_measurer(std::make_unique<write_buffer::WriteBufferMeasurer>(config.write_buffer));
        }

        core::ProbeService probe_service{std::move(registry)};
        const auto& data = probe_service.run();

        if (config.print_summary) {
            core::SummaryPrinter::print(std::cout, data);
        }
        return 0;
    } catch (const platform::PermissionError& error) {
        SPDLOG_ERROR("Permission error: {}", error.what());
        SPDLOG_ERROR("Retry with sudo or disable strict environment flags");
        return 1;
    } catch (const std::exception& error) {
        SPDLOG_ERROR("Fatal error: {}", error.what());
        return 1;
    }
}

}  // namespace silicon_probe::app
