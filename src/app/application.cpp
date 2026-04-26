#include "app/application.hpp"

#include "measurement/cache/cache_measurer.hpp"
#include "measurement/rob/rob_measurer.hpp"
#include "measurement/core/measurer_registry.hpp"
#include "measurement/core/probe_service.hpp"
#include "measurement/core/summary_printer.hpp"
#include "infra/logging.hpp"
#include "platform/os_errors.hpp"

#include <iostream>
#include <memory>

namespace silicon_probe::app {

int execute(const ApplicationConfig& config) {
    infra::Logger logger{config.logging};

    try {
        core::MeasurerRegistry registry{};
        if (config.cache.enabled) {
            registry.register_measurer(std::make_unique<cache::CacheMeasurer>(config.cache));
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
            registry.register_measurer(std::make_unique<store_to_load_forwarding::StoreToLoadForwardingMeasurer>(config.s2l_fwd));
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

} // namespace silicon_probe::app
