#include "silicon_probe/app/config.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

std::string to_ascii_lower_copy(std::string_view value) {
    std::string result(value);

    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    return result;
}

std::runtime_error config_error(const std::string& path, const std::string& message) {
    return std::runtime_error("Invalid config at '" + path + "': " + message);
}

YAML::Node require_node(const YAML::Node& parent, const char* key, const std::string& path) {
    const YAML::Node node{parent[key]};
    if (!node) {
        throw config_error(path, "missing required node");
    }

    return node;
}

std::string read_scalar(const YAML::Node& node, const std::string& path) {
    if (!node || !node.IsScalar()) {
        throw config_error(path, "expected scalar value");
    }

    try {
        return node.as<std::string>();
    } catch (const YAML::Exception& error) {
        throw config_error(path, error.what());
    }
}

size_t parse_size_scalar(const YAML::Node& node, const std::string& path) {
    const std::string raw = to_ascii_lower_copy(read_scalar(node, path));
    if (raw.empty()) {
        throw config_error(path, "expected positive integer");
    }

    std::string number_part = raw;
    size_t multiplier = 1;

    const struct Suffix {
        const char* name;
        size_t multiplier;
    } suffixes[] = {
        {"gib", 1024ULL * 1024ULL * 1024ULL},
        {"gb", 1024ULL * 1024ULL * 1024ULL},
        {"g", 1024ULL * 1024ULL * 1024ULL},
        {"mib", 1024ULL * 1024ULL},
        {"mb", 1024ULL * 1024ULL},
        {"m", 1024ULL * 1024ULL},
        {"kib", 1024ULL},
        {"kb", 1024ULL},
        {"k", 1024ULL},
        {"b", 1ULL},
    };

    for (const auto& suffix : suffixes) {
        const std::string_view suffix_name(suffix.name);
        if (raw.size() > suffix_name.size()
            && raw.compare(raw.size() - suffix_name.size(), suffix_name.size(), suffix.name) == 0) {
            number_part = raw.substr(0, raw.size() - suffix_name.size());
            multiplier = suffix.multiplier;
            break;
        }
    }

    try {
        return static_cast<size_t>(std::stoull(number_part)) * multiplier;
    } catch (const std::exception&) {
        throw config_error(path, "expected positive integer, got '" + raw + "'");
    }
}

unsigned int parse_unsigned_scalar(const YAML::Node& node, const std::string& path) {
    return static_cast<unsigned int>(parse_size_scalar(node, path));
}

bool parse_bool_scalar(const YAML::Node& node, const std::string& path) {
    if (!node || !node.IsScalar()) {
        throw config_error(path, "expected boolean value");
    }

    try {
        return node.as<bool>();
    } catch (const YAML::Exception& error) {
        throw config_error(path, error.what());
    }
}

int parse_int_scalar(const YAML::Node& node, const std::string& path) {
    if (!node || !node.IsScalar()) {
        throw config_error(path, "expected integer value");
    }

    try {
        return node.as<int>();
    } catch (const YAML::Exception& error) {
        throw config_error(path, error.what());
    }
}

void apply_levels(const YAML::Node& probe_node, silicon_probe::cache::CacheMeasurer::Config& config) {
    const YAML::Node levels{probe_node["levels"]};

    if (!levels) {
        return;
    }

    if (!levels.IsSequence()) {
        throw config_error("probe.levels", "expected sequence");
    }

    config.levels.reset();

    for (size_t index = 0; index < levels.size(); ++index) {
        const std::string level = to_ascii_lower_copy(read_scalar(levels[index], "probe.levels[" + std::to_string(index) + "]"));
        if (level == "l1" || level == "l1d") {
            config.levels.set(0);
            continue;
        }
        if (level == "l2") {
            config.levels.set(1);
            continue;
        }
        if (level == "l3") {
            config.levels.set(2);
            continue;
        }

        throw config_error("probe.levels[" + std::to_string(index) + "]", "unsupported cache level '" + level + "'");
    }
}

void apply_limits(const YAML::Node& probe_node, silicon_probe::cache::CacheMeasurer::Config& config) {
    const YAML::Node limits{probe_node["limits"]};
    if (!limits) {
        return;
    }

    if (const YAML::Node node = limits["l1_max"]) {
        config.l1_max = parse_size_scalar(node, "probe.limits.l1_max");
    }

    if (const YAML::Node node = limits["l2_max"]) {
        config.l2_max = parse_size_scalar(node, "probe.limits.l2_max");
    }

    if (const YAML::Node node = limits["l3_max"]) {
        config.l3_max = parse_size_scalar(node, "probe.limits.l3_max");
    }
}

void apply_measurement(const YAML::Node& probe_node, silicon_probe::cache::CacheMeasurer::Config& config) {
    const YAML::Node measurement{probe_node["measurement"]};
    if (!measurement) {
        return;
    }

    if (const YAML::Node node = measurement["cache_min_lines"]) {
        config.cache_min_lines = parse_size_scalar(node, "probe.measurement.cache_min_lines");
    }

    if (const YAML::Node node = measurement["seed"]) {
        config.seed = parse_unsigned_scalar(node, "probe.measurement.seed");
    }

    if (const YAML::Node node = measurement["warmup_iterations"]) {
        config.warmup_iterations = parse_size_scalar(node, "probe.measurement.warmup_iterations");
    }

    if (const YAML::Node node = measurement["precision"]) {
        config.precision = parse_size_scalar(node, "probe.measurement.precision");
    }

    if (const YAML::Node node = measurement["target_accesses"]) {
        config.target_accesses = parse_size_scalar(node, "probe.measurement.target_accesses");
    }

    if (const YAML::Node node = measurement["min_iterations"]) {
        config.min_iterations = parse_size_scalar(node, "probe.measurement.min_iterations");
    }

    if (const YAML::Node node = measurement["max_iterations"]) {
        config.max_iterations = parse_size_scalar(node, "probe.measurement.max_iterations");
    }
}

void apply_environment(const YAML::Node& probe_node, silicon_probe::cache::CacheMeasurer::Config& config) {
    const YAML::Node environment{probe_node["environment"]};
    if (!environment) {
        return;
    }

    if (const YAML::Node node = environment["realtime_priority"]) {
        config.environment.realtime_priority = parse_bool_scalar(node, "probe.environment.realtime_priority");
    }

    if (const YAML::Node node = environment["lock_frequency"]) {
        config.environment.lock_frequency = parse_bool_scalar(node, "probe.environment.lock_frequency");
    }

    if (const YAML::Node node = environment["bind_cpu"]) {
        if (node.IsNull()) {
            config.environment.cpu.reset();
        } else {
            config.environment.cpu = parse_int_scalar(node, "probe.environment.bind_cpu");
        }
    }
}

void apply_environment(const YAML::Node& probe_node, silicon_probe::rob::RobMeasurer::Config& config) {
    const YAML::Node environment{probe_node["environment"]};
    if (!environment) {
        return;
    }

    if (const YAML::Node node = environment["realtime_priority"]) {
        config.environment.realtime_priority = parse_bool_scalar(node, "probe.environment.realtime_priority");
    }

    if (const YAML::Node node = environment["lock_frequency"]) {
        config.environment.lock_frequency = parse_bool_scalar(node, "probe.environment.lock_frequency");
    }

    if (const YAML::Node node = environment["bind_cpu"]) {
        if (node.IsNull()) {
            config.environment.cpu.reset();
        } else {
            config.environment.cpu = parse_int_scalar(node, "probe.environment.bind_cpu");
        }
    }
}

void apply_environment(const YAML::Node& probe_node, silicon_probe::branch_history_table::BranchHistoryTableMeasurer::Config& config) {
    const YAML::Node environment{probe_node["environment"]};
    if (!environment) {
        return;
    }

    if (const YAML::Node node = environment["realtime_priority"]) {
        config.environment.realtime_priority = parse_bool_scalar(node, "probe.environment.realtime_priority");
    }

    if (const YAML::Node node = environment["lock_frequency"]) {
        config.environment.lock_frequency = parse_bool_scalar(node, "probe.environment.lock_frequency");
    }

    if (const YAML::Node node = environment["bind_cpu"]) {
        if (node.IsNull()) {
            config.environment.cpu.reset();
        } else {
            config.environment.cpu = parse_int_scalar(node, "probe.environment.bind_cpu");
        }
    }
}

void apply_environment(const YAML::Node& probe_node, silicon_probe::return_address_stack::ReturnAddressStackMeasurer::Config& config) {
    const YAML::Node environment{probe_node["environment"]};
    if (!environment) {
        return;
    }

    if (const YAML::Node node = environment["realtime_priority"]) {
        config.environment.realtime_priority = parse_bool_scalar(node, "probe.environment.realtime_priority");
    }

    if (const YAML::Node node = environment["lock_frequency"]) {
        config.environment.lock_frequency = parse_bool_scalar(node, "probe.environment.lock_frequency");
    }

    if (const YAML::Node node = environment["bind_cpu"]) {
        if (node.IsNull()) {
            config.environment.cpu.reset();
        } else {
            config.environment.cpu = parse_int_scalar(node, "probe.environment.bind_cpu");
        }
    }
}

void apply_environment(const YAML::Node& probe_node, silicon_probe::exec_ports::ExecPortsMeasurer::Config& config) {
    const YAML::Node environment{probe_node["environment"]};
    if (!environment) {
        return;
    }

    if (const YAML::Node node = environment["realtime_priority"]) {
        config.environment.realtime_priority = parse_bool_scalar(node, "probe.environment.realtime_priority");
    }

    if (const YAML::Node node = environment["lock_frequency"]) {
        config.environment.lock_frequency = parse_bool_scalar(node, "probe.environment.lock_frequency");
    }

    if (const YAML::Node node = environment["bind_cpu"]) {
        if (node.IsNull()) {
            config.environment.cpu.reset();
        } else {
            config.environment.cpu = parse_int_scalar(node, "probe.environment.bind_cpu");
        }
    }
}

silicon_probe::cache::CacheMeasurer::Config load_cache_config_impl(const std::filesystem::path& path) {
    YAML::Node root{};
    try {
        root = YAML::LoadFile(path.string());
    } catch (const YAML::Exception& error) {
        throw std::runtime_error("Failed to load configuration file '" + path.string() + "': " + error.what());
    }

    const YAML::Node probe{require_node(root, "probe", "probe")};

    auto config = silicon_probe::cache::CacheMeasurer::Config{};
    
    apply_levels(probe, config);
    apply_limits(probe, config);
    apply_measurement(probe, config);
    apply_environment(probe, config);

    return config;
}

// TODO: make config parse for rob
silicon_probe::rob::RobMeasurer::Config load_rob_config_impl(const std::filesystem::path& path) {
    YAML::Node root{};
    try {
        root = YAML::LoadFile(path.string());
    } catch (const YAML::Exception& error) {
        throw std::runtime_error("Failed to load configuration file '" + path.string() + "': " + error.what());
    }

    const YAML::Node probe{require_node(root, "probe", "probe")};

    auto config = silicon_probe::rob::RobMeasurer::Config{};
    
    //apply_levels(probe, config);
    //apply_limits(probe, config);
    //apply_measurement(probe, config);
    apply_environment(probe, config);

    return config;
}

// TODO: make config parse for bht
silicon_probe::branch_history_table::BranchHistoryTableMeasurer::Config load_bht_config_impl(const std::filesystem::path& path) {
    YAML::Node root{};
    try {
        root = YAML::LoadFile(path.string());
    } catch (const YAML::Exception& error) {
        throw std::runtime_error("Failed to load configuration file '" + path.string() + "': " + error.what());
    }

    const YAML::Node probe{require_node(root, "probe", "probe")};

    auto config = silicon_probe::branch_history_table::BranchHistoryTableMeasurer::Config{};
    
    //apply_levels(probe, config);
    //apply_limits(probe, config);
    //apply_measurement(probe, config);
    apply_environment(probe, config);

    return config;
}

// TODO: make config parse for ras
silicon_probe::return_address_stack::ReturnAddressStackMeasurer::Config load_ras_config_impl(const std::filesystem::path& path) {
    YAML::Node root{};
    try {
        root = YAML::LoadFile(path.string());
    } catch (const YAML::Exception& error) {
        throw std::runtime_error("Failed to load configuration file '" + path.string() + "': " + error.what());
    }

    const YAML::Node probe{require_node(root, "probe", "probe")};

    auto config = silicon_probe::return_address_stack::ReturnAddressStackMeasurer::Config{};
    
    //apply_levels(probe, config);
    //apply_limits(probe, config);
    //apply_measurement(probe, config);
    apply_environment(probe, config);

    return config;
}

silicon_probe::exec_ports::ExecPortsMeasurer::Config load_exec_ports_config_impl(const std::filesystem::path& path) {
    YAML::Node root{};
    try {
        root = YAML::LoadFile(path.string());
    } catch (const YAML::Exception& error) {
        throw std::runtime_error("Failed to load configuration file '" + path.string() + "': " + error.what());
    }

    const YAML::Node probe{require_node(root, "probe", "probe")};

    auto config = silicon_probe::exec_ports::ExecPortsMeasurer::Config{};
    
    //apply_levels(probe, config);
    //apply_limits(probe, config);
    //apply_measurement(probe, config);
    apply_environment(probe, config);

    return config;
}


} // namespace

namespace silicon_probe::app::detail {

cache::CacheMeasurer::Config load_cache_config(const std::filesystem::path& path) {
    return load_cache_config_impl(path);
}

rob::RobMeasurer::Config load_rob_config(const std::filesystem::path& path) {
    return load_rob_config_impl(path);
}

branch_history_table::BranchHistoryTableMeasurer::Config load_bht_config(const std::filesystem::path& path) {
    return load_bht_config_impl(path);
}

return_address_stack::ReturnAddressStackMeasurer::Config load_ras_config(const std::filesystem::path& path) {
    return load_ras_config_impl(path);
}

exec_ports::ExecPortsMeasurer::Config load_exec_ports_config(const std::filesystem::path& path) {
    return load_exec_ports_config_impl(path);
}
} // namespace silicon_probe::app::detail
