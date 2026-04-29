#include "core/measurer_registry.hpp"
#include "core/probe_service.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace silicon_probe::core {
namespace {

class TableMeasurer final : public Measurer {
public:
    TableMeasurer(std::string_view name,
                  bool available,
                  std::vector<std::string>* calls,
                  std::optional<size_t> rob_size,
                  bool should_throw = false)
        : name_(name)
        , available_(available)
        , calls_(calls)
        , rob_size_(rob_size)
        , should_throw_(should_throw) {}

    std::string_view name() const noexcept override {
        return name_;
    }

    bool is_available() const noexcept override {
        return available_;
    }

    void measure(shared_types::CpuInfoData& data) override {
        if (calls_ != nullptr) {
            calls_->push_back(name_);
        }
        if (rob_size_) {
            data.rob_size = *rob_size_;
        }
        if (should_throw_) {
            throw std::runtime_error("boom");
        }
    }

private:
    std::string name_;
    bool available_;
    std::vector<std::string>* calls_;
    std::optional<size_t> rob_size_;
    bool should_throw_;
};

struct RegistryCase {
    const char* name;
    bool available;
    size_t expected_registered_count;
    bool expect_find;
};

TEST(CoreTableTest, RegistersMeasurersFromTable) {
    const std::vector<RegistryCase> cases{
        {"available_measurer", true, 1, true},
        {"unavailable_measurer", false, 0, false},
    };

    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.name);
        MeasurerRegistry registry;
        registry.register_measurer(std::make_unique<TableMeasurer>("cache", test_case.available, nullptr, std::nullopt));

        EXPECT_EQ(registry.measurers().size(), test_case.expected_registered_count);
        EXPECT_EQ(registry.find("cache") != nullptr, test_case.expect_find);
    }
}

struct ProbeCase {
    const char* name;
    std::vector<bool> should_throw;
    std::vector<bool> available;
    std::vector<size_t> rob_sizes;
    std::vector<std::string> expected_calls;
    std::optional<size_t> expected_final_rob_size;
};

TEST(CoreTableTest, RunsProbeServiceScenariosFromTable) {
    const std::vector<ProbeCase> cases{
        {
            "all_measurers_succeed",
            {false, false},
            {true, true},
            {16, 32},
            {"first", "second"},
            32U,
        },
        {
            "exception_does_not_stop_pipeline",
            {true, false},
            {true, true},
            {16, 64},
            {"first", "second"},
            64U,
        },
        {
            "unavailable_measurer_is_skipped",
            {false, false},
            {false, true},
            {16, 48},
            {"second"},
            48U,
        },
    };

    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.name);

        std::vector<std::string> calls;
        MeasurerRegistry registry;
        registry.register_measurer(std::make_unique<TableMeasurer>(
            "first", test_case.available[0], &calls, test_case.rob_sizes[0], test_case.should_throw[0]));
        registry.register_measurer(std::make_unique<TableMeasurer>(
            "second", test_case.available[1], &calls, test_case.rob_sizes[1], test_case.should_throw[1]));

        ProbeService service{std::move(registry)};
        const shared_types::CpuInfoData& data = service.run();

        EXPECT_EQ(calls, test_case.expected_calls);
        EXPECT_EQ(data.rob_size, test_case.expected_final_rob_size);
        EXPECT_EQ(&service.data(), &data);
    }
}

} // namespace
} // namespace silicon_probe::core
