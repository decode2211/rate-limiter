#include "config.hpp"
#include "server.hpp"
#include <iostream>

int main(int argc, char* argv[]) {
    std::string config_path = "config.yaml";
    if (argc > 1) {
        config_path = argv[1];
    }

    Config config = Config::loadFromFile(config_path);
    Server server(config);

    server.start();

    return 0;
}