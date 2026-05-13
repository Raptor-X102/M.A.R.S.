#pragma once

#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include "shared_types/cpu_info_data.hpp"

namespace silicon_probe::core {

class SummaryPrinter {
   private:
    static constexpr size_t kInnerWidth   = 76;
    static constexpr const char* kReset   = "\033[0m";
    static constexpr const char* kFrame   = "\033[38;5;81m";
    static constexpr const char* kTitle   = "\033[1;38;5;159m";
    static constexpr const char* kSection = "\033[1;38;5;223m";
    static constexpr const char* kAccent  = "\033[38;5;151m";
    static constexpr const char* kValue   = "\033[1;38;5;255m";
    static constexpr const char* kStatus  = "\033[1;38;5;120m";

    static size_t visible_length(const std::string& text) {
        size_t length = 0;
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\033' && i + 1 < text.size() && text[i + 1] == '[') {
                i += 2;
                while (i < text.size() && text[i] != 'm') {
                    ++i;
                }
                continue;
            }
            ++length;
        }
        return length;
    }

    static std::string truncate_visible(const std::string& text, size_t width) {
        std::string result;
        size_t visible = 0;

        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\033' && i + 1 < text.size() && text[i + 1] == '[') {
                const size_t begin = i;
                i += 2;
                while (i < text.size() && text[i] != 'm') {
                    ++i;
                }
                if (i < text.size()) {
                    result.append(text, begin, i - begin + 1);
                }
                continue;
            }

            if (visible >= width) {
                break;
            }

            result.push_back(text[i]);
            ++visible;
        }

        if (result.find('\033') != std::string::npos) {
            result += kReset;
        }

        return result;
    }

    static std::string pad_right(std::string text, size_t width) {
        const size_t length = visible_length(text);
        if (length > width) {
            return truncate_visible(text, width);
        }

        text.append(width - length, ' ');
        return text;
    }

    static std::string center_text(std::string text, size_t width) {
        const size_t length = visible_length(text);
        if (length >= width) {
            return truncate_visible(text, width);
        }

        const size_t total_padding = width - length;
        const size_t left_padding  = total_padding / 2;
        const size_t right_padding = total_padding - left_padding;
        return std::string(left_padding, ' ') + text + std::string(right_padding, ' ');
    }

    static void print_line(std::ostream& stream, const std::string& text) {
        stream << kFrame << "│ " << kReset << pad_right(text, kInnerWidth) << kFrame << " │" << kReset << '\n';
    }

    static std::string format_bytes(size_t bytes) {
        constexpr const char* units[] = {"B", "KiB", "MiB", "GiB"};
        double value                  = static_cast<double>(bytes);
        size_t unit_index             = 0;

        while (value >= 1024.0 && unit_index + 1 < std::size(units)) {
            value /= 1024.0;
            ++unit_index;
        }

        std::ostringstream out;
        if (unit_index == 0 || value >= 100.0) {
            out << static_cast<size_t>(value);
        } else if (value >= 10.0) {
            out.setf(std::ios::fixed);
            out.precision(1);
            out << value;
        } else {
            out.setf(std::ios::fixed);
            out.precision(2);
            out << value;
        }

        out << ' ' << units[unit_index];
        return out.str();
    }

    static std::string format_tlb(size_t pages, const std::optional<size_t>& page_size_bytes) {
        std::ostringstream out;
        out << pages << " pages";
        if (page_size_bytes) {
            out << " (~" << format_bytes(pages * *page_size_bytes) << ")";
        }
        return out.str();
    }

    static std::string format_exec_ports(const std::optional<bool>& value) {
        if (!value) {
            return "not detected";
        }
        return *value ? "independent" : "shared/dependent";
    }

    static std::string colorize(const char* color, const std::string& text) {
        return std::string(color) + text + kReset;
    }

    static std::string make_row(const std::string& label, const std::string& value) {
        return colorize(kAccent, label) + colorize(kValue, value);
    }

    static void print_section(std::ostream& stream, const std::string& title, const std::vector<std::string>& rows) {
        if (rows.empty()) {
            return;
        }

        print_line(stream, "");
        print_line(stream, "  " + colorize(kSection, "[" + title + "]"));
        for (const auto& row : rows) {
            print_line(stream, "  " + row);
        }
    }

   public:
    static void print(std::ostream& stream, const shared_types::CpuInfoData& data) {
        std::vector<std::string> cpu_rows;
        std::vector<std::string> cache_rows;
        std::vector<std::string> tlb_rows;
        std::vector<std::string> core_rows;
        std::vector<std::string> branch_rows;
        std::vector<std::string> memory_rows;

        cpu_rows.push_back(make_row("Vendor: ", data.cpu_vendor ? std::string(data.cpu_vendor->name()) : "unknown"));

        if (data.cache_line_size) {
            cpu_rows.push_back(make_row("Cache line: ", format_bytes(*data.cache_line_size)));
        }
        if (data.pipeline_depth) {
            cpu_rows.push_back(make_row("Pipeline depth: ", std::to_string(*data.pipeline_depth) + " stages"));
        }

        if (data.l1d_size) {
            cache_rows.push_back(make_row("L1D: ", format_bytes(*data.l1d_size)));
        }
        if (data.l1i_size) {
            cache_rows.push_back(make_row("L1I: ", format_bytes(*data.l1i_size)));
        }
        if (data.l2_size) {
            cache_rows.push_back(make_row("L2:  ", format_bytes(*data.l2_size)));
        }
        if (data.l3_size) {
            cache_rows.push_back(make_row("L3:  ", format_bytes(*data.l3_size)));
        }
        if (data.is_inclusive_cache) {
            cache_rows.push_back(make_row("Inclusive LLC: ", *data.is_inclusive_cache ? "yes" : "no"));
        }

        if (data.tlb_l1_size) {
            tlb_rows.push_back(make_row("L1 DTLB: ", format_tlb(*data.tlb_l1_size, data.tlb_page_size_bytes)));
        }
        if (data.tlb_l2_size) {
            tlb_rows.push_back(make_row("L2/STLB: ", format_tlb(*data.tlb_l2_size, data.tlb_page_size_bytes)));
        }

        if (data.rob_size) {
            core_rows.push_back(make_row("ROB: ", std::to_string(*data.rob_size) + " entries"));
        }
        if (data.uops_cache_size) {
            core_rows.push_back(make_row("uOps cache: ", std::to_string(*data.uops_cache_size) + " uops"));
        }
        if (data.execution_ports_independent) {
            core_rows.push_back(make_row("Execution ports: ", format_exec_ports(data.execution_ports_independent)));
        }

        if (data.bht_size) {
            branch_rows.push_back(make_row("BHT: ", std::to_string(*data.bht_size) + " entries"));
        }
        if (data.ras_size) {
            branch_rows.push_back(make_row("RAS: ", std::to_string(*data.ras_size) + " entries"));
        }
        if (data.btb_size) {
            branch_rows.push_back(make_row("BTB: ", std::to_string(*data.btb_size) + " entries"));
        }

        if (data.s2l_fwd_max_size) {
            std::string row = "Store-to-load forwarding: " + format_bytes(*data.s2l_fwd_max_size);
            if (data.s2l_fwd_max_offset) {
                row += ", max offset " + format_bytes(*data.s2l_fwd_max_offset);
            }
            memory_rows.push_back(make_row("", row));
        }
        if (data.write_buffer_size) {
            memory_rows.push_back(make_row("Write buffer: ", std::to_string(*data.write_buffer_size) + " entries"));
        }

        // clang-format off
        stream << '\n';
        stream << kFrame << "╭──────────────────────────────────────────────────────────────────────────────╮"
               << kReset << '\n';
        print_line(stream, colorize(kTitle, "M.A.R.S. / BENCHMARK SUMMARY"));
        stream << kFrame << "├──────────────────────────────────────────────────────────────────────────────┤"
               << kReset << '\n';
        print_line(stream, "");
        print_line(stream, colorize(kTitle, center_text("MICROARCHITECTURE RECON REPORT", kInnerWidth)));

        print_section(stream, "CPU", cpu_rows);
        print_section(stream, "CACHE", cache_rows);
        print_section(stream, "TLB", tlb_rows);
        print_section(stream, "CORE", core_rows);
        print_section(stream, "BRANCH", branch_rows);
        print_section(stream, "MEMORY", memory_rows);

        print_line(stream, "");
        print_line(stream, colorize(kStatus, center_text("[ REPORT COMPLETE ]", kInnerWidth)));
        stream << kFrame << "╰──────────────────────────────────────────────────────────────────────────────╯"
               << kReset << "\n\n";
        // clang-format on
    }
};

}  // namespace silicon_probe::core
