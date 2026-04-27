#include "app/config.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using silicon_probe::platform::MeasurementEnvironmentOptions;

std::string to_ascii_lower_copy(std::string_view value) {
    std::string result(value);

    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    return result;
}

std::string normalize_identifier(std::string_view value) {
    std::string result = to_ascii_lower_copy(value);

    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        if (ch == ' ' || ch == '-') {
            return '_';
        }
        return static_cast<char>(ch);
    });

    return result;
}

std::string sanitize_numeric_text(std::string value) {
    value.erase(
        std::remove_if(value.begin(), value.end(), [](unsigned char ch) { return ch == '_' || ch == '\'' || std::isspace(ch) != 0; }),
        value.end());
    return value;
}

std::string trim_ascii_whitespace(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) { return std::isspace(ch) != 0; });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) { return std::isspace(ch) != 0; }).base();

    if (first >= last) {
        return {};
    }

    return std::string(first, last);
}

std::runtime_error config_error(const std::string& path, const std::string& message) {
    return std::runtime_error("Invalid config at '" + path + "': " + message);
}

void ensure_mapping(const YAML::Node& node, const std::string& path) {
    if (node && !node.IsMap()) {
        throw config_error(path, "expected mapping");
    }
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
    const std::string raw = sanitize_numeric_text(to_ascii_lower_copy(read_scalar(node, path)));
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
        if (raw.size() > suffix_name.size() && raw.compare(raw.size() - suffix_name.size(), suffix_name.size(), suffix.name) == 0) {
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
    const size_t value = parse_size_scalar(node, path);
    if (value > std::numeric_limits<unsigned int>::max()) {
        throw config_error(path, "value is too large");
    }
    return static_cast<unsigned int>(value);
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

double parse_double_scalar(const YAML::Node& node, const std::string& path) {
    if (!node || !node.IsScalar()) {
        throw config_error(path, "expected floating-point value");
    }

    try {
        return node.as<double>();
    } catch (const YAML::Exception& error) {
        throw config_error(path, error.what());
    }
}

template <typename Callback> void with_mapping(const YAML::Node& parent, const char* key, const std::string& path, Callback&& callback) {
    const YAML::Node node{parent[key]};
    if (!node) {
        return;
    }

    const std::string child_path = path + "." + key;
    ensure_mapping(node, child_path);
    callback(node, child_path);
}

template <typename Callback>
void with_optional_node(const YAML::Node& parent, const char* key, const std::string& path, Callback&& callback) {
    const YAML::Node node{parent[key]};
    if (!node) {
        return;
    }

    callback(node, path + "." + key);
}

struct LoadedConfigDocument {
    YAML::Node root;
    YAML::Node common;
    YAML::Node benchmarks;
    bool legacy_probe = false;
};

LoadedConfigDocument load_document(const std::filesystem::path& path) {
    YAML::Node root{};
    try {
        root = YAML::LoadFile(path.string());
    } catch (const YAML::Exception& error) {
        throw std::runtime_error("Failed to load configuration file '" + path.string() + "': " + error.what());
    }

    if (!root || !root.IsMap()) {
        throw std::runtime_error("Failed to load configuration file '" + path.string() + "': root node must be a mapping");
    }

    const bool has_legacy_probe = static_cast<bool>(root["probe"]);
    const bool has_common = static_cast<bool>(root["common"]);
    const bool has_benchmarks = static_cast<bool>(root["benchmarks"]);

    if (has_legacy_probe && (has_common || has_benchmarks)) {
        throw config_error("root", "use either legacy 'probe' or the new 'common'/'benchmarks' layout");
    }

    if (!has_legacy_probe && !has_benchmarks) {
        throw config_error("root", "missing 'benchmarks' section");
    }

    LoadedConfigDocument document{};
    document.root = root;
    document.common = root["common"];
    document.benchmarks = root["benchmarks"];
    document.legacy_probe = has_legacy_probe;

    ensure_mapping(document.common, "common");
    ensure_mapping(document.benchmarks, "benchmarks");
    if (document.legacy_probe) {
        ensure_mapping(root["probe"], "probe");
    }

    return document;
}

void apply_environment_overrides(const YAML::Node& environment_node, const std::string& path, MeasurementEnvironmentOptions& environment) {
    if (!environment_node) {
        return;
    }

    ensure_mapping(environment_node, path);

    with_optional_node(environment_node, "realtime_priority", path, [&](const YAML::Node& node, const std::string& node_path) {
        environment.realtime_priority = parse_bool_scalar(node, node_path);
    });

    with_optional_node(environment_node, "lock_frequency", path, [&](const YAML::Node& node, const std::string& node_path) {
        environment.lock_frequency = parse_bool_scalar(node, node_path);
    });

    with_optional_node(environment_node, "bind_cpu", path, [&](const YAML::Node& node, const std::string& node_path) {
        if (node.IsNull()) {
            environment.cpu.reset();
        } else {
            environment.cpu = parse_int_scalar(node, node_path);
        }
    });
}

MeasurementEnvironmentOptions build_environment(const YAML::Node& common_environment, const YAML::Node& benchmark_environment,
                                                const std::string& benchmark_path) {
    MeasurementEnvironmentOptions environment{};
    apply_environment_overrides(common_environment, "common.environment", environment);
    apply_environment_overrides(benchmark_environment, benchmark_path + ".environment", environment);
    return environment;
}

using InstrType = silicon_probe::platform::arch::InstrType;

struct InstructionSpec {
    const char* key;
    InstrType type;
    const char* default_name;
};

const auto& instruction_specs() {
    static const std::array<InstructionSpec, 21> specs{{
        {"nop", InstrType::NOP, "nop"},
        {"add_imm1", InstrType::ADD_IMM1, "add imm1"},
        {"sub_imm1", InstrType::SUB_IMM1, "sub imm1"},
        {"mul_float", InstrType::MUL_FLOAT, "mul float"},
        {"add_reg", InstrType::ADD_REG, "add reg"},
        {"mov_imm", InstrType::MOV_IMM, "mov imm"},
        {"xor_zero", InstrType::XOR_ZERO, "xor zero"},
        {"inc", InstrType::INC, "inc"},
        {"dec", InstrType::DEC, "dec"},
        {"sub_reg", InstrType::SUB_REG, "sub reg"},
        {"imul_reg", InstrType::IMUL_REG, "imul reg"},
        {"and_reg", InstrType::AND_REG, "and reg"},
        {"or_reg", InstrType::OR_REG, "or reg"},
        {"shl_imm1", InstrType::SHL_IMM1, "shl imm1"},
        {"shr_imm1", InstrType::SHR_IMM1, "shr imm1"},
        {"not", InstrType::NOT, "not"},
        {"neg", InstrType::NEG, "neg"},
        {"load_from_rcx", InstrType::LOAD_FROM_RCX, "load from rcx"},
        {"store_to_rcx", InstrType::STORE_TO_RCX, "store to rcx"},
        {"load_from_rdx", InstrType::LOAD_FROM_RDX, "load from rdx"},
        {"store_to_rdx", InstrType::STORE_TO_RDX, "store to rdx"},
    }};

    return specs;
}

const InstructionSpec& instruction_spec_for(InstrType type) {
    const auto& specs = instruction_specs();
    const auto it = std::find_if(specs.begin(), specs.end(), [type](const InstructionSpec& spec) { return spec.type == type; });

    if (it == specs.end()) {
        throw std::invalid_argument("Unknown instruction enum value");
    }

    return *it;
}

InstrType parse_instruction_type(const YAML::Node& node, const std::string& path) {
    const std::string raw = trim_ascii_whitespace(read_scalar(node, path));
    if (raw.empty()) {
        throw config_error(path, "expected instruction type");
    }

    const bool looks_numeric = std::all_of(raw.begin(), raw.end(), [](unsigned char ch) { return std::isdigit(ch) != 0 || ch == '-'; });

    if (looks_numeric) {
        try {
            const int value = std::stoi(raw);
            const auto& specs = instruction_specs();
            if (value < 0 || static_cast<size_t>(value) >= specs.size()) {
                throw config_error(path, "instruction type index out of range");
            }
            return static_cast<InstrType>(value);
        } catch (const std::invalid_argument&) {
            throw config_error(path, "expected valid instruction type");
        } catch (const std::out_of_range&) {
            throw config_error(path, "instruction type index out of range");
        }
    }

    const std::string normalized = normalize_identifier(raw);
    const auto& specs = instruction_specs();
    const auto it = std::find_if(specs.begin(), specs.end(), [&](const InstructionSpec& spec) { return normalized == spec.key; });

    if (it == specs.end()) {
        throw config_error(path, "unsupported instruction type '" + raw + "'");
    }

    return it->type;
}

template <typename InstructionData> void parse_instruction_data(const YAML::Node& node, const std::string& path, InstructionData& target) {
    if (!node) {
        return;
    }

    if (node.IsMap()) {
        const YAML::Node type_node{node["type"]};
        if (!type_node) {
            throw config_error(path + ".type", "missing required node");
        }

        target.instr_type = parse_instruction_type(type_node, path + ".type");

        const YAML::Node name_node{node["name"]};
        if (name_node) {
            target.instr_name = read_scalar(name_node, path + ".name");
        } else {
            target.instr_name = instruction_spec_for(target.instr_type).default_name;
        }
        return;
    }

    target.instr_type = parse_instruction_type(node, path);
    target.instr_name = instruction_spec_for(target.instr_type).default_name;
}

class AbstractBenchmarkConfigParser {
  protected:
    explicit AbstractBenchmarkConfigParser(std::string key) : key_(std::move(key)) {}

    const std::string& key() const noexcept { return key_; }

    YAML::Node section_node(const LoadedConfigDocument& document) const {
        if (document.legacy_probe) {
            if (key_ == "cache") {
                return document.root["probe"];
            }
            return {};
        }

        if (!document.benchmarks) {
            return {};
        }

        return document.benchmarks[key_];
    }

    std::string section_path(const LoadedConfigDocument& document) const {
        if (document.legacy_probe && key_ == "cache") {
            return "probe";
        }
        return "benchmarks." + key_;
    }

    bool resolve_enabled(const LoadedConfigDocument& document, const YAML::Node& section) const {
        if (!section) {
            return false;
        }

        const std::string path = section_path(document);
        ensure_mapping(section, path);

        if (const YAML::Node enabled = section["enabled"]) {
            return parse_bool_scalar(enabled, path + ".enabled");
        }

        return true;
    }

  private:
    std::string key_;
};

template <typename ConfigT> class BenchmarkConfigParserBase : public AbstractBenchmarkConfigParser {
  public:
    explicit BenchmarkConfigParserBase(std::string key) : AbstractBenchmarkConfigParser(std::move(key)) {}

    ConfigT parse(const LoadedConfigDocument& document) const {
        ConfigT config{};
        const YAML::Node section = section_node(document);
        config.enabled = resolve_enabled(document, section);

        if (!config.enabled) {
            return config;
        }

        const std::string path = section_path(document);
        config.environment = build_environment(document.common ? document.common["environment"] : YAML::Node{},
                                               section ? section["environment"] : YAML::Node{}, path);

        if (!section) {
            throw config_error(path, "enabled benchmark requires a configuration section");
        }

        parse_specific(section, path, config);
        return config;
    }

  protected:
    virtual void parse_specific(const YAML::Node& section, const std::string& path, ConfigT& config) const = 0;
};

class CacheConfigParser final : public BenchmarkConfigParserBase<silicon_probe::cache::CacheMeasurer::Config> {
public:
    CacheConfigParser() : BenchmarkConfigParserBase("cache") {}

private:
    void parse_specific(const YAML::Node& section, const std::string& path,
                        silicon_probe::cache::CacheMeasurer::Config& config) const override {
        with_optional_node(section, "levels", path, [&](const YAML::Node& node, const std::string& node_path) {
            if (!node.IsSequence()) {
                throw config_error(node_path, "expected sequence");
            }

            config.levels.reset();

            for (size_t index = 0; index < node.size(); ++index) {
                const std::string level = normalize_identifier(read_scalar(node[index], node_path + "[" + std::to_string(index) + "]"));
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

                throw config_error(node_path + "[" + std::to_string(index) + "]", "unsupported cache level '" + level + "'");
            }
        });

        with_mapping(section, "limits", path, [&](const YAML::Node& limits, const std::string& limits_path) {
            with_optional_node(limits, "l1_max", limits_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.l1_max = parse_size_scalar(node, node_path);
            });
            with_optional_node(limits, "l2_max", limits_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.l2_max = parse_size_scalar(node, node_path);
            });
            with_optional_node(limits, "l3_max", limits_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.l3_max = parse_size_scalar(node, node_path);
            });
        });

        with_mapping(section, "measurement", path, [&](const YAML::Node& measurement, const std::string& measurement_path) {
            with_optional_node(measurement, "cache_min_lines", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.cache_min_lines = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "seed", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.seed = parse_unsigned_scalar(node, node_path);
            });
            with_optional_node(measurement, "warmup_iterations", measurement_path,
                               [&](const YAML::Node& node, const std::string& node_path) {
                                   config.warmup_iterations = parse_size_scalar(node, node_path);
                               });
            with_optional_node(measurement, "precision", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.precision = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "target_accesses", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.target_accesses = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "min_iterations", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.min_iterations = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "max_iterations", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.max_iterations = parse_size_scalar(node, node_path);
            });
        });

        with_mapping(section, "detection", path, [&](const YAML::Node& detection, const std::string& detection_path) {
            with_optional_node(detection, "refinement_samples", detection_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.refinement_samples = parse_size_scalar(node, node_path);
            });
            with_optional_node(detection, "baseline_stability_threshold", detection_path,
                               [&](const YAML::Node& node, const std::string& node_path) {
                                   config.baseline_stability_threshold = parse_double_scalar(node, node_path);
                               });
            with_optional_node(detection, "decision_tolerance", detection_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.decision_tolerance = parse_double_scalar(node, node_path);
            });
            with_optional_node(detection, "l1_growth_factor", detection_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.l1_growth_factor = parse_double_scalar(node, node_path);
            });
            with_optional_node(detection, "l2_growth_factor", detection_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.l2_growth_factor = parse_double_scalar(node, node_path);
            });
            with_optional_node(detection, "l3_growth_factor", detection_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.l3_growth_factor = parse_double_scalar(node, node_path);
            });
            with_optional_node(detection, "l2_refinement_growth_multiplier", detection_path,
                               [&](const YAML::Node& node, const std::string& node_path) {
                                   config.l2_refinement_growth_multiplier = parse_double_scalar(node, node_path);
                               });
        });

        with_mapping(section, "miss_events", path, [&](const YAML::Node& miss, const std::string& miss_path) {
            with_optional_node(miss, "l1_miss_rate_threshold", miss_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.l1_miss_rate_threshold = parse_double_scalar(node, node_path);
            });
            with_optional_node(miss, "l2_miss_rate_threshold", miss_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.l2_miss_rate_threshold = parse_double_scalar(node, node_path);
            });
            with_optional_node(miss, "l3_miss_rate_threshold", miss_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.l3_miss_rate_threshold = parse_double_scalar(node, node_path);
            });
            with_optional_node(miss, "l1_miss_growth_factor", miss_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.l1_miss_growth_factor = parse_double_scalar(node, node_path);
            });
            with_optional_node(miss, "l2_miss_growth_factor", miss_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.l2_miss_growth_factor = parse_double_scalar(node, node_path);
            });
            with_optional_node(miss, "l3_miss_growth_factor", miss_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.l3_miss_growth_factor = parse_double_scalar(node, node_path);
            });
        });
    }
};

class TlbConfigParser final : public BenchmarkConfigParserBase<silicon_probe::tlb::TlbMeasurer::Config> {
  public:
    TlbConfigParser() : BenchmarkConfigParserBase("tlb") {}

  private:
    static silicon_probe::tlb::GrowthMode parse_growth_mode(const YAML::Node& node, const std::string& path) {
        const std::string mode = normalize_identifier(read_scalar(node, path));
        if (mode == "multiply" || mode == "mul" || mode == "geometric") {
            return silicon_probe::tlb::GrowthMode::Multiply;
        }
        if (mode == "add" || mode == "linear") {
            return silicon_probe::tlb::GrowthMode::Add;
        }
        throw config_error(path, "unsupported growth mode '" + mode + "'");
    }

    void parse_specific(const YAML::Node& section, const std::string& path,
                        silicon_probe::tlb::TlbMeasurer::Config& config) const override {
        with_mapping(section, "measurement", path, [&](const YAML::Node& measurement, const std::string& measurement_path) {
            with_optional_node(measurement, "min_pages", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.min_pages = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "max_pages", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.max_pages = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "pages_step", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.pages_step = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "growth_mode", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.growth_mode = parse_growth_mode(node, node_path);
            });
            with_optional_node(measurement, "iterations", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.iterations = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "repeats", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.repeats = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "warmup_rounds", measurement_path,
                               [&](const YAML::Node& node, const std::string& node_path) {
                                   config.warmup_rounds = parse_size_scalar(node, node_path);
                               });
            with_optional_node(measurement, "page_size", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.page_size_bytes = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "huge_pages", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.use_huge_pages = parse_bool_scalar(node, node_path);
            });
            with_optional_node(measurement, "disable_transparent_huge_pages", measurement_path,
                               [&](const YAML::Node& node, const std::string& node_path) {
                                   config.disable_transparent_huge_pages = parse_bool_scalar(node, node_path);
                               });
            with_optional_node(measurement, "lock_memory", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.lock_memory = parse_bool_scalar(node, node_path);
            });
            with_optional_node(measurement, "seed", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.seed = parse_unsigned_scalar(node, node_path);
            });
        });

        with_mapping(section, "detection", path, [&](const YAML::Node& detection, const std::string& detection_path) {
            with_optional_node(detection, "moving_average_window", detection_path,
                               [&](const YAML::Node& node, const std::string& node_path) {
                                   config.detection.moving_average_window = parse_size_scalar(node, node_path);
                               });
            with_optional_node(detection, "l1_growth_ratio", detection_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.detection.l1_growth_ratio = parse_double_scalar(node, node_path);
            });
            with_optional_node(detection, "l2_growth_ratio", detection_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.detection.l2_growth_ratio = parse_double_scalar(node, node_path);
            });
            with_optional_node(detection, "page_walk_growth_ratio", detection_path,
                               [&](const YAML::Node& node, const std::string& node_path) {
                                   config.detection.page_walk_growth_ratio = parse_double_scalar(node, node_path);
                               });
            with_optional_node(detection, "min_jump_cycles", detection_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.detection.min_jump_cycles = parse_double_scalar(node, node_path);
            });
            with_optional_node(detection, "page_walk_jump_cycles", detection_path,
                               [&](const YAML::Node& node, const std::string& node_path) {
                                   config.detection.page_walk_jump_cycles = parse_double_scalar(node, node_path);
                               });
            with_optional_node(detection, "sustain_points", detection_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.detection.sustain_points = parse_size_scalar(node, node_path);
            });
        });
    }
};

class RobConfigParser final : public BenchmarkConfigParserBase<silicon_probe::rob::RobMeasurer::Config> {
  public:
    RobConfigParser() : BenchmarkConfigParserBase("rob") {}

  private:
    void parse_specific(const YAML::Node& section, const std::string& path,
                        silicon_probe::rob::RobMeasurer::Config& config) const override {
        with_mapping(section, "measurement", path, [&](const YAML::Node& measurement, const std::string& measurement_path) {
            with_optional_node(measurement, "min_instr_cnt", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.min_instr_cnt = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "max_instr_cnt", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.max_instr_cnt = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "instr_cnt_step", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.instr_cnt_step = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "warmup_iterations", measurement_path,
                               [&](const YAML::Node& node, const std::string& node_path) {
                                   config.warmup_iterations = parse_size_scalar(node, node_path);
                               });
            with_optional_node(measurement, "inner_iterations", measurement_path,
                               [&](const YAML::Node& node, const std::string& node_path) {
                                   config.inner_iterations = parse_size_scalar(node, node_path);
                               });
            with_optional_node(measurement, "outer_iterations", measurement_path,
                               [&](const YAML::Node& node, const std::string& node_path) {
                                   config.outer_iterations = parse_size_scalar(node, node_path);
                               });
            with_optional_node(measurement, "instr_type", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.instr_type = static_cast<int>(parse_instruction_type(node, node_path));
            });
        });

        with_mapping(section, "detection", path, [&](const YAML::Node& detection, const std::string& detection_path) {
            with_optional_node(detection, "baseline_fraction", detection_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.baseline_fraction = parse_double_scalar(node, node_path);
            });
            with_optional_node(detection, "baseline_min_samples", detection_path,
                               [&](const YAML::Node& node, const std::string& node_path) {
                                   config.baseline_min_samples = parse_size_scalar(node, node_path);
                               });
            with_optional_node(detection, "required_consecutive_points", detection_path,
                               [&](const YAML::Node& node, const std::string& node_path) {
                                   config.required_consecutive_points = parse_size_scalar(node, node_path);
                               });
            with_optional_node(detection, "saturation_threshold_ratio", detection_path,
                               [&](const YAML::Node& node, const std::string& node_path) {
                                   config.saturation_threshold_ratio = parse_double_scalar(node, node_path);
                               });
            with_optional_node(detection, "fallback_jump_ratio", detection_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.fallback_jump_ratio = parse_double_scalar(node, node_path);
            });
        });
    }
};

class BhtConfigParser final : public BenchmarkConfigParserBase<silicon_probe::branch_history_table::BranchHistoryTableMeasurer::Config> {
  public:
    BhtConfigParser() : BenchmarkConfigParserBase("branch_history_table") {}

  private:
    void parse_specific(const YAML::Node& section, const std::string& path,
                        silicon_probe::branch_history_table::BranchHistoryTableMeasurer::Config& config) const override {
        with_mapping(section, "measurement", path, [&](const YAML::Node& measurement, const std::string& measurement_path) {
            with_optional_node(measurement, "min_period", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.min_period = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "max_period", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.max_period = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "period_coeff", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.period_coeff = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "iterations", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.iterations = parse_size_scalar(node, node_path);
            });
        });
    }
};

class RasConfigParser final : public BenchmarkConfigParserBase<silicon_probe::return_address_stack::ReturnAddressStackMeasurer::Config> {
  public:
    RasConfigParser() : BenchmarkConfigParserBase("return_address_stack") {}

  private:
    void parse_specific(const YAML::Node& section, const std::string& path,
                        silicon_probe::return_address_stack::ReturnAddressStackMeasurer::Config& config) const override {
        with_mapping(section, "measurement", path, [&](const YAML::Node& measurement, const std::string& measurement_path) {
            with_optional_node(measurement, "min_recursion_depth", measurement_path,
                               [&](const YAML::Node& node, const std::string& node_path) {
                                   config.min_recursion_depth = parse_size_scalar(node, node_path);
                               });
            with_optional_node(measurement, "max_recursion_depth", measurement_path,
                               [&](const YAML::Node& node, const std::string& node_path) {
                                   config.max_recursion_depth = parse_size_scalar(node, node_path);
                               });
            with_optional_node(measurement, "recursion_depth_step", measurement_path,
                               [&](const YAML::Node& node, const std::string& node_path) {
                                   config.recursion_depth_step = parse_size_scalar(node, node_path);
                               });
            with_optional_node(measurement, "iterations", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.iterations = parse_size_scalar(node, node_path);
            });
        });

        with_mapping(section, "detection", path, [&](const YAML::Node& detection, const std::string& detection_path) {
            with_optional_node(detection, "trim_ratio", detection_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.trim_ratio = parse_double_scalar(node, node_path);
            });
            with_optional_node(detection, "baseline_min_depth", detection_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.baseline_min_depth = parse_size_scalar(node, node_path);
            });
            with_optional_node(detection, "baseline_max_depth", detection_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.baseline_max_depth = parse_size_scalar(node, node_path);
            });
            with_optional_node(detection, "saturation_threshold_ratio", detection_path,
                               [&](const YAML::Node& node, const std::string& node_path) {
                                   config.saturation_threshold_ratio = parse_double_scalar(node, node_path);
                               });
            with_optional_node(detection, "required_consecutive_points", detection_path,
                               [&](const YAML::Node& node, const std::string& node_path) {
                                   config.required_consecutive_points = parse_size_scalar(node, node_path);
                               });
        });
    }
};

class ExecPortsConfigParser final : public BenchmarkConfigParserBase<silicon_probe::exec_ports::ExecPortsMeasurer::Config> {
  public:
    ExecPortsConfigParser() : BenchmarkConfigParserBase("exec_ports") {}

  private:
    void parse_specific(const YAML::Node& section, const std::string& path,
                        silicon_probe::exec_ports::ExecPortsMeasurer::Config& config) const override {
        with_mapping(section, "measurement", path, [&](const YAML::Node& measurement, const std::string& measurement_path) {
            with_optional_node(measurement, "instr_cnt", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.instr_cnt = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "iterations", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.iterations = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "repeats", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.repeats = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "warmup_iterations", measurement_path,
                               [&](const YAML::Node& node, const std::string& node_path) {
                                   config.warmup_iterations = parse_size_scalar(node, node_path);
                               });
            with_optional_node(measurement, "instr1", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                parse_instruction_data(node, node_path, config.instr1);
            });
            with_optional_node(measurement, "instr2", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                parse_instruction_data(node, node_path, config.instr2);
            });
        });
    }
};

class UopsCacheConfigParser final : public BenchmarkConfigParserBase<silicon_probe::uops_cache::UopsCacheMeasurer::Config> {
  public:
    UopsCacheConfigParser() : BenchmarkConfigParserBase("uops_cache") {}

  private:
    void parse_specific(const YAML::Node& section, const std::string& path,
                        silicon_probe::uops_cache::UopsCacheMeasurer::Config& config) const override {
        with_mapping(section, "measurement", path, [&](const YAML::Node& measurement, const std::string& measurement_path) {
            with_optional_node(measurement, "min_instr_cnt", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.min_instr_cnt = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "max_instr_cnt", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.max_instr_cnt = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "instr_step", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.instr_step = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "iterations", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.iterations = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "repeats", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.repeats = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "warmup_iterations", measurement_path,
                               [&](const YAML::Node& node, const std::string& node_path) {
                                   config.warmup_iterations = parse_size_scalar(node, node_path);
                               });
            with_optional_node(measurement, "instr", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                parse_instruction_data(node, node_path, config.instr);
            });
        });

        with_mapping(section, "detection", path, [&](const YAML::Node& detection, const std::string& detection_path) {
            with_optional_node(detection, "dsb_share_stop", detection_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.dsb_share_stop = parse_double_scalar(node, node_path);
            });
            with_optional_node(detection, "dsb_share_refine", detection_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.dsb_share_refine = parse_double_scalar(node, node_path);
            });
            with_optional_node(detection, "dsb_drop_significant", detection_path,
                               [&](const YAML::Node& node, const std::string& node_path) {
                                   config.dsb_drop_significant = parse_double_scalar(node, node_path);
                               });
            with_optional_node(detection, "coarse_ignore_first", detection_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.coarse_ignore_first = parse_size_scalar(node, node_path);
            });
        });
    }
};

class BtbConfigParser final : public BenchmarkConfigParserBase<silicon_probe::branch_target_buffer::BranchTargetBufferMeasurer::Config> {
  public:
    BtbConfigParser() : BenchmarkConfigParserBase("branch_target_buffer") {}

  private:
    void parse_specific(const YAML::Node& section, const std::string& path,
                        silicon_probe::branch_target_buffer::BranchTargetBufferMeasurer::Config& config) const override {
        with_mapping(section, "measurement", path, [&](const YAML::Node& measurement, const std::string& measurement_path) {
            with_optional_node(measurement, "min_blocks_cnt", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.min_blocks_cnt = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "max_blocks_cnt", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.max_blocks_cnt = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "blocks_step", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.blocks_step = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "iterations", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.iterations = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "repeats", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.repeats = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "warmup_iterations", measurement_path,
                               [&](const YAML::Node& node, const std::string& node_path) {
                                   config.warmup_iterations = parse_size_scalar(node, node_path);
                               });
            with_optional_node(measurement, "alignment", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.alignment = parse_int_scalar(node, node_path);
            });
        });

        with_mapping(section, "detection", path, [&](const YAML::Node& detection, const std::string& detection_path) {
            with_optional_node(detection, "misprediction_saturation_threshold", detection_path,
                               [&](const YAML::Node& node, const std::string& node_path) {
                                   config.misprediction_saturation_threshold = parse_double_scalar(node, node_path);
                               });
            with_optional_node(detection, "misprediction_growth_threshold", detection_path,
                               [&](const YAML::Node& node, const std::string& node_path) {
                                   config.misprediction_growth_threshold = parse_double_scalar(node, node_path);
                               });
            with_optional_node(detection, "time_growth_ratio", detection_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.time_growth_ratio = parse_double_scalar(node, node_path);
            });
            with_optional_node(detection, "time_stability_points", detection_path,
                               [&](const YAML::Node& node, const std::string& node_path) {
                                   config.time_stability_points = parse_size_scalar(node, node_path);
                               });
            with_optional_node(detection, "coarse_ignore_first", detection_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.coarse_ignore_first = parse_size_scalar(node, node_path);
            });
        });
    }
};

class S2LFwdConfigParser final : public BenchmarkConfigParserBase<silicon_probe::store_to_load_forwarding::StoreToLoadForwardingMeasurer::Config> {
  public:
    S2LFwdConfigParser() : BenchmarkConfigParserBase("store_to_load_forwarding") {}

  private:
    void parse_specific(const YAML::Node& section, const std::string& path,
                        silicon_probe::store_to_load_forwarding::StoreToLoadForwardingMeasurer::Config& config) const override {
        with_mapping(section, "measurement", path, [&](const YAML::Node& measurement, const std::string& measurement_path) {
            with_optional_node(measurement, "min_offset", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.min_offset = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "max_offset", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.max_offset = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "offset_step", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.offset_step = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "iterations", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.iterations = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "repeats", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.repeats = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "warmup_iterations", measurement_path,
                               [&](const YAML::Node& node, const std::string& node_path) {
                                   config.warmup_iterations = parse_size_scalar(node, node_path);
                               });
        });

        with_mapping(section, "detection", path, [&](const YAML::Node& detection, const std::string& detection_path) {
            with_optional_node(detection, "pmc_saturation_ratio", detection_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.pmc_saturation_ratio = parse_double_scalar(node, node_path);
            });
            with_optional_node(detection, "time_growth_ratio", detection_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.time_growth_ratio = parse_double_scalar(node, node_path);
            });
        });
    }
};

class WriteBufferConfigParser final : public BenchmarkConfigParserBase<silicon_probe::write_buffer::WriteBufferMeasurer::Config> {
  public:
    WriteBufferConfigParser() : BenchmarkConfigParserBase("write_buffer") {}

  private:
    void parse_specific(const YAML::Node& section, const std::string& path,
                        silicon_probe::write_buffer::WriteBufferMeasurer::Config& config) const override {
        with_mapping(section, "measurement", path, [&](const YAML::Node& measurement, const std::string& measurement_path) {
            with_optional_node(measurement, "max_writes", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.max_writes = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "min_writes", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.min_writes = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "writes_step", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.writes_step = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "iterations", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.iterations = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "repeats", measurement_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.repeats = parse_size_scalar(node, node_path);
            });
            with_optional_node(measurement, "warmup_iterations", measurement_path,
                               [&](const YAML::Node& node, const std::string& node_path) {
                                   config.warmup_iterations = parse_size_scalar(node, node_path);
                               });
        });

        with_mapping(section, "detection", path, [&](const YAML::Node& detection, const std::string& detection_path) {
            with_optional_node(detection, "latency_growth_ratio", detection_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.latency_growth_ratio = parse_double_scalar(node, node_path);
            });
            with_optional_node(detection, "pmc_saturation_ratio", detection_path, [&](const YAML::Node& node, const std::string& node_path) {
                config.pmc_saturation_ratio = parse_double_scalar(node, node_path);
            });
        });
    }
};

} // namespace

namespace silicon_probe::app {

ApplicationConfig ApplicationConfigLoader::load(const BootstrapOptions& options) const {
    const LoadedConfigDocument document = load_document(options.config_path);

    ApplicationConfig config{};
    config.logging = options.logging;
    config.print_summary = options.print_summary;
    config.cache = CacheConfigParser{}.parse(document);
    config.tlb = TlbConfigParser{}.parse(document);
    config.rob = RobConfigParser{}.parse(document);
    config.bht = BhtConfigParser{}.parse(document);
    config.ras = RasConfigParser{}.parse(document);
    config.exec_ports = ExecPortsConfigParser{}.parse(document);
    config.uops_cache = UopsCacheConfigParser{}.parse(document);
    config.btb = BtbConfigParser{}.parse(document);
    config.s2l_fwd = S2LFwdConfigParser{}.parse(document);
    config.write_buffer = WriteBufferConfigParser{}.parse(document);
    return config;
}

} // namespace silicon_probe::app::detail
