#include "config.hpp"
#include "server.hpp"
#include <cstdlib>
#include <iostream>

int main(int argc, char* argv[]) {
    std::string config_path = "config.yaml";
    if (argc > 1) {
        config_path = argv[1];
    }

    Config config;
    try {
        config = Config::loadFromFile(config_path);
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    Server server(config);

    server.start();

    return 0;
}