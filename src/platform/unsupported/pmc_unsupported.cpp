#include "infra/logging.hpp"
#include "platform/pmc.hpp"

namespace silicon_probe::platform::pmc {

class PmcGroupUnsupported final : public PmcGroup {
   public:
    [[noreturn]] void reset() override { throw std::runtime_error("PMC not supported on this platform"); }
    [[noreturn]] void enable() override { throw std::runtime_error("PMC not supported on this platform"); }
    [[noreturn]] void disable() override { throw std::runtime_error("PMC not supported on this platform"); }
    [[nodiscard]] CounterValues read() const override {
        throw std::runtime_error("PMC not supported on this platform");
    }
};

std::unique_ptr<PmcGroup> PmcGroup::create(const std::string&, const std::string&, const std::string&) {
    SPDLOG_ERROR("PMC counters are not supported on this platform");
    return nullptr;
}

bool PmcGroup::is_supported() noexcept { return false; }

}  // namespace silicon_probe::platform::pmc
