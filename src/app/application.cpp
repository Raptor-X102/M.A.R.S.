#include "silicon_probe/app/application.hpp"

#include "silicon_probe/cache/cache_measurer.hpp"
#include "silicon_probe/core/measurer_registry.hpp"
#include "silicon_probe/core/probe_service.hpp"
#include "silicon_probe/core/summary_printer.hpp"
#include "silicon_probe/infra/logging.hpp"
#include "silicon_probe/platform/os_errors.hpp"

#include <iostream>
#include <memory>

namespace silicon_probe::app {

int execute(const ApplicationConfig& config) {
    infra::Logger logger{config.logging};

    try {
        core::MeasurerRegistry registry{};
        registry.register_measurer(std::make_unique<cache::CacheMeasurer>(config.cache));

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

} // namespace silicon_probe::app
