#include "silicon_probe/app/application.hpp"
#include "silicon_probe/app/config.hpp"

#include <CLI/CLI.hpp>
#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    silicon_probe::app::CommandLineParser parser;

    try {
        const auto config = parser.parse(argc, argv);
        return silicon_probe::app::execute(config);
    } catch (const CLI::ParseError& error) {
        return parser.cli().exit(error);
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return 1;
    }
}
