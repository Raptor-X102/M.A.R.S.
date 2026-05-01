#include <CLI/CLI.hpp>
#include <exception>
#include <iostream>

#include "app/application.hpp"
#include "app/config.hpp"

int main(int argc, char** argv) {
    silicon_probe::app::BootstrapOptionsParser parser{};
    silicon_probe::app::ApplicationConfigLoader config_loader{};

    try {
        const auto bootstrap = parser.parse(argc, argv);
        const auto config = config_loader.load(bootstrap);

        return silicon_probe::app::execute(config);
    } catch (const CLI::ParseError& error) {
        return parser.exit(error);
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return 1;
    }
}
