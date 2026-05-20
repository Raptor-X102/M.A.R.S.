#pragma once

#include <string_view>

#include "shared_types/cpu_info_data.hpp"

namespace silicon_probe::core {

/**
 * @brief Base interface for all microarchitecture benchmarks.
 *
 * A measurer owns one experiment.
 * It can report its name, tell if it is supported on the current machine,
 * and write its final result into @ref shared_types::CpuInfoData.
 */
class Measurer {
   public:
    virtual ~Measurer() = default;

    /**
     * @brief Returns the stable benchmark name.
     * @return Short identifier used in config, logs, and summary output.
     */
    virtual std::string_view name() const noexcept = 0;

    /**
     * @brief Reports if this benchmark can run on the current platform.
     * @return `true` when the measurer is available.
     */
    virtual bool is_available() const noexcept { return true; }

    /**
     * @brief Runs the benchmark and stores its final estimate.
     * @param data Output structure updated in place.
     */
    virtual void measure(shared_types::CpuInfoData& data) = 0;
};

}  // namespace silicon_probe::core
